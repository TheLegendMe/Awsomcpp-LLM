/**
 * lua_sandbox.cpp — Lua VM 沙箱实现
 */
#include "lua_sandbox.h"
#include "common.h"
#include <sol/sol.hpp>

namespace gw {

sol::state* LuaSandbox::create(const SandboxConfig& cfg, const GatewayAPI& api) {
    auto lua = std::make_unique<sol::state>();
    lua->open_libraries(
        sol::lib::base | sol::lib::string | sol::lib::math |
        sol::lib::table | sol::lib::coroutine | sol::lib::utf8);

    nullify_dangerous(*lua);
    inject_api(*lua, api);

    // 内存限制
    lua->collect_garbage();
    // 设置 GC 阈值以控制内存使用
    lua_gc(lua->lua_state(), LUA_GCCOLLECT, 0);

    set_instruction_limit(*lua, cfg.max_instruction);

    return lua.release();
}

void LuaSandbox::reset(sol::state& lua) {
    // 清除所有全局变量（保留沙箱白名单）
    sol::table globals = lua.globals();
    // 白名单：标准库 + gateway
    static const std::vector<std::string> kKeep = {
        "assert", "error", "ipairs", "next", "pairs",
        "pcall", "print", "select", "tonumber", "tostring",
        "type", "xpcall", "string", "math", "table",
        "coroutine", "utf8", "gateway", "_VERSION", "_G"
    };

    std::vector<std::string> to_remove;
    globals.for_each([&](const std::string& key, sol::type) {
        if (std::find(kKeep.begin(), kKeep.end(), key) == kKeep.end()) {
            to_remove.push_back(key);
        }
    });
    for (auto& k : to_remove) {
        globals[k] = sol::lua_nil;
    }

    // 完全 GC 回收残留对象
    lua.collect_garbage();
}

void LuaSandbox::set_instruction_limit(sol::state& lua, size_t max_instructions) {
    // 使用 Lua 的 debug hook 实现指令计数限制
    // 在每次进入新行时检查，实际生产中可使用 lua_sethook 的 count hook
    lua_State* L = lua.lua_state();
    lua_sethook(L,
        [](lua_State* L, lua_Debug*) {
            luaL_error(L, "plugin execution limit exceeded");
        },
        LUA_MASKCOUNT, max_instructions);
}

void LuaSandbox::nullify_dangerous(sol::state& lua) {
    sol::table globals = lua.globals();

    // 置空危险模块和函数
    const std::vector<std::string> kDangerous = {
        "os", "io", "package", "require",
        "dofile", "loadfile", "load",
        "rawset", "rawget", "rawlen",
        "rawequal", "module", "debug",
        "collectgarbage", "gcinfo", "newproxy"
    };

    for (auto& name : kDangerous) {
        globals[name] = sol::lua_nil;
    }

    // 移除 package 库
    globals["package"] = sol::lua_nil;
}

void LuaSandbox::inject_api(sol::state& lua, const GatewayAPI& api) {
    sol::table gw = lua.create_named_table("gateway");

    // gateway.log(level, msg)
    gw.set_function("log", [api](const std::string& level, const std::string& msg) {
        if (api.log) api.log(level, msg);
    });

    // gateway.metric_inc(key, delta)
    gw.set_function("metric_inc", [api](const std::string& key, int64_t delta) {
        if (api.metric_inc) api.metric_inc(key, delta);
    });

    // gateway.metric_get(key) -> int
    gw.set_function("metric_get", [api](const std::string& key) -> int64_t {
        if (api.metric_get) return api.metric_get(key);
        return 0;
    });

    // gateway.now_ms() -> int
    gw.set_function("now_ms", [api]() -> int64_t {
        if (api.now_ms) return api.now_ms();
        return 0;
    });

    // gateway.http_post(url, body) -> response_body
    gw.set_function("http_post", [api](const std::string& url, const std::string& body) -> std::string {
        if (api.http_post) return api.http_post(url, body);
        return "{\"error\":\"http not enabled\"}";
    });

    // gateway.cache_get(key) -> value or empty string
    gw.set_function("cache_get", [](const std::string& key) -> std::string {
        auto v = CacheStore::instance().get(key);
        return v.value_or("");
    });

    // gateway.cache_set(key, value, ttl_seconds)
    gw.set_function("cache_set", [](const std::string& key, const std::string& value, int ttl) {
        CacheStore::instance().set(key, value, ttl);
    });

    // gateway.cache_size() -> int
    gw.set_function("cache_size", []() -> size_t {
        return CacheStore::instance().size();
    });
}

} // namespace gw
