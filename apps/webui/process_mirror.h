#ifndef DZTRADER_WEBUI_PROCESS_MIRROR_H_
#define DZTRADER_WEBUI_PROCESS_MIRROR_H_

#include <dztrader/platform/process.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include "mirror_store.h"

namespace dztrader::webui {

/// 进程镜像:在 dzweb 内存中维护所有子进程的状态和详情快照
/// 启动序列通过 QUERY_ALL 触发 master 和子进程上报, WsController 收到
/// RTN_PROCESS_STATUS / RTN_PROCESS_CONFIG / RTN_MD_CONFIG / RTN_MD_STATUS 帧时调用 update_* 刷新镜像
/// 线程安全: 依赖 dzweb 固定单线程事件循环(thread_num=1), 所有访问均发生在主循环串行执行,
/// 故不加锁。若未来引入多线程必须重新评估加锁。
///
/// 视图化 (P1 Task 4): 内部数据不再自持, 而是 MirrorStore 的强类型视图——
/// 写入方法 (update_*) 写 MirrorStore (domain key: process_status/md_config/md_status/process_config),
/// 查询方法从镜像解析回强类型。默认构造自持有 owned_, 生产装配注入共享 MirrorStore。
/// 含引用成员, 禁拷贝/移动 (Rule of Five)。
class ProcessMirror {
public:
    /// 默认构造: 自包含镜像 (测试用, 保持既有调用点零改动)
    ProcessMirror();
    /// 注入共享镜像 (生产用, 与领域服务共用数据)
    explicit ProcessMirror(MirrorStore& mirror);

    // Rule of Five: 含引用成员, 禁拷贝/移动
    ProcessMirror(const ProcessMirror&) = delete;
    ProcessMirror& operator=(const ProcessMirror&) = delete;
    ProcessMirror(ProcessMirror&&) = delete;
    ProcessMirror& operator=(ProcessMirror&&) = delete;

    /// 更新单个进程的状态 (RTN_PROCESS_STATUS 帧到达时调用)
    void update_status(const std::string& name, const platform::ProcessStatus& status);

    /// 更新单个进程的 config (RTN_MD_CONFIG 帧到达时调用)
    /// config 已由子进程 to_safe_json() 脱敏, dzweb 不再手动脱敏
    void update_config(const std::string& name, const nlohmann::json& config);

    /// 更新单个进程的 gateway_status (RTN_MD_STATUS 帧到达时调用)
    void update_gateway_status(const std::string& name, const nlohmann::json& status);

    /// 标记进程镜像为 stale (进程停止/崩溃时调用)
    /// 保留 status (state=stopped/crashed) 但清除 config/gateway_status (避免 stale 残留),
    /// 下次 RTN_MD_CONFIG/RTN_MD_STATUS 到达时自动覆盖 (process_supervisor 重启场景)
    /// list API 会过滤掉 stale 记录 (避免 UI 显示已停止进程的卡片)
    void mark_stale(const std::string& name);

    /// 获取所有进程的状态列表 (GET /api/processes 调用)
    std::vector<platform::ProcessStatus> get_all() const;

    /// 获取指定进程的状态 (RTN_PROCESS_STATUS 帧填充)
    /// 用于 SHM 下发前的在线检查: 进程启动时 RTN_PROCESS_STATUS (state=running) 先到
    std::optional<platform::ProcessStatus> get_status(const std::string& name) const;

    /// 更新进程配置全量镜像 (RTN_PROCESS_CONFIG 118 全量覆盖, 覆盖天然含删除)
    void update_process_configs(const nlohmann::json& full_map);

    /// 获取某进程配置 (未注册返回 nullopt)
    std::optional<nlohmann::json> get_process_config(const std::string& name) const;

    /// 获取指定进程的 config (RTN_MD_CONFIG 帧填充)
    std::optional<nlohmann::json> get_config(const std::string& name) const;

    /// 获取指定进程的 gateway_status (RTN_MD_STATUS 帧填充)
    std::optional<nlohmann::json> get_gateway_status(const std::string& name) const;

    /// 获取指定进程的自动登录排程 (RTN_AUTO_LOGIN 帧填充, 契约 auto-login)
    std::optional<nlohmann::json> get_auto_login(const std::string& name) const;

    /// 移除进程的镜像数据 (进程停止/崩溃时调用,避免 stale 数据长期保留)
    void remove(const std::string& name);

    /// 清空镜像 (dzweb 重启或 master 重连时调用)
    void clear();

private:
    MirrorStore owned_;    // 默认构造自持有 (成员顺序: owned_ 必须先于 mirror_ 声明)
    MirrorStore& mirror_;  // 默认绑 owned_, 注入时绑外部
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_PROCESS_MIRROR_H_
