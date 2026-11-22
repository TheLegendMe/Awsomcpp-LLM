/**
 * ws_chat_controller.cpp — WebSocket 聊天控制器实现
 *   客户端长连接，线程安全的 upstream → WebSocket 桥接
 *   upstream I/O 线程 → EventLoop::queueInLoop → conn->send()
 *   支持: user / executor / agent 三种角色
 */
#include "ws_chat_controller.h"
#include "plugin_manager.h"
#include "plugin_types.h"
#include "router.h"
#include "auth.h"
#include "metrics.h"
#include "logger.h"
#include "i_upstream.h"
#include "agent_cache.h"
#include <trantor/net/EventLoop.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <openssl/evp.h>

using json = nlohmann::json;

// 外部全局上游引擎（main.cpp 全局作用域定义）
extern std::shared_ptr<gw::IUpstream> g_upstream;

namespace gw {

static void ensure_thread_pool() {
    if (!get_thread_pool())
        PluginManager::instance().setup_thread_pool();
}

// ─── 简单 SHA256 ─────────────────────────────────────────────────────
static std::string sha256(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.c_str(), input.size());
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);
    std::string hex;
    for (unsigned int i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        hex += buf;
    }
    return hex;
}

// ─── 把 executor 回传的结果归一化为给 LLM 的 content 字符串 ─────────
// Controller 回传 {success, data, error, exit_code}，可能没有 "content"，
// 旧实现只取 content 会拿到空串。这里做兼容与归一化。
static std::string build_tool_content(const json& msg) {
    if (msg.contains("content") && msg["content"].is_string()) {
        std::string c = msg["content"].get<std::string>();
        if (!c.empty()) return c;
    }

    json out = json::object();
    if (msg.contains("data")) out["data"] = msg["data"];
    if (msg.contains("error")) {
        const auto& e = msg["error"];
        bool meaningful = e.is_string()
            ? !e.get<std::string>().empty()
            : !e.is_null();
        if (meaningful) out["error"] = e;
    }
    if (msg.contains("exit_code")) out["exit_code"] = msg["exit_code"];

    if (out.empty()) {
        if (msg.contains("data"))
            return msg["data"].is_string()
                ? msg["data"].get<std::string>() : msg["data"].dump();
        return "{}";
    }
    return out.dump();
}

// ─── 线程安全的 WebSocket 发送 ──────────────────────────────────────
void WsChatController::ws_send(const drogon::WebSocketConnectionPtr& conn,
                                const std::string& msg) {
    auto ctx = conn->getContext<SessionCtx>();
    ctx->loop->queueInLoop([conn, msg]() {
        if (conn->connected()) conn->send(msg);
    });
}

void WsChatController::ws_send_to_session(const std::string& session_id,
                                           const std::string& msg) {
    drogon::WebSocketConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) conn = it->second;
    }
    if (conn) ws_send(conn, msg);
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

    auto role_param = req->getParameter("role");
    if (!role_param.empty()) ctx->role = role_param;

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

    // 注册到 session map
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        sessions_[ctx->session_id] = conn;
    }

    // 服务端 WebSocket 协议级心跳：每 30s 发 ping
    conn->setPingMessage("", std::chrono::seconds(30));

    GW_LOG_INFO("[" + ctx->client_ip + "] WS connected session=" + ctx->session_id +
                " role=" + ctx->role);

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

    } else if (msg_type == "run" || msg_type == "agent_run") {
        // ── 目标驱动的自主 Agent 循环 ──
        std::string model = msg.value("model", "local-model");

        json msgs = json::array();
        if (msg.contains("messages") && msg["messages"].is_array())
            msgs = msg["messages"];

        std::string system = msg.value("system", "");
        if (!system.empty()) {
            bool has_sys = !msgs.empty() && msgs[0].value("role", "") == "system";
            if (!has_sys)
                msgs.insert(msgs.begin(), json{{"role","system"},{"content",system}});
        }

        std::string goal = msg.value("goal", "");
        if (!goal.empty())
            msgs.push_back(json{{"role","user"},{"content",goal}});

        if (msgs.empty()) {
            conn->send(json{{"type","error"},{"request_id",request_id},
                            {"code","bad_request"},
                            {"message","messages or goal required"}}.dump());
            return;
        }

        std::string tools_json = msg.contains("tools") ? msg["tools"].dump() : "";
        double temp    = msg.value("temperature", 0.7);
        int max_tok    = msg.value("max_tokens", 2048);
        int max_steps  = msg.value("max_steps", 10);
        if (max_steps < 1)  max_steps = 1;
        if (max_steps > 50) max_steps = 50;

        process_run(conn, request_id, model, msgs.dump(), tools_json,
                    temp, max_tok, max_steps);

    } else if (msg_type == "cancel") {
        auto it = ctx->active_requests.find(request_id);
        if (it != ctx->active_requests.end()) it->second->store(true);

        // 取消进行中的自主循环
        {
            std::shared_ptr<AgentRunner> runner;
            {
                std::lock_guard<std::mutex> lk(runner_mu_);
                auto rit = runners_.find(request_id);
                if (rit != runners_.end()) runner = rit->second.runner;
            }
            if (runner) runner->cancel();
        }
        conn->send(json{{"type","cancelled"},{"request_id",request_id}}.dump());

    } else if (msg_type == "ping") {
        conn->send(json{{"type","pong"}}.dump());

    } else if (msg_type == "register") {
        std::string role = msg.value("role", "");
        if (role == "executor") {
            ctx->role = "executor";
            if (msg.contains("tools") && msg["tools"].is_array()) {
                for (auto& t : msg["tools"]) {
                    std::string name = t.value("name", t.is_string() ? t.get<std::string>() : "");
                    if (!name.empty()) {
                        ctx->tools.push_back(name);
                        std::string desc = t.value("description", "");
                        std::string params = "{}";
                        if (t.contains("parameters") && t["parameters"].is_object()) {
                            auto& p = t["parameters"];
                            if (p.contains("type")) {
                                params = p.dump();
                            } else {
                                json schema = {{"type","object"}};
                                json props = json::object();
                                for (auto& [k, v] : p.items()) {
                                    if (v.is_string())
                                        props[k] = {{"type", v.get<std::string>()}};
                                    else
                                        props[k] = v;
                                }
                                schema["properties"] = props;
                                params = schema.dump();
                            }
                        }
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
            GW_LOG_INFO("[" + ctx->client_ip + "] executor registered session=" +
                        ctx->session_id + " tools=" + std::to_string(ctx->tools.size()));

        } else if (role == "agent") {
            // ── Agent 注册 ──
            ctx->role = "agent";
            ctx->agent_name = msg.value("agent_name", "agent-" + ctx->session_id);

            if (msg.contains("capabilities") && msg["capabilities"].is_array()) {
                for (auto& cap : msg["capabilities"]) {
                    AgentCapability ac;
                    ac.name = cap.value("name", "");
                    ac.description = cap.value("description", "");
                    ac.input_schema_json = cap.value("input_schema", "{}");
                    ac.output_schema_json = cap.value("output_schema", "{}");
                    if (cap.contains("weight")) ac.weight = cap["weight"].get<int>();
                    if (!ac.name.empty()) ctx->capabilities.push_back(ac);
                }
            }

            AgentRecord record;
            record.session_id = ctx->session_id;
            record.agent_name = ctx->agent_name;
            record.capabilities = ctx->capabilities;
            record.peer_addr = ctx->client_ip;
            record.registered_at_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            record.last_heartbeat_ms = record.registered_at_ms;

            AgentRegistry::instance().register_agent(ctx->session_id, record);

            json resp = {{"type","registered"},{"role","agent"},
                         {"agent_name", ctx->agent_name}};
            json caps = json::array();
            for (auto& c : ctx->capabilities) {
                caps.push_back({{"name", c.name}, {"description", c.description}});
            }
            resp["capabilities"] = caps;
            resp["all_caps"] = AgentRegistry::instance().all_capabilities().size();
            conn->send(resp.dump());
            GW_LOG_INFO("[" + ctx->client_ip + "] agent registered: " + ctx->agent_name +
                       " session=" + ctx->session_id +
                       " caps=" + std::to_string(ctx->capabilities.size()));
        }

    } else if (msg_type == "tool_call") {
        std::string call_id = msg.value("call_id", "");
        std::string name    = msg.value("name", "");
        std::string args    = msg.contains("arguments") ? msg["arguments"].dump() : "{}";
        route_tool_call(conn, request_id, call_id, name, args);

    } else if (msg_type == "tool_result") {
        std::string call_id  = msg.value("call_id", "");
        bool        success  = msg.value("success", true);
        std::string content  = build_tool_content(msg);
        GW_LOG_DEBUG("[" + ctx->client_ip + "] tool_result req=" + request_id +
                    " call=" + call_id + " success=" + (success ? "1" : "0"));
        route_tool_result(conn, request_id, call_id, content, success);

    // ─── Agent 间消息 (新增) ──────────────────────────────────────
    } else if (msg_type == "agent_call") {
        std::string call_id = msg.value("call_id", "acall-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::string target_agent = msg.value("target_agent", "");
        std::string capability   = msg.value("capability", "");
        std::string input        = msg.value("input", msg.contains("payload")
                                            ? msg["payload"].dump() : "{}");
        bool bypass_cache = msg.value("bypass_cache", false);

        route_agent_call(conn, call_id, target_agent, capability, input,
                        ctx->session_id, bypass_cache);

    } else if (msg_type == "agent_result") {
        std::string call_id  = msg.value("call_id", "");
        std::string output   = msg.value("output", msg.contains("payload")
                                     ? msg["payload"].dump() : "{}");
        double confidence    = msg.value("confidence", 0.8);

        route_agent_result(conn, call_id, output, confidence);

    } else if (msg_type == "cache_invalidate") {
        std::string key = msg.value("key", "");
        std::string reason = msg.value("reason", "");
        bool cascade = msg.value("cascade", true);

        handle_cache_invalidate(conn, key, reason, cascade);

    } else if (msg_type == "context_expand") {
        std::string call_id = msg.value("call_id", "");
        std::string ref     = msg.value("ref", "");
        size_t max_tok = msg.value("max_tokens", 4096);

        handle_context_expand(conn, call_id, ref, max_tok);
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

    std::string auth_err = AuthManager::instance().verify(ctx->api_key, model);
    if (!auth_err.empty()) {
        METRIC_INC(auth_failed);
        conn->send(json{{"type","error"},{"request_id",request_id},
                        {"code","auth_failed"},{"message",auth_err}}.dump());
        return;
    }

    BackendNode* node = Router::instance().route(model);
    if (!node) {
        conn->send(json{{"type","error"},{"request_id",request_id},
                        {"code","no_backend"},{"message","no available backend"}}.dump());
        return;
    }

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

    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    ctx->active_requests[request_id] = cancel_flag;

    std::string node_url  = node->url;
    std::string node_key  = node->api_key;
    auto        loop      = ctx->loop;

    std::string final_tools = tools_json;
    if (final_tools.empty()) {
        final_tools = ToolRegistry::instance().tools_json();
    }

    LLMRequest llm_req;
    llm_req.model         = node->model.empty() ? final_model : node->model;
    llm_req.messages_json = final_messages;
    llm_req.tools_json    = final_tools;
    llm_req.temperature   = final_temp;
    llm_req.max_tokens    = final_max_tok;
    llm_req.cancel_flag   = cancel_flag;

    auto req_start  = std::chrono::steady_clock::now();
    auto first_tok  = std::make_shared<std::atomic<bool>>(false);

    llm_req.on_tool_call = [conn, request_id, loop, this](const std::string& call_id,
                                                            const std::string& name,
                                                            const std::string& arguments) {
        loop->queueInLoop([conn, request_id, call_id, name, arguments, this]() {
            conn->send(json{{"type","tool_call"},{"request_id",request_id},
                           {"call_id",call_id},{"name",name},
                           {"arguments",json::parse(arguments)}}.dump());
            route_tool_call(conn, request_id, call_id, name, arguments);
        });
    };

    llm_req.on_token = [conn, request_id, cancel_flag, req_start, first_tok](const std::string& tok) {
        ensure_thread_pool();
        if (cancel_flag->load()) return;
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

// ─── 处理自主 Agent run 请求 ─────────────────────────────────────────
void WsChatController::process_run(
    const drogon::WebSocketConnectionPtr& conn,
    const std::string& request_id,
    const std::string& model,
    const std::string& messages_json,
    const std::string& tools_json,
    double temperature,
    int max_tokens,
    int max_steps)
{
    ensure_thread_pool();

    auto ctx = conn->getContext<SessionCtx>();
    if (!ctx) return;

    std::string auth_err = AuthManager::instance().verify(ctx->api_key, model);
    if (!auth_err.empty()) {
        METRIC_INC(auth_failed);
        conn->send(json{{"type","error"},{"request_id",request_id},
                        {"code","auth_failed"},{"message",auth_err}}.dump());
        return;
    }

    BackendNode* node = Router::instance().route(model);
    if (!node) {
        conn->send(json{{"type","error"},{"request_id",request_id},
                        {"code","no_backend"},{"message","no available backend"}}.dump());
        return;
    }

    // 请求前插件管线（限流 / 审核 / 缓存改写等）
    PluginContext pctx;
    pctx.model         = model;
    pctx.messages_json = messages_json;
    pctx.temperature   = temperature;
    pctx.max_tokens    = max_tokens;
    pctx.user_ip       = ctx->client_ip;
    pctx.api_key       = ctx->api_key;
    pctx.request_id    = std::chrono::steady_clock::now().time_since_epoch().count();

    auto pr = PluginManager::instance().run_request_pipeline(pctx);
    if (!pr.passed) {
        conn->send(json{{"type","abort"},{"request_id",request_id},
                        {"status",pr.abort_status},{"body",pr.abort_body}}.dump());
        METRIC_INC(rate_limited);
        return;
    }

    std::string tools = tools_json;
    if (tools.empty()) tools = ToolRegistry::instance().tools_json();

    AgentRunner::Config cfg;
    cfg.request_id   = request_id;
    cfg.model        = node->model.empty() ? model : node->model;
    cfg.upstream_url = node->url;
    cfg.upstream_key = node->api_key;
    cfg.tools_json   = tools;
    cfg.temperature  = pr.ctx.temperature;
    cfg.max_tokens   = pr.ctx.max_tokens;
    cfg.max_steps    = max_steps;

    auto loop = ctx->loop;
    std::string session_id = ctx->session_id;
    std::string client_ip  = ctx->client_ip;

    auto send_fn = [this, conn](const json& j) {
        ws_send(conn, j.dump());
    };
    auto dispatch_fn = [this](const std::string& rid,
                              const std::string& cid,
                              const std::string& name,
                              const std::string& args) {
        std::shared_ptr<AgentRunner> runner;
        {
            std::lock_guard<std::mutex> lk(runner_mu_);
            auto it = runners_.find(rid);
            if (it != runners_.end()) runner = it->second.runner;
        }
        route_tool_call_ex(rid, cid, name, args, nullptr, runner);
    };
    auto log_fn = [](const std::string& s) { GW_LOG_INFO(s); };

    auto runner = std::make_shared<AgentRunner>(cfg, send_fn, dispatch_fn, log_fn);

    // 终态后在 loop 线程清理注册表（延迟执行，确保回调已返回）
    runner->set_on_finished([this, request_id, loop]() {
        loop->queueInLoop([this, request_id]() {
            std::lock_guard<std::mutex> lk(runner_mu_);
            runners_.erase(request_id);
        });
    });

    {
        std::lock_guard<std::mutex> lk(runner_mu_);
        runners_[request_id] = RunnerEntry{runner, session_id};
    }

    METRIC_INC(total_requests);
    METRIC_INC(active_requests);

    GW_LOG_INFO("[" + client_ip + "] WS run request_id=" + request_id +
                " model=" + model + " steps<=" + std::to_string(max_steps));

    runner->start(messages_json);
}

// ─── 路由 tool_call ─────────────────────────────────────────────────
void WsChatController::route_tool_call(
    const drogon::WebSocketConnectionPtr& sender,
    const std::string& request_id,
    const std::string& call_id,
    const std::string& name,
    const std::string& arguments_json)
{
    route_tool_call_ex(request_id, call_id, name, arguments_json, sender, nullptr);
}

void WsChatController::route_tool_call_ex(
    const std::string& request_id,
    const std::string& call_id,
    const std::string& name,
    const std::string& arguments_json,
    const drogon::WebSocketConnectionPtr& reply_conn,
    const std::shared_ptr<AgentRunner>& reply_runner)
{
    drogon::WebSocketConnectionPtr executor;
    {
        std::lock_guard<std::mutex> lk(exec_mu_);
        executors_.erase(
            std::remove_if(executors_.begin(), executors_.end(),
                [](const auto& c) { return !c || c->disconnected(); }),
            executors_.end());

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
        if (!pool.empty()) {
            exec_rr_idx_ = (exec_rr_idx_ + 1) % pool.size();
            executor = pool[exec_rr_idx_];
        }
    }

    if (!executor) {
        GW_LOG_WARN("tool_call no executor name=" + name + " call=" + call_id);
        if (reply_runner) {
            // 让循环"观察到"失败，可自行重试/换策略
            reply_runner->on_tool_result(call_id,
                "{\"error\":\"no executor available for tool " + name + "\"}", false);
        } else if (reply_conn) {
            ws_send(reply_conn, json{{"type","error"},{"request_id",request_id},
                                     {"code","no_executor"},
                                     {"call_id",call_id}}.dump());
        }
        return;
    }

    std::string key = request_id + ":" + call_id;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        ToolRoute r;
        r.conn   = reply_conn;
        r.runner = reply_runner;
        pending_tools_[key] = std::move(r);
    }

    GW_LOG_INFO("tool_call → executor req=" + request_id + " call=" + call_id +
                " name=" + name + (reply_runner ? " [agent]" : " [chat]"));

    ws_send(executor, json{
        {"type","tool_call"},
        {"request_id",request_id},
        {"call_id",call_id},
        {"name",name},
        {"arguments",json::parse(arguments_json)}
    }.dump());
}

void WsChatController::route_tool_result(
    const drogon::WebSocketConnectionPtr& executor,
    const std::string& request_id,
    const std::string& call_id,
    const std::string& content,
    bool success)
{
    std::string key = request_id + ":" + call_id;
    ToolRoute route;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_tools_.find(key);
        if (it == pending_tools_.end()) {
            std::string keys;
            for (auto& [k, v] : pending_tools_) keys += k + " ";
            GW_LOG_WARN("tool_result no match: key='" + key +
                        "' pending=[" + keys + "]");
            executor->send(json{{"type","error"},{"request_id",request_id},
                               {"code","no_pending"}}.dump());
            return;
        }
        route = std::move(it->second);
        pending_tools_.erase(it);
    }

    // 注意：pending_mu_ 已释放，避免与 AgentRunner 的锁形成反序
    if (route.runner) {
        auto ectx = executor->getContext<SessionCtx>();
        GW_LOG_INFO("[" + (ectx ? ectx->client_ip : "?") +
                    "] tool_result → agent loop req=" + request_id +
                    " call=" + call_id);
        route.runner->on_tool_result(call_id, content, success);
        return;
    }

    if (!route.conn || route.conn->disconnected()) {
        executor->send(json{{"type","error"},{"request_id",request_id},
                           {"code","client_gone"}}.dump());
        return;
    }

    auto ctx = executor->getContext<SessionCtx>();
    GW_LOG_INFO("[" + (ctx ? ctx->client_ip : "?") + "] tool_result → client req=" +
                request_id + " call=" + call_id);

    ws_send(route.conn, json{
        {"type","tool_result"},
        {"request_id",request_id},
        {"call_id",call_id},
        {"content",content},
        {"success",success}
    }.dump());
}

// ═══════════════════════════════════════════════════════════════════════
// Agent 间调用 (新增)
// ═══════════════════════════════════════════════════════════════════════

void WsChatController::route_agent_call(
    const drogon::WebSocketConnectionPtr& caller,
    const std::string& call_id,
    const std::string& target_agent_id,
    const std::string& capability,
    const std::string& input_payload,
    const std::string& session_id,
    bool bypass_cache)
{
    auto caller_ctx = caller->getContext<SessionCtx>();

    // 1. 查询缓存 (除非显示跳过)
    if (!bypass_cache) {
        std::string input_hash = sha256(input_payload);
        auto cached = CacheCoordinator::instance().query(
            target_agent_id.empty() ? "any" : target_agent_id,
            capability, input_hash, session_id);

        if (cached.hit) {
            // 命中 → 直接返回
            ws_send(caller, json{
                {"type", "agent_result"},
                {"call_id", call_id},
                {"agent_id", cached.entry.agent_id},
                {"output", cached.entry.output_summary},
                {"confidence", cached.entry.confidence},
                {"cache_status", cached.stale ? "stale" : "fresh"},
                {"cache_layer", static_cast<int>(cached.layer)},
                {"context_ref", cached.entry.full_ref}
            }.dump());

            // 追加到会话缓冲区
            SessionTurn turn;
            turn.call_id = call_id;
            turn.agent_id = cached.entry.agent_id;
            turn.capability = capability;
            turn.input_summary = input_payload.substr(0, 200);
            turn.output_summary = cached.entry.output_summary;
            turn.cache_key = cached.entry.key;
            turn.confidence = cached.entry.confidence;
            turn.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto& sb = CacheCoordinator::instance().session_buffer(session_id);
            sb.append(turn);

            if (cached.stale) {
                GW_LOG_INFO("[" + (caller_ctx ? caller_ctx->client_ip : "?") +
                           "] agent_call cache HIT (stale) call=" + call_id +
                           " cap=" + capability + " confidence=" +
                           std::to_string(cached.entry.confidence));
            }
            return;
        }
    }

    // 2. 路由到目标 Agent
    std::optional<AgentRecord> target;
    if (!target_agent_id.empty()) {
        target = AgentRegistry::instance().find(target_agent_id);
    }
    if (!target) {
        target = AgentRegistry::instance().route(capability);
    }

    if (!target) {
        ws_send(caller, json{
            {"type", "error"},
            {"call_id", call_id},
            {"code", "no_agent"},
            {"message", "no agent available for capability: " + capability}
        }.dump());
        GW_LOG_WARN("[" + (caller_ctx ? caller_ctx->client_ip : "?") +
                   "] agent_call no agent for cap=" + capability);
        return;
    }

    // 3. 记录 pending 映射
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_agents_[call_id] = caller;
    }

    // 4. 转发给 callee
    ws_send_to_session(target->session_id, json{
        {"type", "agent_call"},
        {"call_id", call_id},
        {"caller_session", session_id},
        {"capability", capability},
        {"input", json::parse(input_payload.empty() ? "{}" : input_payload)},
        {"timeout_ms", 30000}
    }.dump());

    GW_LOG_INFO("[" + (caller_ctx ? caller_ctx->client_ip : "?") +
               "] agent_call → " + target->agent_name +
               " call=" + call_id + " cap=" + capability);
}

void WsChatController::route_agent_result(
    const drogon::WebSocketConnectionPtr& callee,
    const std::string& call_id,
    const std::string& output_payload,
    double confidence)
{
    auto callee_ctx = callee->getContext<SessionCtx>();

    // 查找调用方
    drogon::WebSocketConnectionPtr caller;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_agents_.find(call_id);
        if (it == pending_agents_.end()) {
            GW_LOG_WARN("agent_result no pending: call_id=" + call_id);
            ws_send(callee, json{
                {"type", "error"},
                {"call_id", call_id},
                {"code", "no_pending"},
                {"message", "no pending agent call for this call_id"}
            }.dump());
            return;
        }
        caller = it->second;
        pending_agents_.erase(it);
    }

    if (!caller || caller->disconnected()) {
        ws_send(callee, json{
            {"type", "error"},
            {"call_id", call_id},
            {"code", "caller_gone"}
        }.dump());
        return;
    }

    auto caller_ctx = caller->getContext<SessionCtx>();

    // 写缓存
    CacheEntry entry;
    entry.key = "agent:" + (callee_ctx ? callee_ctx->agent_name : "unknown") +
                ":agent_call:" + sha256(output_payload);
    entry.agent_id = callee_ctx ? callee_ctx->session_id : "";
    entry.capability = "agent_call";
    entry.input_hash = call_id;
    entry.output = output_payload;
    entry.output_summary = output_payload.substr(0, 200);
    entry.confidence = confidence;
    entry.confidence_decay = 0.02;
    entry.base_ttl_seconds = 600;
    entry.status = confidence > 0.3 ? CacheStatus::FRESH : CacheStatus::STALE;
    CacheCoordinator::instance().write(entry);

    // 追加到调用方会话缓冲区
    if (caller_ctx) {
        SessionTurn turn;
        turn.call_id = call_id;
        turn.agent_id = callee_ctx ? callee_ctx->agent_name : "";
        turn.capability = "agent_call";
        turn.input_summary = "";
        turn.output_summary = output_payload.substr(0, 200);
        turn.cache_key = entry.key;
        turn.confidence = confidence;
        turn.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto& sb = CacheCoordinator::instance().session_buffer(caller_ctx->session_id);
        sb.append(turn);
    }

    // 回传结果
    ws_send(caller, json{
        {"type", "agent_result"},
        {"call_id", call_id},
        {"agent_id", callee_ctx ? callee_ctx->agent_name : ""},
        {"output", json::parse(output_payload.empty() ? "{}" : output_payload)},
        {"confidence", confidence},
        {"cache_status", "fresh"},
        {"context_ref", entry.full_ref}
    }.dump());

    GW_LOG_INFO("[" + (callee_ctx ? callee_ctx->client_ip : "?") +
               "] agent_result → caller call=" + call_id +
               " confidence=" + std::to_string(confidence));
}

void WsChatController::handle_cache_invalidate(
    const drogon::WebSocketConnectionPtr& sender,
    const std::string& key,
    const std::string& reason,
    bool cascade)
{
    auto ctx = sender->getContext<SessionCtx>();
    GW_LOG_WARN("[" + (ctx ? ctx->client_ip : "?") +
               "] cache_invalidate key=" + key + " reason=" + reason +
               " cascade=" + (cascade ? "true" : "false"));

    CacheCoordinator::instance().invalidate(key, reason, cascade);

    ws_send(sender, json{
        {"type", "cache_invalidated"},
        {"key", key},
        {"cascade", cascade}
    }.dump());
}

void WsChatController::handle_context_expand(
    const drogon::WebSocketConnectionPtr& requester,
    const std::string& call_id,
    const std::string& ref,
    size_t max_tokens)
{
    auto expanded = CacheCoordinator::instance().expand_context(ref, max_tokens);
    if (expanded) {
        ws_send(requester, json{
            {"type", "context_expand_result"},
            {"call_id", call_id},
            {"ref", ref},
            {"full_text", expanded->full_text},
            {"total_tokens", expanded->total_tokens},
            {"truncated", expanded->truncated}
        }.dump());
    } else {
        ws_send(requester, json{
            {"type", "error"},
            {"call_id", call_id},
            {"code", "not_found"},
            {"message", "context ref not found: " + ref}
        }.dump());
    }
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

    // 取消并清理该连接发起的自主循环
    {
        std::vector<std::shared_ptr<AgentRunner>> to_cancel;
        {
            std::lock_guard<std::mutex> lk(runner_mu_);
            for (auto it = runners_.begin(); it != runners_.end(); ) {
                if (it->second.session_id == ctx->session_id) {
                    if (it->second.runner) to_cancel.push_back(it->second.runner);
                    it = runners_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& r : to_cancel) r->cancel();
    }

    // 从 executor 池移除
    if (ctx->role == "executor") {
        ToolRegistry::instance().remove_source(ctx->session_id);
        std::lock_guard<std::mutex> lk(exec_mu_);
        executors_.erase(
            std::remove(executors_.begin(), executors_.end(), conn),
            executors_.end());
    }

    // Agent 注销
    if (ctx->role == "agent") {
        AgentRegistry::instance().unregister_agent(ctx->session_id);
        // 归档会话
        CacheCoordinator::instance().archive_session(ctx->session_id);
    }

    // 从 session map 移除
    {
        std::lock_guard<std::mutex> lk(session_mu_);
        sessions_.erase(ctx->session_id);
    }

    GW_LOG_INFO("[" + ctx->client_ip + "] WS disconnected session=" + ctx->session_id +
                " role=" + ctx->role +
                " pending=" + std::to_string(ctx->active_requests.size()));
}

} // namespace gw
