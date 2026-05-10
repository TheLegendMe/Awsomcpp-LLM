# LLM Gateway

高性能 C++ LLM API 网关，统一前端入口、多模型路由、工具编排、插件扩展。

## 亮点

- **多模型路由** — 按 `model` 字段负载均衡到不同后端，加权轮询，健康检查
- **WebSocket 长连接** — 单连接复用多轮对话，支持并发请求和流式增量推送
- **工具编排** — LLM function calling → 路由到 executor 执行 → 结果回传，闭环自动化
- **LRU 内存缓存** — 跨请求跨线程共享，TTL 过期，缓存命中直接返回不消耗推理算力
- **Lua 沙箱插件** — 请求前 / 每 token / 请求后三阶段可编程管道，热加载，inotify 监控
- **统一入口** — 自带 WebUI，HTTP SSE 兼容 OpenAI API，`/health` `/metrics` `/tools` 开箱即用
- **跨平台** — C++20 + Drogon 框架，Linux/Windows/macOS 均可部署

## 架构

```
┌──────────┐     WebSocket      ┌──────────────┐     HTTP SSE      ┌──────────┐
│  WebUI   │◄──────────────────►│              │◄────────────────►│  LLM     │
│  Client  │    JSON {type}     │   Gateway    │   curl_multi     │  Backend │
│  Executor│                    │   :8080      │   POST /tools    │  :8300   │
└──────────┘                    └──────────────┘                  └──────────┘
                                       │
                                       ├─ Auth (API Key + Token Bucket)
                                       ├─ Router (weighted round-robin)
                                       ├─ Plugin Pipeline (Lua sandbox)
                                       ├─ Tool Registry + Executor Pool
                                       └─ CacheStore (LRU + TTL)
```

## 快速开始

### 编译

```bash
cd day01/llm_gateway/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 启动

```bash
cd day01/llm_gateway
./build/llm_gateway conf/gateway.json
```

访问 `http://localhost:8080` 打开 WebUI。

### Docker

```bash
docker build -t llm-gateway -f day01/llm_gateway/Dockerfile day01/llm_gateway/
docker run -p 8080:8080 llm-gateway
```

## API 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/` | GET | WebUI 静态页面 |
| `/ws/chat?key=xxx` | WebSocket | 主通信通道 |
| `/v1/chat/completions` | POST | OpenAI 兼容 SSE |
| `/health` | GET | 健康检查 |
| `/metrics` | GET | 延迟/吞吐/错误率 |
| `/tools` | GET | 已注册工具列表 |
| `/plugins` | GET | 插件列表 |

## WebSocket 消息协议

### Client → Gateway

| type | 说明 |
|------|------|
| `chat` | 发起对话 `{type, request_id, model, messages[], tools?}` |
| `tool_call` | 请求工具执行 `{type, request_id, call_id, name, arguments}` |
| `tool_result` | 返回工具结果 `{type, request_id, call_id, content}` |
| `register` | 注册 executor `{type, role:"executor", tools[...]}` |
| `cancel` | 取消请求 `{type, request_id}` |
| `ping` | 心跳 |

### Gateway → Client

| type | 说明 |
|------|------|
| `connected` | 鉴权通过 `{type, session_id}` |
| `token` | 流式增量 `{type, request_id, content}` |
| `tool_call` | LLM 生成的工具调用 |
| `tool_result` | 工具执行结果回传 |
| `done` | 请求完成 |
| `error` | 错误 `{type, request_id, code, message}` |
| `abort` | 插件拦截 |
| `pong` | 心跳回复 |

## 工具编排流程

```
WebUI ──chat{+tools}──► Gateway ──POST /v1/chat/completions──►  LLM
                         │   ▲                                   │
                         │   │ SSE tool_calls delta              │
                         │   ▼                                   │
                         │  SSEParser 解析累积 tool_call          │
                         │   │                                   │
                         │   ├─► WebUI: 推送 tool_call            │
                         │   └─► Executor: 路由执行               │
                         │         │                              │
                         │   tool_result 回传                     │
                         └────────┘
```

## 配置文件

`conf/gateway.json`:

```json
{
  "app": { "threads_num": 4, "max_connections": 10000 },
  "backends": [
    { "name": "llama-7b", "url": "http://127.0.0.1:8200/v1/chat/completions",
      "model": "big-model", "weight": 1, "route_model": "big-model" }
  ],
  "api_keys": [
    { "key": "test-key-123", "models": "*", "qps": 60, "enabled": true }
  ]
}
```

## 依赖

- [Drogon](https://github.com/drogonframework/drogon) v1.9.10 — HTTP/WebSocket 框架
- [llama.cpp](https://github.com/ggerganov/llama.cpp) — LLM 推理后端
- [sol2](https://github.com/ThePhD/sol2) — Lua/C++ 绑定
- Lua 5.3, curl, nlohmann-json, libevent

## License

MIT
