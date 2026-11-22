/**
 * agent_cache.cpp — 三级缓存 + 会话缓冲区 + 协调器实现
 */
#include "agent_cache.h"
#include "logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <thread>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace gw {

// ═══════════════════════════════════════════════════════════════════════
// L1 — 本地 LRU
// ═══════════════════════════════════════════════════════════════════════

std::optional<CacheEntry> L1CacheStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;

    auto& node = *(it->second);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // TTL 检查
    if (now > node.entry.created_at_ms + node.entry.base_ttl_seconds * 1000L) {
        lru_.erase(it->second);
        map_.erase(it);
        return std::nullopt;
    }

    // 命中 → 提升到 LRU 头部 + 增加访问计数
    node.entry.access_count++;
    lru_.splice(lru_.begin(), lru_, it->second);
    return node.entry;
}

void L1CacheStore::set(const std::string& key, const CacheEntry& entry) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        it->second->entry = entry;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    // 淘汰
    while (lru_.size() >= max_size_) {
        auto& back = lru_.back();
        map_.erase(back.key);
        lru_.pop_back();
    }

    lru_.push_front({key, entry});
    map_[key] = lru_.begin();
}

void L1CacheStore::invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        lru_.erase(it->second);
        map_.erase(it);
    }
}

size_t L1CacheStore::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

void L1CacheStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lru_.clear();
    map_.clear();
}

void L1CacheStore::set_max_size(size_t sz) {
    std::lock_guard<std::mutex> lk(mu_);
    max_size_ = sz;
    while (lru_.size() > max_size_) {
        auto& back = lru_.back();
        map_.erase(back.key);
        lru_.pop_back();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// SessionBuffer
// ═══════════════════════════════════════════════════════════════════════

void SessionBuffer::append(const SessionTurn& turn) {
    turns_.push_back(turn);
    prune();
}

void SessionBuffer::prune() {
    // 按轮次上限裁剪
    while (turns_.size() > kMaxTurns)
        turns_.pop_front();

    // 按 token 估算裁剪
    while (estimated_tokens() > kMaxTotalTokens && turns_.size() > 3)
        turns_.pop_front();
}

std::vector<SessionTurn> SessionBuffer::recent(const std::string& capability,
                                                int n) const {
    std::vector<SessionTurn> result;
    for (auto it = turns_.rbegin(); it != turns_.rend(); ++it) {
        if (it->capability == capability) {
            result.push_back(*it);
            if ((int)result.size() >= n) break;
        }
    }
    return result;
}

std::vector<SessionTurn> SessionBuffer::recent_all(int n) const {
    std::vector<SessionTurn> result;
    auto start = turns_.size() > (size_t)n ? turns_.end() - n : turns_.begin();
    for (auto it = start; it != turns_.end(); ++it)
        result.push_back(*it);
    return result;
}

std::string SessionBuffer::build_context_for(const std::string& capability,
                                              size_t max_tokens) const {
    // 按 1 token ≈ 4 chars 估算
    size_t budget = max_tokens * 4;
    std::string ctx;

    // 先加同 capability 的最近摘要
    auto same_cap = recent(capability, 3);
    for (auto& t : same_cap) {
        std::string line = "[Agent " + t.agent_id + " → " + t.capability
                         + "] " + t.output_summary + "\n";
        if (ctx.size() + line.size() > budget) break;
        ctx += line;
    }

    // 再加最近的所有输出
    auto all = recent_all(5);
    for (auto& t : all) {
        if (t.capability == capability) continue; // 已加
        std::string line = "[Agent " + t.agent_id + "] " + t.output_summary + "\n";
        if (ctx.size() + line.size() > budget) break;
        ctx += line;
    }

    return ctx;
}

size_t SessionBuffer::estimated_tokens() const {
    size_t total = 0;
    for (auto& t : turns_)
        total += (t.input_summary.size() + t.output_summary.size()) / 4 + 10;
    return total;
}

json SessionBuffer::to_json() const {
    json arr = json::array();
    for (auto& t : turns_)
        arr.push_back({
            {"call_id", t.call_id},
            {"agent_id", t.agent_id},
            {"capability", t.capability},
            {"input_summary", t.input_summary},
            {"output_summary", t.output_summary},
            {"cache_key", t.cache_key},
            {"timestamp_ms", t.timestamp_ms},
            {"confidence", t.confidence}
        });
    return {{"session_id", session_id_}, {"turns", arr}};
}

// ═══════════════════════════════════════════════════════════════════════
// L2 — 持久化缓存
// ═══════════════════════════════════════════════════════════════════════

void L2PersistentCache::init(const std::string& storage_dir) {
    storage_dir_ = storage_dir;
    data_file_ = storage_dir + "/l2_cache.jsonl";
    revidx_file_ = storage_dir + "/l2_revidx.json";

    fs::create_directories(storage_dir);
    load_from_disk();
    GW_LOG_INFO("L2PersistentCache initialized: " + storage_dir +
                " entries=" + std::to_string(entries_.size()));
}

void L2PersistentCache::shutdown() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    save_to_disk();
    save_revidx();
    entries_.clear();
    dep_rev_.clear();
}

std::optional<CacheEntry> L2PersistentCache::get(const std::string& key) {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;

    auto& entry = it->second;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // 检查 TTL
    if (now_ms > entry.created_at_ms + entry.base_ttl_seconds * 1000L) {
        return std::nullopt; // 不在这里删，等 purge_expired
    }

    // 检查失效标记
    if (entry.status == CacheStatus::INVALIDATED)
        return std::nullopt;

    entry.access_count++;
    return entry;
}

void L2PersistentCache::set(const std::string& key, const CacheEntry& entry) {
    std::unique_lock<std::shared_mutex> lk(mu_);

    // 更新反向依赖索引: 移除旧索引
    auto old = entries_.find(key);
    if (old != entries_.end()) {
        for (auto& dep : old->second.depends_on) {
            auto& rev = dep_rev_[dep];
            rev.erase(std::remove(rev.begin(), rev.end(), key), rev.end());
        }
    }

    // 写入
    entries_[key] = entry;

    // 重建反向依赖
    for (auto& dep : entry.depends_on) {
        auto& rev = dep_rev_[dep];
        if (std::find(rev.begin(), rev.end(), key) == rev.end())
            rev.push_back(key);
    }

    // 写盘（可优化为异步批量写）
    save_to_disk();
    save_revidx();
}

void L2PersistentCache::remove(const std::string& key) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        for (auto& dep : it->second.depends_on) {
            auto rev_it = dep_rev_.find(dep);
            if (rev_it != dep_rev_.end()) {
                auto& v = rev_it->second;
                v.erase(std::remove(v.begin(), v.end(), key), v.end());
            }
        }
        entries_.erase(it);
    }
    save_to_disk();
    save_revidx();
}

void L2PersistentCache::soft_invalidate(const std::string& key,
                                         const std::string& reason) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;

    it->second.status = CacheStatus::STALE;
    it->second.confidence *= 0.3;
    it->second.invalid_reason = reason;
    GW_LOG_INFO("L2 soft_invalidate: " + key + " reason=" + reason);
    save_to_disk();
}

void L2PersistentCache::hard_invalidate(const std::string& key,
                                         const std::string& reason,
                                         bool cascade) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;

    it->second.status = CacheStatus::INVALIDATED;
    it->second.invalid_reason = reason;
    it->second.confidence = 0.0;
    GW_LOG_WARN("L2 hard_invalidate: " + key + " reason=" + reason +
                " cascade=" + (cascade ? "true" : "false"));

    if (cascade) {
        std::vector<std::string> visited;
        cascade_invalidate_internal(key, reason, visited);
    }
    save_to_disk();
    save_revidx();
}

void L2PersistentCache::cascade_invalidate_internal(
    const std::string& key, const std::string& reason,
    std::vector<std::string>& visited) {

    if (std::find(visited.begin(), visited.end(), key) != visited.end())
        return; // 防止循环依赖
    visited.push_back(key);

    auto rev_it = dep_rev_.find(key);
    if (rev_it == dep_rev_.end()) return;

    for (auto& dep_key : rev_it->second) {
        auto dep = entries_.find(dep_key);
        if (dep == entries_.end()) continue;

        if (dep->second.status != CacheStatus::INVALIDATED) {
            dep->second.status = CacheStatus::STALE;
            dep->second.confidence *= 0.3;
            dep->second.invalid_reason =
                "上游 " + key + " 已失效: " + reason;
            GW_LOG_INFO("L2 cascade: " + dep_key +
                       " ← 上游 " + key + " 已失效");
            // 递归传播
            cascade_invalidate_internal(dep_key, dep->second.invalid_reason,
                                         visited);
        }
    }
}

void L2PersistentCache::apply_confidence_decay() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (auto& [key, entry] : entries_) {
        if (entry.status == CacheStatus::INVALIDATED) continue;
        auto days = (now_ms - entry.created_at_ms) / (1000.0 * 86400.0);
        double new_conf = entry.confidence * std::pow(1.0 - entry.confidence_decay, days);
        if (new_conf < kConfidenceStale) {
            entry.status = CacheStatus::STALE;
            entry.invalid_reason = "置信度衰减至 " + std::to_string(new_conf);
        }
        entry.confidence = new_conf;
    }
    save_to_disk();
}

std::vector<std::string> L2PersistentCache::dependents_of(
    const std::string& key) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = dep_rev_.find(key);
    if (it == dep_rev_.end()) return {};
    return it->second;
}

void L2PersistentCache::purge_expired() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<std::string> to_remove;
    for (auto& [key, entry] : entries_) {
        if (entry.status == CacheStatus::INVALIDATED &&
            now_ms > entry.created_at_ms + entry.base_ttl_seconds * 1000L * 2)
            to_remove.push_back(key);
    }

    for (auto& key : to_remove) {
        for (auto& dep : entries_[key].depends_on) {
            auto rev_it = dep_rev_.find(dep);
            if (rev_it != dep_rev_.end()) {
                auto& v = rev_it->second;
                v.erase(std::remove(v.begin(), v.end(), key), v.end());
            }
        }
        entries_.erase(key);
    }

    if (!to_remove.empty()) {
        GW_LOG_INFO("L2 purge_expired: removed " + std::to_string(to_remove.size()));
        save_to_disk();
        save_revidx();
    }
}

size_t L2PersistentCache::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return entries_.size();
}

void L2PersistentCache::load_from_disk() {
    if (!fs::exists(data_file_)) return;

    std::ifstream f(data_file_);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            auto entry = CacheEntry::from_json(j);
            entries_[entry.key] = std::move(entry);
        } catch (const std::exception& e) {
            GW_LOG_WARN("L2 load: skip bad line: " + std::string(e.what()));
        }
    }

    if (fs::exists(revidx_file_)) {
        try {
            std::ifstream rf(revidx_file_);
            json rj = json::parse(rf);
            for (auto& [k, v] : rj.items()) {
                dep_rev_[k] = v.get<std::vector<std::string>>();
            }
        } catch (...) {
            GW_LOG_WARN("L2: failed to load reverse index, rebuilding");
            for (auto& [key, entry] : entries_) {
                for (auto& dep : entry.depends_on)
                    dep_rev_[dep].push_back(key);
            }
        }
    }
}

void L2PersistentCache::save_to_disk() const {
    std::ofstream f(data_file_, std::ios::trunc);
    for (auto& [key, entry] : entries_) {
        f << entry.to_json().dump() << "\n";
    }
}

void L2PersistentCache::save_revidx() const {
    json rj;
    for (auto& [k, v] : dep_rev_)
        rj[k] = v;
    std::ofstream rf(revidx_file_, std::ios::trunc);
    rf << rj.dump();
}

// ═══════════════════════════════════════════════════════════════════════
// L3 — 归档存储
// ═══════════════════════════════════════════════════════════════════════

void L3ArchiveStore::init(const std::string& base_dir) {
    base_dir_ = base_dir;
    fs::create_directories(base_dir_);
    GW_LOG_INFO("L3ArchiveStore initialized: " + base_dir_);
}

void L3ArchiveStore::shutdown() {}

std::string L3ArchiveStore::put(const std::string& key,
                                  const std::string& traces_json) {
    std::lock_guard<std::mutex> lk(mu_);
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto path = make_key_path(key, ts);
    auto dir = fs::path(path).parent_path();
    fs::create_directories(dir);

    std::ofstream f(path);
    f << traces_json;
    return "l3://" + path;
}

std::optional<std::string> L3ArchiveStore::get(const std::string& ref) {
    auto path = ref_to_path(ref);
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void L3ArchiveStore::append_audit_log(const std::string& key,
                                       const std::string& event) {
    std::lock_guard<std::mutex> lk(mu_);
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto path = make_key_path(key, ts) + ".audit";

    auto dir = fs::path(path).parent_path();
    fs::create_directories(dir);

    std::ofstream f(path, std::ios::app);
    f << ts << " " << event << "\n";
}

std::vector<std::string> L3ArchiveStore::list_by_date(int64_t start_ms,
                                                        int64_t end_ms) {
    std::vector<std::string> result;
    // 简化实现：遍历 base_dir 下的日期目录
    try {
        for (auto& date_entry : fs::directory_iterator(base_dir_)) {
            if (!date_entry.is_directory()) continue;
            for (auto& file_entry : fs::directory_iterator(date_entry.path())) {
                if (file_entry.path().extension() == ".audit") continue;
                result.push_back(file_entry.path().string());
            }
        }
    } catch (...) {}
    return result;
}

std::string L3ArchiveStore::ref_to_path(const std::string& ref) const {
    // "l3:///path/to/file" → "/path/to/file"
    if (ref.rfind("l3://", 0) == 0)
        return ref.substr(5);
    return ref;
}

std::string L3ArchiveStore::make_key_path(const std::string& key,
                                           int64_t ts_ms) const {
    // 安全的文件名：hash key，按日期分区
    auto hash = std::hash<std::string>{}(key);
    auto t = static_cast<time_t>(ts_ms / 1000);
    auto tm = *std::localtime(&t);

    std::ostringstream oss;
    oss << base_dir_ << "/"
        << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900)
        << std::setw(2) << (tm.tm_mon + 1)
        << std::setw(2) << tm.tm_mday << "/"
        << std::hex << hash << ".json";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════════
// CacheCoordinator
// ═══════════════════════════════════════════════════════════════════════

void CacheCoordinator::init(const std::string& l2_dir,
                             const std::string& l3_dir) {
    L2PersistentCache::instance().init(l2_dir);
    L3ArchiveStore::instance().init(l3_dir);
    GW_LOG_INFO("CacheCoordinator initialized");
}

void CacheCoordinator::shutdown() {
    // 关闭所有 session
    std::vector<std::string> session_ids;
    {
        std::unique_lock<std::shared_mutex> lk(sb_mu_);
        for (auto& [id, _] : sessions_)
            session_ids.push_back(id);
    }
    for (auto& id : session_ids)
        archive_session(id);

    L2PersistentCache::instance().shutdown();
    L3ArchiveStore::instance().shutdown();
}

CacheCoordinator::QueryResult CacheCoordinator::query(
    const std::string& agent_id,
    const std::string& capability,
    const std::string& input_hash,
    const std::string& session_id) {

    QueryResult result;
    std::string cache_key = "agent:" + agent_id + ":" + capability + ":" + input_hash;

    // L1 查询
    auto l1 = L1CacheStore::instance().get(cache_key);
    if (l1) {
        result.entry = *l1;
        result.hit = true;
        result.layer = CacheLayer::L1;
        return result;
    }

    // SessionBuffer 查询
    {
        std::shared_lock<std::shared_mutex> lk(sb_mu_);
        auto sbit = sessions_.find(session_id);
        if (sbit != sessions_.end()) {
            auto recent = sbit->second->recent(capability, 3);
            if (!recent.empty()) {
                // 返回最近同 capability 的结果作为上下文引用
                // 不是精确命中，但提供上下文
                auto& last = recent.front();
                auto l2 = L2PersistentCache::instance().get(last.cache_key);
                if (l2 && l2->status != CacheStatus::INVALIDATED) {
                    result.entry = *l2;
                    result.hit = true;
                    result.layer = CacheLayer::L2;
                    result.stale = (l2->status == CacheStatus::STALE);
                    return result;
                }
            }
        }
    }

    // L2 精确查询
    auto l2 = L2PersistentCache::instance().get(cache_key);
    if (l2) {
        if (l2->status == CacheStatus::INVALIDATED) {
            // 已失效，视为未命中
        } else {
            result.entry = *l2;
            result.hit = true;
            result.layer = CacheLayer::L2;
            result.stale = (l2->status == CacheStatus::STALE ||
                          l2->confidence < kConfidenceFresh);
            // 提升到 L1
            L1CacheStore::instance().set(cache_key, *l2);
            return result;
        }
    }

    return result; // 未命中
}

void CacheCoordinator::write(const CacheEntry& entry) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    CacheEntry e = entry;
    if (e.created_at_ms == 0) e.created_at_ms = now_ms;

    // L1: 小数据直接存
    if (e.output.size() < 4096) {
        L1CacheStore::instance().set(e.key, e);
    }

    // L2: 存摘要
    CacheEntry l2_entry = e;
    if (e.output.size() > 2048) {
        // 完整输出 > 2KB 时, L2 只存摘要, output 落 L3
        l2_entry.output = ""; // L2 不存完整 output
        l2_entry.full_ref = L3ArchiveStore::instance().put(e.key, e.output);
    }
    L2PersistentCache::instance().set(e.key, l2_entry);
}

SessionBuffer& CacheCoordinator::session_buffer(const std::string& session_id) {
    std::unique_lock<std::shared_mutex> lk(sb_mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        auto sb = std::make_unique<SessionBuffer>(session_id);
        auto& ref = *sb;
        sessions_[session_id] = std::move(sb);
        return ref;
    }
    return *(it->second);
}

void CacheCoordinator::close_session(const std::string& session_id) {
    std::unique_lock<std::shared_mutex> lk(sb_mu_);
    sessions_.erase(session_id);
}

std::string CacheCoordinator::archive_session(const std::string& session_id) {
    std::unique_lock<std::shared_mutex> lk(sb_mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return "";

    auto& sb = *it->second;
    if (sb.empty()) {
        sessions_.erase(it);
        return "";
    }

    auto json_str = sb.to_json().dump();
    auto l3_ref = L3ArchiveStore::instance().put(
        "session:" + session_id, json_str);

    sessions_.erase(it);
    GW_LOG_INFO("Session archived: " + session_id + " → " + l3_ref);
    return l3_ref;
}

void CacheCoordinator::invalidate(const std::string& key,
                                   const std::string& reason,
                                   bool cascade) {
    L1CacheStore::instance().invalidate(key);
    L2PersistentCache::instance().hard_invalidate(key, reason, cascade);
}

std::optional<ContextExpandResult>
CacheCoordinator::expand_context(const std::string& ref, size_t max_tokens) {
    ContextExpandResult result;
    result.ref = ref;

    // 尝试 L3 读取
    auto content = L3ArchiveStore::instance().get(ref);
    if (content) {
        result.full_text = *content;
        result.total_tokens = result.full_text.size() / 4;
        auto max_chars = max_tokens * 4;
        if (result.full_text.size() > max_chars) {
            result.full_text = result.full_text.substr(0, max_chars) + "...";
            result.truncated = true;
        }
        return result;
    }
    return std::nullopt;
}

void CacheCoordinator::background_maintenance() {
    // 定时任务：置信度衰减 + 清理过期条目
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        L2PersistentCache::instance().apply_confidence_decay();
        L2PersistentCache::instance().purge_expired();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// AgentRegistry
// ═══════════════════════════════════════════════════════════════════════

void AgentRegistry::register_agent(const std::string& session_id,
                                    const AgentRecord& record) {
    std::unique_lock<std::shared_mutex> lk(mu_);

    // 如果已存在，先清理旧的能力索引
    auto old = agents_.find(session_id);
    if (old != agents_.end()) {
        for (auto& cap : old->second.capabilities) {
            auto& idx = cap_index_[cap.name];
            idx.erase(std::remove(idx.begin(), idx.end(), session_id),
                      idx.end());
        }
    }

    agents_[session_id] = record;

    // 重建能力索引
    for (auto& cap : record.capabilities) {
        auto& idx = cap_index_[cap.name];
        if (std::find(idx.begin(), idx.end(), session_id) == idx.end())
            idx.push_back(session_id);
    }

    GW_LOG_INFO("Agent registered: " + record.agent_name +
                " session=" + session_id +
                " caps=" + std::to_string(record.capabilities.size()));
}

void AgentRegistry::unregister_agent(const std::string& session_id) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = agents_.find(session_id);
    if (it == agents_.end()) return;

    for (auto& cap : it->second.capabilities) {
        auto idx_it = cap_index_.find(cap.name);
        if (idx_it != cap_index_.end()) {
            auto& v = idx_it->second;
            v.erase(std::remove(v.begin(), v.end(), session_id), v.end());
            if (v.empty()) cap_index_.erase(idx_it);
        }
    }
    agents_.erase(it);
    GW_LOG_INFO("Agent unregistered: session=" + session_id);
}

std::optional<AgentRecord> AgentRegistry::route(const std::string& capability) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = cap_index_.find(capability);
    if (it == cap_index_.end() || it->second.empty()) {
        // 模糊匹配？（后续扩展）
        return std::nullopt;
    }

    // 选负载最低的
    std::optional<AgentRecord> best;
    for (auto& sid : it->second) {
        auto ait = agents_.find(sid);
        if (ait == agents_.end() || !ait->second.healthy) continue;
        if (!best || ait->second.active_calls < best->active_calls)
            best = ait->second;
    }
    return best;
}

std::optional<AgentRecord> AgentRegistry::find(const std::string& session_id) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = agents_.find(session_id);
    if (it != agents_.end()) return it->second;
    return std::nullopt;
}

void AgentRegistry::heartbeat(const std::string& session_id) {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = agents_.find(session_id);
    if (it != agents_.end()) {
        it->second.last_heartbeat_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

std::vector<AgentCapability> AgentRegistry::all_capabilities() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<AgentCapability> result;
    for (auto& [cap_name, sids] : cap_index_) {
        AgentCapability c; c.name = cap_name;
        result.push_back(c);
    }
    return result;
}

} // namespace gw
