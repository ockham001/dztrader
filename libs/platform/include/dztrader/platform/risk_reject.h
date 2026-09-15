#ifndef DZTRADER_PLATFORM_RISK_REJECT_H_
#define DZTRADER_PLATFORM_RISK_REJECT_H_

// 风控拒绝通知 payload（帧 DZ_FRAME_TD_RISK_REJECT，契约 td-risk-reject）:
// td 风控门拒绝委托时写的事件通道广播帧，单向通知、无 RTN。
// JSON 编码（无 instance_id 的 DzExtFrameHeader ext 帧）: 用 platform::write_ext_json 写、写端
// 与读端共用本头结构体；接收方按 payload account_id 过滤（账户 ID 全局唯一）。
//
// 字段 schema 的真相源是契约 td-risk-reject；改动必须同步契约与
// libs/platform/tests/risk_reject_test.cpp 的 GoldenJson 断言。

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace dztrader::platform {

/// 风控拒绝通知。JSON key 顺序 = 字段声明顺序（GoldenJson 锁定）。
struct DzRiskReject {
    /// 被拒请求所属账户 ID
    std::string account_id;
    /// 触发的规则名（如 "max_order_volume"）
    std::string rule_name;
    /// 拒绝原因（展示文本，UTF-8）
    std::string reason;
    /// Unix 纳秒时间戳；前端 JS 有效整数上限 2^53，dzweb 转发需转 ms 或字符串
    int64_t timestamp_ns = 0;

    /// to_json/from_json 由 intrusive 宏生成（必须写在结构体内）:
    /// from_json 为 WITH_DEFAULT——缺字段回退默认值、整体 null 解码为全默认结构（不抛错），
    /// 故接线时 handler 必须显式校验必填字段，不得依赖解码错误。
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DzRiskReject, account_id, rule_name, reason,
                                                timestamp_ns)
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_RISK_REJECT_H_
