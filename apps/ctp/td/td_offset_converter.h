#ifndef DZTRADER_CTP_TD_OFFSET_CONVERTER_H_
#define DZTRADER_CTP_TD_OFFSET_CONVERTER_H_

#include <cstdint>
#include <string>
#include <vector>

#include <dztrader/core/core_struct.h>  // DzOrderReq
#include <dztrader/data_type.h>          // DzDirection/DzPositionEffect/DzDate

#include "td/td_position.h"  // PositionHolding (聚合持仓模型) + Exchange

namespace dztrader::ctp {

// ============================================================================
// OffsetConverter + OffsetConvertMode + OrderSubRequest
// ============================================================================

/// 偏移转换模式 (决定 convert_order_request 的行为).
enum class OffsetConvertMode : int8_t {
    None,  // 不转换, 直接透传 (CFFEX/DCE/CZCE/GFEX 默认)
    Shfe,  // SHFE/INE 平今昨拆分
    Lock,  // 锁仓模式 (Close -> 反向开仓)
    Net,   // 净持仓模式 (行为同 None, 语义标记供未来扩展)
};

/// 拆分后的子订单请求 (AccountSession 转换为 CTP InputOrderField).
struct OrderSubRequest {
    DzDirection direction;
    DzPositionEffect position_effect;
    DzVolume volume;
    double price;
};

/// OffsetConverter: 根据持仓状态和模式转换订单请求.
/// 无状态 (仅依赖 PositionHolding 的当前快照), 线程安全 (只读).
class OffsetConverter {
public:
    /// 转换订单请求为子订单列表.
    /// - None/Net: 原样返回单条子订单
    /// - Shfe: CLOSE 拆分为 CLOSE_TODAY + CLOSE_YESTERDAY (按可用持仓)
    /// - Lock: CLOSE 转为 OPEN (反向开仓), 其他原样返回
    /// 返回空 vector 表示无法拆分 (持仓不足). 调用方应记录并通知.
    static std::vector<OrderSubRequest> convert_order_request(
        const DzOrderReq& req, const PositionHolding& holding, OffsetConvertMode mode);
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_OFFSET_CONVERTER_H_
