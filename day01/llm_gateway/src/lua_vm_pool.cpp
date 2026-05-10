/**
 * lua_vm_pool.cpp — 线程局部 Lua VM 池实现
 */
#include "lua_vm_pool.h"
#include <sol/sol.hpp>
#include <stdexcept>

namespace gw {

LuaVMPool::LuaVMPool(size_t pool_size,
                     const SandboxConfig& sandbox_cfg,
                     const GatewayAPI& api)
    : pool_size_(pool_size)
    , sandbox_cfg_(sandbox_cfg)
    , api_(api)
{
    all_vms_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        auto* vm = LuaSandbox::create(sandbox_cfg_, api_);
        all_vms_.emplace_back(vm);
        available_.push(vm);
    }
}

LuaVMPool::~LuaVMPool() {
    // 所有 VM 由 unique_ptr 自动释放
    while (!available_.empty()) available_.pop();
    all_vms_.clear();
}

void LuaVMPool::warmup() {
    // VM 已在构造函数中创建，这里可以触发 JIT 预热（LuaJIT）
    for (auto& vm : all_vms_) {
        // 执行一个空脚本来预热
        try {
            vm->script("local _ = 1 + 1");
        } catch (...) {}
    }
}

void LuaVMPool::update_api(const GatewayAPI& api) {
    std::lock_guard<std::mutex> lk(mu_);
    api_ = api;
    // 重建所有 VM（热重载时）
    all_vms_.clear();
    while (!available_.empty()) available_.pop();
    all_vms_.reserve(pool_size_);
    for (size_t i = 0; i < pool_size_; ++i) {
        auto* vm = LuaSandbox::create(sandbox_cfg_, api_);
        all_vms_.emplace_back(vm);
        available_.push(vm);
    }
}

VMRent LuaVMPool::borrow() {
    std::lock_guard<std::mutex> lk(mu_);
    if (available_.empty()) {
        throw std::runtime_error("LuaVMPool exhausted: all VMs in use");
    }
    auto* vm = available_.front();
    available_.pop();
    LuaSandbox::reset(*vm);
    return VMRent(vm, [this](sol::state* vm) { this->return_vm(vm); });
}

void LuaVMPool::return_vm(sol::state* vm) {
    std::lock_guard<std::mutex> lk(mu_);
    available_.push(vm);
}

// ─── 全局 Thread-Local 池 ──────────────────────────────────────────
static thread_local std::shared_ptr<LuaVMPool> t_pool;

void set_thread_pool(std::shared_ptr<LuaVMPool> pool) {
    t_pool = std::move(pool);
}

std::shared_ptr<LuaVMPool> get_thread_pool() {
    return t_pool;
}

VMRent borrow_vm() {
    if (!t_pool) {
        throw std::runtime_error("no thread-local LuaVMPool set");
    }
    return t_pool->borrow();
}

} // namespace gw
