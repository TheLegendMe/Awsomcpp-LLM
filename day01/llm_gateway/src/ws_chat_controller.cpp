/**
 * ws_chat_controller.cpp — WebSocket 聊天控制器实现
 *   客户端长连接，线程安全的 upstream → WebSocket 桥接
 *   upstream I/O 线程 → EventLoop::queueInLoop → conn->send()
 */
#include "ws_chat_controller.h"
#include "plugin_manager.h"
#include "plugin_types.h"
#include "router.h"
#include "auth.h"
#include "metrics.h"
#include "logger.h"
#include "i_upstream.h"
#include <trantor/net/EventLoop.h>
#include <nlohmann/json.hpp>
#include <atomic>

using json = nlohmann::json;

// 外部全局上游引擎（main.cpp 全局作用域定义）
extern std::shared_ptr<gw::IUpstream> g_upstream;

namespace gw {

static void ensure_thread_pool() {
    if (!get_thread_pool())
        PluginManager::instance().setup_thread_pool();
}

// ─── 连接上下文 ──────────────────────────────────────────────────────
struct SessionCtx {
    std::string          session_id;
    std::string          api_key;
    std::string          client_ip;
    std::string          role = "user";   // "user" 或 "executor"
    bool                 authenticated = false;
    trantor::EventLoop*  loop = nullptr;
    std::vector<std::string> tools;       // executor 能执行的工具名
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> active_requests;
};

// ─── 线程安全的 WebSocket 发送 ──────────────────────────────────────
void WsChatController::ws_send(const drogon::WebSocketConnectionPtr& conn,
                                const std::string& msg) {
    auto ctx = conn->getContext<SessionCtx>();
    ctx->loop->queueInLoop([conn, msg]() {
        if (conn->connected()) conn->send(msg);
    });
}

// ─── 连接建立 ───────────────────────────────────────────────────────
void WsChatController::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn)
{
    ensure_thread_pool();

    auto ctx = std::make_shared<SessionCtx>();
    ctx->client_ip = req->getPeerAddr().toIp();
    auto fwd = req->getHeader("X-Forwarded-For");
    if (!fwd.empty()) ctx->client_ip = fwd;
    ctx->loop = trantor::EventLoop::getEventLoopOfCurrentThread();

    // 从 query 参数提取 API Key
    std::string key_param = req->getParameter("key");
    if (!key_param.empty()) {
        ctx->api_key = key_param;
    } else {
        auto ah = req->getHeader("Authorization");
        if (ah.size() > 7 && ah.substr(0, 7) == "Bearer ")
            ctx->api_key = ah.substr(7);
    }

    // 鉴权
    std::string err = AuthManager::instance().verify(ctx->api_key, "*");
    if (!err.empty()) {
        GW_LOG_WARN("[" + ctx->client_ip + "] WS auth failed: " + err);
        conn->send(json{{"type","error"},{"code","auth_failed"},{"message",err}}.dump());
        conn->forceClose();
        return;
    }
    ctx->authenticated = true;

    // 生成 session_id
    static std::atomic<uint64_t> seq{0};
    ctx->session_id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count())
        + "-" + std::to_string(seq.fetch_add(1));

    conn->setContext(ctx);

    // 服务端 WebSocket 协议级心跳：每 30s 发 ping，客户端应自动回 pong
    conn->setPingMessage("", std::chrono::seconds(30));

    GW_LOG_INFO("[" + ctx->client_ip + "] WS connected session=" + ctx->session_id);

    conn->send(json{{"type","connected"},{"session_id",ctx->session_id}}.dump());
}

// ─── 消息处理 ───────────────────────────────────────────────────────
void WsChatController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&& message,
    const drogon::WebSocketMessageType& type)
{
    if (type != drogon::WebSocketMessageType::Text) return;

    auto ctx = conn->getContext<SessionCtx>();
    if (!ctx) return;

    json msg;
    try { msg = json::parse(message); }
    catch (...) {
        conn->send(json{{"type","error"},{"code","invalid_json"}}.dump());
        return;
    }

    std::string msg_type = msg.value("type", "");
    std::string request_id = msg.value("request_id", "");

    if (msg_type == "chat") {
        std::string model = msg.value("model", "local-model");
        if (!msg.contains("messages") || msg["messages"].empty()) {
            conn->send(json{{"type","error"},{"request_id",request_id},
                            {"code","bad_request"},{"message","messages required"}}.dump());
            return;
        }
        std::string messages_json = msg["messages"].dump();
        std::string tools_json    = msg.contains("tools") ? msg["tools"].dump() : "";
        double temp = msg.value("temperature", 0.7);
        int max_tok = msg.value("max_tokens", 2048);

        process_chat(conn, request_id, model, messages_json, tools_json, temp, max_tok);

    } else if (msg_type == "cancel") {
        auto it = ctx->active_requests.find(request_id);
        if (it != ctx->active_requests.end()) {
            it->second->store(true);
            conn->send(json{{"type","cancelled"},{"request_id",request_id}}.dump());
        }
    } else if (msg_type == "ping") {
        conn->send(json{{"type","pong"}}.dump());

    } else if (msg_type == "register") {
        std::string role = msg.value("role", "");
        if (role == "executor") {
            ctx->role = "executor";
            // 注册工具能力
            if (msg.contains("tools") && msg["tools"].is_array()) {
                for (auto& t : msg["tools"]) {
                    std::string name = t.value("name", t.is_string() ? t.get<std::string>() : "");
                    if (!name.empty()) {
                        ctx->tools.push_back(name);
                        std::string desc = t.value("description", "");
                        std::string params = t.contains("parameters") ? t["parameters"].dump() : "{}";
                        ToolRegistry::instance().add(name, desc, params, ctx->session_id);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(exec_mu_);
                executors_.push_back(conn);
            }
            auto resp = json{{"type","registered"},{"role","executor"}};
            resp["tools"] = ToolRegistry::instance().names();
            conn->send(resp.dump());
            GW_LOG_INFO("[" + ctx->client_ip + "] executor registered session=" + ctx->session_id +
                        " tools=" + std::to_string(ctx->tools.size()));
        }

    } else if (msg_type == "tool_call") {
        std::string call_id = msg.value("call_id", "");
        std::string name    = msg.value("name", "");
        std::string args    = msg.contains("arguments") ? msg["arguments"].dump() : "{}";
        route_tool_call(conn, request_id, call_id, name, args);

    } else if (msg_type == "tool_result") {
        std::string call_id  = msg.value("call_id", "");
        std::string content  = msg.value("content", "");
        GW_LOG_DEBUG("[" + ctx->client_ip + "] tool_result req=" + request_id + " call=" + call_id);
        route_tool_result(conn, request_id, call_id, content);
    }
}

// ─── 处理 chat 请求 ─────────────────────────────────────────────────
void WsChatController::process_chat(
    const drogon::WebSocketConnectionPtr& conn,
    const std::string& request_id,
    const std::string& model,
    const std::string& messages_json,
    const std::string& tools_json,
    double temperature,
    int max_tokens)
{
    ensure_thread_pool();

    auto ctx = conn->getContext<SessionCtx>();
    if (!ctx) return;

    // 鉴权
    std::string auth_err = AuthManager::instance().verify(ctx->api_key, model);
    if (!auth_err.empty()) {
        METRIC_INC(auth_failed);
        conn->send(json{{"type","error"},{"request_id",request_id},
                        {"code","auth_failed"},{"message",auth_err}}.dump());
        return;
    }

    // 路由
    BackendNode* node = Router::instance().route(model);
    if (!node) {
        conn->send(json{{"type","error"},{"request_id",request_id},
                        {"code","no_backend"},{"message","no available backend"}}.dump());
        return;
    }

    // 插件管道
    PluginContext pctx;
    pctx.model         = model;
    pctx.messages_json = messages_json;
    pctx.temperature   = temperature;
    pctx.max_tokens    = max_tokens;
    pctx.user_ip       = ctx->client_ip;
    pctx.api_key       = ctx->api_key;
    pctx.request_id    = std::chrono::steady_clock::now().time_since_epoch().count();

    auto pr = PluginManager::instance().run_request_pipeline(pctx);
    std::string final_model     = pr.ctx.model;
    std::string final_messages  = pr.ctx.messages_json;
    double      final_temp      = pr.ctx.temperature;
    int         final_max_tok   = pr.ctx.max_tokens;

    if (!pr.passed) {
        conn->send(json{{"type","abort"},{"request_id",request_id},
                        {"status",pr.abort_status},{"body",pr.abort_body}}.dump());
        METRIC_INC(rate_limited);
        return;
    }

    // 创建取消标记
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    ctx->active_requests[request_id] = cancel_flag;

    std::string node_url  = node->url;
    std::string node_key  = node->api_key;
    auto        loop      = ctx->loop;

    // 如果没有显式传 tools，自动注入全局注册的工具
    std::string final_tools = tools_json;
    if (final_tools.empty()) {
        final_tools = ToolRegistry::instance().tools_json();
    }

    // 构建 LLMRequest，回调中通过 ws_send 安全写回 WebSocket
    LLMRequest llm_req;
    llm_req.model         = final_model;
    llm_req.messages_json = final_messages;
    llm_req.tools_json    = final_tools;
    llm_req.temperature   = final_temp;
    llm_req.max_tokens    = final_max_tok;

    // 延迟统计
    auto req_start  = std::chrono::steady_clock::now();
    auto first_tok  = std::make_shared<std::atomic<bool>>(false);

    llm_req.on_tool_call = [conn, request_id, loop, this](const std::string& call_id,
                                                            const std::string& name,
                                                            const std::string& arguments) {
        // 从上游 I/O 线程回调，dispatch 到 conn 的事件循环
        loop->queueInLoop([conn, request_id, call_id, name, arguments, this]() {
            // 推送给客户端
            conn->send(json{{"type","tool_call"},{"request_id",request_id},
                           {"call_id",call_id},{"name",name},
                           {"arguments",json::parse(arguments)}}.dump());
            // 路由到 executor
            route_tool_call(conn, request_id, call_id, name, arguments);
        });
    };

    llm_req.on_token = [conn, request_id, cancel_flag, req_start, first_tok](const std::string& tok) {
        ensure_thread_pool();
        if (cancel_flag->load()) return;
        // 首 token 延迟
        if (!first_tok->exchange(true)) {
            auto now = std::chrono::steady_clock::now();
            auto us  = std::chrono::duration_cast<std::chrono::microseconds>(now - req_start).count();
            METRIC_ADD(total_first_token_us, us);
            METRIC_INC(first_token_count);
        }
        auto processed = PluginManager::instance().run_token_pipeline(tok);
        if (!processed.empty()) {
            ws_send(conn, json{{"type","token"},{"request_id",request_id},
                               {"content",processed}}.dump());
        }
    };

    llm_req.on_done = [conn, request_id, cancel_flag, loop, req_start]() {
        ensure_thread_pool();
        if (!cancel_flag->load()) {
            PluginManager::instance().run_done_pipeline();
        }
        auto now = std::chrono::steady_clock::now();
        auto us  = std::chrono::duration_cast<std::chrono::microseconds>(now - req_start).count();
        METRIC_ADD(total_latency_us, us);
        METRIC_MAX(max_latency_us, us);
        ws_send(conn, json{{"type","done"},{"request_id",request_id}}.dump());
        METRIC_INC(success_count);
        METRIC_DEC(active_requests);
        // 清理取消标记（必须在连接的事件循环中操作 active_requests）
        loop->queueInLoop([conn, request_id]() {
            auto ctx = conn->getContext<SessionCtx>();
            if (ctx) ctx->active_requests.erase(request_id);
        });
    };

    llm_req.on_error = [conn, request_id, cancel_flag, loop](const std::string& err) {
        ensure_thread_pool();
        if (!cancel_flag->load()) {
            ws_send(conn, json{{"type","error"},{"request_id",request_id},
                               {"code","upstream_error"},{"message",err}}.dump());
        }
        METRIC_INC(errors);
        METRIC_DEC(active_requests);
        loop->queueInLoop([conn, request_id]() {
            auto ctx = conn->getContext<SessionCtx>();
            if (ctx) ctx->active_requests.erase(request_id);
        });
    };

    METRIC_INC(total_requests);
    METRIC_INC(active_requests);

    GW_LOG_DEBUG("[" + ctx->client_ip + "] WS chat request_id=" + request_id +
                 " model=" + final_model);

    g_upstream->submit(llm_req, node_url, node_key);
}

// ─── 路由 tool_call 到 executor ────────────────────────────────────
void WsChatController::route_tool_call(
    const drogon::WebSocketConnectionPtr& sender,
    const std::string& request_id,
    const std::string& call_id,
    const std::string& name,
    const std::string& arguments_json)
{
    drogon::WebSocketConnectionPtr executor;
    {
        std::lock_guard<std::mutex> lk(exec_mu_);
        // 清理已断开的 executor
        executors_.erase(
            std::remove_if(executors_.begin(), executors_.end(),
                [](const auto& c) { return !c || c->disconnected(); }),
            executors_.end());

        // 优先按能力匹配
        std::vector<drogon::WebSocketConnectionPtr> capable;
        for (auto& e : executors_) {
            auto ectx = e->getContext<SessionCtx>();
            if (ectx) {
                for (auto& t : ectx->tools) {
                    if (t == name) { capable.push_back(e); break; }
                }
            }
        }
        auto& pool = capable.empty() ? executors_ : capable;

        if (pool.empty()) {
            ws_send(sender, json{{"type","error"},{"request_id",request_id},
                                {"code","no_executor"},{"message","no executor available"}}.dump());
            return;
        }
        exec_rr_idx_ = (exec_rr_idx_ + 1) % pool.size();
        executor = pool[exec_rr_idx_];
    }

    // 记录 sender → pending，用于 tool_result 回传
    std::string key = request_id + ":" + call_id;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_tools_[key] = sender;
    }

    auto ctx = sender->getContext<SessionCtx>();
    GW_LOG_INFO("[" + (ctx ? ctx->client_ip : "?") + "] tool_call → executor req=" +
                request_id + " call=" + call_id + " name=" + name);

    ws_send(executor, json{
        {"type","tool_call"},
        {"request_id",request_id},
        {"call_id",call_id},
        {"name",name},
        {"arguments",json::parse(arguments_json)}
    }.dump());
}

// ─── 路由 tool_result 回原始客户端 ──────────────────────────────────
void WsChatController::route_tool_result(
    const drogon::WebSocketConnectionPtr& executor,
    const std::string& request_id,
    const std::string& call_id,
    const std::string& content)
{
    std::string key = request_id + ":" + call_id;
    drogon::WebSocketConnectionPtr target;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_tools_.find(key);
        if (it == pending_tools_.end()) {
            // 列出当前所有 pending key 辅助排查
            std::string keys;
            for (auto& [k, v] : pending_tools_) keys += k + " ";
            GW_LOG_WARN("tool_result no match: key='" + key +
                        "' pending=[" + keys + "]");
            executor->send(json{{"type","error"},{"request_id",request_id},
                               {"code","no_pending"},{"message","no pending tool call for this key"}}.dump());
            return;
        }
        target = it->second;
        pending_tools_.erase(it);
    }

    if (!target || target->disconnected()) {
        executor->send(json{{"type","error"},{"request_id",request_id},
                           {"code","client_gone"},{"message","original client disconnected"}}.dump());
        return;
    }

    auto ctx = executor->getContext<SessionCtx>();
    GW_LOG_INFO("[" + (ctx ? ctx->client_ip : "?") + "] tool_result → client req=" +
                request_id + " call=" + call_id);

    target->send(json{
        {"type","tool_result"},
        {"request_id",request_id},
        {"call_id",call_id},
        {"content",content}
    }.dump());
}

// ─── 连接关闭 ───────────────────────────────────────────────────────
void WsChatController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<SessionCtx>();
    if (!ctx) return;

    // 取消所有活跃请求
    for (auto& [rid, flag] : ctx->active_requests) {
        flag->store(true);
    }

    // 从 executor 池移除，清理注册的工具
    if (ctx->role == "executor") {
        ToolRegistry::instance().remove_source(ctx->session_id);
        std::lock_guard<std::mutex> lk(exec_mu_);
        executors_.erase(
            std::remove(executors_.begin(), executors_.end(), conn),
            executors_.end());
    }

    GW_LOG_INFO("[" + ctx->client_ip + "] WS disconnected session=" + ctx->session_id +
                " role=" + ctx->role +
                " pending=" + std::to_string(ctx->active_requests.size()));
}

} // namespace gw
