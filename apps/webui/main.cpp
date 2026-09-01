#include <drogon/drogon.h>
#include <dztrader/core/encoding.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/data_type.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/writer.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include "config.h"
#include "auth_controller.h"
#include "ws_controller.h"
#include "event_monitor.h"
#include "frame_router.h"
#include "shm_writer.h"
#include "db_init.h"
#include "repository.h"
#include "jwt.h"
#include "user_controller.h"
#include "security_controller.h"
#include "market_source_controller.h"
#include "process_controller.h"
#include "process_mirror.h"
#include "settings_controller.h"
#include "mirror_store.h"
#include "log_controller.h"
#include "notify_cache.h"
#include "notify_controller.h"
#include "auto_login_domain_service.h"
#include "log_domain_service.h"
#include "control_domain_service.h"
#include "process_domain_service.h"
#include "progress_domain_service.h"
#include "md_config_domain_service.h"
#include "md_status_domain_service.h"
#include "shm_domain_service.h"
#include "subscription_domain_service.h"
#include "notify_domain_service.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main(int argc, char* argv[]) {
    // spdlog 初始化（配置加载之前）
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    // 配置加载优先级:argv[1] > paths::configs()/"webui.json"
    std::filesystem::path config_path = dztrader::paths::configs() / "webui.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    // 配置不存在则自动生成(日志尚未初始化,用 stderr)
    if (!std::filesystem::exists(config_path)) {
        fprintf(stderr, "配置文件不存在,自动生成默认配置: %s\n", config_path.string().c_str());
        try {
            dztrader::webui::generate_default_config(config_path);
        } catch (const std::exception& e) {
            fprintf(stderr, "生成配置失败: %s\n", e.what());
            return 1;
        }
    }

    dztrader::webui::WebuiConfig cfg;
    try {
        cfg = dztrader::webui::load_webui_config(config_path);
    } catch (const std::exception& e) {
        fprintf(stderr, "config load failed: %s\n", e.what());
        return 1;
    }

    // 根据 TOML 配置调整日志级别 (统一用 LogConfig::parse_level, 契约 log)
    // 非法级别容错为 info
    auto webui_level = dztrader::platform::LogConfig::parse_level(cfg.log_level)
                           .value_or(spdlog::level::info);

    // 初始化项目标准日志（与 dztraderd/dzmd_ctp 一致）
    // logger 名 = 可执行文件名（不含扩展名），避免硬编码
    // 日志写入 logs/ 根目录，通过 logger 名与其他进程日志区分；
    // 前端+后端双重 guard 阻止 tail 订阅自我日志，避免反馈循环：
    //   后端 ws_controller/log_controller 通过 exe_stem() 比对
    //   前端通过 /api/system/info 拉取 process_name 比对
    const auto& exe_stem = dztrader::this_process::exe_stem();
    try {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = exe_stem;
        log_cfg.log_dir = dztrader::paths::logs();
        log_cfg.level = webui_level;
        log_cfg.flush_level = spdlog::level::info;
        dztrader::log::set_default_logger(log_cfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "log init failed: %s (falling back to stderr)\n", e.what());
        spdlog::set_level(webui_level);
    }

    // dzweb 自身日志配置统一走 LogConfig（与 master/md/td 同源 /log section）
    // 启动即 load()：校验/规范化/持久化/应用 spdlog；镜像与直调均以它为准
    auto self_log = std::make_shared<dztrader::platform::LogConfig>(
        dztrader::this_process::exe_stem(), config_path);
    self_log->load();

    // 契约 process 失败路径 A: App Root 未找到时 dzweb 启动失败
    // dzweb 不扫描自身, 但需要 App Root 作为扫描锚点发现 dzmd_*/dztd_* (供 available 列表)
    // app_root() 从当前 exe_dir 向上查找 dztraderd[.exe], 未找到抛 std::runtime_error
    try {
        const auto& root = dztrader::this_process::app_root();
        SPDLOG_INFO("app root | path={}", dztrader::to_utf8_from_system(root.string()));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("failed to find app root | error={}", e.what());
        return 1;
    }

    // 数据库初始化 + 种子管理员账户
    sqlite3* db = nullptr;
    try {
        const std::filesystem::path db_path = dztrader::paths::db() / "webui.db";
        dztrader::webui::init_database(db_path, db);
        dztrader::webui::seed_admin_user(db, cfg.admin_username, cfg.admin_password);
    } catch (const std::exception& e) {
        fprintf(stderr, "database init failed: %s\n", e.what());
        if (db) {
            sqlite3_close(db);
        }
        return 1;
    }
    sqlite3_close(db);  // Repository will open its own connection

    // 默认密码告警：配置未显式指定 password 时使用弱密码 88888888，提醒立即修改
    if (cfg.admin_password_is_default) {
        SPDLOG_WARN("default admin password in use | password=88888888 action=change_now advice=\"edit admin.password in configs/webui.json or login to change\"");
    }

    auto repo = std::make_shared<dztrader::webui::Repository>(dztrader::paths::db() / "webui.db");
    auto shm_dir = dztrader::paths::shm();
    // 共享 MirrorStore 先行构造，ProcessMirror 注入共享镜像（与领域服务共用同一份数据）
    auto mirror_store = std::make_shared<dztrader::webui::MirrorStore>();
    auto process_mirror = std::make_shared<dztrader::webui::ProcessMirror>(*mirror_store);

    // shm 初始化（master 已创建 channel，webui 只 open_only）
    // 共享内存不存在时降级为 API-only 模式（dztraderd 未启动）
    // 事件 Reader 由 EventMonitor 内部创建，此处仅创建事件 Writer
    std::shared_ptr<dztrader::shm::MultiWriter> event_writer;
    try {
        auto event_meta = std::make_shared<dztrader::shm::ChannelMeta>(
            dztrader::shm::ChannelMeta::open_only(dztrader::shm::channel_name("dzevent"), shm_dir));
        event_writer = std::make_shared<dztrader::shm::MultiWriter>(
            dztrader::shm::MultiWriter::create(event_meta, "dzweb"));
        SPDLOG_INFO("shm initialized | channels={}", dztrader::shm::channel_name("dzevent"));
    } catch (const std::exception& e) {
        SPDLOG_WARN("shm init failed | error=\"{}\" mode=api-only advice=start_dztraderd", e.what());
    }
    // ShmWriter 注入同一 MultiWriter 实例：ControlDomainService 处理
    // UPDATE_SHM_EVENT_SUBSCRIBER 刷新的是同一份订阅者缓存，动态重启的子进程
    // 才能在注册后被写帧 notify 到（否则 SET_AUTO_LOGIN 等请求会悬挂至对端超时兜底）
    auto shm_writer = std::make_shared<dztrader::webui::ShmWriter>(event_writer);

    // NOTE: 启动查询 (write_query_all) 移到事件监听启动之后
    // 原因: event_thread 是 SHM 信号量的消费者, 必须先启动监听再发查询
    //       否则 master/子进程的响应可能在 event_thread 启动前到达, 信号量计数被消耗但无人处理

    // 单 IO 线程 + 独立主循环：drogon setThreadNum(1) 创建 1 个 DrogonIoLoop 线程承接所有
    // REST/WS 连接回调；app().getLoop() 是独立主循环（不参与连接分发，勿用于连接相关定时器）。
    // tail 定时器等须注册到 IO 循环（getIOLoop(0)）以与 WS 回调同线程串行，避免 sessions_ 数据竞争。
    // 本重构 LogCtrl 直调 self_log_ + 写 mirror_ 依赖此串行化（HTTP 与 WS 回调同在 IO 线程）
    drogon::app().setThreadNum(1);
    drogon::app().addListener(cfg.server_listen, cfg.server_port);

    // /health 健康检查（Task 1 引入，Task 8 冒烟测试依赖）
    drogon::app().registerHandler(
        "/health",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setBody("ok");
            resp->setContentTypeString("text/plain");
            callback(resp);
        },
        {drogon::Get});

    // /api/system/info 系统信息（免认证）：返回当前后端进程名，供前端比对 logger 名
    // 用于禁用 WebUI 自身日志的实时 tail（避免反馈循环），支持重命名 dzweb.exe 部署
    drogon::app().registerHandler(
        "/api/system/info",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
            const nlohmann::json body = {
                {"process_name", dztrader::this_process::exe_stem()},
            };
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            // 用 replace error handler 容忍无效 UTF-8（项目约定）
            resp->setBody(body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            callback(resp);
        },
        {drogon::Get});

    // 注册 WebSocket 控制器
    // P2 任务①：WsController 实现 DataChangeNotifier 薄接口，先于业务 controller 构造，
    // 作为数据变更通知/踢人能力注入各 controller（取代原全局函数指针）
    // P2 任务③：出方向行情控制领域服务（md_connect/disconnect/query_md_subscriptions）
    // 在 ws_ctrl 之前构造，注入 WsController 用于 C2S 下沉
    auto md_control_domain =
        std::make_shared<dztrader::webui::MdControlDomainService>(*process_mirror, event_writer.get());
    auto ws_ctrl = std::make_shared<dztrader::webui::WsController>(
        cfg, repo, event_writer, self_log,
        *mirror_store, *md_control_domain);
    drogon::app().registerController(ws_ctrl);

    // 注册 /api/login 控制器
    // 共享 WebuiConfig 持有者: SettingsCtrl 热生效 token_ttl_sec 时 LoginCtrl 同步可见
    auto webui_cfg_holder = std::make_shared<dztrader::webui::WebuiConfig>(cfg);
    auto login_ctrl = std::make_shared<dztrader::webui::LoginCtrl>(webui_cfg_holder, repo, *ws_ctrl);
    drogon::app().registerController(login_ctrl);

    // 注册其他业务控制器（Task 3、Task 4），注入 DataChangeNotifier（数据变更通知/踢人）
    drogon::app().registerController(
        std::make_shared<dztrader::webui::UserCtrl>(repo, cfg, *ws_ctrl));
    drogon::app().registerController(
        std::make_shared<dztrader::webui::SecurityCtrl>(repo, *ws_ctrl));
    drogon::app().registerController(
        std::make_shared<dztrader::webui::MarketSourceCtrl>(
            repo, shm_writer, process_mirror, *ws_ctrl));
    drogon::app().registerController(
        std::make_shared<dztrader::webui::ProcessCtrl>(repo, process_mirror));

    // 系统设置控制器: 只读展示 dztraderd.json (master 路径固定) + 事件通道(SET_EVENT_SHM_CONFIG)
    // + webui.json (共享 webui_cfg_holder 与 LoginCtrl, token_ttl_sec 热生效)
    auto settings_ctrl = std::make_shared<dztrader::webui::SettingsCtrl>(
        repo, shm_writer,
        dztrader::paths::configs() / "dztraderd.json",
        config_path,
        webui_cfg_holder);
    drogon::app().registerController(settings_ctrl);

    // UI 通知缓存：NotifyDomainService 从 NOTIFY_UI 帧写入，
    // NotifyCtrl 的 GET /api/notifications 读取
    auto notify_cache = std::make_shared<dztrader::webui::NotifyCache>(cfg.notify_cache_size);
    drogon::app().registerController(
        std::make_shared<dztrader::webui::NotifyCtrl>(repo, notify_cache));

    // JWT 全局认证（pre-handling advice）
    // 对所有 /api/* 路由生效，跳过 /api/login 与非 /api/ 路径（如静态文件、/health）
    auto jwt_secret = cfg.jwt_secret;
    drogon::app().registerPreHandlingAdvice(
        [jwt_secret](const drogon::HttpRequestPtr& req,
                     drogon::AdviceCallback&& cb,
                     const drogon::AdviceChainCallback& ccb) {
            const std::string& path = req->getPath();
            if (path == "/api/login" || path == "/health" || path == "/api/system/info" ||
                !path.starts_with("/api/")) {
                ccb();
                return;
            }
            std::string token;
            auto auth = req->getHeader("Authorization");
            if (auth.size() >= 7 && auth.starts_with("Bearer ")) {
                token = auth.substr(7);
            }
            if (token.empty()) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k401Unauthorized);
                resp->setContentTypeString("application/json");
                resp->setBody(R"({"error":"unauthorized"})");
                cb(resp);
                return;
            }
            std::string user_id;
            if (!dztrader::webui::jwt_verify(token, jwt_secret, user_id)) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k401Unauthorized);
                resp->setContentTypeString("application/json");
                resp->setBody(R"({"error":"invalid_token"})");
                cb(resp);
                return;
            }
            req->getAttributes()->insert("user_id", user_id);
            ccb();
        });

    // ===== 领域服务装配（镜像 + 广播，行为等价搬迁自 WsController::handle_*） =====
    // LogDomainService 归并 log_control（P4 Task 6）：持有 self_log + event_writer，
    // handle_log_control 直调 LogConfig / 写 SHM / NOTIFY_UI / publish 回推
    auto log_domain =
        std::make_shared<dztrader::webui::LogDomainService>(
            *mirror_store, *ws_ctrl, *self_log, event_writer.get());

    // 日志查看与配置操作: set_level/flush 经 LogDomainService::handle_log_control 分发,
    // 进程级别由 WS 镜像(log_config)推送, 不依赖独立缓存类
    drogon::app().registerController(
        std::make_shared<dztrader::webui::LogCtrl>(repo, log_domain));
    auto control_domain =
        std::make_shared<dztrader::webui::ControlDomainService>(exe_stem, event_writer.get());
    auto process_domain =
        std::make_shared<dztrader::webui::ProcessDomainService>(*process_mirror, *ws_ctrl);
    auto md_config_domain =
        std::make_shared<dztrader::webui::MdConfigDomainService>(*repo, *process_mirror, *ws_ctrl);
    auto md_status_domain =
        std::make_shared<dztrader::webui::MdStatusDomainService>(*process_mirror, *ws_ctrl);
    auto subscription_domain =
        std::make_shared<dztrader::webui::SubscriptionDomainService>(*ws_ctrl);
    auto notify_domain =
        std::make_shared<dztrader::webui::NotifyDomainService>(*notify_cache, *ws_ctrl);
    auto shm_domain = std::make_shared<dztrader::webui::ShmDomainService>(*mirror_store, *ws_ctrl);
    auto auto_login_domain =
        std::make_shared<dztrader::webui::AutoLoginDomainService>(*mirror_store, *ws_ctrl);
    auto progress_domain =
        std::make_shared<dztrader::webui::ProgressDomainService>(*mirror_store, *ws_ctrl);

    // self log_config 镜像初值（原 WsController 构造内逻辑，契约 log：dzweb 自身纳入镜像）
    mirror_store->update(exe_stem, "log_config", self_log->current());

    // ===== 帧路由注册（必须全部在事件监听启动之前完成） =====
    // register_json：decode 在监听线程、handler 投递到 IO 线程 getIOLoop(0)（值拷贝，
    // 安全，与 REST/WS 连接回调同线程串行）
    // register_raw：在监听线程同步执行（ControlDomainService 内部投递到 IO 循环）
    dztrader::webui::FrameRouter router;
    // EventMonitor 由 main 持有并直调 start/stop（WsController 不再涉及）。
    // 生命周期：main 栈上 router 先声明、event_monitor 后声明（析构相反），
    // 监听线程在 drogon run 返回后经 event_monitor->stop() join，无线程在 router 析构后访问
    auto event_monitor = std::make_unique<dztrader::webui::EventMonitor>(shm_dir, router,
                                                                         event_writer);
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_LOG_CONFIG, true,
        [log_domain](const std::string& s, const nlohmann::json& p) {
            log_domain->on_rtn_log_config(s, p);
        });
    router.register_raw(DZ_FRAME_SHUTDOWN,
        [control_domain](const dztrader::shm::FrameView& v) {
            control_domain->on_shutdown(std::string(v.ext_inst_id()));
        });
    router.register_raw(DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER,
        [control_domain](const dztrader::shm::FrameView&) { control_domain->on_update_subscribers(); });
    // 事件通道预加载广播: 监听线程解析 payload 后随机延迟执行
    // (reader 半边就地 / writer 半边投递 IO 线程, 见 event_preloader.h)
    router.register_raw(DZ_FRAME_PRELOAD_EVENT_SHM,
        [monitor = event_monitor.get()](const dztrader::shm::FrameView& v) {
            monitor->schedule_event_shm_preload(v.payload<DzShmPreload>());
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_PROCESS_STATUS, false,
        [process_domain](const std::string&, const nlohmann::json& p) {
            process_domain->on_rtn_process_status(p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_PROCESS_CONFIG, false,
        [process_domain](const std::string&, const nlohmann::json& p) {
            process_domain->on_rtn_process_config(p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_MD_CONFIG, true,
        [md_config_domain](const std::string& s, const nlohmann::json& p) {
            md_config_domain->on_rtn_md_config(s, p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_MD_STATUS, true,
        [md_status_domain](const std::string& s, const nlohmann::json& p) {
            md_status_domain->on_rtn_md_status(s, p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_MD_SUBSCRIPTIONS, true,
        [subscription_domain](const std::string& s, const nlohmann::json& p) {
            subscription_domain->on_rtn_md_subscriptions(s, p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_NOTIFY_UI, false,
        [notify_domain](const std::string&, const nlohmann::json& p) {
            notify_domain->on_notify_ui(p);
        });
    // P2 补缺 4 帧：Shm/AutoLogin/Progress 领域服务（Task 1 定义，has_instance_id 与写端帧头一致）
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_EVENT_SHM_CONFIG, false,
        [shm_domain](const std::string&, const nlohmann::json& p) {
            shm_domain->on_rtn_event_shm_config(p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_MD_SHM_CONFIG, true,
        [shm_domain](const std::string& s, const nlohmann::json& p) {
            shm_domain->on_rtn_md_shm_config(s, p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_AUTO_LOGIN, true,
        [auto_login_domain](const std::string& s, const nlohmann::json& p) {
            auto_login_domain->on_rtn_auto_login(s, p);
        });
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_PROGRESS, true,
        [progress_domain](const std::string& s, const nlohmann::json& p) {
            progress_domain->on_rtn_progress(s, p);
        });

    // 启动事件通道监听线程（替代原 50ms 轮询的 event channel 部分）
    // EventMonitor 由 main 持有并直调（Task 8 去掉 WsController 委托链），构造见帧路由注册处
    // 监听线程阻塞在 NamedSemaphore::wait，被 master 的 notify_subscribers 唤醒后
    // 逐帧交给 FrameRouter 分发到领域服务（register_json 投递 IO 线程 / register_raw 监听线程执行）
    // 信号量名 = <exe_stem()>（无 pid 后缀，与 master 注册名一致）
    event_monitor->start();

    // 启动序列: 通过 SHM 事件通道全量查询, 构建 ProcessMirror 初始镜像
    // - write_query_all(): master 回复 RTN_LOG_CONFIG + RTN_EVENT_SHM_CONFIG + N×PROCESS_STATUS,
    //   子进程回复各自的 RTN_*_CONFIG
    // 回复经 FrameRouter 分发到领域服务，异步更新共享 MirrorStore / ProcessMirror
    // 必须在事件监听启动之后调用: 监听线程是 SHM 信号量的消费者,
    // 否则 master/子进程的响应可能在监听线程启动前到达, 信号量计数被消耗但无人处理
    // SHM init 失败时 shm_writer->is_ready() 返回 false, 跳过启动查询 (API-only 模式)
    if (shm_writer && shm_writer->is_ready()) {
        SPDLOG_INFO("building process mirror | phase=startup");
        // fire-and-forget: SHM 写入失败仅由 ShmWriter 内部记录 WARN, 不影响启动流程
        shm_writer->write_query_all();
    } else {
        SPDLOG_WARN("shm writer not ready, skip initial query");
    }

    // 静态文件服务 + SPA fallback
    namespace fs = std::filesystem;
    // 前端在 exe 同级 web/（CMake POST_BUILD 复制 dist 内容平铺到此）
    const fs::path frontend_dist = dztrader::this_process::exe_dir() / "web";
    if (fs::exists(frontend_dist / "index.html")) {
        drogon::app().setDocumentRoot(frontend_dist.string());
        drogon::app().setFileTypes({"html", "js", "css", "png", "jpg", "jpeg", "gif",
                                    "svg", "ico", "woff", "woff2", "ttf", "eot",
                                    "json", "map"});
        SPDLOG_INFO("frontend serving | path={}", frontend_dist.string());
    } else {
        SPDLOG_INFO("frontend dist not found | mode=api-only");
    }

    // SPA fallback：未匹配的 GET 请求回退到 index.html（让 Vue Router 接管客户端路由）
    // 必须在所有 API 路由注册之后注册
    drogon::app().registerHandler(
        "/{path}",
        [frontend_dist](const drogon::HttpRequestPtr&,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                        const std::string& path) {
            // 不要拦截 API 或 WebSocket 路由
            if (path.starts_with("api/")) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                resp->setBody(R"({"error":"not found"})");
                resp->setContentTypeString("application/json");
                callback(resp);
                return;
            }
            auto index = frontend_dist / "index.html";
            if (fs::exists(index)) {
                callback(drogon::HttpResponse::newFileResponse(index.string()));
            } else {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                resp->setBody("frontend not built");
                callback(resp);
            }
        },
        {drogon::Get});

    SPDLOG_INFO("webui starting | listen={}:{} shm_dir={}",
                cfg.server_listen, cfg.server_port, shm_dir.string());
    drogon::app().run();

    // drogon 退出后停止事件通道监听线程
    // dzweb 停止时不调用 unregister（master 的 on_child_exit 负责订阅者列表维护）
    event_monitor->stop();
    return 0;
}
