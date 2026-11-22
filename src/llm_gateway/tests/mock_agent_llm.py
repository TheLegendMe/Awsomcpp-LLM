#!/usr/bin/env python3
"""
mock_agent_llm.py — 面向 AgentRunner 的 mock LLM 上游

行为（由对话里的 assistant.tool_calls 轮数决定）：
  第 1 轮 → 产出 tool_call: echo({"text":"step-1"})
  第 2 轮 → 产出 tool_call: echo({"text":"step-2"})
  第 3 轮 → 产出最终文本 "All done!"

用于验证网关内的自主 Agent 循环 (plan→act→observe→reflect)。
以 SSE 流式返回，符合 OpenAI /v1/chat/completions 格式。
"""
import asyncio
import json
import time
from aiohttp import web

CALL_SEQ = {"n": 0}


def sse(obj) -> bytes:
    return f"data: {json.dumps(obj, ensure_ascii=False)}\n\n".encode()


def chunk(delta: dict, finish=None, model="mock-model") -> bytes:
    return sse({
        "id": "chatcmpl-mock",
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
    })


def tool_rounds(messages) -> int:
    return sum(1 for m in messages
               if m.get("role") == "assistant" and m.get("tool_calls"))


async def handle_chat(request: web.Request) -> web.StreamResponse:
    body = await request.json()
    messages = body.get("messages", [])
    model = body.get("model", "mock-model")

    resp = web.StreamResponse(status=200, headers={
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        "X-Accel-Buffering": "no",
    })
    await resp.prepare(request)

    rounds = tool_rounds(messages)
    try:
        if rounds < 2:
            text = f"step-{rounds + 1}"
            args = json.dumps({"text": text})
            cid = f"call_{rounds + 1}"
            # 首个 tool_call delta：id + name + 空 arguments
            await resp.write(chunk({"tool_calls": [{
                "index": 0, "id": cid, "type": "function",
                "function": {"name": "echo", "arguments": ""}}]}))
            await resp.write(chunk({"tool_calls": [{
                "index": 0,
                "function": {"arguments": args}}]}))
            await resp.write(chunk({}, finish="tool_calls", model=model))
        else:
            for tok in ["All ", "done", "!"]:
                await resp.write(chunk({"content": tok}, model=model))
            await resp.write(chunk({}, finish="stop", model=model))
        await resp.write(b"data: [DONE]\n\n")
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        await resp.write_eof()
    return resp


def make_app() -> web.Application:
    app = web.Application()
    app.router.add_post("/v1/chat/completions", handle_chat)
    app.router.add_get("/health", lambda r: web.json_response({"status": "ok"}))
    return app


if __name__ == "__main__":
    web.run_app(make_app(), host="127.0.0.1", port=18100, print=lambda *_: None)
