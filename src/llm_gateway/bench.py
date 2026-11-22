#!/usr/bin/env python3
"""
LLM Gateway 压测脚本
用法: python3 bench.py <mode> [concurrency] [total] [opts]
  mode: baseline | cache_hit | cache_miss | mixed
"""
import http.client, json, time, threading, sys, uuid, os

URL = "127.0.0.1:8080"
ENDPOINT = "/v1/chat/completions"
API_KEY = "test-key-123"
MODEL = "qwen2.5:0.5b-instruct-q4_K_M"

def make_body(content):
    return json.dumps({
        "model": MODEL,
        "messages": [{"role": "user", "content": content}],
        "max_tokens": 10,
        "stream": False
    })

HEADERS = {
    "Content-Type": "application/json",
    "Authorization": f"Bearer {API_KEY}"
}

CACHE_HIT_MSG = "BENCHMARK_CACHE_HIT"

def miss_msg():
    return f"Say hello in one word. uid={uuid.uuid4()}"

class Result:
    def __init__(self):
        self.lock = threading.Lock()
        self.data = []

    def add(self, t0, latency, code, err, body):
        with self.lock:
            self.data.append((t0, latency, code, err, body[:100]))

def do_request(content, res):
    t0 = time.perf_counter()
    code, body, err = 0, "", ""
    try:
        conn = http.client.HTTPConnection(URL, timeout=120)
        conn.request("POST", ENDPOINT, make_body(content), HEADERS)
        resp = conn.getresponse()
        code = resp.status
        body = resp.read().decode()
        conn.close()
    except Exception as e:
        err = str(e)
    t1 = time.perf_counter()
    res.add(time.time(), t1 - t0, code, err, body)

def bench(concurrency, total, content_fn):
    res = Result()
    start = time.perf_counter()
    threads = []
    for i in range(total):
        t = threading.Thread(target=do_request, args=(content_fn(), res))
        t.start()
        threads.append(t)
        while len(threads) >= concurrency:
            done = [t for t in threads if not t.is_alive()]
            for t in done:
                t.join()
            threads = [t for t in threads if t.is_alive()]
            if len(threads) >= concurrency:
                time.sleep(0.01)
    for t in threads:
        t.join()
    end = time.perf_counter()
    return end - start, res.data

def report(elapsed, data, total):
    ok = [r for r in data if r[2] == 200]
    fail = [r for r in data if r[2] != 200 or r[3]]
    latencies = sorted([r[1] for r in ok])
    if not latencies:
        print("  All requests failed!")
        for r in data[:5]:
            print(f"    status={r[2]} err={r[3][:80]} body={r[4]}")
        return

    avg = sum(latencies) / len(latencies)
    p50 = latencies[len(latencies)//2]
    p90 = latencies[int(len(latencies)*0.9)]
    p99 = latencies[min(int(len(latencies)*0.99), len(latencies)-1)]
    p995 = latencies[min(int(len(latencies)*0.995), len(latencies)-1)]

    print(f"  总耗时:         {elapsed:.2f}s")
    print(f"  成功/失败:      {len(ok)}/{len(fail)}")
    print(f"  QPS:            {len(ok)/elapsed:.1f} req/s")
    print(f"  Avg延迟:        {avg*1000:.0f}ms")
    print(f"  P50延迟:        {p50*1000:.0f}ms")
    print(f"  P90延迟:        {p90*1000:.0f}ms")
    print(f"  P99延迟:        {p99*1000:.0f}ms")
    print(f"  P99.5延迟:      {p995*1000:.0f}ms")
    print(f"  Min/Max:        {latencies[0]*1000:.0f}ms / {latencies[-1]*1000:.0f}ms")
    if fail:
        print(f"\n  失败样本 (前3):")
        for r in fail[:3]:
            print(f"    status={r[2]} err={r[3][:60]}")

def divider(title):
    print(f"\n{'═'*60}")
    print(f"  {title}")
    print(f"{'═'*60}")

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "baseline"
    c = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    MODEL = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != "" else "qwen2.5:7b-instruct-q4_K_M"

    print(f"LLM Gateway 压测 — {mode}")
    print(f"目标: {URL} → Ollama ({MODEL})")

    if mode == "baseline":
        divider("第一步：基准测试 — 单并发，看正常延迟")
        elapsed, data = bench(concurrency=1, total=n, content_fn=lambda: "你好，用一句话介绍你自己")
        report(elapsed, data, n)

    elif mode == "cache_hit":
        divider("第二步：缓存命中压测 — 100并发全部命中缓存")
        print("  预热缓存...")
        bench(concurrency=1, total=1, content_fn=lambda: CACHE_HIT_MSG)
        print(f"  开始 {c}并发/{n}请求 压测...")
        elapsed, data = bench(concurrency=c, total=n, content_fn=lambda: CACHE_HIT_MSG)
        report(elapsed, data, n)

    elif mode == "cache_miss":
        divider("第三步：缓存未命中压测 — 100并发全部走推理")
        print(f"  开始 {c}并发/{n}请求 压测（全部走Ollama）...")
        elapsed, data = bench(concurrency=c, total=n, content_fn=miss_msg)
        report(elapsed, data, n)

    elif mode == "mixed":
        ratio = float(sys.argv[4]) if len(sys.argv) > 4 else 0.7
        n_hit = int(n * ratio)
        n_miss = n - n_hit
        divider(f"第四步：混合压测 — {int(ratio*100)}%命中 + {int((1-ratio)*100)}%推理")
        print("  预热缓存...")
        bench(concurrency=1, total=1, content_fn=lambda: CACHE_HIT_MSG)
        print(f"  缓存命中: {n_hit} | 推理: {n_miss} | 并发: {c}")
        print(f"  开始压测...")

        counter = [0]
        def mixed_fn():
            counter[0] += 1
            if counter[0] <= n_hit:
                return CACHE_HIT_MSG
            return miss_msg()

        elapsed, data = bench(concurrency=c, total=n, content_fn=mixed_fn)
        report(elapsed, data, n)

    else:
        print(f"Unknown mode: {mode}")
        print("用法: python3 bench.py [baseline|cache_hit|cache_miss|mixed] [concurrency] [total] [opts]")
