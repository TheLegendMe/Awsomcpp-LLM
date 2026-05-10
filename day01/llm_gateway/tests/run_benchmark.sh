#!/bin/bash
# run_benchmark.sh — Llama.cpp QPS 压测 (0.5B vs 7B)
#
# 用法:
#   ./run_benchmark.sh              # 全部场景
#   ./run_benchmark.sh --0.5B       # 只测 0.5B
#   ./run_benchmark.sh --7B         # 只测 7B
#   ./run_benchmark.sh --c10k       # C10K 阶梯
#   ./run_benchmark.sh --compare    # 0.5B vs 7B 对比
#   ./run_benchmark.sh --gateway    # 通过 Drogon 网关
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCHMARK="$SCRIPT_DIR/benchmark.py"
C10K="$SCRIPT_DIR/c10k_test.py"

# 目标模型
MODEL_05B="http://127.0.0.1:8300/v1/chat/completions"    # 0.5B 量化
MODEL_7B="http://127.0.0.1:8200/v1/chat/completions"     # 7B 量化
GATEWAY="http://127.0.0.1:8080/v1/chat/completions"       # Drogon 网关

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'; BOLD='\033[1m'

info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
header(){ echo -e "\n${BOLD}━━━ $* ━━━${NC}\n"; }

# ─── 检查依赖 ──────────────────────────────────────────────────
check_deps() {
    if ! python3 -c "import aiohttp" 2>/dev/null; then
        warn "aiohttp not installed. pip install aiohttp"
        exit 1
    fi
    local lim=$(ulimit -n)
    if [ "$lim" -lt 15000 ]; then
        warn "ulimit -n = $lim (< 15000), C10K may fail. Run: ulimit -n 65535"
    else
        ok "ulimit -n = $lim"
    fi
}

# ─── 连通性检查 ────────────────────────────────────────────────
smoke_test() {
    local url="$1" label="$2"
    header "Smoke: $label"
    local resp
    resp=$(timeout 10 curl -s -N -X POST "$url" \
        -H "Content-Type: application/json" \
        -d '{"model":"test","messages":[{"role":"user","content":"hi"}],"max_tokens":3,"stream":true}' 2>/dev/null)
    if echo "$resp" | grep -q "data:"; then
        ok "$label SSE streaming: OK"
        echo "$resp" | head -3
    else
        fail "$label NOT reachable or not streaming"
    fi
    echo
}

# ─── 压测场景 ──────────────────────────────────────────────────

# 低并发基准（测试延迟下限）
run_low() {
    local url="$1" label="$2"
    header "$label — Low (c=10, n=50)"
    python3 "$BENCHMARK" --url "$url" -c 10 -n 50 --api-key "" --direct
}

# 中等并发
run_medium() {
    local url="$1" label="$2"
    header "$label — Medium (c=100, n=500)"
    python3 "$BENCHMARK" --url "$url" -c 100 -n 500 --api-key "" --direct
}

# 高并发
run_high() {
    local url="$1" label="$2"
    header "$label — High (c=500, n=2000)"
    python3 "$BENCHMARK" --url "$url" -c 500 -n 2000 --api-key "" --direct
}

# C10K 阶梯
run_c10k_step() {
    local url="$1" label="$2"
    header "$label — C10K Ladder (10→100→500→...→10000)"
    python3 "$C10K" --url "$url" --api-key "" --direct
}

# 持续压测 30s
run_sustain() {
    local url="$1" label="$2"
    header "$label — Sustain (c=200, 30s)"
    python3 "$BENCHMARK" --url "$url" -c 200 -d 30 --api-key "" --direct
}

# 冲击压测
run_burst() {
    local url="$1" label="$2"
    header "$label — Burst (c=1000, n=2000)"
    python3 "$BENCHMARK" --url "$url" -c 1000 -n 2000 --api-key "" --direct
}

# ─── 0.5B 全套 ─────────────────────────────────────────────────
suite_05b() {
    smoke_test "$MODEL_05B" "0.5B @ 8300"
    run_low "$MODEL_05B" "0.5B"
    run_medium "$MODEL_05B" "0.5B"
    run_high "$MODEL_05B" "0.5B"
    run_sustain "$MODEL_05B" "0.5B"
    run_burst "$MODEL_05B" "0.5B"
}

# ─── 7B 全套 ───────────────────────────────────────────────────
suite_7b() {
    smoke_test "$MODEL_7B" "7B @ 8200"
    run_low "$MODEL_7B" "7B"
    run_medium "$MODEL_7B" "7B"
    run_high "$MODEL_7B" "7B"
    run_sustain "$MODEL_7B" "7B"
    run_burst "$MODEL_7B" "7B"
}

# ─── 对比（只跑关键级别） ──────────────────────────────────────
suite_compare() {
    smoke_test "$MODEL_05B" "0.5B @ 8300"
    smoke_test "$MODEL_7B" "7B @ 8200"

    run_low "$MODEL_05B" "0.5B"
    run_low "$MODEL_7B" "7B"

    run_medium "$MODEL_05B" "0.5B"
    run_medium "$MODEL_7B" "7B"

    run_high "$MODEL_05B" "0.5B"
    run_high "$MODEL_7B" "7B"

    run_burst "$MODEL_05B" "0.5B"
    run_burst "$MODEL_7B" "7B"
}

# ─── main ──────────────────────────────────────────────────────
MODE="all"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --0.5B)    MODE="05b"; shift ;;
        --7B)      MODE="7b"; shift ;;
        --c10k)    MODE="c10k"; shift ;;
        --compare) MODE="compare"; shift ;;
        --gateway) MODE="gateway"; shift ;;
        --quick)   MODE="quick"; shift ;;
        *) shift ;;
    esac
done

echo -e "${BOLD}"
echo "╔═════════════════════════════════════════════════╗"
echo "║  Llama.cpp QPS Benchmark: 0.5B vs 7B           ║"
echo "╠═════════════════════════════════════════════════╣"
echo "║  0.5B: :8300  |  7B: :8200  |  GW: :8080      ║"
echo "║  Mode: $(printf '%-36s' "$MODE")║"
echo "╚═════════════════════════════════════════════════╝"
echo -e "${NC}"

check_deps

case "$MODE" in
    quick)
        smoke_test "$MODEL_05B" "0.5B @ 8300"
        smoke_test "$MODEL_7B" "7B @ 8200"
        ;;
    05b)  suite_05b ;;
    7b)   suite_7b ;;
    c10k)
        run_c10k_step "$MODEL_05B" "0.5B"
        run_c10k_step "$MODEL_7B" "7B"
        ;;
    compare) suite_compare ;;
    gateway)
        smoke_test "$GATEWAY" "Gateway @ 8080"
        run_medium "$GATEWAY" "Gateway"
        run_high "$GATEWAY" "Gateway"
        ;;
    all)
        smoke_test "$MODEL_05B" "0.5B @ 8300"
        smoke_test "$MODEL_7B" "7B @ 8200"
        suite_compare
        if curl -s --max-time 2 http://127.0.0.1:8080/health >/dev/null 2>&1; then
            info "Gateway detected, adding gateway tests..."
            run_medium "$GATEWAY" "Gateway"
            run_high "$GATEWAY" "Gateway"
        fi
        ;;
esac

header "Done"
echo "Key metrics to compare: QPS, P50/P99 latency, error rate"
echo "Run with --compare for side-by-side 0.5B vs 7B"
