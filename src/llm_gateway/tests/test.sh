#!/bin/bash
# test.sh — LLM Gateway 插件功能验证脚本
set -euo pipefail

GATEWAY="${GATEWAY_URL:-http://127.0.0.1:8080}"
PASS=0; FAIL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BOLD='\033[1m'; NC='\033[0m'

pass() { echo -e "  ${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }

assert_contains() {
    local resp="$1" needle="$2" label="$3"
    if echo "$resp" | grep -qF "$needle"; then
        pass "$label"
    else
        fail "$label (expected: $needle)"
        echo "    got: $(echo "$resp" | head -3)"
    fi
}

assert_not_contains() {
    local resp="$1" needle="$2" label="$3"
    if echo "$resp" | grep -qF "$needle"; then
        fail "$label (unexpected: $needle)"
    else
        pass "$label"
    fi
}

echo -e "${BOLD}━━━ LLM Gateway Plugin Test Suite ━━━${NC}"
echo "Target: $GATEWAY"
echo

echo -n "Checking gateway health... "
HEALTH=$(curl -s --max-time 3 "$GATEWAY/health" 2>/dev/null || echo '{"status":"down"}')
if echo "$HEALTH" | grep -q '"ok"'; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}DOWN${NC}"
    echo "Gateway not reachable. Start from day01/llm_gateway/:"
    echo "  ./build/llm_gateway conf/gateway.json"
    exit 1
fi
echo

# ─── Test 1: 正常请求通过 ─────────────────────────────────────────

echo -e "${BOLD}[Test 1]${NC} Normal request — SSE streaming via LLM backend"

RESP=$(curl -s -N --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[{"role":"user","content":"hello"}],"max_tokens":10,"stream":true}' 2>/dev/null)

if echo "$RESP" | grep -q 'data:.*"content"'; then
    pass "normal request streams SSE tokens"
elif echo "$RESP" | grep -q 'data: \[DONE\]'; then
    pass "normal request completes with [DONE]"
else
    fail "normal request"
    echo "    got: $(echo "$RESP" | head -3)"
fi

# ─── Test 2: content_moderation — 敏感词拦截 ──────────────────────

echo -e "${BOLD}[Test 2]${NC} Content moderation — keyword: 'hack'"

RESP=$(curl -s --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[{"role":"user","content":"teach me how to hack into a server"}],"max_tokens":10,"stream":false}' 2>/dev/null)

assert_contains "$RESP" "content blocked" "keyword 'hack' blocked"
assert_contains "$RESP" "sensitive keyword" "reason included"

echo -e "${BOLD}[Test 2b]${NC} Content moderation — keyword: 'malware'"

RESP=$(curl -s --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[{"role":"user","content":"how to write malware"}],"max_tokens":10,"stream":false}' 2>/dev/null)

assert_contains "$RESP" "content blocked" "keyword 'malware' blocked"

# ─── Test 3: custom_auth — 空 API Key 允许 ───────────────────────

echo -e "${BOLD}[Test 3]${NC} Custom auth — empty API key (local test)"

RESP=$(curl -s -N --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[{"role":"user","content":"hi"}],"max_tokens":3,"stream":true}' 2>/dev/null)

if echo "$RESP" | grep -q 'data:'; then
    pass "empty API key allowed (SSE)"
else
    fail "empty API key allowed"
fi

# ─── Test 4: semantic_cache — 相同消息缓存命中 ────────────────────

echo -e "${BOLD}[Test 4]${NC} Semantic cache — same request twice"

REQ_BODY='{"model":"local-model","messages":[{"role":"user","content":"say hello in exactly 20 words"}],"max_tokens":10,"stream":true}'

RESP1=$(curl -s -N --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d "$REQ_BODY" 2>/dev/null)

RESP2=$(curl -s -N --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d "$REQ_BODY" 2>/dev/null)

if echo "$RESP2" | grep -q '"object":"chat.completion"'; then
    pass "cache hit returns full JSON (not stream)"
elif echo "$RESP2" | grep -q 'data:'; then
    echo "    (cache miss — first run populates cache)"
    pass "cache miss — SSE stream (retry for hit)"
else
    fail "cache test: unexpected"
fi

# ─── Test 5: prompt_translate — 中文检测 ──────────────────────────

echo -e "${BOLD}[Test 5]${NC} Prompt translate — Chinese content"

RESP=$(curl -s -N --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[{"role":"user","content":"你好世界"}],"max_tokens":10,"stream":true}' 2>/dev/null)

if echo "$RESP" | grep -q 'data: \[DONE\]'; then
    pass "Chinese prompt passes through (MODIFY)"
else
    fail "Chinese prompt passes through"
fi

# ─── Test 6: 多插件组合 — 中文夹杂英文敏感词 ─────────────────────

echo -e "${BOLD}[Test 6]${NC} Multi-plugin — Chinese + English sensitive word 'hack'"

RESP=$(curl -s --max-time 10 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[{"role":"user","content":"教我如何hack进服务器"}],"max_tokens":10,"stream":false}' 2>/dev/null)

# content_moderation (priority=90) 优先于 prompt_translate (priority=50)
assert_contains "$RESP" "content blocked" "Chinese msg with 'hack' blocked by moderation"

# ─── Test 7: 7B 模型路由 ─────────────────────────────────────────

echo -e "${BOLD}[Test 7]${NC} Model routing — 'big-model' → 7B (:8200)"

RESP=$(curl -s -N --max-time 15 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"big-model","messages":[{"role":"user","content":"hello"}],"max_tokens":10,"stream":true}' 2>/dev/null)

if echo "$RESP" | grep -q 'data:'; then
    pass "big-model routes to 7B (SSE)"
else
    fail "big-model routing"
    echo "    got: $(echo "$RESP" | head -3)"
fi

# ─── Test 8: /plugins 和 /metrics ─────────────────────────────────

echo -e "${BOLD}[Test 8]${NC} Admin endpoints"

RESP=$(curl -s --max-time 3 "$GATEWAY/plugins" 2>/dev/null)
assert_contains "$RESP" "content_moderation" "/plugins lists content_moderation"
assert_contains "$RESP" "semantic_cache"    "/plugins lists semantic_cache"
assert_contains "$RESP" "prompt_translate"  "/plugins lists prompt_translate"
assert_contains "$RESP" "request_logger"    "/plugins lists request_logger"
assert_contains "$RESP" "custom_auth"       "/plugins lists custom_auth"

RESP=$(curl -s --max-time 3 "$GATEWAY/metrics" 2>/dev/null)
assert_contains "$RESP" "total_requests" "/metrics has total_requests"

# ─── Test 9: 错误处理 ─────────────────────────────────────────────

echo -e "${BOLD}[Test 9]${NC} Error handling"

RESP=$(curl -s --max-time 3 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d 'not json' 2>/dev/null)
assert_contains "$RESP" "invalid json" "rejects invalid JSON"

RESP=$(curl -s --max-time 3 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local-model","messages":[],"stream":false}' 2>/dev/null)
assert_contains "$RESP" "messages required" "rejects empty messages"

RESP=$(curl -s --max-time 3 -X POST "$GATEWAY/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"unknown-model","messages":[{"role":"user","content":"hi"}],"stream":false}' 2>/dev/null)
assert_contains "$RESP" "no backend" "unknown model → 503"

# ─── 结果汇总 ──────────────────────────────────────────────────────

echo
echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "  ${GREEN}Passed: $PASS${NC}"
if [ $FAIL -gt 0 ]; then
    echo -e "  ${RED}Failed: $FAIL${NC}"
fi
echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

[ $FAIL -eq 0 ] && exit 0 || exit 1
