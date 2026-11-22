#pragma once
/**
 * ws_chat_controller.h — WebSocket 聊天控制器
 *   长连接，每个连接可复用发送多个 chat 请求
 *   role=executor 客户端注册后可接收 tool_call 并回传 tool_result
 *   role=agent 客户端注册后可接收 agent_call 并回传 agent_result
 *   客户端: WebSocket ↔ Gateway，后端: HTTP SSE (复用 RemoteUpstream，不变)
 */
#include <drogon/WebSocketController.h>
#include <drogon/HttpTypes.h>
#include <trantor/net/EventLoop.h>
#include "agent_types.h"
#include "agent_runner.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <vector>
#include <functional>

namespace gw {

struct SessionCtx {
    std::string          session_id;
    std::string          api_key;
    std::string          client_ip;
    std::string          role = "user";   // "user", "executor", "agent"
    bool                 authenticated = false;
    trantor::EventLoop*  loop = nullptr;
    std::vector<std::string> tools;
    // Agent 扩展
    std::string          agent_name;      // agent 角色名
    std::vector<AgentCapability> capabilities;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> active_requests;
};

class WsChatController : public drogon::WebSocketController<WsChatController> {
public:
    WsChatController() = default;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/chat", drogon::Get);
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

private:
    // ─── chat / tool (已有) ────────────────────────────────────────
    void process_chat(const drogon::WebSocketConnectionPtr& conn,
                      const std::string& request_id,
                      const std::string& model,
                      const std::string& messages_json,
                      const std::string& tools_json,
                      double temperature,
                      int max_tokens);

    // ─── 自主 Agent 循环 (新增) ────────────────────────────────────
    void process_run(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& request_id,
                     const std::string& model,
                     const std::string& messages_json,
                     const std::string& tools_json,
                     double temperature,
                     int max_tokens,
                     int max_steps);

    void route_tool_call(const drogon::WebSocketConnectionPtr& sender,
                         const std::string& request_id,
                         const std::string& call_id,
                         const std::string& name,
                         const std::string& arguments_json);

    // 通用路由：reply_conn（chat 模式回发客户端）与 reply_runner（run 模式回发循环）
    // 二选一，由哪个非空决定。
    void route_tool_call_ex(const std::string& request_id,
                            const std::string& call_id,
                            const std::string& name,
                            const std::string& arguments_json,
                            const drogon::WebSocketConnectionPtr& reply_conn,
                            const std::shared_ptr<AgentRunner>& reply_runner);

    void route_tool_result(const drogon::WebSocketConnectionPtr& executor,
                           const std::string& request_id,
                           const std::string& call_id,
                           const std::string& content,
                           bool success);

    // ─── agent 间调用 (新增) ──────────────────────────────────────
    void route_agent_call(const drogon::WebSocketConnectionPtr& caller,
                          const std::string& call_id,
                          const std::string& target_agent_id,
                          const std::string& capability,
                          const std::string& input_payload,
                          const std::string& session_id,
                          bool bypass_cache);

    void route_agent_result(const drogon::WebSocketConnectionPtr& callee,
                            const std::string& call_id,
                            const std::string& output_payload,
                            double confidence);

    void handle_cache_invalidate(const drogon::WebSocketConnectionPtr& sender,
                                  const std::string& key,
                                  const std::string& reason,
                                  bool cascade);

    void handle_context_expand(const drogon::WebSocketConnectionPtr& requester,
                                const std::string& call_id,
                                const std::string& ref,
                                size_t max_tokens);

    // ─── 基础设施 ────────────────────────────────────────────────
    static void ws_send(const drogon::WebSocketConnectionPtr& conn,
                        const std::string& msg);
    void ws_send_to_session(const std::string& session_id,
                            const std::string& msg);

    // Executor 池
    std::mutex exec_mu_;
    std::vector<drogon::WebSocketConnectionPtr> executors_;
    size_t exec_rr_idx_ = 0;

    // tool_call 回传目标：chat 模式回给客户端 conn，run 模式回给 AgentRunner
    struct ToolRoute {
        drogon::WebSocketConnectionPtr conn;      // 可为空
        std::shared_ptr<AgentRunner>   runner;    // 可为空
    };

    // tool_call → 目标映射（key = request_id:call_id）
    std::mutex pending_mu_;
    std::unordered_map<std::string, ToolRoute> pending_tools_;

    // 进行中的 AgentRunner（key = request_id），用于 cancel / 断连清理
    std::mutex runner_mu_;
    struct RunnerEntry {
        std::shared_ptr<AgentRunner> runner;
        std::string                  session_id;
    };
    std::unordered_map<std::string, RunnerEntry> runners_;

    // agent_call → 发起方 conn 映射 (复用 pending_mu_)
    // key = call_id, value = caller conn
    std::unordered_map<std::string, drogon::WebSocketConnectionPtr> pending_agents_;

    // session_id → conn 映射（用于回传 agent_result）
    std::mutex session_mu_;
    std::unordered_map<std::string, drogon::WebSocketConnectionPtr> sessions_;
};

} // namespace gw
