/**
 * main.cpp — LLM Gateway (Drogon v1.9.10)
 */
#include "plugin_manager.h"
#include "router.h"
#include "auth.h"
#include "i_upstream.h"
#include "remote_upstream.h"
#include "logger.h"
#include "metrics.h"
#include "plugin_types.h"
#include "ws_chat_controller.h"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;
using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;

static void ensure_thread_pool() {
    if (!gw::get_thread_pool())
        gw::PluginManager::instance().setup_thread_pool();
}
std::shared_ptr<gw::IUpstream> g_upstream;

// ─── POST /v1/chat/completions ─────────────────────────────────────
static void handle_chat(
    const drogon::HttpRequestPtr& req,
    HttpCallback &&callback)
{
    ensure_thread_pool();
    std::string client_ip = req->getPeerAddr().toIp();
    auto fwd = req->getHeader("X-Forwarded-For");
    if (!fwd.empty()) client_ip = fwd;

    json body_json;
    try { body_json = json::parse(std::string(req->getBody())); }
    catch (...) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k400BadRequest); r->setBody("{\"error\":\"invalid json\"}");
        std::move(callback)(r); return;
    }

    std::string model = body_json.value("model", "local-model");
    if (!body_json.contains("messages") || body_json["messages"].empty()) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k400BadRequest); r->setBody("{\"error\":\"messages required\"}");
        std::move(callback)(r); return;
    }
    std::string messages_json = body_json["messages"].dump();
    double temp = body_json.value("temperature", 0.7);
    int max_tok = body_json.value("max_tokens", 2048);

    std::string api_key;
    auto ah = req->getHeader("Authorization");
    if (ah.size() > 7 && ah.substr(0, 7) == "Bearer ") api_key = ah.substr(7);

    if (auto err = gw::AuthManager::instance().verify(api_key, model); !err.empty()) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k401Unauthorized); r->setBody("{\"error\":\""+err+"\"}");
        METRIC_INC(auth_failed); std::move(callback)(r); return;
    }
    gw::BackendNode* node = gw::Router::instance().route(model);
    if (!node) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k503ServiceUnavailable); r->setBody("{\"error\":\"no backend\"}");
        std::move(callback)(r); return;
    }

    gw::PluginContext pctx;
    pctx.model=model; pctx.messages_json=messages_json; pctx.temperature=temp;
    pctx.max_tokens=max_tok; pctx.user_ip=client_ip; pctx.api_key=api_key;
    pctx.request_id=std::chrono::steady_clock::now().time_since_epoch().count();

    auto pr = gw::PluginManager::instance().run_request_pipeline(pctx);
    model=pr.ctx.model; messages_json=pr.ctx.messages_json;
    temp=pr.ctx.temperature; max_tok=pr.ctx.max_tokens;

    if (!pr.passed) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(static_cast<drogon::HttpStatusCode>(pr.abort_status));
        r->setBody(std::move(pr.abort_body)); METRIC_INC(rate_limited);
        std::move(callback)(r); return;
    }

    std::string nurl = node->url, nkey = node->api_key;
    auto async_resp = drogon::HttpResponse::newAsyncStreamResponse(
        [=](drogon::ResponseStreamPtr stream) mutable {
        ensure_thread_pool();  // 上游 I/O 线程也需要 VM 池
        auto s = std::make_shared<drogon::ResponseStreamPtr>(std::move(stream));
        gw::LLMRequest lr;
        lr.model=model; lr.messages_json=messages_json;
        lr.temperature=temp; lr.max_tokens=max_tok;

        lr.on_token = [s](const std::string& tok) {
            ensure_thread_pool();
            auto p = gw::PluginManager::instance().run_token_pipeline(tok);
            if (!p.empty()) {
                json c={{"object","chat.completion.chunk"},{"model","gateway"},
                    {"choices",json::array({{{"index",0},{"delta",{{"content",p}}},{"finish_reason",nullptr}}})}};
                (*s)->send("data: "+c.dump()+"\n\n");
            }
        };
        lr.on_done = [s]() {
            ensure_thread_pool();
            gw::PluginManager::instance().run_done_pipeline();
            (*s)->send("data: [DONE]\n\n"); (*s)->close();
            METRIC_INC(success_count); METRIC_DEC(active_requests);
        };
        lr.on_error = [s](const std::string& err) {
            json e={{"error",err}}; (*s)->send("data: "+e.dump()+"\n\n"); (*s)->close();
            METRIC_INC(errors); METRIC_DEC(active_requests);
        };
        METRIC_INC(total_requests); METRIC_INC(active_requests);
        g_upstream->submit(lr, nurl, nkey);
    });
    async_resp->setContentTypeString("text/event-stream");
    async_resp->addHeader("Cache-Control", "no-cache");
    async_resp->addHeader("X-Accel-Buffering", "no");
    std::move(callback)(async_resp);
}

// ─── 简单 handlers ─────────────────────────────────────────────────
static void handle_health(const drogon::HttpRequestPtr&, HttpCallback &&cb) {
    auto r=drogon::HttpResponse::newHttpResponse(); r->setBody("{\"status\":\"ok\"}");
    std::move(cb)(r);
}
static void handle_metrics(const drogon::HttpRequestPtr&, HttpCallback &&cb) {
    auto r=drogon::HttpResponse::newHttpResponse(); r->setBody(gw::Metrics::instance().snapshot());
    std::move(cb)(r);
}
static void handle_tools(const drogon::HttpRequestPtr&, HttpCallback &&cb) {
    auto r = drogon::HttpResponse::newHttpResponse();
    std::string json = gw::ToolRegistry::instance().tools_json();
    r->setBody(json.empty() ? "[]" : json);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    std::move(cb)(r);
}
static void handle_plugins(const drogon::HttpRequestPtr&, HttpCallback &&cb) {
    auto p=gw::PluginManager::instance().list_plugins(); json arr=json::array();
    for (auto& x:p) arr.push_back({{"name",x.name},{"version",x.version},{"priority",x.priority}});
    auto r=drogon::HttpResponse::newHttpResponse(); r->setBody(arr.dump(2)); std::move(cb)(r);
}

// ─── 加载配置 ─────────────────────────────────────────────────────
static void load_config(const std::string& path) {
    std::ifstream f(path); if (!f) { GW_LOG_WARN("config not found: "+path); return; }
    json cfg=json::parse(f);
    gw::PluginManager::instance().init(cfg.value("plugins_dir","plugins"),cfg.value("lua_pool_size",8));
    if (cfg.contains("backends"))
        for (auto& be:cfg["backends"]) {
            gw::BackendNode n; n.name=be.value("name",""); n.url=be.value("url","");
            n.api_key=be.value("api_key",""); n.model=be.value("model",""); n.weight=be.value("weight",1);
            gw::Router::instance().register_node(be.value("route_model","*"),std::move(n));
        }
    if (cfg.contains("api_keys"))
        for (auto& k:cfg["api_keys"]) {
            gw::KeyInfo ki; ki.key=k.value("key",""); ki.allowed_models=k.value("models","*");
            ki.qps_limit=k.value("qps",10); ki.enabled=k.value("enabled",true);
            gw::AuthManager::instance().add_key(std::move(ki));
        }
    auto ut=cfg.value("upstream_type","remote");
    if (ut=="local_llama") {
        GW_LOG_WARN("LocalLlamaUpstream requires libllama.so — falling back to remote");
        g_upstream=gw::create_upstream(gw::UpstreamType::REMOTE);
    } else { g_upstream=gw::create_upstream(gw::UpstreamType::REMOTE); }
    g_upstream->start();
}

// ─── main ─────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    Logger::instance().init("logs/gateway.log", LogLevel::INFO);
    std::string cp = argc>1 ? argv[1] : "conf/gateway.json";
    try { drogon::app().loadConfigFile(cp); } catch (...) {
        drogon::app().addListener("0.0.0.0",8080); drogon::app().setThreadNum(4);
    }
    load_config(cp);
    gw::PluginManager::instance().start_hot_reload();
    gw::BackendNode ln; ln.name="llama-local"; ln.url="http://127.0.0.1:8100/v1/chat/completions";
    ln.model="local-model"; ln.weight=1;
    if (!gw::Router::instance().route("local-model")) {
        gw::Router::instance().register_node("local-model",ln);
        gw::Router::instance().register_node("*",ln);
    }
    gw::AuthManager::instance().add_key({"test-key-123","*",60,true});
    gw::AuthManager::instance().add_key({"default-key","*",60,true});
    // +handle 将函数引用转为函数指针，使 FunctionTraits 正确匹配
    drogon::app().registerHandler("/v1/chat/completions", +handle_chat, {drogon::Post});
    drogon::app().registerHandler("/health", +handle_health, {drogon::Get});
    drogon::app().registerHandler("/metrics", +handle_metrics, {drogon::Get});
    drogon::app().registerHandler("/tools", +handle_tools, {drogon::Get});
    drogon::app().registerHandler("/plugins", +handle_plugins, {drogon::Get});
    fprintf(stderr,"LLM Gateway (Drogon) on :8080\n");
    drogon::app().run();
    gw::PluginManager::instance().shutdown(); Logger::instance().shutdown(); return 0;
}
