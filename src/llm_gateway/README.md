# LLM Gateway

完整文档（架构、API、WebSocket 协议、Lua 插件、缓存、压测、已知限制）见仓库根目录：

**[../../README.md](../../README.md)**

## 本目录速览

```
include/   src/   plugins/   conf/   static/   tests/
```

- `src/main.cpp` — HTTP 入口与生命周期
- `src/ws_chat_controller.cpp` — WebSocket 协议、工具/Agent 路由
- `src/agent_runner.cpp` — 服务端 ReAct 循环
- `src/plugin_manager.cpp` + `lua_sandbox.cpp` + `lua_vm_pool.cpp` — Lua 插件管线
- `src/remote_upstream.cpp` — libcurl multi + SSE
- `src/semantic_cache.cpp`、`src/agent_cache.cpp` — 缓存

## 快速启动

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/llm_gateway conf/gateway.json
```

默认 `http://localhost:8080`，推理后端默认指向本机 Ollama `:11434`。
