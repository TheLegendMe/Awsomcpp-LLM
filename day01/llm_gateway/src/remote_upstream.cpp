/**
 * remote_upstream.cpp — 远程 HTTP 上游引擎实现
 */
#include "remote_upstream.h"
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
            const auto& c0 = obj["choices"][0];
            const auto& delta = c0["delta"];

            // tool_calls 解析
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()
                && !delta["tool_calls"].empty()) {
                const auto& tc = delta["tool_calls"][0];
                if (tc.contains("id") && tc["id"].is_string())
                    tc_id_ = tc["id"].get<std::string>();
                if (tc.contains("function")) {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                        tc_name_ = fn["name"].get<std::string>();
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                        tc_args_ += fn["arguments"].get<std::string>();
                }
            }

            // finish_reason == "tool_calls" → 触发 tool_call 回调
            if (c0.contains("finish_reason") &&
                c0["finish_reason"].is_string() &&
                c0["finish_reason"].get<std::string>() == "tool_calls") {
                if (tool_cb_ && !tc_id_.empty()) {
                    tool_cb_(tc_id_, tc_name_, tc_args_);
                }
                tc_id_.clear(); tc_name_.clear(); tc_args_.clear();
                if (done_cb_) done_cb_();
                return;
            }

            // content token
            if (delta.contains("content") && delta["content"].is_string()) {
                std::string tok = delta["content"].get<std::string>();
                if (!tok.empty()) {
                    METRIC_INC(total_tokens);
                    if (tok_cb_) tok_cb_(tok);
                }
            }
        }
    } catch (const std::exception& e) {
        GW_LOG_WARN("SSEParser JSON parse failed: " + std::string(e.what()) + " payload=" + payload.substr(0,200));
        if (!payload.empty() && tok_cb_) {
            tok_cb_(payload);
        }
    }
}

// ─── RemoteUpstream ───────────────────────────────────────────────
void RemoteUpstream::start() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi_ = curl_multi_init();
    running_.store(true);
    io_thread_ = std::thread(&RemoteUpstream::io_loop, this);
    GW_LOG_INFO("RemoteUpstream started");
}

void RemoteUpstream::stop() {
    running_.store(false);
    if (multi_) curl_multi_wakeup(multi_);
    if (io_thread_.joinable()) io_thread_.join();
    for (auto& [easy, ctx] : active_) {
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
    }
    active_.clear();
    if (multi_) { curl_multi_cleanup(multi_); multi_ = nullptr; }
    curl_global_cleanup();
    GW_LOG_INFO("RemoteUpstream stopped");
}

void RemoteUpstream::submit(const LLMRequest& req, const std::string& url,
                            const std::string& api_key) {
    PendingTask task;
    task.req      = req;
    task.url      = url;
    task.api_key  = api_key;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push(std::move(task));
    }
    if (multi_) curl_multi_wakeup(multi_);
}

size_t RemoteUpstream::write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    size_t bytes = size * nmemb;
    reinterpret_cast<SSEParser*>(ud)->feed(ptr, bytes);
    return bytes;
}

void RemoteUpstream::io_loop() {
    while (running_.load()) {
        drain_queue();
        int running = 0;
        curl_multi_perform(multi_, &running);
        reap_finished();
        curl_multi_poll(multi_, nullptr, 0, 200, nullptr);
    }
}

void RemoteUpstream::drain_queue() {
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

void RemoteUpstream::add_transfer(PendingTask task) {
    const LLMRequest& req      = task.req;
    const std::string& url     = task.url;
    const std::string& api_key = task.api_key;

    json body_json = {
        {"model",       req.model},
        {"messages",    json::parse(req.messages_json)},
        {"temperature", req.temperature},
        {"max_tokens",  req.max_tokens},
        {"stream",      true}
    };
    if (!req.tools_json.empty()) {
        body_json["tools"] = json::parse(req.tools_json);
        body_json["tool_choice"] = "auto";
    }

    CURL* easy = curl_easy_init();
    if (!easy) {
        if (req.on_error) req.on_error("curl_easy_init failed");
        return;
    }

    auto ctx = std::make_unique<TransferCtx>();
    ctx->body   = body_json.dump();
    ctx->err_cb = req.on_error;
    ctx->parser = std::make_unique<SSEParser>(req.on_token, req.on_tool_call, req.on_done, req.on_error);

    ctx->hdrs = curl_slist_append(ctx->hdrs, "Content-Type: application/json");
    if (!api_key.empty()) {
        std::string auth = "Authorization: Bearer " + api_key;
        ctx->hdrs = curl_slist_append(ctx->hdrs, auth.c_str());
    }
    ctx->hdrs = curl_slist_append(ctx->hdrs, "Accept: text/event-stream");
    ctx->hdrs = curl_slist_append(ctx->hdrs, "Cache-Control: no-cache");
    ctx->hdrs = curl_slist_append(ctx->hdrs, "Expect:");

    curl_easy_setopt(easy, CURLOPT_URL,            url.c_str());
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

void RemoteUpstream::reap_finished() {
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
                if (it->second->err_cb) it->second->err_cb(curl_easy_strerror(res));
                METRIC_INC(errors);
            } else if (http_code != 200) {
                if (it->second->err_cb) it->second->err_cb("HTTP " + std::to_string(http_code));
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

// ─── 工厂函数 ─────────────────────────────────────────────────────
std::unique_ptr<IUpstream> create_upstream(UpstreamType type) {
    switch (type) {
        case UpstreamType::REMOTE:
            return std::make_unique<RemoteUpstream>();
        case UpstreamType::LOCAL_LLAMA:
            // LocalLlamaUpstream 在 local_llama_upstream.cpp 中定义
            return nullptr;  // 由 local_llama_upstream 提供
    }
    return nullptr;
}

} // namespace gw
