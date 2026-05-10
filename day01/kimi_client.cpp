/**
 * kimi_client.cpp
 * Kimi API C++ 客户端
 *   架构：curl_multi + 独立 I/O 线程 + 线程安全任务队列
 *   UI 线程只负责提交任务，I/O 线程驱动 curl_multi_perform 事件循环
 */

#include <iostream>
#include <string>
#include <functional>
#include <stdexcept>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─────────────────────────────────────────────
// 全局中断标志
// ─────────────────────────────────────────────
static std::atomic<bool> g_interrupted{false};
static void on_sigint(int) { g_interrupted = true; }

// ─────────────────────────────────────────────
// SSEParser（与之前相同，无需修改）
// ─────────────────────────────────────────────
class SSEParser {
public:
    using TokCb  = std::function<void(const std::string&)>;
    using DoneCb = std::function<void()>;

    SSEParser(TokCb tok_cb, DoneCb done_cb)
        : tok_cb_(std::move(tok_cb)), done_cb_(std::move(done_cb)) {}

    void feed(const char* data, size_t len) {
        buf_.append(data, len);
        flush_lines();
    }

private:
    std::string buf_;
    TokCb tok_cb_;
    DoneCb done_cb_;

    void flush_lines() {
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

    void process_line(const std::string& line) {
        if (line.empty() || line[0] == ':') return;
        static const std::string kPrefix = "data: ";
        if (line.size() < kPrefix.size() ||
            line.compare(0, kPrefix.size(), kPrefix) != 0) return;

        std::string payload = line.substr(kPrefix.size());
        if (payload == "[DONE]") { done_cb_(); return; }

        try {
            json obj = json::parse(payload);
            if (obj.contains("choices") && obj["choices"].is_array()
                    && !obj["choices"].empty()) {
                const auto& delta = obj["choices"][0]["delta"];
                if (delta.contains("content") && delta["content"].is_string()) {
                    std::string tok = delta["content"].get<std::string>();
                    if (!tok.empty()) tok_cb_(tok);
                }
            }
        } catch (const json::parse_error&) {}
    }
};

// ─────────────────────────────────────────────
// 线程安全队列
// ─────────────────────────────────────────────
template<typename T>
class TSQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // 非阻塞 pop，返回 false 表示队列为空
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    // 阻塞等待，直到有元素或 stop 被置位
    bool wait_pop(T& out, std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]{ return !q_.empty() || stop.load(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void notify_all() { cv_.notify_all(); }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<T> q_;
};

// ─────────────────────────────────────────────
// 请求描述（UI 线程填写，投入队列）
// ─────────────────────────────────────────────
struct Request {
    std::string              body;       // 序列化好的 JSON 请求体
    SSEParser::TokCb         tok_cb;
    SSEParser::DoneCb        done_cb;
    std::function<void(std::string)> err_cb;  // 错误回调
};

// ─────────────────────────────────────────────
// 每个进行中的 easy handle 对应的上下文
// ─────────────────────────────────────────────
struct TransferCtx {
    std::unique_ptr<SSEParser> parser;
    std::function<void(std::string)> err_cb;
    std::string body;   // 保持 body 生命周期（CURLOPT_POSTFIELDS 需要）
};

// ─────────────────────────────────────────────
// libcurl write callback
// ─────────────────────────────────────────────
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    size_t bytes = size * nmemb;
    reinterpret_cast<SSEParser*>(ud)->feed(ptr, bytes);
    return g_interrupted ? 0 : bytes;
}

// ─────────────────────────────────────────────
// AsyncEngine：I/O 线程 + curl_multi 事件循环
// ─────────────────────────────────────────────
class AsyncEngine {
public:
    struct Config {
        std::string api_key;
        std::string base_url    = "https://api.moonshot.cn/v1/chat/completions";
        std::string model       = "moonshot-v1-8k";
        double      temperature = 0.7;
        int         max_tokens  = 2048;
        bool        verbose     = false;
    };

    explicit AsyncEngine(Config cfg) : cfg_(std::move(cfg)), stop_(false) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
            throw std::runtime_error("curl_global_init 失败");
        multi_ = curl_multi_init();
        if (!multi_) throw std::runtime_error("curl_multi_init 失败");

        // 启动 I/O 线程
        io_thread_ = std::thread(&AsyncEngine::io_loop, this);
    }

    ~AsyncEngine() {
        stop_.store(true);
        queue_.notify_all();
        if (io_thread_.joinable()) io_thread_.join();
        curl_multi_cleanup(multi_);
        curl_global_cleanup();
    }

    AsyncEngine(const AsyncEngine&)            = delete;
    AsyncEngine& operator=(const AsyncEngine&) = delete;

    // UI 线程调用：提交一个流式请求，立即返回
    void submit(const json& messages,
                SSEParser::TokCb tok_cb,
                SSEParser::DoneCb done_cb,
                std::function<void(std::string)> err_cb)
    {
        json body_json = {
            {"model",       cfg_.model},
            {"messages",    messages},
            {"temperature", cfg_.temperature},
            {"max_tokens",  cfg_.max_tokens},
            {"stream",      true}
        };
        Request req;
        req.body     = body_json.dump();
        req.tok_cb   = std::move(tok_cb);
        req.done_cb  = std::move(done_cb);
        req.err_cb   = std::move(err_cb);
        queue_.push(std::move(req));
    }

private:
    Config                   cfg_;
    CURLM*                   multi_;
    TSQueue<Request>         queue_;
    std::atomic<bool>        stop_;
    std::thread              io_thread_;

    // 当前活跃的 easy handle → TransferCtx 映射（只在 I/O 线程访问，无需加锁）
    std::vector<std::pair<CURL*, std::unique_ptr<TransferCtx>>> active_;

    // ── I/O 线程主循环 ──────────────────────────────────────────
    void io_loop() {
        while (!stop_.load()) {
            // 1. 把队列里所有待处理请求加入 multi
            drain_queue();

            if (active_.empty()) {
                // 没有活跃传输，阻塞等待新任务
                Request req;
                if (!queue_.wait_pop(req, stop_)) break;
                add_transfer(std::move(req));
            }

            // 2. 驱动 curl_multi_perform
            int running = 0;
            curl_multi_perform(multi_, &running);

            // 3. 收割完成的 easy handle
            reap_finished();

            // 4. 用 curl_multi_wait 等待 socket 事件（最多 50ms），避免忙等
            int numfds = 0;
            curl_multi_wait(multi_, nullptr, 0, 50, &numfds);
        }

        // 清理残留
        for (auto& [easy, ctx] : active_) {
            curl_multi_remove_handle(multi_, easy);
            curl_easy_cleanup(easy);
        }
        active_.clear();
    }

    // 把队列里所有就绪的 Request 转成 easy handle 加入 multi
    void drain_queue() {
        Request req;
        while (queue_.try_pop(req)) {
            add_transfer(std::move(req));
        }
    }

    void add_transfer(Request req) {
        CURL* easy = curl_easy_init();
        if (!easy) {
            req.err_cb("curl_easy_init 失败");
            return;
        }

        auto ctx = std::make_unique<TransferCtx>();
        ctx->body = std::move(req.body);
        ctx->err_cb = std::move(req.err_cb);
        ctx->parser = std::make_unique<SSEParser>(
            std::move(req.tok_cb), std::move(req.done_cb));

        // Headers
        curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        std::string auth = "Authorization: Bearer " + cfg_.api_key;
        hdrs = curl_slist_append(hdrs, auth.c_str());
        hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
        hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache");
        hdrs = curl_slist_append(hdrs, "Expect:");

        curl_easy_setopt(easy, CURLOPT_URL,            cfg_.base_url.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER,     hdrs);
        curl_easy_setopt(easy, CURLOPT_POST,           1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS,     ctx->body.c_str());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,  (long)ctx->body.size());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,  curl_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA,      ctx->parser.get());
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(easy, CURLOPT_BUFFERSIZE,     1024L);
        curl_easy_setopt(easy, CURLOPT_TCP_NODELAY,    1L);
        // 注意：curl_multi 不设 CURLOPT_TIMEOUT，用 CURLOPT_TIMEOUT_MS 或不设
        if (cfg_.verbose)
            curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);

        // 把 hdrs 指针存到 easy 的 private 字段，方便 reap 时释放
        curl_easy_setopt(easy, CURLOPT_PRIVATE, hdrs);

        curl_multi_add_handle(multi_, easy);
        active_.emplace_back(easy, std::move(ctx));
    }

    // 收割已完成的 easy handle，触发错误回调
    void reap_finished() {
        CURLMsg* msg;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(multi_, &msgs_left))) {
            if (msg->msg != CURLMSG_DONE) continue;

            CURL* easy = msg->easy_handle;
            CURLcode res = msg->data.result;

            // 找到对应的 ctx
            auto it = std::find_if(active_.begin(), active_.end(),
                [easy](const auto& p){ return p.first == easy; });

            if (it != active_.end()) {
                // 取出 hdrs 指针并释放
                curl_slist* hdrs = nullptr;
                curl_easy_getinfo(easy, CURLINFO_PRIVATE, &hdrs);
                if (hdrs) curl_slist_free_all(hdrs);

                long http_code = 0;
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

                if (!g_interrupted) {
                    if (res != CURLE_OK) {
                        it->second->err_cb(
                            std::string("curl 错误: ") + curl_easy_strerror(res));
                    } else if (http_code != 200) {
                        it->second->err_cb(
                            "HTTP " + std::to_string(http_code) + " 错误");
                    }
                }

                curl_multi_remove_handle(multi_, easy);
                curl_easy_cleanup(easy);
                active_.erase(it);
            }
        }
    }
};

// ─────────────────────────────────────────────
// Conversation
// ─────────────────────────────────────────────
class Conversation {
public:
    explicit Conversation(std::string sys = "") {
        if (!sys.empty())
            h_.push_back({{"role","system"},{"content",std::move(sys)}});
    }
    void add_user(const std::string& t)      { h_.push_back({{"role","user"},{"content",t}}); }
    void add_assistant(const std::string& t) { h_.push_back({{"role","assistant"},{"content",t}}); }
    void pop_last_user() {
        if (!h_.empty() && h_.back()["role"] == "user") h_.erase(h_.end()-1);
    }
    void clear() {
        if (!h_.empty() && h_.front()["role"] == "system") {
            json s = h_.front(); h_.clear(); h_.push_back(s);
        } else { h_.clear(); }
    }
    const json& messages() const { return h_; }
private:
    json h_ = json::array();
};

// ─────────────────────────────────────────────
// REPL
// ─────────────────────────────────────────────
static std::string trim(const std::string& s) {
    size_t l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return "";
    return s.substr(l, s.find_last_not_of(" \t\r\n") - l + 1);
}

static void run_repl(AsyncEngine& engine, Conversation& conv) {
    std::cout
        << "╔══════════════════════════════════════════╗\n"
        << "║   Kimi Chat  (curl_multi + I/O 线程版)   ║\n"
        << "╠══════════════════════════════════════════╣\n"
        << "║  :quit/:q  退出   :clear  清空历史       ║\n"
        << "╚══════════════════════════════════════════╝\n\n";

    while (!g_interrupted) {
        std::cout << "\033[1;32m你\033[0m > " << std::flush;
        std::string input;
        if (!std::getline(std::cin, input)) break;
        if (g_interrupted) break;

        input = trim(input);
        if (input.empty()) continue;
        if (input == ":quit" || input == ":q") break;
        if (input == ":clear") { conv.clear(); std::cout << "[历史已清空]\n"; continue; }

        conv.add_user(input);
        std::cout << "\033[1;34mKimi\033[0m > " << std::flush;

        // 用条件变量等待本次流结束
        std::mutex done_mu;
        std::condition_variable done_cv;
        bool finished = false;
        std::string reply;
        bool had_error = false;

        engine.submit(
            conv.messages(),
            // tok_cb（在 I/O 线程调用）
            [&reply](const std::string& tok) {
                std::cout << tok << std::flush;
                reply += tok;
            },
            // done_cb
            [&]{
                std::lock_guard<std::mutex> lk(done_mu);
                finished = true;
                done_cv.notify_one();
            },
            // err_cb
            [&](std::string err){
                std::cerr << "\n\033[1;31m[错误]\033[0m " << err << "\n";
                had_error = true;
                std::lock_guard<std::mutex> lk(done_mu);
                finished = true;
                done_cv.notify_one();
            }
        );

        // UI 线程阻塞等待本次请求完成
        {
            std::unique_lock<std::mutex> lk(done_mu);
            done_cv.wait(lk, [&]{ return finished || g_interrupted; });
        }

        std::cout << "\n";

        if (had_error) {
            conv.pop_last_user();
        } else if (!reply.empty()) {
            conv.add_assistant(reply);
        }
    }

    std::cout << "\n再见！\n";
}

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::signal(SIGINT, on_sigint);

    std::string api_key;
    if (argc > 1) {
        api_key = argv[1];
    } else {
        const char* env = std::getenv("MOONSHOT_API_KEY");
        if (env) api_key = env;
    }

    if (api_key.empty()) {
        std::cerr << "用法: " << argv[0] << " <API_KEY>\n"
                  << "或:   export MOONSHOT_API_KEY=sk-... && " << argv[0] << "\n";
        return 1;
    }

    AsyncEngine::Config cfg;
    cfg.api_key     = api_key;
    cfg.model       = "moonshot-v1-8k";
    cfg.temperature = 0.7;
    cfg.max_tokens  = 2048;

    AsyncEngine engine(cfg);
    Conversation conv(
        "你是 Kimi，由 Moonshot AI 提供的智能助手。"
        "请用清晰、准确的中文回答问题。"
    );

    run_repl(engine, conv);
    return 0;
}
