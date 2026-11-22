#!/usr/bin/env python3
"""
mock_llm_backend.py — 模拟 LLM 上游服务
  监听 0.0.0.0:8100，返回符合 OpenAI 格式的 SSE 流式响应
  支持高并发（asyncio），模拟真实 token 生成延迟

用法:
  python3 mock_llm_backend.py [--port 8100] [--tokens 20] [--delay-ms 30]
"""
import asyncio
import argparse
import json
import time
import sys
from aiohttp import web

# ─── 统计 ──────────────────────────────────────────────────────────
stats = {
    "total_requests": 0,
    "active_connections": 0,
    "start_time": time.time(),
}


def make_sse_chunk(token_idx: int, content: str, model: str = "mock-model") -> str:
    """构造一个 OpenAI 兼容的 SSE chunk"""
    chunk = {
        "id": f"chatcmpl-{token_idx:06d}",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [{
            "index": 0,
            "delta": {"content": content},
            "finish_reason": None,
        }],
    }
    return f"data: {json.dumps(chunk, ensure_ascii=False)}\n\n"


async def handle_chat(request: web.Request) -> web.StreamResponse:
    """POST /v1/chat/completions — SSE 流式响应"""
    stats["total_requests"] += 1
    stats["active_connections"] += 1

    # 解析请求，获取参数
    try:
        body = await request.json()
        model = body.get("model", "mock-model")
        # 从请求中提取 tokens 和 delay 参数（用于压测控制）
        user_tokens = body.get("mock_tokens", None)
        user_delay_ms = body.get("mock_delay_ms", None)
    except Exception:
        model = "mock-model"
        user_tokens = None
        user_delay_ms = None

    tokens = user_tokens if user_tokens is not None else request.config_dict["tokens"]
    delay_ms = user_delay_ms if user_delay_ms is not None else request.config_dict["delay_ms"]

    # 构造模拟 token 序列（从 messages 中取最后一个 user content 的单词）
    words = ["Hello", " world", "!", " This", " is", " a", " mock",
             " response", " from", " the", " simulated", " LLM",
             " backend", ".", " Each", " token", " streams",
             " progressively", " to", " the", " client", "."]

    # 扩充到足够数量
    while len(words) < tokens:
        words.extend(words)
    words = words[:tokens]

    # 发送 SSE 响应头
    resp = web.StreamResponse(
        status=200,
        reason="OK",
        headers={
            "Content-Type": "text/event-stream",
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )
    await resp.prepare(request)

    try:
        for i, word in enumerate(words):
            chunk = make_sse_chunk(i, word, model)
            await resp.write(chunk.encode("utf-8"))

            if delay_ms > 0:
                await asyncio.sleep(delay_ms / 1000.0)

        # 发送 [DONE]
        await resp.write(b"data: [DONE]\n\n")

    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
        # 客户端断开连接 — 正常情况
        pass
    finally:
        stats["active_connections"] -= 1
        await resp.write_eof()

    return resp


async def handle_health(request: web.Request) -> web.Response:
    """GET /health"""
    uptime = time.time() - stats["start_time"]
    return web.json_response({
        "status": "ok",
        "uptime_sec": round(uptime, 1),
        "total_requests": stats["total_requests"],
        "active_connections": stats["active_connections"],
    })


async def handle_stats(request: web.Request) -> web.Response:
    """GET /stats — 详细统计"""
    uptime = time.time() - stats["start_time"]
    qps = stats["total_requests"] / uptime if uptime > 0 else 0
    return web.json_response({
        "uptime_sec": round(uptime, 1),
        "total_requests": stats["total_requests"],
        "active_connections": stats["active_connections"],
        "avg_qps": round(qps, 1),
    })


def main():
    parser = argparse.ArgumentParser(description="Mock LLM SSE Backend")
    parser.add_argument("--port", type=int, default=8100, help="监听端口")
    parser.add_argument("--tokens", type=int, default=20, help="每个响应生成的 token 数")
    parser.add_argument("--delay-ms", type=int, default=30, help="每个 token 之间的延迟 (ms)")
    args = parser.parse_args()

    application = web.Application()
    application["tokens"] = args.tokens
    application["delay_ms"] = args.delay_ms

    application.router.add_post("/v1/chat/completions", handle_chat)
    application.router.add_get("/health", handle_health)
    application.router.add_get("/stats", handle_stats)

    print(f"Mock LLM Backend starting on 0.0.0.0:{args.port}")
    print(f"   tokens/response: {args.tokens}")
    print(f"   delay/token:     {args.delay_ms}ms")
    print(f"   endpoints:")
    print(f"     POST /v1/chat/completions  — SSE stream")
    print(f"     GET  /health               — health check")
    print(f"     GET  /stats                — statistics")
    sys.stdout.flush()

    web.run_app(application, host="0.0.0.0", port=args.port, print=lambda *_: None)


if __name__ == "__main__":
    main()
