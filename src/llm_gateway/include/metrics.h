#pragma once
/**
 * metrics.h — 轻量监控指标（原子计数器 + 延迟统计）
 *   GET /metrics 返回
 */
#include <atomic>
#include <string>
#include <chrono>
#include <sstream>
#include <cmath>

namespace gw {

struct Metrics {
    // 请求计数
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> active_requests{0};
    std::atomic<uint64_t> total_tokens{0};
    std::atomic<uint64_t> success_count{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> rate_limited{0};
    std::atomic<uint64_t> auth_failed{0};

    // 延迟 (微秒，原子累加)
    std::atomic<uint64_t> total_latency_us{0};       // 请求总耗时
    std::atomic<uint64_t> total_first_token_us{0};   // 首 token 总耗时
    std::atomic<uint64_t> first_token_count{0};      // 首 token 统计次数

    // 极值 (近似，非精确)
    std::atomic<uint64_t> max_latency_us{0};

    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    std::string snapshot() const {
        auto now   = std::chrono::steady_clock::now().time_since_epoch().count();
        auto total = total_requests.load();
        auto succ  = success_count.load();
        auto err   = errors.load();
        auto toks  = total_tokens.load();
        auto lat   = total_latency_us.load();
        auto ft_lat= total_first_token_us.load();
        auto ft_cnt= first_token_count.load();

        double avg_ms = succ > 0 ? (lat / 1000.0 / succ) : 0;
        double avg_ft_ms = ft_cnt > 0 ? (ft_lat / 1000.0 / ft_cnt) : 0;
        double max_ms = max_latency_us.load() / 1000.0;

        std::ostringstream ss;
        ss << "total_requests="   << total
           << " active="          << active_requests.load()
           << " success="         << succ
           << " errors="          << err
           << " total_tokens="    << toks
           << " rate_limited="    << rate_limited.load()
           << " auth_failed="     << auth_failed.load()
           << "\navg_latency_ms=" << std::round(avg_ms * 100) / 100
           << " avg_first_token_ms=" << std::round(avg_ft_ms * 100) / 100
           << " max_latency_ms="  << std::round(max_ms * 100) / 100;
        return ss.str();
    }

private:
    Metrics() = default;
};

} // namespace gw

#define METRIC_INC(field)        gw::Metrics::instance().field.fetch_add(1)
#define METRIC_DEC(field)        gw::Metrics::instance().field.fetch_sub(1)
#define METRIC_ADD(field, val)   gw::Metrics::instance().field.fetch_add(val)
#define METRIC_MAX(field, val)   do { \
    auto& _f = gw::Metrics::instance().field; \
    auto _cur = _f.load(); \
    while (val > _cur && !_f.compare_exchange_weak(_cur, val)); \
} while(0)
