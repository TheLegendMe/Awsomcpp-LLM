#pragma once
/**
 * plugin_types.h — Plugin return enum, context struct, hook signatures
 */
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <any>

namespace gw {

// ─── Plugin 返回值枚举 ─────────────────────────────────────────────
// CONTINUE:   放行，交给下一个插件
// MODIFY:     修改了请求数据（如翻译 Prompt），放行
// ABORT_EARLY: 短路拦截，Lua 直接生成 HTTP Response，不请求大模型
enum class PluginResult {
    CONTINUE,
    MODIFY,
    ABORT_EARLY
};

inline const char* to_string(PluginResult r) {
    switch (r) {
        case PluginResult::CONTINUE:    return "CONTINUE";
        case PluginResult::MODIFY:      return "MODIFY";
        case PluginResult::ABORT_EARLY: return "ABORT_EARLY";
    }
    return "UNKNOWN";
}

// ─── Plugin 请求上下文 ─────────────────────────────────────────────
struct PluginContext {
    // 请求信息（可被插件修改）
    std::string model;
    std::string messages_json;
    double      temperature = 0.7;
    int         max_tokens  = 2048;

    // 元信息（只读）
    std::string user_ip;
    std::string api_key;
    uint64_t    request_id = 0;

    // ABORT_EARLY 时，插件设置的响应
    int         abort_status  = 200;
    std::string abort_body;
    bool        aborted = false;

    // 插件间传递的共享数据
    std::unordered_map<std::string, std::any> shared;

    template<typename T>
    T get(const std::string& key, T default_val = {}) const {
        auto it = shared.find(key);
        if (it != shared.end()) {
            try { return std::any_cast<T>(it->second); }
            catch (...) {}
        }
        return default_val;
    }

    template<typename T>
    void set(const std::string& key, T val) {
        shared[key] = std::move(val);
    }
};

// ─── Hook 函数签名 ─────────────────────────────────────────────────
// on_request:  请求到达时调用，返回 PluginResult
// on_token:    每个流式 token 到达时调用，返回（可能修改的）token，空字符串表示抑制
// on_done:     请求完成时调用
using OnRequestHook = std::function<PluginResult(PluginContext&)>;
using OnTokenHook   = std::function<std::string(const std::string& token)>;
using OnDoneHook    = std::function<void()>;

} // namespace gw
