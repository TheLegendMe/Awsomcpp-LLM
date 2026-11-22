#pragma once
/**
 * agent_runner.h — 目标驱动的自主 Agent 循环 (ReAct: plan → act → observe → reflect)
 *
 * 与 process_chat 的「单轮 + 客户端驱动多轮」不同，AgentRunner 在网关内部自动迭代：
 *
 *   ┌──────────┐   提交 LLM 请求
 *   │  Acting  │ ──────────────────────────────► upstream (SSE)
 *   └────┬─────┘                                    │
 *        │ on_token / on_tool_call / on_done        │
 *        ▼                                          ▼
 *   ┌────────────────┐ 有 tool_calls   ┌──────────────────┐
 *   │ finish_turn    │ ──────────────► │ WaitingTools     │
 *   │ (决策)         │                 │ 派发给 executor  │
 *   └────┬───────────┘                 └────────┬─────────┘
 *        │ 无 tool_calls → Completed            │ 全部 tool_result
 *        ▼                                      ▼
 *      结束                                 追加 role=tool 消息 → 下一轮 Acting
 *
 * 终止条件：模型不再调用工具 / 达到 max_steps / 出错 / 用户 cancel。
 *
 * 线程模型
 *   - LLM 回调运行在 upstream I/O 线程
 *   - tool_result 回传运行在 WebSocket EventLoop 线程
 *   所有共享状态由 mu_ 保护；send_ / dispatch_ 由宿主保证线程安全（非阻塞）。
 *   每个 LLM 请求的 lambda 捕获 shared_from_this()，保证 in-flight 期间对象存活。
 */
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gw {

using json = nlohmann::json;

class AgentRunner : public std::enable_shared_from_this<AgentRunner> {
public:
    enum class Phase { Idle, Acting, WaitingTools, Completed, Failed, Cancelled };
    static const char* phase_name(Phase p);

    struct Config {
        std::string request_id;
        std::string model;              // 上游实际模型名
        std::string upstream_url;       // 由 Router 解析
        std::string upstream_key;
        std::string tools_json;         // OpenAI tools 数组，空则不启用工具
        double      temperature = 0.7;
        int         max_tokens  = 2048;
        int         max_steps   = 10;   // 最多 LLM 轮次
    };

    /// 向客户端发送事件（宿主负责队列化 / 线程安全）
    using SendFn = std::function<void(const json&)>;
    /// 将一次 tool_call 路由到 executor；结果通过 on_tool_result 回到本对象
    using DispatchFn = std::function<void(const std::string& request_id,
                                          const std::string& call_id,
                                          const std::string& name,
                                          const std::string& arguments)>;
    using LogFn     = std::function<void(const std::string&)>;
    using FinishFn  = std::function<void()>;   // 终态回调（用于清理持有者）

    AgentRunner(Config cfg, SendFn send, DispatchFn dispatch, LogFn log = nullptr);

    AgentRunner(const AgentRunner&)            = delete;
    AgentRunner& operator=(const AgentRunner&) = delete;

    void set_on_finished(FinishFn fn) { on_finished_ = std::move(fn); }

    /// 启动循环。messages_json 为初始消息数组（OpenAI 格式）
    void start(const std::string& messages_json);

    // ── 上游回调（由宿主转接）─────
    void on_llm_token(const std::string& tok);
    void on_llm_tool_call(const std::string& call_id,
                          const std::string& name,
                          const std::string& arguments);
    void on_llm_done();
    void on_llm_error(const std::string& err);

    // ── 工具执行结果 ──────────────
    void on_tool_result(const std::string& call_id,
                        const std::string& content,
                        bool success);

    void cancel();

    const std::string& request_id() const { return cfg_.request_id; }
    Phase phase() const;

private:
    struct TurnGuard { std::atomic<bool> active{true}; };
    struct PendingTool {
        std::string name;
        std::string args;
        bool        done    = false;
        bool        success = false;
        std::string result;
    };

    void begin_turn_locked();
    void submit_turn_locked();
    void finish_llm_turn_locked();
    void maybe_advance_locked();
    void append_assistant_locked();
    void finalize_locked(const std::string& reason);
    void send_event(const json& j);
    void log(const std::string& s);

    Config     cfg_;
    SendFn     send_;
    DispatchFn dispatch_;
    LogFn      log_;
    FinishFn   on_finished_;
    std::shared_ptr<std::atomic<bool>> cancel_flag_;

    mutable std::mutex mu_;
    Phase       phase_ = Phase::Idle;
    json        messages_ = json::array();
    int         step_ = 0;

    // 当前轮状态
    std::shared_ptr<TurnGuard> turn_guard_;
    std::string  turn_text_;
    std::vector<std::string> tool_order_;
    std::unordered_map<std::string, PendingTool> pending_;
    size_t       tool_seq_ = 0;
};

} // namespace gw
