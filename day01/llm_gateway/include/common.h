#pragma once
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <list>
#include <optional>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace gw {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };
enum class Provider { KIMI, OPENAI, ANTHROPIC };

struct LLMRequest {
    uint64_t    id         = 0;
    Provider    provider   = Provider::KIMI;
    std::string model;
    std::string messages_json;
    std::string tools_json;
    double      temperature = 0.7;
    int         max_tokens  = 2048;
    std::function<void(const std::string& token)>      on_token;
    std::function<void(const std::string& call_id,
                       const std::string& name,
                       const std::string& arguments)>  on_tool_call;
    std::function<void()>                              on_done;
    std::function<void(const std::string& err)>        on_error;
};

struct RateLimitConfig {
    double capacity   = 60.0;
    double refill_rps = 10.0;
};

struct BackendNode {
    std::string  name;
    std::string  url;
    std::string  api_key;
    Provider     provider     = Provider::KIMI;
    std::string  model;
    int          weight       = 1;
    std::atomic<int> active_conns{0};
    std::atomic<bool> healthy{true};

    BackendNode() = default;
    BackendNode(const BackendNode& o)
        : name(o.name), url(o.url), api_key(o.api_key)
        , provider(o.provider), model(o.model), weight(o.weight)
    {
        active_conns.store(o.active_conns.load());
        healthy.store(o.healthy.load());
    }
};

// ─── 工具注册表 ──────────────────────────────────────────────
struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters_json;
};

class ToolRegistry {
public:
    static ToolRegistry& instance() { static ToolRegistry r; return r; }
    void add(const std::string& name, const std::string& description,
             const std::string& parameters_json, const std::string& source = "");
    void remove_source(const std::string& source);
    std::string tools_json() const;
    std::vector<std::string> names() const;
private:
    ToolRegistry() = default;
    mutable std::mutex mu_;
    struct Entry { ToolDef def; std::string source; };
    std::vector<Entry> entries_;
};

// ─── LRU 内存缓存 ──────────────────────────────────────────
class CacheStore {
public:
    static CacheStore& instance() { static CacheStore c; return c; }
    std::optional<std::string> get(const std::string& key);
    void set(const std::string& key, const std::string& value, int ttl_seconds);
    size_t size() const;
    void clear();
private:
    CacheStore() = default;
    mutable std::mutex mu_;
    struct Entry { std::string key, value; std::chrono::steady_clock::time_point expires; };
    std::list<Entry> lru_;
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    size_t max_size_ = 10000;
};

// ═══════ inline 实现 ═══════════════════════════════════════

inline void ToolRegistry::add(const std::string& name, const std::string& description,
                              const std::string& parameters_json, const std::string& source) {
    std::lock_guard<std::mutex> lk(mu_);
    auto src = source.empty() ? "static" : source;
    for (auto& e : entries_) {
        if (e.def.name == name && e.source == src) {
            e.def.description = description;
            e.def.parameters_json = parameters_json;
            return;
        }
    }
    entries_.push_back({{name, description, parameters_json}, src});
}

inline void ToolRegistry::remove_source(const std::string& source) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const Entry& e) { return e.source == source; }), entries_.end());
}

inline std::string ToolRegistry::tools_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& e : entries_) {
        // 解析并清理 parameters schema
        nlohmann::json params;
        try {
            params = nlohmann::json::parse(
                e.def.parameters_json.empty() ? "{}" : e.def.parameters_json);
        } catch (...) { params = nlohmann::json::object(); }
        if (!params.is_object()) params = nlohmann::json::object();
        if (!params.contains("type") || params["type"] != "object")
            params["type"] = "object";
        if (!params.contains("properties") || !params["properties"].is_object())
            params["properties"] = nlohmann::json::object();
        if (!params.contains("required") || !params["required"].is_array())
            params["required"] = nlohmann::json::array();

        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", e.def.name},
                {"description", e.def.description},
                {"parameters", params}
            }}
        });
    }
    return arr.dump();
}

inline std::vector<std::string> ToolRegistry::names() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> ns;
    for (auto& e : entries_) ns.push_back(e.def.name);
    return ns;
}

inline std::optional<std::string> CacheStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    auto& entry = *(it->second);
    if (std::chrono::steady_clock::now() > entry.expires) {
        lru_.erase(it->second);
        map_.erase(it);
        return std::nullopt;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    return entry.value;
}

inline void CacheStore::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        it->second->value = value;
        it->second->expires = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    if (lru_.size() >= max_size_) { map_.erase(lru_.back().key); lru_.pop_back(); }
    lru_.push_front({key, value, std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds)});
    map_[key] = lru_.begin();
}

inline size_t CacheStore::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

inline void CacheStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lru_.clear(); map_.clear();
}

} // namespace gw
