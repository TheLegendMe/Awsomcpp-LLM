#pragma once
/**
 * agent_cache.h — 三级缓存 + 会话缓冲区 + 协调器
 *   L1: 本地 LRU (tool 结果, 小数据, 短 TTL)
 *   SessionBuffer: 同会话 Agent 接力上下文 (内存, 会话级)
 *   L2: Redis-like 持久化 (Agent 结果摘要, 置信度衰减, 级联失效)
 *   L3: S3/MinIO (完整推理轨迹, 归档/审计)
 */
#include "agent_types.h"
#include <string>
#include <unordered_map>
#include <list>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <functional>
#include <optional>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace gw {

// 全局置信度阈值
constexpr double kConfidenceFresh = 0.7;
constexpr double kConfidenceStale = 0.3;

// ═══════════════════════════════════════════════════════════════════════
// L1 — 本地 LRU 缓存 (enhanced CacheStore)
// ═══════════════════════════════════════════════════════════════════════
class L1CacheStore {
public:
    static L1CacheStore& instance() { static L1CacheStore c; return c; }

    std::optional<CacheEntry> get(const std::string& key);
    void set(const std::string& key, const CacheEntry& entry);
    void invalidate(const std::string& key);
    size_t size() const;
    void clear();
    void set_max_size(size_t sz);

private:
    L1CacheStore() = default;
    mutable std::mutex mu_;
    struct Node { std::string key; CacheEntry entry; };
    std::list<Node> lru_;
    std::unordered_map<std::string, std::list<Node>::iterator> map_;
    size_t max_size_ = 4096;
};

// ═══════════════════════════════════════════════════════════════════════
// SessionBuffer — 同会话 Agent 接力上下文
// ═══════════════════════════════════════════════════════════════════════
class SessionBuffer {
public:
    explicit SessionBuffer(const std::string& session_id)
        : session_id_(session_id) {}

    // 追加一轮 Agent 交互结果
    void append(const SessionTurn& turn);

    // 获取最近 N 轮某个 capability 的输出（按时间倒序）
    std::vector<SessionTurn> recent(const std::string& capability, int n = 3) const;

    // 获取最近 N 轮所有输出
    std::vector<SessionTurn> recent_all(int n = 5) const;

    // 构建给下游 Agent 的精简上下文（token 预算受限）
    std::string build_context_for(const std::string& capability,
                                  size_t max_tokens = 1000) const;

    // 摘要当前 buffer 状态
    size_t turn_count() const { return turns_.size(); }
    size_t estimated_tokens() const;
    bool empty() const { return turns_.empty(); }

    // 序列化到 JSON（会话结束时迁入 L2）
    json to_json() const;

private:
    std::string session_id_;
    std::deque<SessionTurn> turns_;
    static constexpr size_t kMaxTurns = 50;
    static constexpr size_t kMaxTotalTokens = 50000;

    void prune();
};

// ═══════════════════════════════════════════════════════════════════════
// L2 — 持久化缓存 (Redis-compatible)
//   当前实现: JSON 文件存储（单机 first），后续替换为 Redis 网络存储
//   支持: 置信度衰减, 级联失效, 反向依赖索引
// ═══════════════════════════════════════════════════════════════════════
class L2PersistentCache {
public:
    static L2PersistentCache& instance() {
        static L2PersistentCache c;
        return c;
    }

    // 初始化：指定存储目录
    void init(const std::string& storage_dir);
    void shutdown();

    // CRUD
    std::optional<CacheEntry> get(const std::string& key);
    void set(const std::string& key, const CacheEntry& entry);
    void remove(const std::string& key);

    // 软失效 (降低置信度，不删除)
    void soft_invalidate(const std::string& key, const std::string& reason);

    // 硬失效 (级联标记所有依赖方)
    void hard_invalidate(const std::string& key, const std::string& reason,
                         bool cascade = true);

    // 置信度衰减 (后台定时调用)
    void apply_confidence_decay();

    // 反向依赖查询
    std::vector<std::string> dependents_of(const std::string& key) const;

    // 清理过期条目
    void purge_expired();

    // 统计
    size_t size() const;

private:
    L2PersistentCache() = default;
    ~L2PersistentCache() { shutdown(); }

    std::string storage_dir_;
    std::string data_file_;     // JSON lines
    std::string revidx_file_;   // 反向依赖索引 "dep_key\n"

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, CacheEntry> entries_;
    // 反向索引: 被依赖的 key → 依赖它的 key 集合
    std::unordered_map<std::string, std::vector<std::string>> dep_rev_;

    void load_from_disk();
    void save_to_disk() const;
    void save_revidx() const;

    std::string key_to_path(const std::string& key) const;
    void cascade_invalidate_internal(const std::string& key,
                                      const std::string& reason,
                                      std::vector<std::string>& visited);
};

// ═══════════════════════════════════════════════════════════════════════
// L3 — 归档存储 (S3/MinIO 兼容)
//   当前: 本地文件系统 + 目录分区
//   用途: 完整推理轨迹, 只在 debugging/审计/恢复时读取
// ═══════════════════════════════════════════════════════════════════════
class L3ArchiveStore {
public:
    static L3ArchiveStore& instance() { static L3ArchiveStore s; return s; }

    void init(const std::string& base_dir);
    void shutdown();

    // 写入完整 Agent 推理轨迹
    // key: 与 L2 相同的 cache key
    // traces_json: sub-agent 的完整对话轮次 + tool 调用链
    std::string put(const std::string& key, const std::string& traces_json);

    // 读取
    std::optional<std::string> get(const std::string& ref);

    // 仅追加（审计）
    void append_audit_log(const std::string& key, const std::string& event);

    // 按日期范围列举
    std::vector<std::string> list_by_date(int64_t start_ms, int64_t end_ms);

private:
    L3ArchiveStore() = default;
    std::string base_dir_;
    std::mutex mu_;

    std::string ref_to_path(const std::string& ref) const;
    std::string make_key_path(const std::string& key, int64_t ts_ms) const;
};

// ═══════════════════════════════════════════════════════════════════════
// CacheCoordinator — 三级缓存协调器
// ═══════════════════════════════════════════════════════════════════════
class CacheCoordinator {
public:
    static CacheCoordinator& instance() {
        static CacheCoordinator cc;
        return cc;
    }

    void init(const std::string& l2_dir, const std::string& l3_dir);
    void shutdown();

    // ─── 查询: L1 → SessionBuffer → L2 → 执行调用 ────────
    struct QueryResult {
        CacheEntry  entry;
        bool        hit = false;
        CacheLayer  layer;
        bool        stale = false;  // confidence 衰减到阈值以下
    };
    QueryResult query(const std::string& agent_id,
                      const std::string& capability,
                      const std::string& input_hash,
                      const std::string& session_id);

    // ─── 写入: 三层同时写 ─────────────────────────────────
    void write(const CacheEntry& entry);

    // ─── 会话缓冲区管理 ───────────────────────────────────
    SessionBuffer& session_buffer(const std::string& session_id);
    void close_session(const std::string& session_id);
    // 会话关闭时: L2 存摘要, L3 存完整上下文, 释放 SessionBuffer
    std::string archive_session(const std::string& session_id);

    // ─── 失效 ────────────────────────────────────────────
    void invalidate(const std::string& key, const std::string& reason,
                    bool cascade = true);

    // ─── 上下文扩展 (按需拉取完整推理) ────────────────────
    std::optional<ContextExpandResult> expand_context(const std::string& ref,
                                                       size_t max_tokens);

    // ─── 后台维护 ─────────────────────────────────────────
    void background_maintenance();

private:
    CacheCoordinator() = default;

    // 会话缓冲区
    mutable std::shared_mutex sb_mu_;
    std::unordered_map<std::string, std::unique_ptr<SessionBuffer>> sessions_;
};

// ═══════════════════════════════════════════════════════════════════════
// AgentRegistry — Agent 能力注册和路由
// ═══════════════════════════════════════════════════════════════════════
class AgentRegistry {
public:
    static AgentRegistry& instance() { static AgentRegistry r; return r; }

    // 注册 Agent
    void register_agent(const std::string& session_id,
                        const AgentRecord& record);
    void unregister_agent(const std::string& session_id);

    // 按 capability 路由: 找最匹配的 Agent
    std::optional<AgentRecord> route(const std::string& capability) const;

    // 按 session_id 查找
    std::optional<AgentRecord> find(const std::string& session_id) const;

    // 心跳
    void heartbeat(const std::string& session_id);

    // 列出所有注册的 capability
    std::vector<AgentCapability> all_capabilities() const;

private:
    AgentRegistry() = default;
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, AgentRecord> agents_;     // session_id → record
    std::unordered_map<std::string, std::vector<std::string>> cap_index_; // cap → session_ids
};

} // namespace gw
