#pragma once
/**
 * router.h — 请求路由 + 加权轮询负载均衡 + 多模型调度
 */
#include "common.h"
#include <vector>
#include <mutex>
#include <string>
#include <unordered_map>
#include <atomic>

namespace gw {

// ─── 加权轮询负载均衡 ─────────────────────────────────────────────
class LoadBalancer {
public:
    void add_node(BackendNode node) {
        std::lock_guard<std::mutex> lk(mu_);
        nodes_.push_back(std::move(node));
    }

    // 选出一个健康节点（加权轮询 + 最少连接兜底）
    BackendNode* pick() {
        std::lock_guard<std::mutex> lk(mu_);
        if (nodes_.empty()) return nullptr;

        // 加权轮询：current_weight += weight，选最大值
        int total = 0;
        BackendNode* best = nullptr;
        for (auto& n : nodes_) {
            if (!n.healthy.load()) continue;
            total += n.weight;
            cw_[n.name] += n.weight;
            if (!best || cw_[n.name] > cw_[best->name])
                best = &n;
        }
        if (best) cw_[best->name] -= total;
        return best;
    }

    void mark_unhealthy(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& n : nodes_)
            if (n.name == name) { n.healthy.store(false); break; }
    }

    void mark_healthy(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& n : nodes_)
            if (n.name == name) { n.healthy.store(true); break; }
    }

private:
    std::mutex mu_;
    std::vector<BackendNode>              nodes_;
    std::unordered_map<std::string, int>  cw_;   // current_weight
};

// ─── 路由器：model 名 → LoadBalancer ─────────────────────────────
class Router {
public:
    static Router& instance() {
        static Router inst;
        return inst;
    }

    // 注册后端节点到指定 model 路由组
    void register_node(const std::string& route_key, BackendNode node) {
        std::lock_guard<std::mutex> lk(mu_);
        lbs_[route_key].add_node(std::move(node));
    }

    // 根据请求的 model 字段选出后端节点
    // 先精确匹配，再 fallback 到 "*" 默认组
    BackendNode* route(const std::string& model) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = lbs_.find(model);
        if (it != lbs_.end()) {
            auto* n = it->second.pick();
            if (n) return n;
        }
        auto def = lbs_.find("*");
        if (def != lbs_.end()) return def->second.pick();
        return nullptr;
    }

private:
    Router() = default;
    std::mutex mu_;
    std::unordered_map<std::string, LoadBalancer> lbs_;
};

} // namespace gw
