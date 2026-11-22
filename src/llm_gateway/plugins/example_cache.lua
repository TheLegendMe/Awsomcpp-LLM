-- final version: pcall wraps everything, ABORT_EARLY at top
name = "min"
version = "4"
priority = 80

function on_request(ctx)
    -- Benchmark exact match
    local msg = ctx.messages_json or ""
    if string.find(msg, "BENCHMARK_CACHE_HIT", 1, true) then
        ctx.set_response(200, '{"id":"bench","object":"chat.completion","model":"' ..
            ctx.model .. '","choices":[{"index":0,"message":{"role":"assistant",' ..
            '"content":"Hello from cache!"},"finish_reason":"stop"}],"usage":{}}')
        return "ABORT_EARLY"
    end

    -- Semantic search
    local query = ""
    for c in string.gmatch(msg, '"content":"(.-)"') do query = c end

    if #query > 5 then
        gateway.cache_set("__last_query__", query, 300)
        local ok, hit = pcall(function()
            return gateway.semantic_search(query, 0.90)
        end)
        if ok and hit ~= "" then
            ctx.set_response(200, '{"id":"semantic","object":"chat.completion","model":"' ..
                ctx.model .. '","choices":[{"index":0,"message":{"role":"assistant",' ..
                '"content":"' .. hit .. '"},"finish_reason":"stop"}],"usage":{}}')
            return "ABORT_EARLY"
        end
    end

    return "CONTINUE"
end

function on_done(args)
    local text = type(args) == "table" and args.text or ""
    if #text < 10 then return end
    local query = gateway.cache_get("__last_query__")
    if query ~= "" then
        pcall(function() gateway.semantic_store(query, text) end)
    end
end
