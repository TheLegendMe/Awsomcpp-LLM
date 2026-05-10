/**
 * chat_controller.cpp — 聊天 SSE 控制器
 *   Drogon v1.9.10 async stream API
 */
#include "chat_controller.h"
#include "plugin_manager.h"
#include "router.h"
#include "auth.h"
#include "metrics.h"
#include "logger.h"
#include "common.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpRequest.h>
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace gw {

// 静态成员
std::shared_ptr<IUpstream> ChatController::upstream_ = nullptr;

ChatController::ChatController() = default;

// ─── 工具：确保当前线程有 VM 池 ────────────────────────────────────
static void ensure_thread_pool() {
    if (!get_thread_pool()) {
        PluginManager::instance().setup_thread_pool();
    }
}

// ─── POST /v1/chat/completions ─────────────────────────────────────
void ChatController::chat(
    drogon::HttpRequestPtr req,
    std::function<void(const drogon::HttpResponsePtr&)> callback)
{
    auto start_time = std::chrono::steady_clock::now();
    ensure_thread_pool();

    std::string client_ip = req->getPeerAddr().toIp();
    auto fwd = req->getHeader("X-Forwarded-For");
    if (!fwd.empty()) client_ip = fwd;

    // 1. 解析请求
    std::string body_str = std::string(req->getBody());
    json body_json;
    try {
        body_json = json::parse(body_str);
    } catch (...) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setBody("{\"error\":\"invalid json\"}");
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        callback(resp);
        return;
    }

    std::string model = body_json.value("model", "local-model");
    auto messages = body_json["messages"];
    if (messages.empty()) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setBody("{\"error\":\"messages required\"}");
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        callback(resp);
        return;
    }

    std::string messages_json = messages.dump();
    double temperature = body_json.value("temperature", 0.7);
    int max_tokens = body_json.value("max_tokens", 2048);

    // 2. 鉴权
    std::string auth_hdr = req->getHeader("Authorization");
    std::string api_key;
    if (auth_hdr.size() > 7 && auth_hdr.substr(0, 7) == "Bearer ") {
        api_key = auth_hdr.substr(7);
    }

    std::string auth_err = gw::AuthManager::instance().verify(api_key, model);
    if (!auth_err.empty()) {
        METRIC_INC(auth_failed);
        LOG_WARN("[" + client_ip + "] Auth failed: " + auth_err);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k401Unauthorized);
        resp->setBody("{\"error\":\"" + auth_err + "\"}");
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        callback(resp);
        return;
    }

    // 3. 路由
    BackendNode* node = gw::Router::instance().route(model);
    if (!node) {
        LOG_WARN("[" + client_ip + "] No backend for model: " + model);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        resp->setBody("{\"error\":\"no available backend\"}");
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        callback(resp);
        return;
    }

    // 4. 插件 Pipeline
    PluginContext plugin_ctx;
    plugin_ctx.model         = model;
    plugin_ctx.messages_json = messages_json;
    plugin_ctx.temperature   = temperature;
    plugin_ctx.max_tokens    = max_tokens;
    plugin_ctx.user_ip       = client_ip;
    plugin_ctx.api_key       = api_key;
    plugin_ctx.request_id    =
        std::chrono::steady_clock::now().time_since_epoch().count();

    auto pipe_result = PluginManager::instance().run_request_pipeline(plugin_ctx);

    model         = pipe_result.ctx.model;
    messages_json = pipe_result.ctx.messages_json;
    temperature   = pipe_result.ctx.temperature;
    max_tokens    = pipe_result.ctx.max_tokens;

    if (!pipe_result.passed) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(
            static_cast<drogon::HttpStatusCode>(pipe_result.abort_status));
        resp->setBody(std::move(pipe_result.abort_body));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        callback(resp);
        LOG_INFO("[" + client_ip + "] ABORT_EARLY: HTTP " +
                 std::to_string(pipe_result.abort_status));
        METRIC_INC(rate_limited);
        return;
    }

    // 5. 创建异步流式响应（Drogon v1.9.10 API）
    // 捕获需要的变量到 shared_ptr 以延长生命周期
    auto node_url = node->url;
    auto node_api_key = node->api_key;

    auto async_resp = drogon::HttpResponse::newAsyncStreamResponse(
        [=](drogon::ResponseStreamPtr stream) mutable
    {
        // 构建请求并提交上游
        LLMRequest llm_req;
        llm_req.model         = model;
        llm_req.messages_json = messages_json;
        llm_req.temperature   = temperature;
        llm_req.max_tokens    = max_tokens;

        llm_req.on_token = [stream, start_time](const std::string& tok) {
            std::string processed =
                PluginManager::instance().run_token_pipeline(tok);
            if (!processed.empty()) {
                json chunk = {
                    {"object", "chat.completion.chunk"},
                    {"model",  "gateway"},
                    {"choices", json::array({{
                        {"index", 0},
                        {"delta", {{"content", processed}}},
                        {"finish_reason", nullptr}
                    }})}
                };
                std::string frame = "data: " + chunk.dump() + "\n\n";
                stream->send(frame);
            }
        };

        llm_req.on_done = [stream, client_ip, model, start_time]() {
            auto end = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start_time);
            LOG_INFO("[" + client_ip + "] SUCCESS model=" + model +
                     " duration=" + std::to_string(dur.count()) + "ms");

            PluginManager::instance().run_done_pipeline();

            stream->send("data: [DONE]\n\n");
            stream->close();
            METRIC_INC(success_count);
            METRIC_DEC(active_requests);
        };

        llm_req.on_error = [stream, client_ip, model, start_time](
                               const std::string& err) {
            auto end = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start_time);
            LOG_WARN("[" + client_ip + "] ERROR model=" + model +
                     " error=" + err +
                     " duration=" + std::to_string(dur.count()) + "ms");

            json e = {{"error", err}};
            stream->send("data: " + e.dump() + "\n\n");
            stream->close();
            METRIC_INC(errors);
            METRIC_DEC(active_requests);
        };

        LOG_INFO("[" + client_ip + "] REQUEST model=" + model +
                 " tokens=" + std::to_string(max_tokens));

        METRIC_INC(total_requests);
        METRIC_INC(active_requests);

        upstream_->submit(llm_req, node_url, node_api_key);
    });

    // 设置 SSE 响应头
    async_resp->setContentTypeCode(drogon::CT_TEXT_EVENT_STREAM);
    async_resp->addHeader("Cache-Control", "no-cache");
    async_resp->addHeader("X-Accel-Buffering", "no");

    callback(async_resp);
}

// ─── GET /metrics ──────────────────────────────────────────────────
void ChatController::metrics(
    drogon::HttpRequestPtr,
    std::function<void(const drogon::HttpResponsePtr&)> callback)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(gw::Metrics::instance().snapshot());
    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    callback(resp);
}

// ─── GET /health ───────────────────────────────────────────────────
void ChatController::health(
    drogon::HttpRequestPtr,
    std::function<void(const drogon::HttpResponsePtr&)> callback)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody("{\"status\":\"ok\"}");
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    callback(resp);
}

// ─── GET /plugins ──────────────────────────────────────────────────
void ChatController::plugins_list(
    drogon::HttpRequestPtr,
    std::function<void(const drogon::HttpResponsePtr&)> callback)
{
    auto plugins = PluginManager::instance().list_plugins();
    json arr = json::array();
    for (auto& p : plugins) {
        arr.push_back({
            {"name", p.name},
            {"version", p.version},
            {"priority", p.priority},
            {"enabled", p.enabled},
            {"filepath", p.filepath}
        });
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(arr.dump(2));
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    callback(resp);
}

// ─── POST /plugins/reload ──────────────────────────────────────────
void ChatController::plugins_reload(
    drogon::HttpRequestPtr req,
    std::function<void(const drogon::HttpResponsePtr&)> callback)
{
    std::string path = std::string(req->getBody());
    bool ok = false;
    if (!path.empty()) {
        ok = PluginManager::instance().reload_plugin(path);
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setBody(ok ? "{\"status\":\"reloaded\"}" : "{\"status\":\"error\"}");
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    callback(resp);
}

} // namespace gw
