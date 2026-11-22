#pragma once
/**
 * lua_sandbox.h — Lua VM 沙箱
 *   每个 VM 独立沙箱：置空危险全局函数，仅暴露白名单 gateway.* API
 */
#include <string>
#include <functional>

// 前向声明 sol（避免头文件依赖传播）
namespace sol {
    class state;
}

namespace gw {

// ─── 沙箱配置 ──────────────────────────────────────────────────────
struct SandboxConfig {
    size_t   max_memory_kb   = 1024;   // Lua VM 最大内存 (KB)
    size_t   max_instruction = 500000; // 最大指令数（防止死循环）
    bool     enable_http     = false;  // 是否允许插件发起 HTTP 请求
};

// ─── 暴露给 Lua 的 Gateway API ─────────────────────────────────────
// 这些函数在 Lua 中以 gateway.xxx() 形式调用
struct GatewayAPI {
    // 日志
    std::function<void(const std::string& level, const std::string& msg)> log;

    // 指标
    std::function<void(const std::string& key, int64_t delta)> metric_inc;
    std::function<int64_t(const std::string& key)>             metric_get;

    // 获取当前时间戳 (ms)
    std::function<int64_t()> now_ms;

    // 可选：HTTP 请求（仅在 enable_http 时可用）
    std::function<std::string(const std::string& url, const std::string& body)>
        http_post;
};

// ─── 沙箱工厂 ──────────────────────────────────────────────────────
class LuaSandbox {
public:
    // 创建一个已沙箱化的 sol::state，并注入 GatewayAPI
    // 返回的 VM 中：
    //   - os, io, package, loadfile, dofile, load, require 已置 nil
    //   - rawset, rawget 已置 nil
    //   - 仅保留: print, string, math, table, coroutine, utf8, error, pcall, xpcall,
    //            select, tonumber, tostring, type, next, ipairs, pairs, assert
    //   - gateway 表已注入
    static sol::state* create(const SandboxConfig& cfg, const GatewayAPI& api);

    // 重置 VM 到干净状态（保留沙箱和 API，清除全局变量和注册表）
    static void reset(sol::state& lua);

    // 设置指令计数限制
    static void set_instruction_limit(sol::state& lua, size_t max_instructions);

private:
    static void nullify_dangerous(sol::state& lua);
    static void inject_api(sol::state& lua, const GatewayAPI& api);
};

} // namespace gw
