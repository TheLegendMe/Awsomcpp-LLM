-- example_moderation.lua — 内容审查插件 (priority=90)
-- 敏感词检测，命中后 ABORT_EARLY 短路拦截，不请求大模型
name = "content_moderation"
version = "1.0.0"
priority = 90

-- 敏感词列表（实际项目从外部配置/数据库加载）
local sensitive_keywords = {
    "violence", "hack", "exploit", "bypass",
    "malware", "ransomware", "phishing",
    "bomb", "weapon", "attack",
}

function init()
    gateway.log("INFO", "content_moderation plugin loaded with " ..
                #sensitive_keywords .. " keywords")
end

function on_request(ctx)
    gateway.log("DEBUG", "messages_json = " .. ctx.messages_json)

    local ok, result = pcall(function()
        local lower = string.lower(ctx.messages_json)
        gateway.log("DEBUG", "lowered msg = " .. lower)

        for _, kw in ipairs(sensitive_keywords) do
            -- plain=true 禁用 Lua pattern 匹配，纯文本搜索
            local found_pos = string.find(lower, kw, 1, true)
            gateway.log("DEBUG", "  search '" .. kw .. "' -> " .. tostring(found_pos))
            if found_pos then
                ctx.set_response(403,
                    '{"error":"content blocked","reason":"sensitive keyword: ' .. kw .. '"}')
                return "ABORT_EARLY"
            end
        end
        return "CONTINUE"
    end)

    if not ok then
        gateway.log("ERROR", "on_request error: " .. tostring(result))
        return "CONTINUE"
    end
    return result
end
