#ifndef SOL_COMPAT_HPP
#define SOL_COMPAT_HPP
/**
 * sol_compat.hpp -- Minimal Lua 5.3 C++ binding (sol2 subset)
 * Self-contained. Only requires Lua 5.3 C headers.
 */

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include <string>
#include <functional>
#include <stdexcept>
#include <memory>
#include <vector>
#include <cstring>

namespace sol {

enum class type {
    none = LUA_TNONE, nil = LUA_TNIL, boolean = LUA_TBOOLEAN,
    lightuserdata = LUA_TLIGHTUSERDATA, number = LUA_TNUMBER,
    string = LUA_TSTRING, table = LUA_TTABLE, function = LUA_TFUNCTION,
    userdata = LUA_TUSERDATA, thread = LUA_TTHREAD,
};

inline type type_of(lua_State* L, int idx) { return static_cast<type>(lua_type(L, idx)); }
inline const char* type_name(type t) {
    switch (t) {
        case type::none: return "none"; case type::nil: return "nil";
        case type::boolean: return "boolean"; case type::number: return "number";
        case type::string: return "string"; case type::table: return "table";
        case type::function: return "function"; case type::userdata: return "userdata";
        case type::thread: return "thread";
    } return "unknown";
}

namespace lib {
    inline constexpr int base = 1, string = 2, math = 4, table = 8, coroutine = 16, utf8 = 32;
}

struct nil_t {};
static constexpr nil_t lua_nil{};

class error : public std::runtime_error {
public: explicit error(const std::string& msg) : std::runtime_error(msg) {}
};

// Forward
class table;
class protected_function;

// ─── state (Lua VM) ──────────────────────────────────────────────
class state {
public:
    state() : L_(luaL_newstate()) { if (!L_) throw std::runtime_error("lua_newstate failed"); }
    ~state() { if (L_) { lua_gc(L_, LUA_GCCOLLECT, 0); lua_close(L_); } }
    state(const state&) = delete; state& operator=(const state&) = delete;
    state(state&& o) noexcept : L_(o.L_) { o.L_ = nullptr; }

    lua_State* lua_state() { return L_; }

    void open_libraries(int libs) {
        if (libs & lib::base) { luaL_requiref(L_, "_G", luaopen_base, 1); lua_pop(L_, 1); }
        if (libs & lib::string) { luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L_, 1); }
        if (libs & lib::math) { luaL_requiref(L_, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L_, 1); }
        if (libs & lib::table) { luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L_, 1); }
        if (libs & lib::coroutine) { luaL_requiref(L_, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L_, 1); }
        if (libs & lib::utf8) { luaL_requiref(L_, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L_, 1); }
    }

    // safe_script: load + pcall, returns status
    struct safe_script_result {
        lua_State* L; int status;
        bool valid() const { return status == LUA_OK; }
        error get_error() const { return error(lua_tostring(L, -1)); }
    };

    safe_script_result safe_script(const std::string& code) {
        int s = luaL_loadstring(L_, code.c_str());
        if (s != LUA_OK) { error e(lua_tostring(L_, -1)); lua_pop(L_, 1); return {L_, s}; }
        s = lua_pcall(L_, 0, LUA_MULTRET, 0);
        return {L_, s};
    }

    // script (throw on error)
    void script(const std::string& code) {
        auto r = safe_script(code);
        if (!r.valid()) throw r.get_error();
    }

    table globals();

    table create_named_table(const std::string& name);

    template<typename T> T get_or(const std::string& name, T def) {
        return globals().get_or(name, def);
    }

    void set(const std::string& name, const std::string& val) {
        lua_pushlstring(L_, val.c_str(), val.size());
        lua_setglobal(L_, name.c_str());
    }
    void set(const std::string& name, int val) {
        lua_pushinteger(L_, val); lua_setglobal(L_, name.c_str());
    }

    void collect_garbage() { lua_gc(L_, LUA_GCCOLLECT, 0); }

private:
    lua_State* L_ = nullptr;
};

// ─── reference ──────────────────────────────────────────────────
class reference {
public:
    reference() = default;
    reference(lua_State* L, int idx) : L_(L) {
        if (L_ && idx != LUA_NOREF) {
            lua_pushvalue(L_, idx > 0 ? idx : lua_gettop(L_) + idx + 1);
            ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        }
    }
    ~reference() { unref(); }
    reference(const reference& o) : L_(o.L_) {
        if (L_ && o.ref_ != LUA_NOREF) { lua_rawgeti(L_, LUA_REGISTRYINDEX, o.ref_); ref_ = luaL_ref(L_, LUA_REGISTRYINDEX); }
    }
    reference(reference&& o) noexcept : L_(o.L_), ref_(o.ref_) { o.L_ = nullptr; o.ref_ = LUA_NOREF; }
    reference& operator=(const reference& o) {
        if (this != &o) { unref(); L_ = o.L_; push(); ref_ = luaL_ref(L_, LUA_REGISTRYINDEX); } return *this;
    }

    void push() const { if (L_ && ref_ != LUA_NOREF) lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_); }
    type get_type() const {
        if (!L_ || ref_ == LUA_NOREF) return type::none;
        push(); type t = type_of(L_, -1); lua_pop(L_, 1); return t;
    }
    lua_State* lua_state() const { return L_; }
    int ref_id() const { return ref_; }

protected:
    void unref() { if (L_ && ref_ != LUA_NOREF) { luaL_unref(L_, LUA_REGISTRYINDEX, ref_); ref_ = LUA_NOREF; } }
    lua_State* L_ = nullptr;
    int ref_ = LUA_NOREF;
};

// ─── table ──────────────────────────────────────────────────────
class table : public reference {
public:
    table() = default;
    table(lua_State* L, int idx) : reference(L, idx) {}

    static table create(lua_State* L) { lua_newtable(L); return table(L, lua_gettop(L)); }

    template<typename T> T get_or(const std::string& key, T def) const {
        push(); lua_getfield(L_, -1, key.c_str()); T r = def;
        if constexpr (std::is_same_v<T, std::string>) {
            if (lua_isstring(L_, -1)) { size_t len; const char* s = lua_tolstring(L_, -1, &len); r = std::string(s, len); }
        } else if constexpr (std::is_same_v<T, int>) {
            if (lua_isinteger(L_, -1)) r = (int)lua_tointeger(L_, -1);
            else if (lua_isnumber(L_, -1)) r = (int)lua_tonumber(L_, -1);
        } else if constexpr (std::is_same_v<T, double>) {
            if (lua_isnumber(L_, -1)) r = lua_tonumber(L_, -1);
        } else if constexpr (std::is_same_v<T, bool>) {
            r = lua_toboolean(L_, -1);
        }
        lua_pop(L_, 2); return r;
    }

    // operator[] returns a proxy that supports operator=
    struct table_proxy {
        table* t; std::string key;
        table_proxy& operator=(const std::string& val) {
            t->push(); lua_pushlstring(t->lua_state(), val.c_str(), val.size());
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(const char* val) {
            t->push(); lua_pushstring(t->lua_state(), val ? val : "");
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(int val) {
            t->push(); lua_pushinteger(t->lua_state(), val);
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(int64_t val) {
            t->push(); lua_pushinteger(t->lua_state(), val);
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(uint64_t val) {
            t->push(); lua_pushinteger(t->lua_state(), static_cast<lua_Integer>(val));
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(double val) {
            t->push(); lua_pushnumber(t->lua_state(), val);
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(bool val) {
            t->push(); lua_pushboolean(t->lua_state(), val);
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
        table_proxy& operator=(nil_t) {
            t->push(); lua_pushnil(t->lua_state());
            lua_setfield(t->lua_state(), -2, key.c_str()); lua_pop(t->lua_state(), 1); return *this;
        }
    };

    table_proxy operator[](const std::string& k) { return {this, k}; }

    template<typename V> void set_field(const std::string& key, V&& val) {
        push();
        push_value(L_, std::forward<V>(val));
        lua_setfield(L_, -2, key.c_str());
        lua_pop(L_, 1);
    }

    // set_function
    template<typename F> void set_function(const std::string& name, F&& fn);

    // get_fn: get a function from this table by name
    protected_function get_fn(const std::string& name);

    bool empty() const {
        push(); lua_pushnil(L_);
        bool has = lua_next(L_, -2);
        if (has) lua_pop(L_, 2); else lua_pop(L_, 1);
        return !has;
    }

    void for_each(std::function<void(const std::string&, type)> cb) const {
        push(); lua_pushnil(L_);
        while (lua_next(L_, -2)) {
            std::string k; if (lua_isstring(L_, -2)) k = lua_tostring(L_, -2);
            type vt = type_of(L_, -1); cb(k, vt); lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
    }

private:
    template<typename V> static void push_value(lua_State* L, V&& v) {
        if constexpr (std::is_same_v<std::decay_t<V>, std::string>) {
            lua_pushlstring(L, v.c_str(), v.size());
        } else if constexpr (std::is_same_v<std::decay_t<V>, const char*>) {
            lua_pushstring(L, v ? v : "");
        } else if constexpr (std::is_integral_v<std::decay_t<V>>) {
            lua_pushinteger(L, (lua_Integer)v);
        } else if constexpr (std::is_floating_point_v<std::decay_t<V>>) {
            lua_pushnumber(L, (lua_Number)v);
        } else if constexpr (std::is_same_v<std::decay_t<V>, bool>) {
            lua_pushboolean(L, v);
        } else if constexpr (std::is_same_v<std::decay_t<V>, nil_t>) {
            lua_pushnil(L);
        }
    }
};

// operator= for table setter - REMOVED (using table_proxy::operator= instead)

// ─── protected_function ─────────────────────────────────────────
class protected_function : public reference {
public:
    protected_function() = default;
    protected_function(lua_State* L, int idx) : reference(L, idx) {}

    struct function_result {
        lua_State* L; int nresults; bool is_err = false;
        function_result(lua_State* L_, int n) : L(L_), nresults(n) {}
        function_result(lua_State* L_, int n, bool e) : L(L_), nresults(n), is_err(e) {}
        bool valid() const { return nresults > 0 && !is_err; }
        type get_type() const { return nresults > 0 ? type_of(L, -1) : type::none; }
        error get_error() const { return nresults > 0 ? error(lua_tostring(L, -1)) : error("unknown"); }
        template<typename T> T get() const;
    };

    function_result operator()() const {
        push(); int err = lua_pcall(L_, 0, 1, 0);
        if (err) { error e(lua_tostring(L_, -1)); lua_pop(L_, 1); return {L_, 0, true}; }
        return {L_, 1};
    }

    function_result operator()(const std::string& arg) const {
        push(); lua_pushlstring(L_, arg.c_str(), arg.size());
        int err = lua_pcall(L_, 1, 1, 0);
        if (err) { error e(lua_tostring(L_, -1)); lua_pop(L_, 1); return {L_, 0, true}; }
        return {L_, 1};
    }

    function_result operator()(table& tbl) const {
        push(); tbl.push();
        int err = lua_pcall(L_, 1, 1, 0);
        if (err) { error e(lua_tostring(L_, -1)); lua_pop(L_, 1); return {L_, 0, true}; }
        return {L_, 1};
    }
};

template<> inline std::string protected_function::function_result::get<std::string>() const {
    if (nresults <= 0 || is_err) return "";
    size_t len; const char* s = lua_tolstring(L, -1, &len);
    std::string r(s, len); lua_pop(L, 1); return r;
}
template<> inline int protected_function::function_result::get<int>() const {
    if (nresults <= 0 || is_err) return 0;
    int r = (int)lua_tointeger(L, -1); lua_pop(L, 1); return r;
}

// ─── table member functions that need protected_function ─────────
inline protected_function table::get_fn(const std::string& name) {
    push(); lua_getfield(L_, -1, name.c_str());
    protected_function fn(L_, lua_gettop(L_));
    lua_pop(L_, 2); // pop table + function
    return fn;
}

// ─── state member functions that need table ─────────────────────
inline table state::globals() {
    lua_pushglobaltable(L_); return table(L_, lua_gettop(L_));
}
inline table state::create_named_table(const std::string& name) {
    lua_newtable(L_); lua_pushvalue(L_, -1); lua_setglobal(L_, name.c_str());
    return table(L_, lua_gettop(L_));
}

// ─── Wrapper for C++ closures ───────────────────────────────────
struct ClosureBase { virtual ~ClosureBase() = default; };
template<typename F> struct Closure : ClosureBase { F fn; Closure(F&& f) : fn(std::forward<F>(f)) {} };

// table::set_function implementation
template<typename F>
void table::set_function(const std::string& name, F&& fn) {
    push();
    auto* w = new Closure<std::decay_t<F>>(std::forward<F>(fn));

    // Determine the Lua C closure based on signature
    using Fn = std::decay_t<F>;
    lua_CFunction cfn = nullptr;

    if constexpr (std::is_invocable_r_v<void, Fn>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            cl->fn(); return 0;
        };
    } else if constexpr (std::is_invocable_r_v<std::string, Fn, int, const std::string&>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            int a = (int)luaL_checkinteger(L, 1);
            size_t len; const char* s = luaL_checklstring(L, 2, &len);
            std::string r = cl->fn(a, std::string(s, len));
            lua_pushlstring(L, r.c_str(), r.size()); return 1;
        };
    } else if constexpr (std::is_invocable_r_v<bool, Fn>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            lua_pushboolean(L, cl->fn()); return 1;
        };
    } else if constexpr (std::is_invocable_r_v<int64_t, Fn>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            lua_pushinteger(L, cl->fn()); return 1;
        };
    } else if constexpr (std::is_invocable_r_v<void, Fn, int, const std::string&>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            int a = (int)luaL_checkinteger(L, 1);
            size_t len; const char* s = luaL_checklstring(L, 2, &len);
            cl->fn(a, std::string(s, len)); return 0;
        };
    } else if constexpr (std::is_invocable_r_v<int64_t, Fn, const std::string&>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            size_t len; const char* s = luaL_checklstring(L, 1, &len);
            lua_pushinteger(L, cl->fn(std::string(s, len))); return 1;
        };
    } else if constexpr (std::is_invocable_r_v<int64_t, Fn, const std::string&, int64_t>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            size_t len; const char* s = luaL_checklstring(L, 1, &len);
            lua_Integer n = luaL_checkinteger(L, 2);
            lua_pushinteger(L, cl->fn(std::string(s, len), n)); return 1;
        };
    } else if constexpr (std::is_invocable_r_v<std::string, Fn, const std::string&>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            size_t len; const char* s = luaL_checklstring(L, 1, &len);
            std::string r = cl->fn(std::string(s, len));
            lua_pushlstring(L, r.c_str(), r.size()); return 1;
        };
    } else if constexpr (std::is_invocable_r_v<std::string, Fn, const std::string&, const std::string&>) {
        cfn = [](lua_State* L) -> int {
            auto* cl = (Closure<Fn>*)lua_touserdata(L, lua_upvalueindex(1));
            size_t l1, l2; const char* s1 = luaL_checklstring(L, 1, &l1);
            const char* s2 = luaL_checklstring(L, 2, &l2);
            std::string r = cl->fn(std::string(s1, l1), std::string(s2, l2));
            lua_pushlstring(L, r.c_str(), r.size()); return 1;
        };
    } else {
        cfn = [](lua_State*) -> int { return 0; };
    }

    // Push the closure with userdata as upvalue for GC
    lua_pushlightuserdata(L_, w);
    lua_pushcclosure(L_, cfn, 1);
    // Set __gc metamethod on the closure so w gets freed
    lua_createtable(L_, 0, 1);
    lua_pushlightuserdata(L_, w);
    lua_pushcclosure(L_, [](lua_State* L) -> int {
        delete (ClosureBase*)lua_touserdata(L, 1); return 0;
    }, 1);
    lua_setfield(L_, -2, "__gc");
    lua_setmetatable(L_, -2);

    lua_setfield(L_, -2, name.c_str());
    lua_pop(L_, 1);
}

} // namespace sol
#endif
