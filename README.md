# LLM Gateway

基于 C++20 + [Drogon](https://github.com/drogonframework/drogon) 的 LLM API 网关：OpenAI 风格 SSE、WebSocket 流式对话、服务端 ReAct Agent 循环、三阶段 Lua 插件管线、多级缓存与多后端路由。

## 亮点

- **双通道接入** — OpenAI 风格 `POST /v1/chat/completions`（SSE）与 WebSocket `/ws/chat`，同一套插件与上游管线
- **服务端 Agent 循环** — `run` 由网关自动迭代：LLM → `tool_call` → executor 执行 → `tool_result` → 下一轮，直到完成/`max_steps`/取消
- **工具编排** — executor 通过 WebSocket 注册工具，网关解析流式 `tool_calls` 并轮询路由到可用 executor，结果归一化后回填对话
- **Lua 插件管线** — `on_request` / `on_token` / `on_done` 三阶段，`CONTINUE/MODIFY/ABORT_EARLY` 语义，优先级排序 + inotify 热加载
- **缓存体系** — 精确匹配 LRU+TTL、embedding 语义缓存、面向多 Agent 接力的 L1/L2/L3 三级缓存（置信度衰减、级联失效）
- **多后端路由** — 按 `model` 精确匹配、`*` 兜底，组内加权轮询
- **鉴权限流** — API Key 校验 + 令牌桶；HTTP 每请求、WS 连接期鉴权
- **可观测性** — `/health`、`/metrics`（请求量、在途、token 数、错误、平均/最大延迟、首 token 延迟），自带 WebUI

## 架构

```
                HTTP SSE / WebSocket
  WebUI / Executor / Agent (WS)
              │
              ▼
  ┌────────────────────────────────┐      HTTP SSE       ┌──────────────────┐
  │        LLM Gateway :8080       │ ──────────────────► │ Ollama / 远端后端 │
  │                                │ ◄────────────────── │                  │
  │ Auth → Router → Plugin         │  token / tool_call  └──────────────────┘
  │ → Upstream → AgentRunner       │
  │ Cache (LRU / Semantic / L1-L3) │
  └────────────────────────────────┘
```

一次 `chat` 请求的生命周期：

```
请求 → 鉴权(可选 Key) → 路由(model → 后端)
     → Lua on_request ──ABORT_EARLY──► 直接返回（缓存命中 / 审查拦截）
     → 上游 SSE → 每个 token 过 on_token → 客户端
     → on_done → 缓存写回
```

`run` 则在此之上多一层服务端循环，见 [服务端 Agent 循环](#服务端-agent-循环agentrunner)。

## 快速开始

### 依赖

- Linux（热重载依赖 inotify；WSL2 下验证）
- C++20 编译器、CMake ≥ 3.20
- Drogon 1.9.x、Lua 5.3、sol2（`third_party/sol`）、libcurl、nlohmann-json、libevent

```bash
# Ubuntu 依赖示例（Drogon 按官方文档编译安装）
sudo apt install build-essential cmake pkg-config \
    libcurl4-openssl-dev liblua5.3-dev nlohmann-json3-dev libevent-dev
```

### 编译与启动

```bash
# 启动推理后端
ollama serve

cd src/llm_gateway
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/llm_gateway conf/gateway.json
```

访问 `http://localhost:8080` 打开 WebUI。默认监听 `0.0.0.0:8080`、4 个 worker 线程。

### Docker

```bash
docker build -t llm-gateway -f src/llm_gateway/Dockerfile src/llm_gateway
docker run -p 8080:8080 llm-gateway
```

## API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/` | GET | WebUI |
| `/v1/chat/completions` | POST | OpenAI 风格，SSE 流式返回 |
| `/ws/chat?key=xxx` | WebSocket | 对话、Agent 循环、工具/Agent 注册 |
| `/health` | GET | 健康检查 |
| `/metrics` | GET | 纯文本指标快照 |
| `/tools` | GET | 已注册工具列表（OpenAI function schema） |
| `/plugins` | GET | 已加载插件列表 |

鉴权：HTTP 读 `Authorization: Bearer <key>`，WS 读 `?key=` 或 Authorization。**空 Key 直接放行**（便于本地开发）；非空 Key 必须匹配配置且未超过令牌桶限流。

> 当前 `/v1/chat/completions` 始终以 SSE 流式返回，请求体里的 `stream:false` 不生效。

## WebSocket 协议

握手阶段鉴权，消息均为 JSON，以 `type` 分派。

### Client → Gateway

| type | 说明 |
|------|------|
| `chat` | `{type, request_id, model, messages[], tools?, temperature?, max_tokens?}` |
| `run` / `agent_run` | 服务端 Agent 循环 `{type, request_id, model, goal 或 messages[], system?, tools?, max_steps?}` |
| `cancel` | 取消请求或 Agent 循环 `{type, request_id}` |
| `register` | 注册 `role:"executor"`（携带 `tools[]`）或 `role:"agent"`（携带 `capabilities[]`） |
| `tool_call` | 客户端主动请求工具执行 `{type, request_id, call_id, name, arguments}` |
| `tool_result` | 回传工具结果 `{type, request_id, call_id, success, content/data/error}` |
| `agent_call` | Agent 间调用 `{type, call_id, target_agent 或 capability, input, bypass_cache?}` |
| `agent_result` | 回传 Agent 结果 `{type, call_id, output, confidence?}` |
| `cache_invalidate` | 失效缓存 `{type, key, reason, cascade?}` |
| `context_expand` | 展开缓存/会话上下文 `{type, call_id, ref, max_tokens?}` |
| `ping` | 心跳 |

### Gateway → Client

| type | 说明 |
|------|------|
| `connected` | 鉴权通过 `{session_id}` |
| `registered` | 注册成功 `{role, tools/capabilities}` |
| `token` | 流式增量 `{request_id, step?, content}` |
| `tool_call` | 下发给 executor 的工具调用 |
| `tool_result` | 工具结果事件（Agent 循环观测） |
| `run_started` / `step` / `run_done` | Agent 循环生命周期 |
| `agent_result` | Agent 间调用结果 |
| `done` | 单轮对话完成 |
| `cancelled` | 取消确认 |
| `error` / `abort` | 错误 / 插件拦截 |
| `pong` | 心跳回复 |

## 服务端 Agent 循环（AgentRunner）

与 `chat` 的「单轮 + 客户端自己驱动下一轮」不同，`run` 让网关在**服务端自动迭代**，直到任务完成：

```
run(goal) ─► AgentRunner ──chat──► LLM
                  │                  │  流式 token / tool_calls
                  │◄─────────────────┘
                  ├─ 有 tool_calls → 派发给 executor → 收集 tool_result
                  │                  追加 role=tool 消息 → 下一轮
                  └─ 无 tool_calls → run_done(final content)
```

终止条件：模型不再调用工具 / 达到 `max_steps` / 出错 / `cancel`。

### 请求

```json
{
  "type": "run",
  "request_id": "r1",
  "model": "local-model",
  "system": "你是一个会使用工具的助手",
  "goal": "打开记事本并新建一个文件",
  "messages": [],
  "tools": [],
  "max_steps": 10,
  "temperature": 0.7,
  "max_tokens": 2048
}
```

- `goal` 会作为一条 `role:user` 消息追加；也可直接用 `messages` 给完整历史。
- `tools` 省略时使用 `ToolRegistry`（即所有已注册 executor 的工具）。
- `system` 可选；若 `messages` 首条不是 system 会前置一条。
- `max_steps` 默认 10，范围 1–50。

### 事件流（Gateway → Client）

| type | 字段 | 说明 |
|------|------|------|
| `run_started` | `max_steps`, `tools` | 循环开始 |
| `step` | `step`, `phase` | 阶段切换：`acting` / `observing` |
| `token` | `step`, `content` | 模型流式增量 |
| `tool_call` | `step`, `call_id`, `name`, `arguments` | 模型请求调用工具 |
| `tool_result` | `step`, `call_id`, `success`, `content` | 工具执行结果 |
| `run_done` | `steps`, `reason`, `content` | 结束。`reason`: `completed` / `max_steps` / `error` / `cancelled` |
| `error` / `abort` | | 与 chat 一致 |

取消：发送 `{"type":"cancel","request_id":"r1"}`；断开连接会取消该连接的所有循环。

### 实现要点

- `include/agent_runner.h` / `src/agent_runner.cpp` — 循环状态机。
- 上游回调来自 upstream I/O 线程，工具结果来自 WS EventLoop 线程；状态由 `mu_` 保护，
  事件通过 `ws_send`（`queueInLoop`）投递。
- `SSEParser` 在 `finish_reason=tool_calls` 后还会因 `data:[DONE]` **再次**触发 `on_done`；
  每个 LLM 轮次用 `TurnGuard::exchange` 保证 `on_done` 只生效一次。
- 每个 LLM 请求的 lambda 捕获 `shared_from_this()`，保证 in-flight 期间 runner 存活。
- 工具回传结果会把 executor 的 `{success, data, error, exit_code}` 归一化成给模型的
  `content` 字符串（旧 `chat` 路径只取 `content` 字段，常为空）。

## Lua 插件

插件是 `plugins_dir` 下的 `.lua` 文件，按 `priority` 从大到小执行。修改文件通过 inotify 自动热加载。

### Hook 契约

| hook | 入参 | 返回值 |
|------|------|--------|
| `on_request(ctx)` | `ctx` 表（见下） | 必须返回字符串：`"CONTINUE"` / `"MODIFY"` / `"ABORT_EARLY"` |
| `on_token(token)` | 当前 token 字符串 | 返回（可修改的）字符串；返回 `nil` 表示抑制该 token |
| `on_done(args)` | `{text = 完整回复}`，或 `chat` 路径下无参数 | 无 |

插件顶层可声明 `name`、`version`、`priority` 全局变量。

`ctx` 可用字段：`model`、`messages_json`、`temperature`、`max_tokens`、`user_ip`、`api_key`、`request_id`。
`on_request` 内可修改 `ctx.model / messages_json / temperature / max_tokens`，返回 `MODIFY` 后生效。附加函数：

- `ctx.set_response(status, body)`：设置 `ABORT_EARLY` 时的 HTTP 状态码与响应体
- `ctx.contains_sensitive()`：内置敏感词快速检测

### gateway API

| 函数 | 说明 |
|------|------|
| `gateway.log(level, msg)` | 写入网关日志 |
| `gateway.metric_inc(key, delta)` / `metric_get(key)` | 指标增减/读取 |
| `gateway.now_ms()` | 当前毫秒时间戳 |
| `gateway.cache_get(key)` / `cache_set(key, value, ttl)` / `cache_size()` | 精确匹配 LRU 缓存 |
| `gateway.semantic_search(text, threshold)` / `semantic_store(text, response)` / `semantic_size()` | embedding 语义缓存 |
| `gateway.http_post(url, body)` | 未启用，恒返回错误 JSON |

### 示例：缓存命中短路

```lua
name = "min"
version = "4"
priority = 80

function on_request(ctx)
    local msg = ctx.messages_json or ""
    if string.find(msg, "BENCHMARK_CACHE_HIT", 1, true) then
        ctx.set_response(200, '{"choices":[{"message":{"content":"Hello from cache!"}}]}')
        return "ABORT_EARLY"
    end
    return "CONTINUE"
end
```

`ABORT_EARLY` 会跳过上游调用，`example_moderation.lua` 正是用它在敏感词命中时直接返回 403。

### 沙箱边界

每个 worker 线程持有 `lua_pool_size` 个 VM（默认 8），请求开始时借出并在 `reset()` 后使用，RAII 归还：

- 只打开 `base / string / math / table / coroutine / utf8`
- 置空 `os / io / package / require / dofile / loadfile / load / raw* / debug` 等
- 仅暴露 `gateway.*` 一组 C++ 函数
- 50 万条 Lua 指令上限（count hook），防止死循环
- 每次请求前重置全局变量，跨请求不保留状态；跨请求状态请用 `gateway.cache_*`

**注意：这是可信一线插件的运行时隔离，不是安全沙箱。** 内存上限尚未强制、加载期脚本在独立 VM 中执行、
`semantic_*` 会在 worker 线程内做同步 HTTP，详见[已知限制](#已知限制--roadmap)。

## 配置

`conf/gateway.json` 同时作为 Drogon 配置和应用配置：

| 键 | 说明 |
|----|------|
| `listeners` / `app` | Drogon 监听地址、线程数、连接数、静态目录等 |
| `plugins_dir` | 插件目录，默认 `plugins` |
| `lua_pool_size` | 每线程 Lua VM 数量，默认 8 |
| `upstream_type` | 目前仅 `remote` 生效；`local_llama` 会回退到 remote |
| `backends[]` | `name`、`url`、`api_key`、`model`、`weight`、`route_model`（`*` 为兜底组） |
| `api_keys[]` | `key`、`models`、`qps`、`enabled` |

```json
{
  "app": { "threads_num": 4, "max_connections": 10000 },
  "plugins_dir": "plugins",
  "lua_pool_size": 8,
  "backends": [
    { "name": "ollama-qwen", "url": "http://127.0.0.1:11434/v1/chat/completions",
      "model": "qwen2.5:0.5b-instruct-q4_K_M", "weight": 1,
      "route_model": "qwen2.5:0.5b-instruct-q4_K_M" }
  ],
  "api_keys": [
    { "key": "test-key-123", "models": "*", "qps": 60, "enabled": true }
  ]
}
```

## 缓存

| 缓存 | 实现 | 容量/策略 |
|------|------|-----------|
| 精确匹配 | `CacheStore`，LRU + TTL | 10000 条，插件通过 `gateway.cache_*` 读写 |
| 语义缓存 | Ollama `nomic-embed-text` embedding + 余弦相似度 | 内存 deque 5000 条，线性扫描，插件阈值 0.90 |
| Agent 三级缓存 | L1 LRU（4096）+ 会话缓冲 + L2 文件持久化（置信度衰减/级联失效）+ L3 归档 | `data/l2_cache`、`data/l3_archive` |

示例缓存插件先走精确匹配，未命中再做语义检索；语义检索本身需要先调用 embedding 接口，属于额外延迟来源。

## 压测

环境：RTX 5070 12GB / WSL2 / Ollama 0.24.0 / `OLLAMA_NUM_PARALLEL=4`。

### 7B Qwen（主力模型，全 GPU）

```
┌──────────────────┬─────────┬──────────┬──────────┬──────────┐
│ 场景              │ 并发/请求 │   QPS    │   P50    │   P99    │
├──────────────────┼─────────┼──────────┼──────────┼──────────┤
│ 基准 (单并发)      │  1 / 10  │   6.1    │  159ms   │  173ms   │
│ 缓存命中 100%      │ 100/100 │  2030    │   3ms    │   7ms    │
│ 缓存未命中 100%    │ 100/ 50 │   23.5   │ 1231ms   │ 2081ms   │
│ 混合 70%命中+30%   │ 100/100 │   71.7   │   3ms*   │ 1343ms   │
└──────────────────┴─────────┴──────────┴──────────┴──────────┘
* P50=3ms 来自 70% 缓存命中
```

### 0.5B Qwen（轻量 fallback，全 GPU）

```
┌──────────────────┬─────────┬──────────┬──────────┬──────────┐
│ 场景              │ 并发/请求 │   QPS    │   P50    │   P99    │
├──────────────────┼─────────┼──────────┼──────────┼──────────┤
│ 基准 (单并发)      │  1 / 10  │  11.4    │   75ms   │  133ms   │
│ 缓存未命中 100%    │ 100/ 50 │   26.4   │ 1062ms   │ 1882ms   │
└──────────────────┴─────────┴──────────┴──────────┴──────────┘
```

### 14B DeepSeek R1（CPU/GPU split，仅参考）

13GB 加载 > 12GB 显存，46% offload 到 CPU，性能不可用。

```
┌──────────────────┬─────────┬──────────┬──────────┬──────────┐
│ 场景              │ 并发/请求 │   QPS    │   P50    │   P99    │
├──────────────────┼─────────┼──────────┼──────────┼──────────┤
│ 基准 (单并发)      │  1 /  5  │   0.9    │ 1115ms   │ 1162ms   │
│ 缓存未命中 100%    │ 10 / 15 │   1.4    │ 5981ms   │ 8341ms   │
└──────────────────┴─────────┴──────────┴──────────┴──────────┘
```

结论：

- **网关侧缓存命中路径 2030 QPS**（精确匹配短路，不含 embedding），不是模型吞吐
- **7B Q4 全 GPU** 是 12GB 显存甜点位，单次 159ms；缓存命中后 P50 仅 3ms
- **70% 缓存命中**下 QPS 从 6.1 提升到 71.7（约 11x）
- **`OLLAMA_NUM_PARALLEL=4`** 高并发下 P90 改善约 20%
- **14B 模型**无法完整装入 12GB 显存，CPU/GPU split 性能不可接受

## 测试

```bash
cd src/llm_gateway

# 插件功能测试（需已启动网关与推理后端）
bash tests/test.sh

# Agent 循环端到端测试：自动拉起 mock LLM 与网关
python3 -m venv /tmp/wsenv && /tmp/wsenv/bin/pip install websockets aiohttp
/tmp/wsenv/bin/python tests/test_agent_loop.py
```

`test_agent_loop.py` 验证事件序列 `run_started → step → tool_call → tool_result → ... → run_done`，
期望 `run_done.reason=completed`、`steps=3`。

压测：

```bash
python3 bench.py baseline  1  10    # 基准
python3 bench.py cache_hit 100 100  # 缓存命中
python3 bench.py cache_miss 100 50  # 缓存未命中
python3 bench.py mixed 100 100 0.7  # 混合
```

## 目录结构

```
src/llm_gateway/
├── include/                 # 头文件（网关、插件、Agent、缓存、路由）
├── src/
│   ├── main.cpp             # HTTP 入口、配置加载、生命周期
│   ├── ws_chat_controller.cpp # WebSocket 协议、工具/Agent 路由、三级缓存接入
│   ├── agent_runner.cpp     # 服务端 ReAct 循环状态机
│   ├── remote_upstream.cpp  # libcurl multi + SSE 解析
│   ├── plugin_manager.cpp   # 插件扫描/排序/热加载/管线执行
│   ├── lua_sandbox.cpp      # VM 沙箱创建与 API 注入
│   ├── lua_vm_pool.cpp      # 线程局部 VM 池
│   ├── semantic_cache.cpp   # embedding 语义缓存
│   └── agent_cache.cpp      # L1/L2/L3 + 会话缓冲
├── plugins/                 # Lua 插件（审查/缓存/翻译/日志等）
├── conf/gateway.json        # Drogon + 网关配置
├── static/                  # WebUI
├── tests/                   # 插件测试、Agent 端到端测试、mock 后端
├── bench.py                 # 压测脚本
├── CMakeLists.txt
└── Dockerfile
```

## 已知限制 / Roadmap

- **沙箱内存上限未强制**：`max_memory_kb` 目前只触发 GC，未接入自定义分配器
- 插件加载期会在独立 VM 中执行顶层脚本；`init()` 无法访问 `gateway.*`
- 插件无跨请求状态；每个请求重新执行源码（暂无字节码缓存）
- 每线程一个 VM 服务整条管线，插件之间共享 VM；`on_token` 每个 token 都会借 VM 重新执行脚本
- `gateway.semantic_*` 在 worker 线程内同步调用 embedding 接口，会阻塞该线程
- 上游时间配置 `upstream.connect_timeout/request_timeout` 尚未接入（连接超时硬编码 15s）
- 路由的健康标志没有主动探活，也没有失败节点自动摘除
- API Key 为空时放行；`models` 字段暂未做按模型授权
- `/v1/chat/completions` 忽略 `stream:false`，始终 SSE
- 语义缓存为内存态线性扫描，容量 5000；重启丢失
- 热重载依赖 inotify，仅 Linux；`orchestrator_dag.lua` 为实验样例，与当前全局 hook 约定不兼容

## 依赖

- [Drogon](https://github.com/drogonframework/drogon) 1.9.x — HTTP/WebSocket 框架
- [sol2](https://github.com/ThePhD/sol2) — Lua/C++ 绑定
- Lua 5.3、libcurl、nlohmann-json、libevent
- [Ollama](https://ollama.com/) — 本地推理后端（可选）

## License

MIT
