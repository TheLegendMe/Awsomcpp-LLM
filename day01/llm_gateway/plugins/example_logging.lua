-- example_logging.lua — 请求/响应日志插件 (priority=10, 最后执行)
name = "request_logger"
version = "1.0.0"
priority = 10

local request_count = 0

function init()
    gateway.log("INFO", "request_logger plugin loaded")
end

function on_request(ctx)
    request_count = request_count + 1

    gateway.log("INFO", string.format(
        "[req=%d] model=%s temperature=%.1f max_tokens=%d ip=%s",
        request_count, ctx.model, ctx.temperature, ctx.max_tokens, ctx.user_ip))

    gateway.metric_inc("total_requests", 1)

    return "CONTINUE"
end

function on_done(ctx)
    gateway.log("DEBUG", "request completed")
end
