/**
 * semantic_cache.cpp — 语义缓存实现
 */
#include "semantic_cache.h"
#include "logger.h"
#include <algorithm>
#include <numeric>

namespace gw {

float SemanticCache::cosine_similarity(const std::vector<float>& a,
                                        const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = std::sqrt(na) * std::sqrt(nb);
    return denom > 0.0f ? dot / denom : 0.0f;
}

void SemanticCache::store(const std::vector<float>& emb,
                           const std::string& prompt,
                           const std::string& response) {
    std::lock_guard<std::mutex> lk(mu_);
    if (entries_.size() >= kMaxEntries) {
        entries_.pop_front();
    }
    entries_.push_back({emb, response, prompt});
    GW_LOG_DEBUG("SemanticCache stored, size=" + std::to_string(entries_.size()));
}

std::pair<std::string, float> SemanticCache::search(
    const std::vector<float>& query_emb, float threshold) {
    std::lock_guard<std::mutex> lk(mu_);

    float best_sim = -1.0f;
    size_t best_idx = 0;
    bool found = false;

    for (size_t i = 0; i < entries_.size(); ++i) {
        float sim = cosine_similarity(query_emb, entries_[i].embedding);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
            found = true;
        }
    }

    if (found && best_sim >= threshold) {
        GW_LOG_INFO("SemanticCache HIT sim=" + std::to_string(best_sim).substr(0,4) +
                    " prompt=" + entries_[best_idx].prompt.substr(0,60));
        return {entries_[best_idx].response, best_sim};
    }
    return {"", 0.0f};
}

void SemanticCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
}

} // namespace gw
