-- orchestrator_dag.lua — Multi-Agent 编排 DAG 引擎
--   优先级最低（最后执行），负责将主 Agent 的 agent_call 请求
--   按预定义 DAG 拆分为子任务，编排多个 sub-agent 并发/串行执行

local M = {}
M.name = "orchestrator_dag"
M.version = "1.0.0"
M.priority = 5   -- 最低优先级，最后执行

-- ═══════════════════════════════════════════════════════════════════
-- DAG 定义: 将复杂任务拆解为 Agent 调用链
-- ═══════════════════════════════════════════════════════════════════
local workflows = {
    -- 研究 + 总结 串行 DAG
    ["research_then_summarize"] = {
        stages = {
            {
                id = "research",
                capability = "web_research",
                input_template = "研究以下主题: {topic}",
                depends_on = {},
                timeout_ms = 30000,
                cache_ttl = 3600
            },
            {
                id = "summarize",
                capability = "summarize",
                input_template = "总结以下研究报告，生成 200 字中文摘要:\n{research_result}",
                depends_on = {"research"},
                timeout_ms = 15000,
                cache_ttl = 600
            }
        },
        default_params = { topic = "" }
    },

    -- 并行翻译 + 校对
    ["parallel_translate"] = {
        stages = {
            {
                id = "translate_cn",
                capability = "translate",
                input_template = "翻译为中文: {text}",
                depends_on = {},
                timeout_ms = 10000
            },
            {
                id = "translate_ja",
                capability = "translate",
                input_template = "翻译为日文: {text}",
                depends_on = {},
                timeout_ms = 10000
            },
            {
                id = "review",
                capability = "review_translation",
                input_template = "校对以下翻译:\n中文: {translate_cn_result}\n日文: {translate_ja_result}",
                depends_on = {"translate_cn", "translate_ja"},
                timeout_ms = 10000
            }
        },
        default_params = { text = "" }
    },

    -- 分析 + 建议 串行
    ["analyze_then_recommend"] = {
        stages = {
            {
                id = "analyze",
                capability = "data_analysis",
                input_template = "分析以下数据: {data}",
                depends_on = {},
                timeout_ms = 45000
            },
            {
                id = "recommend",
                capability = "recommendation",
                input_template = "基于以下分析给出 3 条行动建议:\n{analyze_result}",
                depends_on = {"analyze"},
                timeout_ms = 20000
            }
        },
        default_params = { data = "" }
    }
}

-- ═══════════════════════════════════════════════════════════════════
-- DAG 执行状态机
-- ═══════════════════════════════════════════════════════════════════
local active_dags = {}

local function new_dag_state(workflow_name, params, gateway_session)
    local wf = workflows[workflow_name]
    if not wf then return nil end

    local state = {
        workflow = workflow_name,
        gateway_session = gateway_session,
        params = params or {},
        stage_results = {},
        pending_stages = {},
        completed_stages = {},
        started_at = os.time() * 1000
    }

    -- 初始化所有 stage
    for _, stage in ipairs(wf.stages) do
        state.pending_stages[stage.id] = {
            id = stage.id,
            capability = stage.capability,
            depends_on = stage.depends_on,
            status = "pending"  -- pending | running | done | failed
        }
    end

    return state
end

-- 检查一个 stage 的所有依赖是否已完成
local function deps_satisfied(state, stage_id)
    local wf = workflows[state.workflow]
    for _, stage in ipairs(wf.stages) do
        if stage.id == stage_id then
            for _, dep_id in ipairs(stage.depends_on) do
                if state.completed_stages[dep_id] ~= true then
                    return false
                end
            end
            return true
        end
    end
    return false
end

-- 构建 stage 的输入（替换模板变量）
local function build_stage_input(state, stage_id)
    local wf = workflows[state.workflow]
    for _, stage in ipairs(wf.stages) do
        if stage.id == stage_id then
            local input = stage.input_template
            -- 替换参数
            for k, v in pairs(state.params) do
                input = input:gsub("{" .. k .. "}", tostring(v))
            end
            -- 替换依赖 stage 的结果
            for _, dep_id in ipairs(stage.depends_on) do
                local result_key = dep_id .. "_result"
                local result = state.stage_results[result_key] or ""
                input = input:gsub("{" .. result_key .. "}", result)
            end
            return input
        end
    end
    return ""
end

-- ═══════════════════════════════════════════════════════════════════
-- Plugin Hooks
-- ═══════════════════════════════════════════════════════════════════

function M.init()
    return "CONTINUE"
end

-- on_request: 检查是否需要启动 DAG 工作流
function M.on_request(ctx)
    -- 从 PluginContext.shared 中检查是否有 workfow 标记
    -- 由客户端通过 messages 中的特殊 system message 触发
    local messages = ctx.messages_json
    if not messages then return "CONTINUE" end

    local decoded = require("json").decode(messages)
    if not decoded then return "CONTINUE" end

    -- 查找 system 消息中的 workflow 指令
    local workflow_name = nil
    local workflow_params = nil
    local gateway_session = nil

    for _, msg in ipairs(decoded) do
        if msg.role == "system" then
            -- 格式: "##workflow:research_then_summarize## topic=新能源市场 gateway_session=sess_123"
            local content = msg.content or ""
            local wf_match = content:match("##workflow:(%w+_%w+)##")
            if wf_match then
                workflow_name = wf_match
                -- 解析参数
                workflow_params = {}
                for k, v in content:gmatch("(%w+)=([%w_%-]+)") do
                    workflow_params[k] = v
                end
                gateway_session = workflow_params["gateway_session"] or ""
            end
        end
    end

    if not workflow_name or not workflows[workflow_name] then
        return "CONTINUE"
    end

    -- 初始化 DAG 状态
    local state = new_dag_state(workflow_name, workflow_params, gateway_session)
    if not state then return "CONTINUE" end

    local dag_id = gateway_session .. ":" .. workflow_name .. ":" .. tostring(state.started_at)
    active_dags[dag_id] = state

    -- 分配 gateway_session 给 ctx.shared，供 ws_chat_controller 使用
    ctx.set("workflow_dag_id", dag_id)
    ctx.set("workflow_name", workflow_name)
    ctx.set("has_workflow", true)

    -- 修改消息，告知主 Agent "你的任务已被编排引擎接管"
    -- 注入 DAG 状态到消息中
    local status_msg = {
        role = "system",
        content = "##工作流已启动## 流程: " .. workflow_name ..
                  " 阶段数: " .. tostring(#workflows[workflow_name].stages)
    }
    table.insert(decoded, status_msg)
    ctx.messages_json = require("json").encode(decoded)

    return "MODIFY"
end

-- on_token: 不处理 streaming token
function M.on_token(token)
    return token
end

-- on_done: DAG 完成时清理
function M.on_done()
    -- 不做主动清理，由 TTL 管理
end

-- ═══════════════════════════════════════════════════════════════════
-- 暴露给 Gateway 调用的管理函数（通过 PluginContext.shared 交互）
-- ═══════════════════════════════════════════════════════════════════

-- Gateway 调用: 推进 DAG，返回下一个需要执行的 stage 列表
function M.next_stages(dag_id)
    local state = active_dags[dag_id]
    if not state then return nil end

    local ready = {}
    for stage_id, ps in pairs(state.pending_stages) do
        if ps.status == "pending" and deps_satisfied(state, stage_id) then
            local input = build_stage_input(state, stage_id)
            table.insert(ready, {
                stage_id = stage_id,
                capability = ps.capability,
                input = input,
                timeout_ms = ps.timeout_ms
            })
        end
    end
    return ready
end

-- Gateway 调用: 记录 stage 完成
function M.record_result(dag_id, stage_id, result_text, success)
    local state = active_dags[dag_id]
    if not state then return false end

    if state.pending_stages[stage_id] then
        if success then
            state.pending_stages[stage_id].status = "done"
            state.completed_stages[stage_id] = true
            state.stage_results[stage_id .. "_result"] = result_text or ""
        else
            state.pending_stages[stage_id].status = "failed"
        end
    end

    return true
end

-- Gateway 调用: 检查 DAG 是否所有 stage 都已完成
function M.is_complete(dag_id)
    local state = active_dags[dag_id]
    if not state then return true end

    for _, ps in pairs(state.pending_stages) do
        if ps.status == "pending" or ps.status == "running" then
            return false
        end
    end
    return true
end

-- Gateway 调用: 获取完成的 DAG 的汇总结果
function M.collect_results(dag_id)
    local state = active_dags[dag_id]
    if not state then return nil end

    local results = {}
    for stage_id, _ in pairs(state.completed_stages) do
        local result_key = stage_id .. "_result"
        results[stage_id] = state.stage_results[result_key] or ""
    end

    -- 返回最后一个 stage 的结果作为最终输出
    local wf = workflows[state.workflow]
    local last_stage_id = wf.stages[#wf.stages].id
    local final_result = state.stage_results[last_stage_id .. "_result"] or ""

    return {
        workflow = state.workflow,
        results = results,
        final = final_result,
        duration_ms = (os.time() * 1000) - state.started_at,
        stage_count = #wf.stages
    }
end

return M
