-- example_auth.lua — 自定义鉴权插件 (priority=100, 最先执行)
name = "custom_auth"
version = "1.0.0"
priority = 100

-- IP 黑名单
local blocked_ips = {
    ["10.0.0.1"] = true,
    ["192.168.1.100"] = true,
}

function init()
    gateway.log("INFO", "custom_auth plugin loaded")
end

function on_request(ctx)
    -- IP 黑名单检查
    if blocked_ips[ctx.user_ip] then
        ctx.set_response(403, '{"error":"ip blocked"}')
        return "ABORT_EARLY"
    end

    -- 允许空 API Key（本地测试）
    if ctx.api_key == "" then
        gateway.log("INFO", "request from " .. ctx.user_ip .. " without api key, allowed")
        return "CONTINUE"
    end

    return "CONTINUE"
end
