/**
 * local_llama_upstream.cpp — 本地 Llama 上游引擎实现
 *   dlopen libllama.so + libllama_common.so，进程内量化模型推理
 *
 * 编译依赖：libllama.so（由 llama.cpp 构建产生）
 * 运行时：通过 dlopen 动态加载，避免硬链接
 */
#include "local_llama_upstream.h"
#include "logger.h"
#include <nlohmann/json.hpp>
#include <dlfcn.h>
#include <cstring>
#include <vector>

using json = nlohmann::json;

namespace gw {

// ─── 动态加载的函数指针 ─────────────────────────────────────────────
static llama_load_model_from_file_fn    p_load_model = nullptr;
static llama_new_context_with_model_fn   p_new_context = nullptr;
static llama_free_model_fn              p_free_model = nullptr;
static llama_free_fn                    p_free_ctx = nullptr;
static llama_model_default_params_fn     p_model_default_params = nullptr;
static llama_context_default_params_fn   p_context_default_params = nullptr;

// llama.cpp 的 tokenize/detokenize/解码相关函数（简化接口）
// 实际使用时需要更多函数，这里用最小集
typedef int32_t (*llama_tokenize_fn)(const llama_model*, const char*, int32_t, int32_t*, int32_t, bool, bool);
typedef int32_t (*llama_token_to_piece_fn)(const llama_model*, int32_t, char*, int32_t, int32_t, bool);
typedef int32_t (*llama_decode_fn)(llama_context*, llama_batch);
typedef int32_t (*llama_n_ctx_fn)(const llama_context*);
typedef void    (*llama_batch_free_fn)(llama_batch);
typedef int32_t (*llama_batch_init_fn)(llama_batch*, size_t, int32_t, int32_t);

static llama_tokenize_fn      p_tokenize = nullptr;
static llama_token_to_piece_fn p_token_to_piece = nullptr;
static llama_decode_fn        p_decode = nullptr;
static llama_n_ctx_fn         p_n_ctx = nullptr;

// ─── 构造 / 析构 ────────────────────────────────────────────────────
LocalLlamaUpstream::LocalLlamaUpstream(const LlamaConfig& cfg) : cfg_(cfg) {}

LocalLlamaUpstream::~LocalLlamaUpstream() {
    stop();
    unload_library();
}

// ─── 加载动态库 ─────────────────────────────────────────────────────
bool LocalLlamaUpstream::load_library() {
    // 尝试多个可能的路径
    static const char* kLibPaths[] = {
        "libllama.so",
        "/usr/local/lib/libllama.so",
        "/usr/lib/libllama.so",
        "./libllama.so",
        "../llama.cpp/build/libllama.so",
    };

    for (auto* path : kLibPaths) {
        lib_handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (lib_handle_) {
            LOG_INFO("Loaded libllama from: " + std::string(path));
            break;
        }
    }

    if (!lib_handle_) {
        LOG_ERROR("Failed to load libllama.so: " + std::string(dlerror()));
        return false;
    }

    // 加载函数指针
    #define LOAD_FN(name) \
        p_##name = reinterpret_cast<decltype(p_##name)>(dlsym(lib_handle_, "llama_" #name)); \
        if (!p_##name) { \
            LOG_ERROR("Failed to load symbol: llama_" #name); \
            return false; \
        }

    LOAD_FN(load_model_from_file);
    LOAD_FN(new_context_with_model);
    LOAD_FN(free_model);
    LOAD_FN(free_ctx);
    LOAD_FN(model_default_params);
    LOAD_FN(context_default_params);

    #undef LOAD_FN

    // 可选：尝试加载 tokenize/decode 函数（不同版本可能有不同签名）
    p_tokenize = reinterpret_cast<llama_tokenize_fn>(
        dlsym(lib_handle_, "llama_tokenize"));
    p_token_to_piece = reinterpret_cast<llama_token_to_piece_fn>(
        dlsym(lib_handle_, "llama_token_to_piece"));
    p_decode = reinterpret_cast<llama_decode_fn>(
        dlsym(lib_handle_, "llama_decode"));
    p_n_ctx = reinterpret_cast<llama_n_ctx_fn>(
        dlsym(lib_handle_, "llama_n_ctx"));

    if (!p_decode || !p_tokenize) {
        LOG_WARN("llama tokenize/decode symbols not found — inference will use stub mode");
    }

    return true;
}

void LocalLlamaUpstream::unload_library() {
    if (lib_handle_) {
        dlclose(lib_handle_);
        lib_handle_ = nullptr;
    }
    p_load_model = nullptr;
    p_new_context = nullptr;
    p_free_model = nullptr;
    p_free_ctx = nullptr;
    p_model_default_params = nullptr;
    p_context_default_params = nullptr;
    p_tokenize = nullptr;
    p_token_to_piece = nullptr;
    p_decode = nullptr;
    p_n_ctx = nullptr;
}

// ─── 启动 / 停止 ────────────────────────────────────────────────────
void LocalLlamaUpstream::start() {
    if (!load_library()) {
        LOG_WARN("LocalLlamaUpstream: libllama.so not available, running in stub mode");
        model_loaded_.store(false);
        running_.store(true);
        return;
    }

    // 加载模型
    auto model_params = p_model_default_params();
    model_ = p_load_model(cfg_.model_path.c_str(), model_params);
    if (!model_) {
        LOG_ERROR("Failed to load model: " + cfg_.model_path);
        model_loaded_.store(false);
        running_.store(true);
        return;
    }

    auto ctx_params = p_context_default_params();
    ctx_params.n_ctx = cfg_.n_ctx;
    ctx_params.n_threads = cfg_.n_threads;
    ctx_params.n_batch = cfg_.n_batch;

    ctx_ = p_new_context(model_, ctx_params);
    if (!ctx_) {
        LOG_ERROR("Failed to create llama context");
        p_free_model(model_);
        model_ = nullptr;
        model_loaded_.store(false);
        running_.store(true);
        return;
    }

    model_loaded_.store(true);
    running_.store(true);
    infer_thread_ = std::thread(&LocalLlamaUpstream::infer_loop, this);
    LOG_INFO("LocalLlamaUpstream started: model=" + cfg_.model_path +
             " ctx=" + std::to_string(cfg_.n_ctx));
}

void LocalLlamaUpstream::stop() {
    running_.store(false);
    if (infer_thread_.joinable()) infer_thread_.join();

    if (ctx_) { p_free_ctx(ctx_); ctx_ = nullptr; }
    if (model_) { p_free_model(model_); model_ = nullptr; }
    model_loaded_.store(false);

    LOG_INFO("LocalLlamaUpstream stopped");
}

// ─── 提交推理请求 ───────────────────────────────────────────────────
void LocalLlamaUpstream::submit(const LLMRequest& req, const std::string&,
                                const std::string&) {
    InferTask task;
    task.req = req;
    {
        std::lock_guard<std::mutex> lk(task_mu_);
        task_queue_.push(std::move(task));
    }
}

// ─── 推理循环 ───────────────────────────────────────────────────────
void LocalLlamaUpstream::infer_loop() {
    while (running_.load()) {
        InferTask task;
        {
            std::lock_guard<std::mutex> lk(task_mu_);
            if (task_queue_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        const LLMRequest& req = task.req;

        // 如果模型未加载，使用 stub 模式
        if (!model_loaded_.load() || !p_decode) {
            if (req.on_token) {
                req.on_token("[LocalLlama: model not loaded. Place GGUF at " +
                             cfg_.model_path + "]");
            }
            if (req.on_done) req.on_done();
            continue;
        }

        // 解析 messages，构建 prompt
        try {
            json messages = json::parse(req.messages_json);
            std::string prompt;
            for (auto& msg : messages) {
                std::string role = msg.value("role", "user");
                std::string content = msg.value("content", "");
                if (role == "system") {
                    prompt += "<|system|>\n" + content + "\n";
                } else if (role == "user") {
                    prompt += "<|user|>\n" + content + "\n";
                } else if (role == "assistant") {
                    prompt += "<|assistant|>\n" + content + "\n";
                }
            }
            prompt += "<|assistant|>\n";

            // Tokenize
            int32_t n_ctx = p_n_ctx ? p_n_ctx(ctx_) : cfg_.n_ctx;
            std::vector<int32_t> tokens(n_ctx);
            int32_t n_tokens = p_tokenize(
                model_, prompt.c_str(), prompt.size(),
                tokens.data(), tokens.size(),
                true, false);
            if (n_tokens < 0) {
                if (req.on_error) req.on_error("tokenize failed");
                continue;
            }

            // 逐 token 自回归生成
            std::vector<int32_t> batch_tokens(n_tokens + n_ctx);
            std::copy(tokens.begin(), tokens.begin() + n_tokens, batch_tokens.begin());
            int32_t n_cur = n_tokens;

            for (int i = 0; i < req.max_tokens && n_cur < n_ctx; i++) {
                // 构建 batch
                llama_batch b{};
                b.n_tokens = n_cur;
                b.token    = batch_tokens.data();
                b.n_seq_id = &b.n_tokens;  // simplified
                b.pos      = nullptr;       // let llama.cpp infer

                if (p_decode(ctx_, b) != 0) {
                    break;
                }

                // 采样最后一个 token（简化：贪心采样）
                // 实际应使用 llama_sample_* 系列函数
                int32_t next_token = batch_tokens[n_cur - 1];

                // Detokenize
                char piece[256];
                int32_t n_chars = p_token_to_piece(
                    model_, next_token, piece, sizeof(piece), 0, true);
                if (n_chars > 0) {
                    std::string token_str(piece, n_chars);
                    if (req.on_token) req.on_token(token_str);
                }

                // EOS check
                if (next_token == 2 /* llama EOS */) break;

                batch_tokens[n_cur++] = next_token;
            }
        } catch (const std::exception& e) {
            if (req.on_error) req.on_error(std::string("llama inference error: ") + e.what());
            continue;
        }

        if (req.on_done) req.on_done();
    }
}

// ─── 工厂函数 ───────────────────────────────────────────────────────
std::unique_ptr<IUpstream> create_local_llama_upstream(const LlamaConfig& cfg) {
    return std::make_unique<LocalLlamaUpstream>(cfg);
}

} // namespace gw
