#include "td/td_api.h"

#include <format>
#include <string_view>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>  // DZ_NOTIFY_ERROR
#include <dztrader/data_type.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/struct.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

// ============================================================================
// td_api_config.cpp: 配置热更新编排 (设计 §5.9 日志配置热更新, §3.3 op-based 变更)
// - apply_config_change: TD_REQ_MODIFY_CONFIG op-based 变更 (副本 + 校验 + 持久化 + 回滚)
// - report_config: 上报当前 td 配置 (脱敏, RTN_TD_CONFIG)
// - report_log_config: 上报当前 log 配置
// - report_full_snapshot: 上报完整快照 (RTN_TD_CONFIG + RTN_LOG_CONFIG + per-session RTN_TD_STATUS)
// 注意: tdctp 不处理 SHM SET / 不上报 SHM 配置 (事件通道由 master 管理)
// 与 md_api_config.cpp 模式一致, 差异: TdConfig 脱敏 (to_safe_json) + 多账户 (sessions_ 状态保护)
// ============================================================================

void TdApi::apply_config_change(const TdConfigOpReq& req) {
    SPDLOG_DEBUG("td config change received | op={}", magic_enum::enum_name(req.op));

    // 1. 复制 config_ 副本, 应用 op (纯函数, 无副作用)
    //    apply_config_op 抛 std::invalid_argument 表示 op 参数非法 (重复 AddAccount /
    //    UpdateAccount 不存在 / 越界), 由编排层捕获并通知 UI
    TdConfig new_config = config_;
    try {
        apply_config_op(new_config, req);
    } catch (const std::invalid_argument& e) {
        SPDLOG_WARN("td config op rejected | reason=\"invalid_argument\" msg=\"{}\"", e.what());
        notify_ui_.error(std::format("配置变更参数非法: {}", e.what()));
        report_config();  // 上报当前旧值, 清前端 pending
        return;
    }

    // 2. 集中校验 (格式检查: account_id 唯一/非空, broker_id/user_id 非空, intervals > 0 等)
    if (auto err = validate(new_config)) {
        SPDLOG_WARN("td config change rejected | reason=\"validate_error\" msg=\"{}\"", *err);
        notify_ui_.error(std::format("配置校验失败: {}", *err));
        report_config();
        return;
    }

    // 3. 状态保护: 连接参数变更 (RemoveAccount/UpdateAccount) 在 session 活跃时拒绝
    //    (运行中的 AccountSession 不能修改连接参数, 需先 disconnect 再改)
    if (is_connection_op(req.op)) {
        auto account_id = req.params.value("account_id", "");
        if (!account_id.empty()) {
            auto* session = find_session(account_id);
            if (session != nullptr) {
                SPDLOG_WARN("td config change rejected | op={} account={} reason=session_active",
                            magic_enum::enum_name(req.op), account_id);
                notify_ui_.error(
                    std::format("账户 {} 正在连接, 请先断开再修改配置", account_id));
                report_config();
                return;
            }
        }
    }

    // 4. 持久化到 config_path_ 的 td section (先持久化, 成功后再应用内存, 失败回滚)
    TdConfig old_config = config_;
    config_ = new_config;
    try {
        new_config.save(config_path_);
        SPDLOG_INFO("td config persisted | path={}", config_path_.string());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("failed to persist td config, rolling back | path={} error=\"{}\"",
                     config_path_.string(), e.what());
        config_ = old_config;
        notify_ui_.error(std::format("配置保存失败, 已回滚: {}", e.what()));
        report_config();
        return;
    }

    // 5. 审计日志: 关键字段 old->new (不记录敏感值如 password)
    // 注: enable_auto_login_logout 已随契约 auto-login 迁移（SET/RTN_AUTO_LOGIN 帧）
    if (old_config.enable_lock_mode != new_config.enable_lock_mode) {
        SPDLOG_INFO("lock_mode changed | old={} new={}",
                    old_config.enable_lock_mode, new_config.enable_lock_mode);
    }
    if (old_config.qry_account_interval_s != new_config.qry_account_interval_s) {
        SPDLOG_INFO("qry_account_interval_s changed | old={} new={}",
                    old_config.qry_account_interval_s, new_config.qry_account_interval_s);
    }
    if (old_config.qry_position_interval_s != new_config.qry_position_interval_s) {
        SPDLOG_INFO("qry_position_interval_s changed | old={} new={}",
                    old_config.qry_position_interval_s, new_config.qry_position_interval_s);
    }
    if (old_config.qry_flush_interval_ms != new_config.qry_flush_interval_ms) {
        SPDLOG_INFO("qry_flush_interval_ms changed | old={} new={}",
                    old_config.qry_flush_interval_ms, new_config.qry_flush_interval_ms);
    }
    if (old_config.accounts.size() != new_config.accounts.size()) {
        SPDLOG_INFO("accounts count changed | old={} new={}",
                    old_config.accounts.size(), new_config.accounts.size());
    }
    SPDLOG_INFO("td config updated | op={}", magic_enum::enum_name(req.op));

    // 契约 account-status: 配置账户集变化 (含盘中新加未连接账户) -> 全量重推 2018,
    // 无会话账户推 Offline, 让策略立即感知新账户存在且未登录
    // (删配置账户走 disconnect 路径已有 Offline 补推, 此处全量重推幂等无害)
    report_account_status_all();

    // 6. 成功路径必须上报 RTN_TD_CONFIG (脱敏), 让 dzweb 镜像更新
    //    契约: REQ (TD_REQ_MODIFY_CONFIG) 必须有对应 RTN (TD_RTN_CONFIG)
    //    注: dzweb 当前未注册 TD 配置帧（TD 领域尚未接入契约 webui-ws 镜像模型），
    //    本帧为 TD 配置变更的权威回报，供未来 TD 领域消费
    report_config();
}

void TdApi::report_config() {
    // 上报当前 td 配置 (脱敏: broker.password -> "****"), 推 RTN_TD_CONFIG
    try {
        platform::write_ext_inst_json_obj(event_writer_, DZ_FRAME_TD_RTN_CONFIG, name_, config_.to_safe_json());
    } catch (const dztrader::Exception& e) {
        SPDLOG_ERROR("rtn_td_config failed | err_code={} msg=\"{}\"", e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("rtn_td_config failed | error=\"{}\"", e.what());
    }
}

void TdApi::report_log_config() {
    log_config_.rtn_log_config(event_writer_);
}

void TdApi::report_auto_login() {
    try {
        auto_login_config_.rtn_auto_login();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("rtn_auto_login failed | error=\"{}\"", e.what());
    }
}

void TdApi::report_full_snapshot() {
    // 设计 §5.9: 上报完整快照, 让 dzweb 冷启动时填充镜像
    // 由 run() 启动和 DZ_FRAME_QUERY_FULL_SNAPSHOT 帧触发
    // 顺序: RTN_TD_CONFIG (脱敏) + RTN_LOG_CONFIG + RTN_AUTO_LOGIN + per-session RTN_TD_STATUS + RTN_PROGRESS
    // 注意: tdctp 不上报 SHM 配置 (事件通道由 master 管理)
    // per-session RTN_TD_STATUS 由 report_state() 转发
    report_config();
    report_log_config();
    report_auto_login();
    report_state();
    // 契约 account-status: 快照含账户三态 (无 session 账户推 Offline), 冷启动镜像一次到位
    report_account_status_all();
    SPDLOG_DEBUG("td full snapshot reported | name={}", name_);
}

}  // namespace dztrader::ctp
