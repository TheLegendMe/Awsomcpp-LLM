#!/usr/bin/env python3
"""
c10k_test.py — C10K 专项压力测试
  逐步提高并发数，找出网关的连接数极限和性能拐点

用法:
  python3 c10k_test.py --url http://127.0.0.1:8080/v1/chat/completions
  python3 c10k_test.py --url http://127.0.0.1:8100/v1/chat/completions --direct
"""
import asyncio
import argparse
import json
import time
import sys
from dataclasses import dataclass

try:
    import aiohttp
except ImportError:
    print("pip install aiohttp")
    sys.exit(1)


@dataclass
class ConcurrencyResult:
    concurrency: int = 0
    total_requests: int = 0
    success: int = 0
    failed: int = 0
    qps: float = 0.0
    p50_ms: float = 0.0
    p99_ms: float = 0.0
    avg_ms: float = 0.0
    max_ms: float = 0.0
    duration_sec: float = 0.0


def percentile(data, p):
    if not data:
        return 0.0
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = k - f
    if f + 1 < len(s):
        return s[f] + c * (s[f + 1] - s[f])
    return s[f]


async def run_level(url, concurrency, num_requests, body, headers, direct):
    """测试一个并发级别"""
    if direct:
        try:
            b = json.loads(body)
            b["mock_tokens"] = 5
            b["mock_delay_ms"] = 10
            body = json.dumps(b)
        except Exception:
            pass

    timeout = aiohttp.ClientTimeout(total=60, connect=5, sock_read=30)
    connector = aiohttp.TCPConnector(
        limit=concurrency + 100,
        limit_per_host=concurrency + 100,
        force_close=False,
        enable_cleanup_closed=True,
    )

    latencies = []
    success = 0
    failed = 0

    async with aiohttp.ClientSession(connector=connector) as session:
        sem = asyncio.Semaphore(concurrency)

        async def worker(idx):
            nonlocal success, failed
            async with sem:
                t0 = time.monotonic()
                try:
                    async with session.post(url, data=body, headers=headers, timeout=timeout) as resp:
                        if resp.status == 200:
                            async for _ in resp.content:
                                pass
                            success += 1
                            latencies.append((time.monotonic() - t0) * 1000)
                        else:
                            failed += 1
                except Exception:
                    failed += 1

        t_start = time.monotonic()
        tasks = [asyncio.create_task(worker(i)) for i in range(num_requests)]
        await asyncio.gather(*tasks, return_exceptions=True)
        duration = time.monotonic() - t_start

    return ConcurrencyResult(
        concurrency=concurrency,
        total_requests=num_requests,
        success=success,
        failed=failed,
        qps=num_requests / duration if duration > 0 else 0,
        p50_ms=percentile(latencies, 50),
        p99_ms=percentile(latencies, 99),
        avg_ms=sum(latencies) / len(latencies) if latencies else 0,
        max_ms=max(latencies) if latencies else 0,
        duration_sec=duration,
    )


async def main_async(args):
    body = json.dumps({
        "model": "local-model",
        "messages": [{"role": "user", "content": "Hello"}],
        "temperature": 0.7, "max_tokens": 20, "stream": True,
    })
    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
    }
    if args.api_key:
        headers["Authorization"] = f"Bearer {args.api_key}"

    # 测试阶梯: 10 → 50 → 100 → 500 → 1000 → 2000 → 5000 → 10000
    levels = [10, 50, 100, 500, 1000, 2000, 5000]
    if not args.skip_10k:
        levels.append(10000)

    print(f"{'='*80}")
    print(f"  C10K Stress Test — Finding the breaking point")
    print(f"  Target: {args.url}")
    print(f"{'='*80}")
    print(f"{'Conc':>6} {'Success':>8} {'Failed':>7} {'QPS':>8} {'P50(ms)':>9} {'P99(ms)':>9} {'Avg(ms)':>9} {'Max(ms)':>9} {'Dur(s)':>7} {'Status':>10}")
    print(f"{'-'*6} {'-'*8} {'-'*7} {'-'*8} {'-'*9} {'-'*9} {'-'*9} {'-'*9} {'-'*7} {'-'*10}")
    sys.stdout.flush()

    results = []
    for conc in levels:
        # 每个并发级别，请求数 = concurrency * 1 (至少 10)
        num_req = max(conc, 10)

        # 检查当前 ulimit
        import resource
        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        if conc + 100 > soft:
            print(f"\n⚠️  ulimit ({soft}) too low for concurrency={conc}. Skipping higher levels.")
            break

        r = await run_level(args.url, conc, num_req, body, headers, args.direct)
        results.append(r)

        status = "✅" if r.failed == 0 else ("⚠️" if r.failed / max(r.total_requests, 1) < 0.05 else "❌")
        print(f"{r.concurrency:>6} {r.success:>8} {r.failed:>7} {r.qps:>8.0f} {r.p50_ms:>9.1f} {r.p99_ms:>9.1f} {r.avg_ms:>9.1f} {r.max_ms:>9.1f} {r.duration_sec:>7.2f} {status:>10}")
        sys.stdout.flush()

        # 如果失败率太高，提前停止
        if r.failed / max(r.total_requests, 1) > 0.1:
            print(f"\n⚠️  Error rate exceeds 10%, stopping cascade.")
            break

    # 总结
    print(f"\n{'='*80}")
    print(f"  Summary")
    print(f"{'='*80}")

    if results:
        best = max(results, key=lambda r: r.qps)
        last = results[-1]
        print(f"  Max QPS: {best.qps:.0f} req/s @ concurrency={best.concurrency}")
        print(f"  Max Concurrency Tested: {last.concurrency}")
        print(f"  Success Rate @ Max: {last.success}/{last.total_requests} ({last.success/max(last.total_requests, 1)*100:.1f}%)")

        if last.concurrency >= 10000 and last.failed / max(last.total_requests, 1) < 0.01:
            print(f"\n  🎉 C10K: PASS — 10,000 concurrent with <1% errors")
        elif last.concurrency >= 10000:
            print(f"\n  ⚠️  C10K: PARTIAL — 10,000 connections attempted but errors occurred")

    print()


def main():
    parser = argparse.ArgumentParser(description="C10K Stress Test")
    parser.add_argument("--url", required=True)
    parser.add_argument("--api-key", default="test-key-123")
    parser.add_argument("--direct", action="store_true")
    parser.add_argument("--skip-10k", action="store_true", help="Skip the 10K level (faster)")
    args = parser.parse_args()

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
