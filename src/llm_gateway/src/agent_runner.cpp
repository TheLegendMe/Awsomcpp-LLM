/**
 * agent_runner.cpp — AgentRunner 实现
 */
#include "agent_runner.h"
#include "common.h"
#include "i_upstream.h"
#include "logger.h"

// main.cpp 全局作用域定义（注意：不在 gw 命名空间内）
extern std::shared_ptr<gw::IUpstream> g_upstream;

namespace gw {

const char* AgentRunner::phase_name(Phase p) {
    switch (p) {
        case Phase::Idle:         return "idle";
        case Phase::Acting:       return "acting";
        case Phase::WaitingTools: return "waiting_tools";
        case Phase::Completed:    return "completed";
        case Phase::Failed:       return "failed";
        case Phase::Cancelled:    return "cancelled";
    }
    return "unknown";
}

AgentRunner::AgentRunner(Config cfg, SendFn send, DispatchFn dispatch, LogFn log)
    : cfg_(std::move(cfg))
    , send_(std::move(send))
    , dispatch_(std::move(dispatch))
    , log_(std::move(log))
    , cancel_flag_(std::make_shared<std::atomic<bool>>(false))
{}

AgentRunner::Phase AgentRunner::phase() const {
    std::lock_guard<std::mutex> lk(mu_);
    return phase_;
}

// ─── 启动 ─────────────────────────────────────────────────────────────
void AgentRunner::start(const std::string& messages_json) {
    json msgs;
    try { msgs = json::parse(messages_json); }
    catch (...) { msgs = json::array(); }
    if (!msgs.is_array()) msgs = json::array();

    std::lock_guard<std::mutex> lk(mu_);
    if (phase_ != Phase::Idle) return;

    messages_ = std::move(msgs);
    step_     = 0;
    phase_    = Phase::Acting;

    send_event({{"type","run_started"},
                {"request_id", cfg_.request_id},
                {"model", cfg_.model},
                {"max_steps", cfg_.max_steps},
                {"tools", !cfg_.tools_json.empty()}});
    log("run_started req=" + cfg_.request_id +
        " steps<=" + std::to_string(cfg_.max_steps));

    begin_turn_locked();
}

// ─── 开启一轮 LLM ─────────────────────────────────────────────────────
void AgentRunner::begin_turn_locked() {
    if (cancel_flag_->load()) { finalize_locked("cancelled"); return; }

    step_ += 1;
    turn_guard_ = std::make_shared<TurnGuard>();
    turn_text_.clear();
    tool_order_.clear();
    pending_.clear();
    tool_seq_ = 0;

    send_event({{"type","step"},
                {"request_id", cfg_.request_id},
                {"step", step_},
                {"phase","acting"},
                {"max_steps", cfg_.max_steps}});

    submit_turn_locked();
}

void AgentRunner::submit_turn_locked() {
    if (!g_upstream) { finalize_locked("no_upstream"); return; }

    auto guard = turn_guard_;
    std::shared_ptr<AgentRunner> self = shared_from_this();

    LLMRequest lr;
    lr.model         = cfg_.model;
    lr.messages_json = messages_.dump();
    lr.tools_json    = cfg_.tools_json;
    lr.temperature   = cfg_.temperature;
    lr.max_tokens    = cfg_.max_tokens;
    lr.cancel_flag   = cancel_flag_;

    lr.on_token = [self, guard](const std::string& tok) {
        if (!guard->active.load() || self->cancel_flag_->load()) return;
        self->on_llm_token(tok);
    };
    lr.on_tool_call = [self, guard](const std::string& cid,
                                    const std::string& name,
                                    const std::string& args) {
        if (!guard->active.load() || self->cancel_flag_->load()) return;
        self->on_llm_tool_call(cid, name, args);
    };
    lr.on_done = [self, guard]() {
        // exchange(true→false) 保证重复 done（finish_reason + [DONE]）只处理一次
        if (!guard->active.exchange(false)) return;
        self->on_llm_done();
    };
    lr.on_error = [self, guard](const std::string& e) {
        if (!guard->active.exchange(false)) return;
        self->on_llm_error(e);
    };

    g_upstream->submit(lr, cfg_.upstream_url, cfg_.upstream_key);
}

// ─── 上游回调 ─────────────────────────────────────────────────────────
void AgentRunner::on_llm_token(const std::string& tok) {
    std::lock_guard<std::mutex> lk(mu_);
    if (phase_ != Phase::Acting) return;
    turn_text_ += tok;
    send_event({{"type","token"},
                {"request_id", cfg_.request_id},
                {"step", step_},
                {"content", tok}});
}

void AgentRunner::on_llm_tool_call(const std::string& call_id,
                                   const std::string& name,
                                   const std::string& arguments) {
    std::lock_guard<std::mutex> lk(mu_);
    if (phase_ != Phase::Acting) return;

    std::string cid = call_id.empty()
        ? ("call_" + std::to_string(++tool_seq_)) : call_id;
    if (pending_.count(cid)) return;

    PendingTool pt;
    pt.name = name;
    pt.args = arguments;
    pending_[cid] = std::move(pt);
    tool_order_.push_back(cid);

    json args_json;
    try { args_json = json::parse(arguments.empty() ? "{}" : arguments); }
    catch (...) { args_json = arguments; }

    send_event({{"type","tool_call"},
                {"request_id", cfg_.request_id},
                {"step", step_},
                {"call_id", cid},
                {"name", name},
                {"arguments", args_json}});
    log("tool_call req=" + cfg_.request_id + " step=" + std::to_string(step_) +
        " name=" + name + " call=" + cid);

    if (dispatch_) dispatch_(cfg_.request_id, cid, name, arguments);
}

void AgentRunner::on_llm_done() {
    std::lock_guard<std::mutex> lk(mu_);
    finish_llm_turn_locked();
}

void AgentRunner::on_llm_error(const std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (phase_ != Phase::Acting && phase_ != Phase::WaitingTools) return;

    send_event({{"type","error"},
                {"request_id", cfg_.request_id},
                {"step", step_},
                {"code","upstream_error"},
                {"message", err}});
    finalize_locked("error");
}

// ─── 一轮结束后的决策 ─────────────────────────────────────────────────
void AgentRunner::finish_llm_turn_locked() {
    if (phase_ != Phase::Acting) return;

    if (pending_.empty()) {
        // 无工具调用 → 最终答案
        append_assistant_locked();
        finalize_locked("completed");
        return;
    }

    // 有工具调用 → 记录 assistant(tool_calls)，等待执行
    append_assistant_locked();
    phase_ = Phase::WaitingTools;
    send_event({{"type","step"},
                {"request_id", cfg_.request_id},
                {"step", step_},
                {"phase","observing"},
                {"pending", pending_.size()}});
    maybe_advance_locked();
}

void AgentRunner::append_assistant_locked() {
    json msg;
    msg["role"] = "assistant";
    msg["content"] = turn_text_.empty() ? json(nullptr) : json(turn_text_);

    if (!pending_.empty()) {
        json tcs = json::array();
        for (const auto& cid : tool_order_) {
            auto it = pending_.find(cid);
            if (it == pending_.end()) continue;
            tcs.push_back({
                {"id", cid},
                {"type", "function"},
                {"function", {{"name", it->second.name},
                              {"arguments", it->second.args}}}
            });
        }
        msg["tool_calls"] = std::move(tcs);
    }
    messages_.push_back(std::move(msg));
}

// ─── 工具结果回传 ─────────────────────────────────────────────────────
void AgentRunner::on_tool_result(const std::string& call_id,
                                 const std::string& content,
                                 bool success) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pending_.find(call_id);
    if (it == pending_.end() || it->second.done) return;

    it->second.done    = true;
    it->second.success = success;
    it->second.result  = content;

    send_event({{"type","tool_result"},
                {"request_id", cfg_.request_id},
                {"step", step_},
                {"call_id", call_id},
                {"success", success},
                {"content", content}});

    maybe_advance_locked();
}

void AgentRunner::maybe_advance_locked() {
    if (phase_ != Phase::WaitingTools) return;
    for (const auto& [cid, pt] : pending_) {
        if (!pt.done) return;   // 还有未完成的工具
    }

    // 全部完成 → 追加 role=tool 消息
    for (const auto& cid : tool_order_) {
        auto it = pending_.find(cid);
        if (it == pending_.end()) continue;
        std::string payload = it->second.result;
        if (payload.empty()) payload = it->second.success
            ? std::string("{\"ok\":true}")
            : std::string("{\"error\":\"tool failed\"}");
        messages_.push_back({
            {"role", "tool"},
            {"tool_call_id", cid},
            {"content", payload}
        });
    }

    if (step_ >= cfg_.max_steps) {
        finalize_locked("max_steps");
        return;
    }

    phase_ = Phase::Acting;
    begin_turn_locked();
}

void AgentRunner::finalize_locked(const std::string& reason) {
    if (phase_ == Phase::Completed || phase_ == Phase::Failed
        || phase_ == Phase::Cancelled) {
        return;
    }

    if (reason == "cancelled") {
        phase_ = Phase::Cancelled;
    } else if (reason == "error" || reason == "no_upstream") {
        phase_ = Phase::Failed;
    } else {
        phase_ = Phase::Completed;
    }

    send_event({{"type","run_done"},
                {"request_id", cfg_.request_id},
                {"steps", step_},
                {"reason", reason},
                {"phase", phase_name(phase_)},
                {"content", turn_text_}});
    log("run_done req=" + cfg_.request_id + " reason=" + reason +
        " steps=" + std::to_string(step_));

    if (on_finished_) on_finished_();
}

void AgentRunner::cancel() {
    cancel_flag_->store(true);
    std::lock_guard<std::mutex> lk(mu_);
    if (phase_ == Phase::Completed || phase_ == Phase::Failed
        || phase_ == Phase::Cancelled) {
        return;
    }
    finalize_locked("cancelled");
}

void AgentRunner::send_event(const json& j) {
    if (send_) send_(j);
}

void AgentRunner::log(const std::string& s) {
    if (log_) log_(s);
    else GW_LOG_INFO("[agent] " + s);
}

} // namespace gw
