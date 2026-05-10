-- example_translate.lua — Prompt 翻译插件 (priority=50)
-- 将中文 prompt 翻译成英文后发给 LLM，实现 MODIFY 语义
name = "prompt_translate"
version = "1.0.0"
priority = 50

function init()
    gateway.log("INFO", "prompt_translate plugin loaded")
end

function on_request(ctx)
    -- 检查 messages 中是否有中文
    if string.find(ctx.messages_json, "[\228-\233]") then
        -- 简化：标记需要翻译（真实场景调用翻译 API 或本地模型）
        -- 这里演示 MODIFY 模式：在 messages 前加上 "Translate to English: "
        local msgs = ctx.messages_json
        -- 在最后一个 user message 的 content 前加翻译指令
        msgs = string.gsub(msgs, '"content":"(.-)"',
            function(content)
                if string.find(content, "[\228-\233]") then
                    return '"content":"[zh->en] ' .. content .. '"'
                end
                return '"content":"' .. content .. '"'
            end)
        ctx.messages_json = msgs

        gateway.log("DEBUG", "prompt_translate: marked Chinese content for translation")
        return "MODIFY"
    end

    return "CONTINUE"
end

function on_token(token)
    -- 如果 token 包含中文，尝试做简单替换
    -- 真实场景用字典或模型翻译
    return token
end
