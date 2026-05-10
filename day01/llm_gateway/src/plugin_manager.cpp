/**
 * plugin_manager.cpp — LUA 插件管理器实现
 */
#include "plugin_manager.h"
#include "lua_vm_pool.h"
#include "logger.h"
#include "metrics.h"
#include <sol/sol.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <sys/inotify.h>
#include <unistd.h>
#include <cstring>

namespace fs = std::filesystem;

namespace gw {

// ─── 工具：读取文件内容 ────────────────────────────────────────────
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ─── 工具：获取 Lua 全局函数 ───────────────────────────────────────
static sol::protected_function get_global_fn(sol::state& lua, const std::string& name) {
    sol::table globals = lua.globals();
    return globals.get_fn(name);
}

// ─── 初始化 ────────────────────────────────────────────────────────
void PluginManager::init(const std::string& plugins_dir,
                         size_t pool_size_per_thread) {
    plugins_dir_ = plugins_dir;
    pool_size_per_thread_ = pool_size_per_thread;

    if (!fs::exists(plugins_dir_)) {
        fs::create_directories(plugins_dir_);
        GW_LOG_WARN("Created plugins directory: " + plugins_dir_);
    }

    api_ = build_api();

    sandbox_cfg_.max_memory_kb   = 2048;
    sandbox_cfg_.max_instruction = 500000;
    sandbox_cfg_.enable_http     = false;

    scan_and_load();

    GW_LOG_INFO("PluginManager initialized: " +
             std::to_string(plugins_.size()) + " plugins loaded from " +
             plugins_dir_);
}

// ─── 构建 GatewayAPI ───────────────────────────────────────────────
GatewayAPI PluginManager::build_api() {
    GatewayAPI api;

    api.log = [](const std::string& level, const std::string& msg) {
        if (level == "ERROR")      { GW_LOG_ERROR("[lua] " + msg); }
        else if (level == "WARN")  { GW_LOG_WARN("[lua] " + msg); }
        else if (level == "DEBUG") { GW_LOG_DEBUG("[lua] " + msg); }
        else                       { GW_LOG_INFO("[lua] " + msg); }
    };

    api.metric_inc = [](const std::string& key, int64_t delta) {
        if (key == "total_requests") gw::Metrics::instance().total_requests.fetch_add(delta);
        else if (key == "errors")    gw::Metrics::instance().errors.fetch_add(delta);
        else if (key == "auth_failed") gw::Metrics::instance().auth_failed.fetch_add(delta);
    };

    api.metric_get = [](const std::string& key) -> int64_t {
        if (key == "total_requests") return gw::Metrics::instance().total_requests.load();
        if (key == "errors")         return gw::Metrics::instance().errors.load();
        if (key == "total_tokens")   return gw::Metrics::instance().total_tokens.load();
        return 0;
    };

    api.now_ms = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    api.http_post = nullptr;
    return api;
}

// ─── 扫描并加载所有插件 ────────────────────────────────────────────
void PluginManager::scan_and_load() {
    std::lock_guard<std::mutex> lk(plugins_mu_);
    plugins_.clear();

    if (!fs::exists(plugins_dir_)) return;

    for (const auto& entry : fs::directory_iterator(plugins_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".lua") continue;
        load_plugin(entry.path().string());
    }

    sort_plugins();
}

// ─── 加载单个插件 ──────────────────────────────────────────────────
void PluginManager::load_plugin(const std::string& filepath) {
    std::string body = read_file(filepath);
    if (body.empty()) {
        GW_LOG_WARN("Plugin file empty or unreadable: " + filepath);
        return;
    }

    sol::state temp;
    temp.open_libraries(
        sol::lib::base | sol::lib::string | sol::lib::table);

    try {
        auto result = temp.safe_script(body);
        if (!result.valid()) {
            sol::error err = result.get_error();
            GW_LOG_ERROR("Plugin syntax error [" + filepath + "]: " + err.what());
            return;
        }

        PluginRuntime rt;
        rt.info.filepath = filepath;
        rt.info.name     = temp.get_or("name",     std::string("unnamed"));
        rt.info.version  = temp.get_or("version",  std::string("0.0.0"));
        rt.info.priority = temp.get_or("priority", 0);
        rt.info.enabled  = true;
        rt.script_body   = std::move(body);

        // 检查 hook 存在性
        sol::table globals = temp.globals();
        auto check_fn = [&](const std::string& n) -> bool {
            return globals.get_fn(n).get_type() == sol::type::function;
        };
        rt.has_init       = check_fn("init");
        rt.has_on_request = check_fn("on_request");
        rt.has_on_token   = check_fn("on_token");
        rt.has_on_done    = check_fn("on_done");

        if (rt.has_init) {
            auto init_fn = globals.get_fn("init");
            auto r = init_fn();
            if (!r.valid()) {
                GW_LOG_WARN("Plugin init() failed [" + rt.info.name + "]: " +
                         std::string(r.get_error().what()));
            }
        }

        plugins_.push_back(std::move(rt));
        GW_LOG_INFO("Loaded plugin: " + plugins_.back().info.name +
                 " v" + plugins_.back().info.version +
                 " priority=" + std::to_string(plugins_.back().info.priority));

    } catch (const std::exception& e) {
        GW_LOG_ERROR("Plugin load error [" + filepath + "]: " + e.what());
    }
}

// ─── 卸载插件 ──────────────────────────────────────────────────────
void PluginManager::unload_plugin(const std::string& filepath) {
    std::lock_guard<std::mutex> lk(plugins_mu_);
    plugins_.erase(
        std::remove_if(plugins_.begin(), plugins_.end(),
            [&](const PluginRuntime& rt) { return rt.info.filepath == filepath; }),
        plugins_.end());
}

// ─── 按优先级排序（降序）───────────────────────────────────────────
void PluginManager::sort_plugins() {
    std::sort(plugins_.begin(), plugins_.end(),
        [](const PluginRuntime& a, const PluginRuntime& b) {
            return a.info.priority > b.info.priority;
        });
}

// ─── 为 worker 线程设置 thread_local 池 ────────────────────────────
void PluginManager::setup_thread_pool() {
    auto pool = std::make_shared<LuaVMPool>(
        pool_size_per_thread_, sandbox_cfg_, api_);
    set_thread_pool(std::move(pool));
}

// ─── 执行请求 Pipeline ────────────────────────────────────────────
PluginManager::PipelineResult
PluginManager::run_request_pipeline(const PluginContext& ctx) {
    PipelineResult result;
    result.passed = true;
    result.ctx    = ctx;

    std::vector<PluginRuntime> plugins_snapshot;
    {
        std::lock_guard<std::mutex> lk(plugins_mu_);
        plugins_snapshot = plugins_;
    }

    VMRent vm_rent = borrow_vm();
    sol::state& lua = *vm_rent;

    for (auto& plugin : plugins_snapshot) {
        if (!plugin.info.enabled || !plugin.has_on_request) continue;

        try {
            auto load_result = lua.safe_script(plugin.script_body);
            if (!load_result.valid()) {
                GW_LOG_WARN("Plugin script error [" + plugin.info.name + "]: " +
                         std::string(load_result.get_error().what()));
                continue;
            }

            auto on_request = lua.globals().get_fn("on_request");
            if (on_request.get_type() != sol::type::function) continue;

            // 构建 Lua ctx 表
            sol::table lua_ctx = sol::table::create(lua.lua_state());
            lua_ctx["model"]         = result.ctx.model;
            lua_ctx["messages_json"] = result.ctx.messages_json;
            lua_ctx["temperature"]   = result.ctx.temperature;
            lua_ctx["max_tokens"]    = result.ctx.max_tokens;
            lua_ctx["user_ip"]       = result.ctx.user_ip;
            lua_ctx["api_key"]       = result.ctx.api_key;
            lua_ctx["request_id"]    = result.ctx.request_id;

            // set_response 方法
            lua_ctx.set_function("set_response",
                [&result](int status, const std::string& body) {
                    result.passed       = false;
                    result.abort_status = status;
                    result.abort_body   = body;
                });

            // contains_sensitive
            lua_ctx.set_function("contains_sensitive",
                [&lua_ctx]() -> bool {
                    std::string msgs = lua_ctx.get_or("messages_json", std::string(""));
                    for (auto* kw : {"violence", "hack", "exploit", "bypass"}) {
                        if (msgs.find(kw) != std::string::npos) return true;
                    }
                    return false;
                });

            auto hook_result = on_request(lua_ctx);
            if (!hook_result.valid()) {
                sol::error err = hook_result.get_error();
                GW_LOG_WARN("on_request hook failed [" + plugin.info.name + "]: " + err.what());
                continue;
            }

            std::string ret_str = hook_result.get<std::string>();

            // 同步 Lua ctx 回 C++
            result.ctx.model         = lua_ctx.get_or("model",         result.ctx.model);
            result.ctx.messages_json = lua_ctx.get_or("messages_json", result.ctx.messages_json);
            result.ctx.temperature   = lua_ctx.get_or("temperature",   result.ctx.temperature);
            result.ctx.max_tokens    = lua_ctx.get_or("max_tokens",    result.ctx.max_tokens);

            if (ret_str == "ABORT_EARLY") {
                result.passed = false;
                GW_LOG_INFO("Plugin [" + plugin.info.name + "] ABORT_EARLY: status=" +
                         std::to_string(result.abort_status));
                return result;
            }

            if (ret_str == "MODIFY") {
                GW_LOG_DEBUG("Plugin [" + plugin.info.name + "] MODIFY applied");
            }

        } catch (const std::exception& e) {
            GW_LOG_ERROR("Plugin exception [" + plugin.info.name + "]: " + e.what());
        }
    }

    return result;
}

// ─── 执行 Token Pipeline ───────────────────────────────────────────
std::string PluginManager::run_token_pipeline(const std::string& token) {
    std::string current = token;

    std::vector<PluginRuntime> plugins_snapshot;
    {
        std::lock_guard<std::mutex> lk(plugins_mu_);
        plugins_snapshot = plugins_;
    }

    VMRent vm_rent = borrow_vm();
    sol::state& lua = *vm_rent;

    for (auto& plugin : plugins_snapshot) {
        if (!plugin.info.enabled || !plugin.has_on_token) continue;

        try {
            auto load_result = lua.safe_script(plugin.script_body);
            if (!load_result.valid()) continue;

            auto on_token = lua.globals().get_fn("on_token");
            if (on_token.get_type() != sol::type::function) continue;

            auto r = on_token(current);
            if (!r.valid()) continue;

            // 返回值可以是 string（修改）或 nil（抑制）
            if (r.get_type() == sol::type::string) {
                current = r.get<std::string>();
            } else if (r.get_type() == sol::type::nil || r.get_type() == sol::type::none) {
                return "";
            }
        } catch (...) {}
    }

    return current;
}

// ─── 执行 Done Pipeline ────────────────────────────────────────────
void PluginManager::run_done_pipeline() {
    std::vector<PluginRuntime> plugins_snapshot;
    {
        std::lock_guard<std::mutex> lk(plugins_mu_);
        plugins_snapshot = plugins_;
    }

    VMRent vm_rent = borrow_vm();
    sol::state& lua = *vm_rent;

    for (auto& plugin : plugins_snapshot) {
        if (!plugin.info.enabled || !plugin.has_on_done) continue;

        try {
            auto load_result = lua.safe_script(plugin.script_body);
            if (!load_result.valid()) continue;

            auto on_done = lua.globals().get_fn("on_done");
            if (on_done.get_type() == sol::type::function) {
                on_done();
            }
        } catch (...) {}
    }
}

// ─── 重新加载单个插件 ──────────────────────────────────────────────
bool PluginManager::reload_plugin(const std::string& filepath) {
    GW_LOG_INFO("Hot-reloading plugin: " + filepath);

    unload_plugin(filepath);
    load_plugin(filepath);
    sort_plugins();

    GW_LOG_INFO("Plugin reloaded: " + filepath);
    return true;
}

// ─── 列出所有插件 ──────────────────────────────────────────────────
std::vector<PluginInfo> PluginManager::list_plugins() const {
    std::lock_guard<std::mutex> lk(plugins_mu_);
    std::vector<PluginInfo> infos;
    infos.reserve(plugins_.size());
    for (auto& rt : plugins_) {
        infos.push_back(rt.info);
    }
    return infos;
}

// ─── 热重载监控线程 ────────────────────────────────────────────────
void PluginManager::start_hot_reload() {
    if (watching_.load()) return;

    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        GW_LOG_ERROR("inotify init failed, hot-reload disabled");
        return;
    }

    int wd = inotify_add_watch(inotify_fd_, plugins_dir_.c_str(),
                               IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) {
        GW_LOG_ERROR("inotify add watch failed for: " + plugins_dir_);
        close(inotify_fd_);
        inotify_fd_ = -1;
        return;
    }

    watching_.store(true);
    watch_thread_ = std::thread(&PluginManager::hot_reload_loop, this);
    GW_LOG_INFO("Hot-reload watcher started on: " + plugins_dir_);
}

void PluginManager::stop_hot_reload() {
    watching_.store(false);
    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }
    if (inotify_fd_ >= 0) {
        close(inotify_fd_);
        inotify_fd_ = -1;
    }
}

void PluginManager::hot_reload_loop() {
    constexpr size_t kBufSize = 4096;
    char buf[kBufSize];

    while (watching_.load()) {
        ssize_t n = read(inotify_fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            break;
        }

        ssize_t offset = 0;
        while (offset < n) {
            auto* event = reinterpret_cast<struct inotify_event*>(buf + offset);
            if (event->len > 0) {
                std::string filename(event->name);
                if (filename.size() > 4 &&
                    filename.substr(filename.size() - 4) == ".lua") {
                    std::string fullpath = plugins_dir_ + "/" + filename;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    reload_plugin(fullpath);
                }
            }
            offset += sizeof(struct inotify_event) + event->len;
        }
    }
}

void PluginManager::shutdown() {
    stop_hot_reload();
    std::lock_guard<std::mutex> lk(plugins_mu_);
    plugins_.clear();
    GW_LOG_INFO("PluginManager shutdown");
}

} // namespace gw
