#pragma once
/**
 * upstream.h — 上游 HTTP 引擎
 *   curl_multi + 独立 I/O 线程，SSE 流式解析
 */
#include "common.h"
#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace gw {

// ─── SSE 解析器 ───────────────────────────────────────────────────
class SSEParser {
public:
    SSEParser(std::function<void(const std::string&)> tok_cb,
              std::function<void()> done_cb)
        : tok_cb_(std::move(tok_cb)), done_cb_(std::move(done_cb)) {}

    void feed(const char* data, size_t len);

private:
    std::string buf_;
    std::function<void(const std::string&)> tok_cb_;
    std::function<void()>                   done_cb_;

    void flush_lines();
    void process_line(const std::string& line);
};

// ─── 每个 easy handle 的传输上下文 ───────────────────────────────
struct TransferCtx {
    std::string                              body;     // POST body（保持生命周期）
    std::unique_ptr<SSEParser>               parser;
    std::function<void(const std::string&)>  err_cb;
    curl_slist*                              hdrs = nullptr;

    ~TransferCtx() { if (hdrs) curl_slist_free_all(hdrs); }
};

// ─── 上游引擎 ─────────────────────────────────────────────────────
class UpstreamEngine {
public:
    static UpstreamEngine& instance();

    void start();   // 启动 I/O 线程
    void stop();    // 停止 I/O 线程

    // 提交一个流式请求（线程安全，可从任意线程调用）
    void submit(const LLMRequest& req, const BackendNode& node);

private:
    UpstreamEngine() = default;

    CURLM*                   multi_ = nullptr;
    std::thread              io_thread_;
    std::atomic<bool>        running_{false};

    // 任务队列（UI/网关线程 → I/O 线程）
    // 只存 BackendNode 的 POD 字段，避免 atomic 拷贝问题
    struct NodeSnapshot {
        std::string name;
        std::string url;
        std::string api_key;
        std::string model;
    };
    struct PendingTask {
        LLMRequest   req;
        NodeSnapshot node;
    };
    std::mutex               mu_;
    std::queue<PendingTask>  pending_;

    // 活跃传输表（只在 I/O 线程访问）
    std::unordered_map<CURL*, std::unique_ptr<TransferCtx>> active_;

    void io_loop();
    void drain_queue();
    void add_transfer(PendingTask task);
    void reap_finished();

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud);
};

} // namespace gw
