#pragma once
/**
 * chat_controller.h — 聊天 SSE 控制器
 *   POST /v1/chat/completions → AuthFilter → PluginPipeline → Upstream → SSE Response
 */
#include <drogon/HttpController.h>
#include <drogon/HttpTypes.h>
#include "plugin_types.h"
#include "i_upstream.h"
#include <memory>

namespace gw {

class ChatController : public drogon::HttpController<ChatController> {
public:
    ChatController();

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ChatController::chat, "/v1/chat/completions", drogon::Post);
    ADD_METHOD_TO(ChatController::metrics, "/metrics", drogon::Get);
    ADD_METHOD_TO(ChatController::health, "/health", drogon::Get);
    ADD_METHOD_TO(ChatController::plugins_list, "/plugins", drogon::Get);
    ADD_METHOD_TO(ChatController::plugins_reload, "/plugins/reload", drogon::Post);
    METHOD_LIST_END

    // 流式聊天 (Drogon async stream)
    void chat(drogon::HttpRequestPtr req,
              std::function<void(const drogon::HttpResponsePtr&)> callback);

    // 指标
    void metrics(drogon::HttpRequestPtr req,
                 std::function<void(const drogon::HttpResponsePtr&)> callback);

    // 健康检查
    void health(drogon::HttpRequestPtr req,
                std::function<void(const drogon::HttpResponsePtr&)> callback);

    // 插件列表
    void plugins_list(drogon::HttpRequestPtr req,
                      std::function<void(const drogon::HttpResponsePtr&)> callback);

    // 手动触发插件重载
    void plugins_reload(drogon::HttpRequestPtr req,
                        std::function<void(const drogon::HttpResponsePtr&)> callback);

    // 设置全局上游引擎
    static void set_upstream(std::shared_ptr<IUpstream> upstream) {
        upstream_ = std::move(upstream);
    }

private:
    static std::shared_ptr<IUpstream> upstream_;
};

} // namespace gw
