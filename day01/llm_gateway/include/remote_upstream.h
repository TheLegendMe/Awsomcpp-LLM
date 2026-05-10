#pragma once
/**
 * remote_upstream.h — 远程 HTTP 上游引擎
 *   curl_multi + 独立 I/O 线程 + SSE 流式解析
 *   实现 IUpstream 接口，请求远端 OpenAI 兼容 API
 */
#include "i_upstream.h"
#include "common.h"
#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <functional>

namespace gw {

// ─── SSE 解析器 ───────────────────────────────────────────────────
class SSEParser {
public:
    using TokenCb = std::function<void(const std::string&)>;
    using ToolCallCb = std::function<void(const std::string& call_id,
                                           const std::string& name,
                                           const std::string& arguments)>;
    using DoneCb = std::function<void()>;
    using ErrorCb = std::function<void(const std::string&)>;

    SSEParser(TokenCb tok_cb, ToolCallCb tool_cb, DoneCb done_cb, ErrorCb err_cb)
        : tok_cb_(std::move(tok_cb))
        , tool_cb_(std::move(tool_cb))
        , done_cb_(std::move(done_cb))
        , err_cb_(std::move(err_cb)) {}

    void feed(const char* data, size_t len);

private:
    std::string buf_;
    TokenCb    tok_cb_;
    ToolCallCb tool_cb_;
    DoneCb     done_cb_;
    ErrorCb    err_cb_;

    // tool call 累积状态
    std::string tc_id_;
    std::string tc_name_;
    std::string tc_args_;  // 片段拼接

    void flush_lines();
    void process_line(const std::string& line);
};

// ─── 传输上下文 ───────────────────────────────────────────────────
struct TransferCtx {
    std::string                              body;
    std::unique_ptr<SSEParser>               parser;
    std::function<void(const std::string&)>  err_cb;
    curl_slist*                              hdrs = nullptr;

    ~TransferCtx() { if (hdrs) curl_slist_free_all(hdrs); }
};

// ─── RemoteUpstream ────────────────────────────────────────────────
class RemoteUpstream : public IUpstream {
public:
    RemoteUpstream() = default;
    ~RemoteUpstream() override { stop(); }

    void start() override;
    void stop() override;
    const char* name() const override { return "RemoteUpstream (curl_multi)"; }

    void submit(const LLMRequest& req, const std::string& url,
                const std::string& api_key) override;

private:
    CURLM*                   multi_ = nullptr;
    std::thread              io_thread_;
    std::atomic<bool>        running_{false};

    struct PendingTask {
        LLMRequest   req;
        std::string  url;
        std::string  api_key;
    };
    std::mutex               mu_;
    std::queue<PendingTask>  pending_;
    std::unordered_map<CURL*, std::unique_ptr<TransferCtx>> active_;

    void io_loop();
    void drain_queue();
    void add_transfer(PendingTask task);
    void reap_finished();

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud);
};

} // namespace gw
