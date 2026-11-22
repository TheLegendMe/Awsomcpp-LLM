#pragma once
/**
 * lua_vm_pool.h — 线程局部 Lua VM 池
 *   每个 Drogon Worker 线程持有一个 thread_local 的 VM 池
 *   borrow/return 模式，避免 GC 停顿，消除跨线程竞争
 */
#include "lua_sandbox.h"
#include <vector>
#include <queue>
#include <mutex>
#include <memory>

// 前向声明
namespace sol { class state; }

namespace gw {

// ─── VM 句柄（RAII，自动归还）───────────────────────────────────────
class VMRent {
public:
    VMRent(sol::state* vm, std::function<void(sol::state*)> return_fn)
        : vm_(vm), return_fn_(std::move(return_fn)) {}

    ~VMRent() { if (vm_ && return_fn_) return_fn_(vm_); }

    VMRent(const VMRent&) = delete;
    VMRent& operator=(const VMRent&) = delete;
    VMRent(VMRent&& o) noexcept
        : vm_(o.vm_), return_fn_(std::move(o.return_fn_))
        { o.vm_ = nullptr; }

    VMRent& operator=(VMRent&& o) noexcept {
        if (this != &o) { vm_ = o.vm_; return_fn_ = std::move(o.return_fn_); o.vm_ = nullptr; }
        return *this;
    }

    sol::state* operator->() { return vm_; }
    sol::state& operator*()  { return *vm_; }
    sol::state* get()        { return vm_; }

private:
    sol::state* vm_ = nullptr;
    std::function<void(sol::state*)> return_fn_;
};

// ─── 线程局部 VM 池 ────────────────────────────────────────────────
class LuaVMPool {
public:
    // pool_size: 每个线程预热的 VM 数量
    // sandbox_cfg: 沙箱配置
    // api: 注入到每个 VM 的 Gateway API
    LuaVMPool(size_t pool_size,
              const SandboxConfig& sandbox_cfg,
              const GatewayAPI& api);
    ~LuaVMPool();

    LuaVMPool(const LuaVMPool&) = delete;
    LuaVMPool& operator=(const LuaVMPool&) = delete;

    // 借出一个 VM（如果没有可用，阻塞等待）
    VMRent borrow();

    // 预热所有 VM（在 worker 线程启动时调用）
    void warmup();

    // 池大小
    size_t size() const { return pool_size_; }

    // 更新 GatewayAPI（热重载时使用）
    void update_api(const GatewayAPI& api);

private:
    void return_vm(sol::state* vm);

    size_t              pool_size_;
    SandboxConfig       sandbox_cfg_;
    GatewayAPI          api_;
    std::mutex          mu_;
    std::queue<sol::state*> available_;
    std::vector<std::unique_ptr<sol::state>> all_vms_;
};

// ─── 全局 Thread-Local 池访问 ──────────────────────────────────────
// 由 PluginManager 在 worker 线程初始化时设置

void set_thread_pool(std::shared_ptr<LuaVMPool> pool);
std::shared_ptr<LuaVMPool> get_thread_pool();
VMRent borrow_vm();

} // namespace gw
