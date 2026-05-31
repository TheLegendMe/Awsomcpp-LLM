-- example_cache.lua — 语义缓存插件 (priority=80)
name = "semantic_cache"
version = "5.0"
priority = 80

function fingerprint(model, messages_json)
    local msg = messages_json or ""
    local key = model .. ":" .. tostring(string.sub(msg, 1, 512))
    local h = 0
    for i = 1, #key do
        h = (h * 31 + string.byte(key, i)) % 2147483647
    end
    return "cache:" .. tostring(h)
end

function on_request(ctx)
    local msg = ctx.messages_json or ""

    -- Benchmark: detect magic message for cache-hit simulation
    if string.find(msg, "BENCHMARK_CACHE_HIT", 1, true) then
        local resp = '{"id":"cache-hit","object":"chat.completion","model":"' .. ctx.model ..
                     '","choices":[{"index":0,"message":{"role":"assistant","content":"Hello from cache!"},' ..
                     '"finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":3,"total_tokens":4}}'
        ctx.set_response(200, resp)
        return "ABORT_EARLY"
    end

    -- Normal cache logic
    local fp = fingerprint(ctx.model, msg)
    local cached = gateway.cache_get(fp)
    if cached ~= "" then
        local resp = '{"id":"cached","object":"chat.completion","model":"' .. ctx.model ..
                     '","choices":[{"index":0,"message":{"role":"assistant","content":' ..
                     string.format("%q", cached) ..
                     '},"finish_reason":"stop"}],"usage":{"completion_tokens":' ..
                     tostring(#cached) .. '}}'
        ctx.set_response(200, resp)
        return "ABORT_EARLY"
    end

    return "CONTINUE"
end

function on_done()
end
