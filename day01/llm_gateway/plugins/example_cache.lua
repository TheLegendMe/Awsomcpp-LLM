-- example_cache.lua — 语义缓存插件 (priority=80)
-- 使用网关内置 LRU 内存缓存，跨请求跨线程共享
name = "semantic_cache"
version = "1.0.0"
priority = 80

local cache_ttl = 300  -- 缓存 5 分钟

function init()
    gateway.log("INFO", "semantic_cache plugin loaded (LRU, TTL=" .. cache_ttl .. "s)")
end

-- 计算 prompt 指纹
local function fingerprint(model, messages_json)
    local key = model .. ":" .. string.sub(messages_json, 1, 512)
    local h = 0
    for i = 1, #key do
        h = (h * 31 + string.byte(key, i)) % 2147483647
    end
    return "cache:" .. tostring(h)
end

function on_request(ctx)
    local fp = fingerprint(ctx.model, ctx.messages_json)
    local cached = gateway.cache_get(fp)

    if cached ~= "" then
        gateway.log("INFO", "cache HIT for model=" .. ctx.model)

        local resp = '{"object":"chat.completion","model":"' .. ctx.model ..
                     '","choices":[{"index":0,"message":{"role":"assistant","content":' ..
                     string.format("%q", cached) ..
                     '},"finish_reason":"stop"}],"usage":{"completion_tokens":' ..
                     #cached .. '}}'

        ctx:set_response(200, resp)
        return "ABORT_EARLY"
    end

    gateway.log("DEBUG", "cache MISS")
    return "CONTINUE"
end

-- 收集完整响应后缓存
function on_done(ctx)
    -- 注：当前网关未传递完整响应文本，此处保留接口
    -- 实际使用需网关在 LLMRequest 中增加 on_complete(text) 回调
end
