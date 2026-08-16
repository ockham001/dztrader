#include "md/md_api.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include <dztrader/data_type.h>
#include <dztrader/struct.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/frame_codec.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

// md_api_config.cpp: 配置变更处理
// - apply_config_change: 主入口, 编排 状态保护/持久化/审计/上报
// - set_md_config (平台库): 内部处理 validate_op_req -> apply + D-C5 -> validate_config -> save -> commit
// - report_config: 上报当前配置到 UI (RTN_MD_CONFIG)
// - find_current_broker: 在 ctp_md_config_.config().brokers 中查找 current_broker_name

const dztrader::platform::CtpBrokerEntry* MdApi::find_current_broker() const {
    return find_current_broker_in(ctp_md_config_.config().brokers,
                                  ctp_md_config_.config().current_broker_name);
}

void MdApi::report_config() {
    // 契约 md-config: RTN_MD_CONFIG payload 始终为纯脱敏 CtpMdConfigData JSON, 无 error 字段。
    // 失败原因通过 NOTIFY_UI 传达 (错误级别弹窗), 清前端 pending 只需回 RTN (旧值)。
    try {
        platform::write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_MD_CONFIG, name_,
            dztrader::platform::ctp_config_to_safe_json(ctp_md_config_.config()));
    } catch (const Exception& e) {
        SPDLOG_ERROR("rtn_config failed | err_code={} msg=\"{}\"", e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("rtn_config failed | error=\"{}\"", e.what());
    }
}

void MdApi::apply_config_change(const dztrader::platform::CtpMdConfigOpReq& req) {
    SPDLOG_DEBUG("config change received | op={}", magic_enum::enum_name(req.op));

    // 状态保护 (per-op: 连接参数变更在非 Idle 时拒绝)
    if (dztrader::platform::is_ctp_connection_op(req.op) && state_machine_.state() != MdState::Idle) {
        SPDLOG_WARN("config change rejected | op={} state={}", magic_enum::enum_name(req.op),
                    magic_enum::enum_name(state_machine_.state()));
        // 契约 md-config: 状态保护拒绝属失败, 必须 NOTIFY_UI 错误级别弹窗 + 回 RTN (旧值, 无 error 字段)
        notify_ui_.error("请先断开行情再修改连接配置");
        report_config();
        return;
    }

    // 保存旧值用于审计
    auto old_cfg = ctp_md_config_.config();

    // 应用变更 (内部: validate_op_req -> apply + D-C5 -> validate_config -> save -> commit)
    // 失败抛异常, cfg_ 不变 (强保证)
    ctp_md_config_.set_md_config(req);

    // 审计日志: 关键字段 old->new (不记录敏感值)
    const auto& new_cfg = ctp_md_config_.config();
    if (old_cfg.subscribe_batch_size != new_cfg.subscribe_batch_size) {
        SPDLOG_INFO("subscribe_batch_size changed | old={} new={}",
                    old_cfg.subscribe_batch_size, new_cfg.subscribe_batch_size);
    }
    if (old_cfg.brokers.size() != new_cfg.brokers.size()) {
        SPDLOG_INFO("brokers count changed | old={} new={}",
                    old_cfg.brokers.size(), new_cfg.brokers.size());
    }
    if (old_cfg.current_broker_name != new_cfg.current_broker_name) {
        SPDLOG_INFO("current_broker changed | old=\"{}\" new=\"{}\"",
                    old_cfg.current_broker_name, new_cfg.current_broker_name);
    }
    // 审计每个 broker 的连接参数变更 (不记录 password 值)
    for (const auto& old_b : old_cfg.brokers) {
        for (const auto& new_b : new_cfg.brokers) {
            if (old_b.name != new_b.name) continue;
            if (old_b.broker_id != new_b.broker_id) {
                SPDLOG_INFO("broker_id changed | broker=\"{}\" old=\"{}\" new=\"{}\"",
                            old_b.name, old_b.broker_id, new_b.broker_id);
            }
            if (old_b.user_id != new_b.user_id) {
                SPDLOG_INFO("user_id changed | broker=\"{}\" old=\"{}\" new=\"{}\"",
                            old_b.name, old_b.user_id, new_b.user_id);
            }
            if (old_b.product_info != new_b.product_info) {
                SPDLOG_INFO("product_info changed | broker=\"{}\" old=\"{}\" new=\"{}\"",
                            old_b.name, old_b.product_info, new_b.product_info);
            }
            if (old_b.password != new_b.password) {
                SPDLOG_INFO("password changed | broker=\"{}\"", old_b.name);  // 不记录值
            }
            if (old_b.frontends.size() != new_b.frontends.size()) {
                SPDLOG_INFO("frontends count changed | broker=\"{}\" old={} new={}",
                            old_b.name, old_b.frontends.size(), new_b.frontends.size());
            }
            break;
        }
    }
    SPDLOG_INFO("config updated | op={}", magic_enum::enum_name(req.op));

    // 成功路径必须上报 DZ_FRAME_RTN_MD_CONFIG, 让 dzweb 镜像更新
    report_config();
}

}  // namespace dztrader::ctp
