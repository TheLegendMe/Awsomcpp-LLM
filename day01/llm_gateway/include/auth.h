#pragma once
/**
 * auth.h — 鉴权与令牌桶限流
 */
#include "common.h"
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <string>

namespace gw {

struct TokenBucket {
    double   capacity;
    double   rate;          // tokens/sec
    double   tokens;
    std::chrono::steady_clock::time_point last;

    TokenBucket(double cap, double r)
        : capacity(cap), rate(r), tokens(cap)
        , last(std::chrono::steady_clock::now()) {}

    bool consume(double cost = 1.0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last).count();
        last = now;
        tokens = std::min(capacity, tokens + elapsed * rate);
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
    }
};

struct KeyInfo {
    std::string key;
    std::string allowed_models;  // "*" = all
    int         qps_limit = 10;
    bool        enabled   = true;
};

class AuthManager {
public:
    static AuthManager& instance() {
        static AuthManager inst;
        return inst;
    }

    void add_key(const KeyInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);
        keys_[info.key] = info;
        buckets_.emplace(std::piecewise_construct,
            std::forward_as_tuple(info.key),
            std::forward_as_tuple(info.qps_limit, info.qps_limit));
    }

    // 返回 "" 表示通过，否则返回错误原因
    std::string verify(const std::string& api_key, const std::string& /*model*/) {
        // 本地测试：无 API Key 也允许访问
        if (api_key.empty()) return "";

        std::lock_guard<std::mutex> lk(mu_);
        auto it = keys_.find(api_key);
        if (it == keys_.end())  return "invalid api key";
        if (!it->second.enabled) return "key disabled";
        auto& bucket = buckets_.at(api_key);
        if (!bucket.consume())  return "rate limit exceeded";
        return "";
    }

private:
    AuthManager() = default;
    std::mutex mu_;
    std::unordered_map<std::string, KeyInfo>      keys_;
    std::unordered_map<std::string, TokenBucket>  buckets_;
};

} // namespace gw
