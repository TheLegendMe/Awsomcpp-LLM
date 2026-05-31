#pragma once
/**
 * agent_types.h — 多 Agent 系统的数据结构和消息类型
 */
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace gw {

using json = nlohmann::json;

// ─── Agent 能力注册 ───────────────────────────────────────────────────
struct AgentCapability {
    std::string name;               // "summarize", "search", "file_ops"
    std::string description;
    std::string input_schema_json;  // JSON Schema，描述期望的输入格式
    std::string output_schema_json; // JSON Schema，描述输出格式
    double      avg_cost_ms = 0;    // 平均耗时，用于调度决策
    int         weight = 1;         // 同类 capability 多个 executor 时的加权
};

// ─── 缓存条目元数据 ────────────────────────────────────────────────────
enum class CacheLayer { L1, L2, L3 };
enum class CacheStatus { FRESH, STALE, INVALIDATED };

struct CacheEntry {
    std::string key;                // "agent:{name}:{cap}:{input_hash}"
    std::string agent_id;
    std::string capability;
    std::string input_hash;         // sha256(input_payload)
    std::string input_summary;      // 前 200 chars，用于会话缓冲区快速浏览
    std::string output;             // 完整输出（L1/L2）
    std::string output_summary;     // 结构化摘要
    std::string full_ref;           // L3 引用路径: "s3://bucket/key"

    // 失效 / 置信度
    double      confidence = 0.0;   // Agent 自评，0~1
    double      confidence_decay = 0.02; // 每天衰减率
    int64_t     created_at_ms = 0;
    int         base_ttl_seconds = 300;
    CacheStatus status = CacheStatus::FRESH;

    // 依赖链
    std::vector<std::string> depends_on;       // 依赖的其他缓存 key
    std::vector<std::string> source_urls;      // 依赖的外部数据源
    std::string              invalid_reason;   // 级联失效时的原因

    // 淘汰评分: 综合热度、新鲜度、置信度
    int         access_count = 0;
    double      priority = 1.0;

    double eviction_score(int64_t now_ms) const;

    json to_json() const;
    static CacheEntry from_json(const json& j);

    // SessionBuffer 使用的轻量版
    json to_summary() const;
};

// ─── 会话上下文缓冲区条目 ──────────────────────────────────────────────
struct SessionTurn {
    std::string call_id;
    std::string agent_id;
    std::string capability;
    std::string input_summary;    // 200 chars
    std::string output_summary;   // 200 chars
    std::string cache_key;        // L1/L2 引用
    int64_t     timestamp_ms = 0;
    double      confidence = 0.0;
};

// ─── Agent 间调用请求 ──────────────────────────────────────────────────
struct AgentCallRequest {
    std::string call_id;
    std::string caller_session_id;
    std::string callee_agent_id;   // 空 = 按 capability 自动匹配
    std::string capability;        // 需要的能力
    std::string input_payload;     // JSON 字符串
    std::string context_ref;       // L2/L3 引用，Agent 可扩展
    int         timeout_ms = 30000;
    bool        bypass_cache = false; // 强制重新执行，不走缓存
};

struct AgentCallResult {
    std::string call_id;
    std::string agent_id;          // 实际执行的 agent
    std::string output_payload;    // JSON 字符串
    double      confidence = 0.0;
    CacheStatus cache_status = CacheStatus::FRESH; // FRESH / STALE
    int64_t     duration_ms = 0;
};

// ─── 缓存失效请求 ──────────────────────────────────────────────────────
struct CacheInvalidateRequest {
    std::string key;               // 要失效的 key
    std::string reason;
    bool        cascade = true;    // 级联传播到依赖方
};

// ─── 上下文扩展请求 ───────────────────────────────────────────────────
struct ContextExpandRequest {
    std::string call_id;
    std::string ref;               // "mem://session/key" 或 "s3://..."
    size_t      max_tokens = 4096;  // 最多返回这么多 token
};

struct ContextExpandResult {
    std::string call_id;
    std::string ref;
    std::string full_text;         // 完整内容
    size_t      total_tokens = 0;
    bool        truncated = false; // 是否被 max_tokens 截断
};

// ─── Agent 注册表条目 ──────────────────────────────────────────────────
struct AgentRecord {
    std::string                session_id;
    std::string                agent_name;        // "ResearchAgent-v2"
    std::vector<AgentCapability> capabilities;
    std::string                peer_addr;         // IP:port
    int64_t                    registered_at_ms;
    int64_t                    last_heartbeat_ms;
    int                        active_calls = 0;   // 当前正在处理的调用数
    bool                       healthy = true;
};

// ═══════ inline 实现 ═══════════════════════════════════════════════════

inline double CacheEntry::eviction_score(int64_t now_ms) const {
    if (status == CacheStatus::INVALIDATED) return -1.0;  // 立即淘汰
    auto age_s = (now_ms - created_at_ms) / 1000.0;
    auto age_penalty = age_s / (base_ttl_seconds * 1.0);
    return (priority + access_count * 0.1) * confidence / (1.0 + age_penalty);
}

inline json CacheEntry::to_json() const {
    return {
        {"key", key},
        {"agent_id", agent_id},
        {"capability", capability},
        {"input_hash", input_hash},
        {"input_summary", input_summary},
        {"output", output},
        {"output_summary", output_summary},
        {"full_ref", full_ref},
        {"confidence", confidence},
        {"confidence_decay", confidence_decay},
        {"created_at_ms", created_at_ms},
        {"base_ttl_seconds", base_ttl_seconds},
        {"status", static_cast<int>(status)},
        {"depends_on", depends_on},
        {"source_urls", source_urls},
        {"invalid_reason", invalid_reason},
        {"access_count", access_count},
        {"priority", priority}
    };
}

inline CacheEntry CacheEntry::from_json(const json& j) {
    CacheEntry e;
    e.key = j.value("key", "");
    e.agent_id = j.value("agent_id", "");
    e.capability = j.value("capability", "");
    e.input_hash = j.value("input_hash", "");
    e.input_summary = j.value("input_summary", "");
    e.output = j.value("output", "");
    e.output_summary = j.value("output_summary", "");
    e.full_ref = j.value("full_ref", "");
    e.confidence = j.value("confidence", 0.0);
    e.confidence_decay = j.value("confidence_decay", 0.02);
    e.created_at_ms = j.value("created_at_ms", 0L);
    e.base_ttl_seconds = j.value("base_ttl_seconds", 300);
    e.status = static_cast<CacheStatus>(j.value("status", 0));
    if (j.contains("depends_on")) for (auto& d : j["depends_on"]) e.depends_on.push_back(d.get<std::string>());
    if (j.contains("source_urls")) for (auto& s : j["source_urls"]) e.source_urls.push_back(s.get<std::string>());
    e.invalid_reason = j.value("invalid_reason", "");
    e.access_count = j.value("access_count", 0);
    e.priority = j.value("priority", 1.0);
    return e;
}

inline json CacheEntry::to_summary() const {
    return {
        {"key", key},
        {"agent_id", agent_id},
        {"capability", capability},
        {"output_summary", output_summary},
        {"confidence", confidence},
        {"status", static_cast<int>(status)}
    };
}

} // namespace gw
