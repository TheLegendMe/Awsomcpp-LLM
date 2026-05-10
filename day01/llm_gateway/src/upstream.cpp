/**
 * upstream.cpp — 上游 HTTP 引擎实现
 *   curl_multi + I/O 线程 + SSE 解析
 */
#include "upstream.h"
#include "logger.h"
#include "metrics.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace gw {

// ─── SSEParser ────────────────────────────────────────────────────
void SSEParser::feed(const char* data, size_t len) {
    buf_.append(data, len);
    flush_lines();
}

void SSEParser::flush_lines() {
    size_t start = 0;
    while (true) {
        size_t nl = buf_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = buf_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = nl + 1;
        process_line(line);
    }
    buf_.erase(0, start);
}

void SSEParser::process_line(const std::string& line) {
    if (line.empty() || line[0] == ':') return;
    static const std::string kPrefix = "data: ";
    if (line.size() < kPrefix.size() ||
        line.compare(0, kPrefix.size(), kPrefix) != 0) return;

    std::string payload = line.substr(kPrefix.size());
    if (payload == "[DONE]") { done_cb_(); return; }

    try {
        json obj = json::parse(payload);
        if (obj.contains("choices") && !obj["choices"].empty()) {
            const auto& delta = obj["choices"][0]["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
                std::string tok = delta["content"].get<std::string>();
                if (!tok.empty()) {
                    METRIC_INC(total_tokens);
                    tok_cb_(tok);
                }
            }
        }
    } catch (...) {}
}

// ─── UpstreamEngine ───────────────────────────────────────────────
UpstreamEngine& UpstreamEngine::instance() {
    static UpstreamEngine inst;
    return inst;
}

void UpstreamEngine::start() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi_ = curl_multi_init();
    running_.store(true);
    io_thread_ = std::thread(&UpstreamEngine::io_loop, this);
    LOG_INFO("UpstreamEngine started");
}

void UpstreamEngine::stop() {
    running_.store(false);
    if (multi_) curl_multi_wakeup(multi_);
    if (io_thread_.joinable()) io_thread_.join();
    for (auto& [easy, ctx] : active_) {
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
    }
    active_.clear();
    curl_multi_cleanup(multi_);
    curl_global_cleanup();
    LOG_INFO("UpstreamEngine stopped");
}

void UpstreamEngine::submit(const LLMRequest& req, const BackendNode& node) {
    NodeSnapshot snap{node.name, node.url, node.api_key, node.model};
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push({req, std::move(snap)});
    }
    if (multi_) curl_multi_wakeup(multi_);
}

size_t UpstreamEngine::write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    size_t bytes = size * nmemb;
    reinterpret_cast<SSEParser*>(ud)->feed(ptr, bytes);
    return bytes;
}

void UpstreamEngine::io_loop() {
    while (running_.load()) {
        drain_queue();
        int running = 0;
        curl_multi_perform(multi_, &running);
        reap_finished();
        curl_multi_poll(multi_, nullptr, 0, 200, nullptr);
    }
}

void UpstreamEngine::drain_queue() {
    std::queue<PendingTask> local;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::swap(local, pending_);
    }
    while (!local.empty()) {
        add_transfer(std::move(local.front()));
        local.pop();
    }
}

void UpstreamEngine::add_transfer(PendingTask task) {
    const LLMRequest&   req  = task.req;
    const NodeSnapshot& node = task.node;

    json body_json = {
        {"model",       node.model.empty() ? req.model : node.model},
        {"messages",    json::parse(req.messages_json)},
        {"temperature", req.temperature},
        {"max_tokens",  req.max_tokens},
        {"stream",      true}
    };

    CURL* easy = curl_easy_init();
    if (!easy) {
        req.on_error("curl_easy_init failed");
        return;
    }

    auto ctx = std::make_unique<TransferCtx>();
    ctx->body   = body_json.dump();
    ctx->err_cb = req.on_error;
    ctx->parser = std::make_unique<SSEParser>(req.on_token, req.on_done);

    ctx->hdrs = curl_slist_append(ctx->hdrs, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + node.api_key;
    ctx->hdrs = curl_slist_append(ctx->hdrs, auth.c_str());
    ctx->hdrs = curl_slist_append(ctx->hdrs, "Accept: text/event-stream");
    ctx->hdrs = curl_slist_append(ctx->hdrs, "Cache-Control: no-cache");
    ctx->hdrs = curl_slist_append(ctx->hdrs, "Expect:");

    curl_easy_setopt(easy, CURLOPT_URL,            node.url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER,     ctx->hdrs);
    curl_easy_setopt(easy, CURLOPT_POST,           1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS,     ctx->body.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,  (long)ctx->body.size());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA,      ctx->parser.get());
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE,     1024L);
    curl_easy_setopt(easy, CURLOPT_TCP_NODELAY,    1L);

    METRIC_INC(active_requests);

    active_[easy] = std::move(ctx);
    curl_multi_add_handle(multi_, easy);
}

void UpstreamEngine::reap_finished() {
    CURLMsg* msg;
    int msgs_left = 0;
    while ((msg = curl_multi_info_read(multi_, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE) continue;
        CURL* easy = msg->easy_handle;
        CURLcode res = msg->data.result;

        auto it = active_.find(easy);
        if (it != active_.end()) {
            long http_code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

            if (res != CURLE_OK) {
                it->second->err_cb(curl_easy_strerror(res));
                METRIC_INC(errors);
            } else if (http_code != 200) {
                it->second->err_cb("HTTP " + std::to_string(http_code));
                METRIC_INC(errors);
            } else {
                METRIC_INC(success_count);
            }

            METRIC_DEC(active_requests);
            active_.erase(it);
        }

        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
    }
}

} // namespace gw
