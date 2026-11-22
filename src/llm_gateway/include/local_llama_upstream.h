#pragma once
/**
 * local_llama_upstream.h — 本地 Llama 上游引擎
 *   dlopen libllama.so，在网关进程内直接跑量化模型推理
 *   实现 IUpstream 接口
 */
#include "i_upstream.h"
#include "common.h"
#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <memory>

// ─── llama.cpp C API 函数指针类型定义 ────────────────────────────
struct llama_model;
struct llama_context;
struct llama_model_params;
struct llama_context_params;
struct llama_batch;
struct llama_token_data;
struct llama_sampler;

// 简化的函数指针（实际使用时通过 dlopen 加载）
typedef llama_model*        (*llama_load_model_from_file_fn)(const char*, llama_model_params);
typedef llama_context*      (*llama_new_context_with_model_fn)(llama_model*, llama_context_params);
typedef void                (*llama_free_model_fn)(llama_model*);
typedef void                (*llama_free_fn)(llama_context*);
typedef int32_t             (*llama_n_vocab_fn)(const llama_model*);
typedef llama_model_params  (*llama_model_default_params_fn)();
typedef llama_context_params (*llama_context_default_params_fn)();

namespace gw {

// ─── Llama 配置 ────────────────────────────────────────────────────
struct LlamaConfig {
    std::string model_path;        // GGUF 模型文件路径
    int         n_ctx       = 512; // 上下文长度
    int         n_threads   = 2;   // 推理线程数
    int         n_gpu_layers = 0;  // GPU offload 层数（0 = CPU only）
    int         n_batch     = 512; // 批处理大小
};

// ─── LocalLlamaUpstream ────────────────────────────────────────────
class LocalLlamaUpstream : public IUpstream {
public:
    explicit LocalLlamaUpstream(const LlamaConfig& cfg);
    ~LocalLlamaUpstream() override;

    void start() override;
    void stop() override;
    const char* name() const override { return "LocalLlamaUpstream (libllama.so)"; }

    void submit(const LLMRequest& req, const std::string& url,
                const std::string& api_key) override;

    // 模型是否加载成功
    bool is_loaded() const { return model_loaded_.load(); }

private:
    LlamaConfig cfg_;

    // dlopen 句柄
    void* lib_handle_ = nullptr;

    // llama.cpp 对象
    llama_model*   model_   = nullptr;
    llama_context* ctx_     = nullptr;

    std::atomic<bool> model_loaded_{false};
    std::atomic<bool> running_{false};

    // 推理线程
    std::thread infer_thread_;

    // 任务队列
    struct InferTask {
        LLMRequest   req;
    };
    std::mutex              task_mu_;
    std::queue<InferTask>   task_queue_;

    // 推理循环
    void infer_loop();

    // 加载动态库
    bool load_library();
    void unload_library();
};

// LocalLlamaUpstream 的工厂函数
std::unique_ptr<IUpstream> create_local_llama_upstream(const LlamaConfig& cfg);

} // namespace gw
