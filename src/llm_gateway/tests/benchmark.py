#!/usr/bin/env python3
"""
benchmark.py — LLM Gateway 并发压测工具
  支持高并发 SSE 流式请求，测量 QPS、延迟分布、C10K 表现

用法:
  # 基础压测
  python3 benchmark.py --url http://127.0.0.1:8080/v1/chat/completions \\
      --concurrency 100 --requests 1000

  # C10K 测试
  python3 benchmark.py --url http://127.0.0.1:8080/v1/chat/completions \\
      --concurrency 10000 --requests 10000 --c10k

  # 直接压 mock 后端（跳过网关）
  python3 benchmark.py --url http://127.0.0.1:8100/v1/chat/completions \\
      --concurrency 500 --requests 5000 --direct

  # 持续压测（指定时长）
  python3 benchmark.py --url http://127.0.0.1:8080/v1/chat/completions \\
      --concurrency 200 --duration 30
"""
import asyncio
import argparse
import json
import time
import sys
import os
from dataclasses import dataclass, field
from typing import List

try:
    import aiohttp
except ImportError:
    print("需要安装 aiohttp: pip install aiohttp")
    sys.exit(1)


# ─── 数据结构 ──────────────────────────────────────────────────────
@dataclass
class RequestResult:
    """单次请求结果"""
    index: int = 0
    success: bool = False
    status_code: int = 0
    connect_time_ms: float = 0.0      # 建立连接耗时
    first_byte_time_ms: float = 0.0   # 首字节时间 (TTFB)
    total_time_ms: float = 0.0        # 总耗时
    tokens_received: int = 0          # 收到的 token 数
    bytes_received: int = 0           # 收到的字节数
    error_msg: str = ""


@dataclass
class BenchmarkReport:
    """压测报告"""
    target_url: str = ""
    total_requests: int = 0
    concurrency: int = 0
    duration_sec: float = 0.0
    success_count: int = 0
    fail_count: int = 0
    total_tokens: int = 0
    total_bytes: int = 0
    # QPS
    qps_overall: float = 0.0
    # 延迟 (ms)
    latencies: List[float] = field(default_factory=list)
    p50_ms: float = 0.0
    p90_ms: float = 0.0
    p95_ms: float = 0.0
    p99_ms: float = 0.0
    avg_ms: float = 0.0
    min_ms: float = 0.0
    max_ms: float = 0.0
    # 连接时间
    connect_p50_ms: float = 0.0
    connect_p99_ms: float = 0.0
    # TTFB
    ttfb_p50_ms: float = 0.0
    ttfb_p99_ms: float = 0.0
    # 错误详情
    errors: List[str] = field(default_factory=list)


# ─── 默认请求体 ────────────────────────────────────────────────────
DEFAULT_BODY = json.dumps({
    "model": "local-model",
    "messages": [
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": "Say hello in exactly 20 words."},
    ],
    "temperature": 0.7,
    "max_tokens": 50,
    "stream": True,
})


# ─── 百分位计算 ────────────────────────────────────────────────────
def percentile(data: List[float], p: float) -> float:
    if not data:
        return 0.0
    sorted_data = sorted(data)
    k = (len(sorted_data) - 1) * p / 100.0
    f = int(k)
    c = k - f
    if f + 1 < len(sorted_data):
        return sorted_data[f] + c * (sorted_data[f + 1] - sorted_data[f])
    return sorted_data[f]


# ─── 单个请求协程 ──────────────────────────────────────────────────
async def do_request(session: aiohttp.ClientSession,
                     url: str,
                     idx: int,
                     body: str,
                     headers: dict,
                     timeout: aiohttp.ClientTimeout) -> RequestResult:
    result = RequestResult(index=idx)
    t_start = time.monotonic()

    try:
        async with session.post(
            url,
            data=body,
            headers=headers,
            timeout=timeout,
        ) as resp:
            result.status_code = resp.status

            if resp.status != 200:
                result.success = False
                result.error_msg = f"HTTP {resp.status}"
                result.total_time_ms = (time.monotonic() - t_start) * 1000
                return result

            result.success = True

            # 读取 SSE 流
            first_byte = True
            async for line in resp.content:
                if first_byte:
                    result.first_byte_time_ms = (time.monotonic() - t_start) * 1000
                    first_byte = False

                line_str = line.decode("utf-8", errors="ignore").strip()
                if line_str.startswith("data: "):
                    result.tokens_received += 1
                    result.bytes_received += len(line)

    except asyncio.TimeoutError:
        result.success = False
        result.error_msg = "timeout"
    except aiohttp.ClientConnectorError as e:
        result.success = False
        result.error_msg = f"connection refused: {e}"
    except aiohttp.ClientError as e:
        result.success = False
        result.error_msg = f"client error: {type(e).__name__}"
    except Exception as e:
        result.success = False
        result.error_msg = f"error: {type(e).__name__}: {e}"

    result.total_time_ms = (time.monotonic() - t_start) * 1000
    return result


# ─── 主压测函数 ────────────────────────────────────────────────────
async def run_benchmark(url: str,
                        concurrency: int,
                        total_requests: int,
                        duration_sec: float = 0,
                        body: str = DEFAULT_BODY,
                        api_key: str = "",
                        direct: bool = False,
                        verbose: bool = False) -> BenchmarkReport:
    """
    执行并发压测

    Args:
        url: 目标 URL
        concurrency: 并发数
        total_requests: 总请求数（duration_sec > 0 时忽略）
        duration_sec: 持续时间（秒），> 0 时忽略 total_requests
        body: 请求体 JSON
        api_key: API Key
        direct: True=直接压测后端，body 中加 mock_* 参数加速
        verbose: 是否输出每个请求的结果
    """
    report = BenchmarkReport()
    report.target_url = url
    report.concurrency = concurrency
    report.total_requests = total_requests

    # 构造请求头
    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    # 如果直接压后端，插入 mock 参数加速
    req_body = body
    if direct:
        try:
            b = json.loads(body)
            b["mock_tokens"] = 10
            b["mock_delay_ms"] = 5
            req_body = json.dumps(b)
        except Exception:
            pass

    # 超时配置
    timeout = aiohttp.ClientTimeout(
        total=120,        # 总超时
        connect=10,       # 连接超时
        sock_read=60,     # 读取超时
    )

    # 连接器配置（关键：支持高并发）
    connector = aiohttp.TCPConnector(
        limit=concurrency + 100,     # 最大连接数
        limit_per_host=concurrency + 100,
        ttl_dns_cache=300,
        force_close=False,           # 复用连接 (keep-alive)
        enable_cleanup_closed=True,
    )

    results: List[RequestResult] = []

    async with aiohttp.ClientSession(
        connector=connector,
        headers=headers,
    ) as session:

        sem = asyncio.Semaphore(concurrency)
        t_bench_start = time.monotonic()

        async def worker(idx: int):
            async with sem:
                return await do_request(session, url, idx, req_body, headers, timeout)

        if duration_sec > 0:
            # 持续压测模式
            tasks = set()
            idx = 0
            deadline = time.monotonic() + duration_sec

            while time.monotonic() < deadline:
                # 保持 concurrency 个请求在飞行中
                while len(tasks) < concurrency and time.monotonic() < deadline:
                    task = asyncio.create_task(worker(idx))
                    tasks.add(task)
                    idx += 1

                if not tasks:
                    break

                done, tasks = await asyncio.wait(
                    tasks, timeout=0.1, return_when=asyncio.FIRST_COMPLETED
                )
                for t in done:
                    try:
                        r = await t
                        results.append(r)
                        if verbose:
                            status = "✓" if r.success else "✗"
                            print(f"  [{status}] req={r.index:04d} "
                                  f"HTTP={r.status_code} "
                                  f"latency={r.total_time_ms:.0f}ms "
                                  f"tokens={r.tokens_received}")
                    except Exception as e:
                        pass

            # 等待剩余任务
            if tasks:
                done, _ = await asyncio.wait(tasks, timeout=30)
                for t in done:
                    try:
                        results.append(await t)
                    except Exception:
                        pass

            report.total_requests = len(results)
        else:
            # 固定请求数模式
            tasks = [asyncio.create_task(worker(i)) for i in range(total_requests)]
            t_batch_start = time.monotonic()

            # 进度显示
            if verbose and total_requests > 100:
                print(f"  Sending {total_requests} requests with concurrency={concurrency}...")

            results = await asyncio.gather(*tasks, return_exceptions=True)
            results = [r for r in results if isinstance(r, RequestResult)]

        t_bench_end = time.monotonic()
        report.duration_sec = t_bench_end - t_bench_start

    # ─── 汇总统计 ──────────────────────────────────────────────────
    report.total_requests = len(results)

    for r in results:
        if r.success:
            report.success_count += 1
            report.total_tokens += r.tokens_received
            report.total_bytes += r.bytes_received
            report.latencies.append(r.total_time_ms)
        else:
            report.fail_count += 1
            if len(report.errors) < 20:  # 最多保留前 20 个错误
                report.errors.append(f"[req={r.index}] {r.error_msg}")

    if report.duration_sec > 0:
        report.qps_overall = report.total_requests / report.duration_sec

    if report.latencies:
        report.avg_ms = sum(report.latencies) / len(report.latencies)
        report.min_ms = min(report.latencies)
        report.max_ms = max(report.latencies)
        report.p50_ms = percentile(report.latencies, 50)
        report.p90_ms = percentile(report.latencies, 90)
        report.p95_ms = percentile(report.latencies, 95)
        report.p99_ms = percentile(report.latencies, 99)

    return report


# ─── 报告输出 ──────────────────────────────────────────────────────
def print_report(report: BenchmarkReport):
    """格式化输出压测报告"""
    print()
    print("=" * 64)
    print("  LLM Gateway Benchmark Report")
    print("=" * 64)
    print(f"  Target:         {report.target_url}")
    print(f"  Concurrency:    {report.concurrency}")
    print(f"  Total Requests: {report.total_requests}")
    print(f"  Duration:       {report.duration_sec:.2f}s")
    print("-" * 64)
    print(f"  Success:        {report.success_count} ({report.success_count / max(report.total_requests, 1) * 100:.1f}%)")
    print(f"  Failed:         {report.fail_count}")
    print(f"  Total Tokens:   {report.total_tokens}")
    print(f"  Total Bytes:    {report.total_bytes:,}")
    print("-" * 64)
    print(f"  QPS (overall):  {report.qps_overall:.1f} req/s")
    if report.latencies:
        print("-" * 64)
        print(f"  Latency (ms):")
        print(f"    Avg:  {report.avg_ms:8.1f}")
        print(f"    Min:  {report.min_ms:8.1f}")
        print(f"    P50:  {report.p50_ms:8.1f}")
        print(f"    P90:  {report.p90_ms:8.1f}")
        print(f"    P95:  {report.p95_ms:8.1f}")
        print(f"    P99:  {report.p99_ms:8.1f}")
        print(f"    Max:  {report.max_ms:8.1f}")
    print("-" * 64)
    if report.errors:
        print(f"  Errors (first {len(report.errors)}):")
        for e in report.errors[:10]:
            print(f"    - {e}")
    print("=" * 64)
    print()

    # C10K 判定
    if report.concurrency >= 10000:
        if report.fail_count / max(report.total_requests, 1) < 0.01:
            print("✅ C10K: PASS — 并发 10000+ 连接，错误率 < 1%")
        else:
            print(f"⚠️  C10K: FAIL — 错误率 {report.fail_count / max(report.total_requests, 1) * 100:.1f}%")

    # 性能评级
    if report.latencies:
        p99 = report.p99_ms
        if p99 < 100:
            print("🏆 延迟评级: EXCELLENT (P99 < 100ms)")
        elif p99 < 500:
            print("✅ 延迟评级: GOOD (P99 < 500ms)")
        elif p99 < 2000:
            print("⚠️  延迟评级: FAIR (P99 < 2s)")
        else:
            print("❌ 延迟评级: POOR (P99 >= 2s)")

    if report.qps_overall > 10000:
        print("🚀 吞吐评级: HIGH (QPS > 10,000)")
    elif report.qps_overall > 1000:
        print("✅ 吞吐评级: MEDIUM (QPS > 1,000)")
    else:
        print("🐢 吞吐评级: LOW (QPS < 1,000)")

    print()


# ─── main ──────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="LLM Gateway Benchmark Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # 基础压测: 100 并发, 1000 请求
  %(prog)s --url http://127.0.0.1:8080/v1/chat/completions -c 100 -n 1000

  # C10K 测试: 10000 并发
  %(prog)s --url http://127.0.0.1:8080/v1/chat/completions -c 10000 -n 10000

  # 持续压测 30 秒
  %(prog)s --url http://127.0.0.1:8080/v1/chat/completions -c 200 -d 30

  # 直接压 mock 后端（绕过网关，测试后端极限）
  %(prog)s --url http://127.0.0.1:8100/v1/chat/completions -c 1000 -n 5000 --direct
        """,
    )
    parser.add_argument("--url", required=True, help="目标 URL")
    parser.add_argument("-c", "--concurrency", type=int, default=100, help="并发数")
    parser.add_argument("-n", "--requests", type=int, default=1000, help="总请求数")
    parser.add_argument("-d", "--duration", type=float, default=0, help="持续时间(秒), >0 时忽略 -n")
    parser.add_argument("--api-key", default="", help="API Key (empty = bypass rate-limit for bench)")
    parser.add_argument("--body-file", help="请求体 JSON 文件")
    parser.add_argument("--direct", action="store_true", help="直接压后端(跳过网关)")
    parser.add_argument("-v", "--verbose", action="store_true", help="详细输出")
    args = parser.parse_args()

    # 检查 URL
    if not args.url.startswith("http"):
        print("❌ URL 必须以 http:// 或 https:// 开头")
        sys.exit(1)

    # 读取请求体
    body = DEFAULT_BODY
    if args.body_file:
        with open(args.body_file) as f:
            body = f.read()

    # C10K 警告
    if args.concurrency >= 5000:
        print(f"⚠️  高并发测试 (c={args.concurrency})，可能需要调整系统限制:")
        print(f"    ulimit -n {args.concurrency + 1000}")
        print()

    print(f"🔧 Benchmark Config:")
    print(f"    Target:      {args.url}")
    print(f"    Concurrency: {args.concurrency}")
    print(f"    Requests:    {args.requests if args.duration <= 0 else f'{args.duration}s duration'}")
    print(f"    API Key:     {'***' if args.api_key else '(none)'}")
    print(f"    Direct:      {args.direct}")
    print()

    report = asyncio.run(run_benchmark(
        url=args.url,
        concurrency=args.concurrency,
        total_requests=args.requests,
        duration_sec=args.duration,
        body=body,
        api_key=args.api_key,
        direct=args.direct,
        verbose=args.verbose,
    ))

    print_report(report)


if __name__ == "__main__":
    main()
