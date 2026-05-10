#pragma once
/**
 * i_upstream.h — 上游引擎抽象接口
 *   实现: RemoteUpstream (curl_multi) / LocalLlamaUpstream (libllama)
 */
#include <string>
#include <functional>
#include <memory>

namespace gw {

struct LLMRequest;

// ─── 上游引擎接口 ──────────────────────────────────────────────────
class IUpstream {
public:
    virtual ~IUpstream() = default;

    // 启动 / 停止引擎
    virtual void start() = 0;
    virtual void stop()  = 0;

    // 提交流式聊天请求
    // req:  请求结构（model, messages_json, temperature, max_tokens, callbacks）
    // url:  上游 URL（RemoteUpstream 使用，LocalLlamaUpstream 忽略）
    // api_key: API Key
    virtual void submit(const LLMRequest& req,
                        const std::string& url,
                        const std::string& api_key) = 0;

    // 引擎名称（用于日志）
    virtual const char* name() const = 0;
};

// ─── 上游工厂 ──────────────────────────────────────────────────────
enum class UpstreamType {
    REMOTE,       // curl_multi → 远端 OpenAI/vLLM
    LOCAL_LLAMA   // libllama.so → 进程内量化模型推理
};

std::unique_ptr<IUpstream> create_upstream(UpstreamType type);

} // namespace gw
