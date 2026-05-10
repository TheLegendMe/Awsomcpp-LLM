#pragma once
/**
 * ws_chat_controller.h — WebSocket 聊天控制器
 *   长连接，每个连接可复用发送多个 chat 请求
 *   role=executor 客户端注册后可接收 tool_call 并回传 tool_result
 *   客户端: WebSocket ↔ Gateway，后端: HTTP SSE (复用 RemoteUpstream，不变)
 */
#include <drogon/WebSocketController.h>
#include <drogon/HttpTypes.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <vector>

namespace gw {

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
    // 处理 chat 请求
    void process_chat(const drogon::WebSocketConnectionPtr& conn,
                      const std::string& request_id,
                      const std::string& model,
                      const std::string& messages_json,
                      const std::string& tools_json,
                      double temperature,
                      int max_tokens);

    // 转发 tool_call 到 executor
    void route_tool_call(const drogon::WebSocketConnectionPtr& sender,
                         const std::string& request_id,
                         const std::string& call_id,
                         const std::string& name,
                         const std::string& arguments_json);

    // 转发 tool_result 回原始客户端
    void route_tool_result(const drogon::WebSocketConnectionPtr& executor,
                           const std::string& request_id,
                           const std::string& call_id,
                           const std::string& content);

    // 发送 JSON 消息到连接（线程安全）
    static void ws_send(const drogon::WebSocketConnectionPtr& conn,
                        const std::string& msg);

    // Executor 池
    std::mutex exec_mu_;
    std::vector<drogon::WebSocketConnectionPtr> executors_;
    size_t exec_rr_idx_ = 0;  // round-robin 索引

    // tool_call → 原始发起客户端映射: "request_id:call_id" → conn
    std::mutex pending_mu_;
    std::unordered_map<std::string, drogon::WebSocketConnectionPtr> pending_tools_;
};

} // namespace gw
