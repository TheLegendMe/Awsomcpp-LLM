#!/usr/bin/env python3
"""
test_agent_loop.py — AgentRunner 自主循环端到端测试

链路：
  client(WS) --run--> gateway --chat--> mock LLM (SSE tool_calls)
                     gateway --tool_call--> executor(WS) --tool_result--> gateway
                     gateway 自动进行下一轮，直到模型不再调用工具

验证：
  - 经历 3 个 step（2 次工具调用 + 1 次最终答复）
  - 收到 run_started / step / tool_call / tool_result / run_done
  - run_done.reason == "completed"

依赖： pip install websockets aiohttp  (见 requirements)
运行： python3 tests/test_agent_loop.py
"""
import asyncio
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import aiohttp
from aiohttp import web
from websockets.asyncio.client import connect

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                       # llm_gateway/
GATEWAY_BIN = ROOT / "build" / "llm_gateway"
GW_PORT = 18080
LLM_PORT = 18100
API_KEY = "test-key-123"
WS_URL = f"ws://127.0.0.1:{GW_PORT}/ws/chat?key={API_KEY}"

sys.path.insert(0, str(HERE))
from mock_agent_llm import make_app  # noqa: E402

GREEN, RED, NC = "\033[0;32m", "\033[0;31m", "\033[0m"


def gw_config(path: Path):
    path.write_text(json.dumps({
        "listeners": [{"address": "127.0.0.1", "port": GW_PORT, "https": False}],
        "app": {"threads_num": 2, "document_root": "./static",
                "home_page": "index.html", "max_connections": 1000},
        "plugins_dir": "plugins",
        "lua_pool_size": 2,
        "upstream_type": "remote",
        "backends": [{
            "name": "mock", "url": f"http://127.0.0.1:{LLM_PORT}/v1/chat/completions",
            "api_key": "", "model": "mock-model", "weight": 1, "route_model": "*",
        }],
        "api_keys": [{"key": API_KEY, "models": "*", "qps": 10000, "enabled": True}],
    }, indent=2))


async def wait_health(timeout=15.0):
    deadline = time.time() + timeout
    async with aiohttp.ClientSession() as s:
        while time.time() < deadline:
            try:
                async with s.get(f"http://127.0.0.1:{GW_PORT}/health") as r:
                    if r.status == 200:
                        return True
            except aiohttp.ClientError:
                pass
            await asyncio.sleep(0.3)
    return False


EXPECTED_TOOLS = [{
    "name": "echo",
    "description": "echo text back",
    "parameters": {"type": "object",
                   "properties": {"text": {"type": "string"}},
                   "required": ["text"]},
}]


async def run_executor(stop: asyncio.Event, seen: list):
    async with connect(WS_URL, max_size=None) as ws:
        await ws.recv()  # connected
        await ws.send(json.dumps({
            "type": "register", "role": "executor",
            "agent_id": "test-exec", "tools": EXPECTED_TOOLS,
        }))
        await ws.recv()  # registered
        seen.append("executor_registered")
        while not stop.is_set():
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            msg = json.loads(raw)
            if msg.get("type") == "tool_call":
                args = msg.get("arguments", {})
                seen.append(("exec", msg["call_id"], args.get("text")))
                await ws.send(json.dumps({
                    "type": "tool_result",
                    "request_id": msg.get("request_id", ""),
                    "call_id": msg["call_id"],
                    "success": True,
                    "data": {"echo": args.get("text", "")},
                }))


async def run_client(events: list, timeout=25.0):
    async with connect(WS_URL, max_size=None) as ws:
        await ws.recv()  # connected
        await ws.send(json.dumps({
            "type": "run",
            "request_id": "req-agent-1",
            "model": "local-model",
            "goal": "Please complete the task using the echo tool.",
            "max_steps": 5,
        }))
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=deadline - time.time())
            except asyncio.TimeoutError:
                break
            ev = json.loads(raw)
            events.append(ev)
            if ev.get("type") in ("run_done", "error"):
                break
            if ev.get("type") == "cancelled":
                break


async def main():
    if not GATEWAY_BIN.exists():
        print(f"{RED}gateway binary not found: {GATEWAY_BIN}{NC}")
        return 2

    # 1) mock LLM
    runner = web.AppRunner(make_app())
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", LLM_PORT)
    await site.start()

    # 2) gateway
    cfg = Path(tempfile.gettempdir()) / "agent_loop_gateway.json"
    gw_config(cfg)
    logf = open("/tmp/opencode/agent_loop_gateway.log", "w")
    proc = subprocess.Popen(
        [str(GATEWAY_BIN), str(cfg)],
        cwd=str(ROOT), stdout=logf, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
    )

    ok = False
    try:
        if not await wait_health():
            print(f"{RED}gateway did not become healthy{NC}")
            return 1

        stop = asyncio.Event()
        exec_seen: list = []
        exec_task = asyncio.create_task(run_executor(stop, exec_seen))
        await asyncio.sleep(0.8)  # 等 executor 注册进 ToolRegistry

        events: list = []
        await run_client(events)

        stop.set()
        await asyncio.wait_for(exec_task, timeout=5)

        # ── 校验 ──
        types = [e.get("type") for e in events]
        tool_calls = [e for e in events if e.get("type") == "tool_call"]
        tool_results = [e for e in events if e.get("type") == "tool_result"]
        done = next((e for e in events if e.get("type") == "run_done"), None)
        steps = [e["step"] for e in events if e.get("type") == "step"]

        print("events:", types)
        print("executor saw:", exec_seen)
        print("steps:", steps)
        print("run_done:", json.dumps(done, ensure_ascii=False) if done else None)

        checks = [
            ("run_started", "run_started" in types),
            ("2 tool_calls", len(tool_calls) == 2),
            ("2 tool_results", len(tool_results) == 2),
            ("executor handled 2 calls", len([s for s in exec_seen if isinstance(s, tuple)]) == 2),
            ("completed", bool(done) and done.get("reason") == "completed"),
            ("3 steps", bool(done) and done.get("steps") == 3),
        ]
        allok = all(v for _, v in checks)
        for name, v in checks:
            print(f"  [{'PASS' if v else 'FAIL'}] {name}")
        ok = allok
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        proc.wait(timeout=10)
        logf.close()
        await runner.cleanup()

    if ok:
        print(f"{GREEN}AGENT LOOP TEST PASSED{NC}")
        return 0
    print(f"{RED}AGENT LOOP TEST FAILED{NC}")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
