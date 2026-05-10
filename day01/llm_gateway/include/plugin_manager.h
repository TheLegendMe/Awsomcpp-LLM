#pragma once
/**
 * plugin_manager.h — LUA 插件管理器
 *   扫描 plugins/*.lua → 按 priority 排序 → 热加载 (inotify)
 *   执行 pipeline: on_request → on_token → on_done
 */
#include "plugin_types.h"
#include "lua_sandbox.h"
#include "lua_vm_pool.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace sol { class state; }

namespace gw {

// ─── 插件描述 ──────────────────────────────────────────────────────
struct PluginInfo {
    std::string filepath;      // 文件完整路径
    std::string name;          // 插件名（来自 Lua 脚本）
    std::string version;       // 版本号
    int         priority = 0;  // 优先级（越大越先执行）
    bool        enabled  = true;
};

// ─── 插件运行时 ────────────────────────────────────────────────────
// 每个插件在 PluginManager 中有一份编译后的字节码缓存和元信息
struct PluginRuntime {
    PluginInfo  info;
    std::string script_body;   // Lua 源码（热重载用）

    // 从脚本中提取的 hook 函数名
    bool has_init     = false;
    bool has_on_request = false;
    bool has_on_token   = false;
    bool has_on_done    = false;

    // 插件通过 Lua 的 gateway.set_response() 设置 ABORT_EARLY 响应
    // 这里存储上次执行的返回结果
};

// ─── 插件管理器 ────────────────────────────────────────────────────
class PluginManager {
public:
    static PluginManager& instance() {
        static PluginManager mgr;
        return mgr;
    }

    // 初始化：扫描插件目录，加载所有 .lua 插件
    // plugins_dir: 插件目录路径
    // pool_size_per_thread: 每个 worker 线程的 VM 池大小
    void init(const std::string& plugins_dir,
              size_t pool_size_per_thread = 8);

    // 启动热重载监控线程（inotify）
    void start_hot_reload();

    // 停止热重载
    void stop_hot_reload();

    // 关闭：释放所有资源
    void shutdown();

    // 为新 worker 线程创建并设置 thread_local 池
    void setup_thread_pool();

    // ─── Pipeline 执行 ─────────────────────────────────────────────
    // 按优先级执行所有插件的 on_request hook
    // 返回: (是否通过, 最终 PluginContext)
    //   如果某插件返回 ABORT_EARLY，立即停止，返回 (false, ctx)
    //   如果全部 CONTINUE/MODIFY，返回 (true, ctx)
    struct PipelineResult {
        bool           passed;    // true = 继续请求 LLM, false = ABORT_EARLY
        PluginContext  ctx;       // 可能已被 MODIFY
        int            abort_status = 200;
        std::string    abort_body;
    };
    PipelineResult run_request_pipeline(const PluginContext& ctx);

    // 执行每个插件的 on_token hook（每个 token 调用）
    // 返回: 经过所有插件处理后的 token（可能为空，表示被抑制）
    std::string run_token_pipeline(const std::string& token);

    // 执行所有插件的 on_done hook
    void run_done_pipeline();

    // ─── 信息 ──────────────────────────────────────────────────────
    std::vector<PluginInfo> list_plugins() const;

    // 重新加载单个插件
    bool reload_plugin(const std::string& filepath);

private:
    PluginManager() = default;

    void scan_and_load();
    void load_plugin(const std::string& filepath);
    void unload_plugin(const std::string& filepath);
    void sort_plugins();
    void exec_plugin_hook(const std::string& name, const std::string& hook,
                          sol::state& lua, const std::string& arg);

    // 暴露给 Lua 的 C++ API（在 VM 中通过 gateway.* 调用）
    GatewayAPI build_api();

    // 热重载线程
    void hot_reload_loop();
    static constexpr int kInotifyWatchFlags =
        // IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE
        0x00000002 | 0x00000008 | 0x00000080 | 0x00000100;

    std::string plugins_dir_;
    size_t      pool_size_per_thread_ = 8;
    SandboxConfig sandbox_cfg_;

    // 插件列表（按 priority 降序排列）
    std::vector<PluginRuntime> plugins_;
    mutable std::mutex         plugins_mu_;

    // inotify 相关
    std::thread          watch_thread_;
    std::atomic<bool>    watching_{false};
    int                  inotify_fd_ = -1;

    // GatewayAPI 持有 PluginManager 的成员函数引用
    GatewayAPI api_;
};

} // namespace gw
