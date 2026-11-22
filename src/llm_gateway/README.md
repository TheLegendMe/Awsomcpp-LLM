# LLM Gateway

基于 Drogon (C++) 的 LLM API 网关，支持多模型路由、Lua 插件管线、语义缓存。

## 硬件环境

| 项目 | 配置 |
|------|------|
| GPU | NVIDIA RTX 5070 12GB |
| 环境 | WSL2 |
| 推理引擎 | Ollama 0.24.0 |
| 并发推理 | OLLAMA_NUM_PARALLEL=4 |

## 模型

| 模型 | 量化 | 大小 | 显存 | 状态 |
|------|------|------|------|------|
| qwen2.5:0.5b-instruct | Q4_K_M | 397MB | ~0.5GB | 全 GPU |
| qwen2.5:7b-instruct | Q4_K_M | 4.7GB | ~5GB | 全 GPU |

## 压测结果

### 7B Qwen (主力模型，全 GPU)

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

### 0.5B Qwen (轻量 fallback，全 GPU)

```
┌──────────────────┬─────────┬──────────┬──────────┬──────────┐
│ 场景              │ 并发/请求 │   QPS    │   P50    │   P99    │
├──────────────────┼─────────┼──────────┼──────────┼──────────┤
│ 基准 (单并发)      │  1 / 10  │  11.4    │   75ms   │  133ms   │
│ 缓存未命中 100%    │ 100/ 50 │   26.4   │ 1062ms   │ 1882ms   │
└──────────────────┴─────────┴──────────┴──────────┴──────────┘
```

### 14B DeepSeek R1 (CPU/GPU split，仅参考)

13GB 加载 > 12GB 显存，46% offload 到 CPU，性能不可用。

```
┌──────────────────┬─────────┬──────────┬──────────┬──────────┐
│ 场景              │ 并发/请求 │   QPS    │   P50    │   P99    │
├──────────────────┼─────────┼──────────┼──────────┼──────────┤
│ 基准 (单并发)      │  1 /  5  │   0.9    │ 1115ms   │ 1162ms   │
│ 缓存未命中 100%    │ 10 / 15 │   1.4    │ 5981ms   │ 8341ms   │
└──────────────────┴─────────┴──────────┴──────────┴──────────┘
```

### 结论

- **网关本身 2000 QPS**，不是瓶颈
- **7B Q4 全 GPU** 是 12GB 显存甜点位，单次 159ms，缓存命中后 P50 仅 3ms
- **70% 缓存命中** 场景下 QPS 从 6.1 提升到 71.7（11x）
- **OLLAMA_NUM_PARALLEL=4** 高并发下 P90 改善 ~20%
- **14B 模型** 无法完整装入 12GB 显存，CPU/GPU split 性能不可接受

## 快速开始

```bash
# 启动 Ollama
ollama serve

# 启动网关
cd llm_gateway && ./build/llm_gateway conf/gateway.json

# 压测
python3 bench.py baseline  1  10   # 基准
python3 bench.py cache_hit 100 100 # 缓存命中
python3 bench.py cache_miss 100 50 # 缓存未命中
python3 bench.py mixed 100 100 0.7 # 混合
```

## 架构

```
Client → Gateway (:8080) → Plugin Pipeline → Ollama (:11434)
                              │
                        ┌─────┼─────┐
                        │ Auth  (100)│
                        │ Mod   (90) │
                        │ Cache (80) │  ← priority
                        │ Trans (50) │
                        │ Log   (10) │
                        └───────────┘
```

缓存插件命中时 `ABORT_EARLY` 直接返回，不调用 Ollama。

---

## 自主 Agent 循环 (AgentRunner)

网关内置一个目标驱动的 ReAct 循环。与 `chat` 的「单轮 + 客户端自己驱动下一轮」不同，
`run` 让网关在**服务端自动迭代**，直到任务完成：

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

### 测试

端到端（mock LLM 两轮工具调用 + mock executor）：

```bash
python3 -m venv /tmp/wsenv && /tmp/wsenv/bin/pip install websockets aiohttp
/tmp/wsenv/bin/python tests/test_agent_loop.py
```

期望：`events: run_started → step → tool_call → tool_result → ... → run_done`，
`run_done.reason=completed`，`steps=3`。

