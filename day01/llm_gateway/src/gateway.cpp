/**
 * gateway.cpp — 网关主服务
 *   libevent HTTP 服务器（事件驱动，异步非阻塞）
 *   处理：
 *     POST /v1/chat/completions  — 流式聊天（SSE 透传）
 *     GET  /metrics              — 监控指标
 *     GET  /health               — 健康检查
 */
#include "auth.h"
#include "router.h"
#include "upstream.h"
#include "metrics.h"
#include "logger.h"

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include <nlohmann/json.hpp>
#include <csignal>
#include <cstring>
#include <atomic>
#include <string>
#include <sstream>

using json = nlohmann::json;

static std::atomic<bool> g_stop{false};
static struct event_base* g_base = nullptr;

// ─── 信号处理 ─────────────────────────────────────────────────────
static void on_signal(evutil_socket_t, short, void*) {
    g_stop.store(true);
    if (g_base) event_base_loopbreak(g_base);
}

// ─── 工具：从请求头取值 ───────────────────────────────────────────
static std::string get_header(struct evhttp_request* req, const char* key) {
    const struct evkeyvalq* hdrs = evhttp_request_get_input_headers(req);
    const char* val = evhttp_find_header(hdrs, key);
    return val ? val : "";
}

// ─── 工具：读取请求 body ──────────────────────────────────────────
static std::string read_body(struct evhttp_request* req) {
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(buf);
    std::string body(len, '\0');
    evbuffer_copyout(buf, &body[0], len);
    return body;
}

// ─── 工具：发送 JSON 错误响应 ─────────────────────────────────────
static void send_error(struct evhttp_request* req, int code, const std::string& msg) {
    struct evbuffer* out = evbuffer_new();
    json err = {{"error", {{"message", msg}, {"code", code}}}};
    std::string body = err.dump();
    evbuffer_add(out, body.c_str(), body.size());
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "application/json");
    evhttp_send_reply(req, code, "Error", out);
    evbuffer_free(out);
}

// ─── SSE 辅助：向客户端写一个 SSE 事件 ───────────────────────────
static void sse_write(struct evhttp_request* req, const std::string& data) {
    struct evbuffer* out = evhttp_request_get_output_buffer(req);
    std::string frame = "data: " + data + "\n\n";
    evbuffer_add(out, frame.c_str(), frame.size());
    evhttp_send_reply_chunk(req, out);
}

// ─── 协议转换：客户端请求 → LLMRequest ───────────────────────────
// 支持 OpenAI 兼容格式，统一转成内部 LLMRequest
static bool parse_request(const std::string& body, LLMRequest& out, std::string& err) {
    try {
        json j = json::parse(body);
        out.model         = j.value("model", "moonshot-v1-8k");
        out.temperature   = j.value("temperature", 0.7);
        out.max_tokens    = j.value("max_tokens", 2048);
        out.messages_json = j.at("messages").dump();
        return true;
    } catch (const std::exception& e) {
        err = std::string("invalid request: ") + e.what();
        return false;
    }
}

// ─── 协议转换：token → OpenAI SSE chunk ──────────────────────────
static std::string make_sse_chunk(const std::string& token, const std::string& model) {
    json chunk = {
        {"object", "chat.completion.chunk"},
        {"model",  model},
        {"choices", json::array({{
            {"index", 0},
            {"delta", {{"content", token}}},
            {"finish_reason", nullptr}
        }})}
    };
    return chunk.dump();
}

// ─── 请求上下文（跨回调共享状态） ────────────────────────────────
struct ReqCtx {
    struct evhttp_request* req;
    std::string            model;
    std::string            client_ip;
    std::chrono::steady_clock::time_point start_time;
    std::atomic<bool>      headers_sent{false};
    std::atomic<int>       token_count{0};
};

// ─── 处理 POST /v1/chat/completions ──────────────────────────────
static void handle_chat(struct evhttp_request* req, void*) {
    auto start_time = std::chrono::steady_clock::now();

    // 获取客户端 IP（从 X-Forwarded-For 或本地）
    std::string client_ip = get_header(req, "X-Forwarded-For");
    if (client_ip.empty()) client_ip = "127.0.0.1";

    // 1. 鉴权
    std::string auth_hdr = get_header(req, "Authorization");
    std::string api_key;
    if (auth_hdr.rfind("Bearer ", 0) == 0)
        api_key = auth_hdr.substr(7);

    std::string body = read_body(req);
    LLMRequest llm_req;
    std::string parse_err;
    if (!parse_request(body, llm_req, parse_err)) {
        LOG_WARN("[" + client_ip + "] Invalid request: " + parse_err);
        send_error(req, 400, parse_err);
        return;
    }

    std::string auth_err = gw::AuthManager::instance().verify(api_key, llm_req.model);
    if (!auth_err.empty()) {
        METRIC_INC(auth_failed);
        LOG_WARN("[" + client_ip + "] Auth failed: " + auth_err + " key=" + api_key);
        send_error(req, 401, auth_err);
        return;
    }

    // 2. 路由选节点
    BackendNode* node = gw::Router::instance().route(llm_req.model);
    if (!node) {
        LOG_WARN("[" + client_ip + "] No available backend for model: " + llm_req.model);
        send_error(req, 503, "no available backend");
        return;
    }

    LOG_INFO("[" + client_ip + "] REQUEST model=" + llm_req.model + " tokens=" + std::to_string(llm_req.max_tokens));

    // 3. 发送 SSE 响应头
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "text/event-stream");
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Cache-Control", "no-cache");
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "X-Accel-Buffering", "no");
    evhttp_send_reply_start(req, 200, "OK");

    // 4. 构建回调（在 I/O 线程调用，通过 libevent 线程安全写回）
    std::string model_name = llm_req.model;
    std::string req_ip = client_ip;
    auto req_start = start_time;

    llm_req.on_token = [req, model_name, req_ip](const std::string& tok) {
        std::string chunk = make_sse_chunk(tok, model_name);
        sse_write(req, chunk);
    };

    llm_req.on_done = [req, model_name, req_ip, req_start]() {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - req_start);
        LOG_INFO("[" + req_ip + "] SUCCESS model=" + model_name + " duration=" + std::to_string(duration.count()) + "ms");

        sse_write(req, "[DONE]");
        evhttp_send_reply_end(req);
        METRIC_INC(success_count);
        METRIC_DEC(active_requests);
    };

    llm_req.on_error = [req, model_name, req_ip, req_start](const std::string& err) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - req_start);
        LOG_WARN("[" + req_ip + "] ERROR model=" + model_name + " error=" + err + " duration=" + std::to_string(duration.count()) + "ms");

        json e = {{"error", err}};
        sse_write(req, e.dump());
        evhttp_send_reply_end(req);
        METRIC_INC(errors);
        METRIC_DEC(active_requests);
    };

    METRIC_INC(total_requests);
    METRIC_INC(active_requests);

    // 5. 提交到上游引擎（非阻塞，立即返回）
    gw::UpstreamEngine::instance().submit(llm_req, *node);
}

// ─── 处理 GET /metrics ────────────────────────────────────────────
static void handle_metrics(struct evhttp_request* req, void*) {
    std::string snap = gw::Metrics::instance().snapshot();
    struct evbuffer* out = evbuffer_new();
    evbuffer_add(out, snap.c_str(), snap.size());
    evbuffer_add(out, "\n", 1);
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "text/plain");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
}

// ─── 处理 GET /health ─────────────────────────────────────────────
static void handle_health(struct evhttp_request* req, void*) {
    const char* body = "{\"status\":\"ok\"}\n";
    struct evbuffer* out = evbuffer_new();
    evbuffer_add(out, body, strlen(body));
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "application/json");
    evhttp_send_reply(req, 200, "OK", out);
    evbuffer_free(out);
}

// ─── 通用请求分发 ─────────────────────────────────────────────────
static void dispatch(struct evhttp_request* req, void*) {
    const char* uri = evhttp_request_get_uri(req);
    evhttp_cmd_type method = evhttp_request_get_command(req);

    if (strcmp(uri, "/v1/chat/completions") == 0 && method == EVHTTP_REQ_POST) {
        handle_chat(req, nullptr);
    } else if (strcmp(uri, "/metrics") == 0 && method == EVHTTP_REQ_GET) {
        handle_metrics(req, nullptr);
    } else if (strcmp(uri, "/health") == 0 && method == EVHTTP_REQ_GET) {
        handle_health(req, nullptr);
    } else {
        send_error(req, 404, "not found");
    }
}

// ─── main ─────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const char* host = "0.0.0.0";
    int         port = 8080;
    if (argc > 1) port = atoi(argv[1]);

    fprintf(stderr, "LLM Gateway starting on %s:%d\n", host, port);
    fflush(stderr);

    // 日志初始化
    Logger::instance().init("logs/gateway.log", LogLevel::INFO);
    LOG_INFO("LLM Gateway starting on " + std::string(host) + ":" + std::to_string(port));

    // 注册后端节点（实际项目从配置文件读取）

    // ─── 本地 llama-cpp 后端（优先）────────────────────
    BackendNode local_node;
    local_node.name    = "llama-local";
    local_node.url     = "http://127.0.0.1:8100/v1/chat/completions";
    local_node.api_key = "";  // llama-cpp 本地不需要认证
    local_node.model   = "local-model";
    local_node.weight  = 1;
    gw::Router::instance().register_node("local-model", local_node);
    gw::Router::instance().register_node("*", local_node);  // 默认走本地

    fprintf(stderr, "✓ Registered llama backend at http://127.0.0.1:8100\n");
    fflush(stderr);
    LOG_INFO("Registered local llama backend at http://127.0.0.1:8100");

    // ─── 远程 Moonshot 后端（备用）──────────────────────
    // 如果需要同时支持远程 Moonshot，取消注释以下代码：
    /*
    const char* kimi_key = getenv("MOONSHOT_API_KEY");
    if (kimi_key) {
        BackendNode remote_node;
        remote_node.name    = "kimi-8k";
        remote_node.url     = "https://api.moonshot.cn/v1/chat/completions";
        remote_node.api_key = kimi_key;
        remote_node.model   = "moonshot-v1-8k";
        remote_node.weight  = 1;
        gw::Router::instance().register_node("moonshot-v1-8k", remote_node);
        LOG_INFO("Registered Moonshot backend");
    }
    */

    // 注册客户端 API Key（实际项目从数据库/配置读取）
    // 本地测试可以不提供 API Key，直接发送请求
    gw::AuthManager::instance().add_key({"test-key-123", "*", 60, true});

    // 启动上游引擎
    gw::UpstreamEngine::instance().start();

    fprintf(stderr, "✓ UpstreamEngine started\n");
    fflush(stderr);

    // libevent 初始化（多线程安全）
    evthread_use_pthreads();
    g_base = event_base_new();

    // 信号处理
    struct event* sig_int  = evsignal_new(g_base, SIGINT,  on_signal, nullptr);
    struct event* sig_term = evsignal_new(g_base, SIGTERM, on_signal, nullptr);
    event_add(sig_int,  nullptr);
    event_add(sig_term, nullptr);

    // HTTP 服务器
    struct evhttp* http = evhttp_new(g_base);
    evhttp_set_gencb(http, dispatch, nullptr);
    evhttp_set_timeout(http, 120);

    struct evhttp_bound_socket* sock =
        evhttp_bind_socket_with_handle(http, host, port);
    if (!sock) {
        fprintf(stderr, "Failed to bind %s:%d\n", host, port);
        return 1;
    }

    fprintf(stderr, "✓ Gateway listening on http://%s:%d\n", host, port);
    fprintf(stderr, "  POST /v1/chat/completions  — stream chat\n");
    fprintf(stderr, "  GET  /metrics              — metrics\n");
    fprintf(stderr, "  GET  /health               — health check\n");
    fprintf(stderr, "Press Ctrl+C to stop.\n\n");
    fflush(stderr);

    LOG_INFO("Gateway listening on " + std::string(host) + ":" + std::to_string(port));

    // 事件循环（阻塞直到收到信号）
    event_base_dispatch(g_base);

    // 清理
    LOG_INFO("Shutting down...");
    gw::UpstreamEngine::instance().stop();
    evhttp_free(http);
    event_free(sig_int);
    event_free(sig_term);
    event_base_free(g_base);
    Logger::instance().shutdown();

    return 0;
}
