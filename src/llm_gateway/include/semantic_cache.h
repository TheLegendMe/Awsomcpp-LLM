#pragma once
/**
 * semantic_cache.h — 语义缓存 (embedding 向量相似度匹配)
 */
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <cmath>

namespace gw {

struct SemanticEntry {
    std::vector<float> embedding;
    std::string        response;
    std::string        prompt;  // 原始 query，用于日志
};

class SemanticCache {
public:
    static SemanticCache& instance() {
        static SemanticCache inst;
        return inst;
    }

    // 存储 prompt→response 及其 embedding
    void store(const std::vector<float>& emb,
               const std::string& prompt,
               const std::string& response);

    // 搜索最相似的缓存条目，返回 (response, similarity)
    // threshold: 0.0-1.0，推荐 0.90 (高精度) 或 0.85 (高召回)
    std::pair<std::string, float> search(const std::vector<float>& query_emb,
                                          float threshold = 0.85f);

    size_t size() const { return entries_.size(); }
    void clear();

private:
    SemanticCache() = default;

    static float cosine_similarity(const std::vector<float>& a,
                                    const std::vector<float>& b);

    mutable std::mutex mu_;
    std::deque<SemanticEntry> entries_;
    static constexpr size_t kMaxEntries = 5000;
};

} // namespace gw
