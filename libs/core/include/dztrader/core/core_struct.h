#ifndef DZTRADER_CORE_CORE_STRUCT_H
#define DZTRADER_CORE_CORE_STRUCT_H

#include <string>
#include <nlohmann/json.hpp>

#include <dztrader/struct.h>
#include <dztrader/data_type.h>

DZ_BEGIN_C_DECLS

DZ_DECLARE_ALIGNED_STRUCT(DzLogicalPosition, {
    DzAccountId account_id;
    DzInstrumentId instrument_id;
    DzStrategyId strategy_id;
    int32_t net_volume;
    char reserved[4];
});

DZ_DECLARE_ALIGNED_STRUCT(DzOrderReq, {
    DzOrderId order_id;
    DzStrategyId strategy_id;
    DzAccountId account_id;
    DzInstrumentId instrument_id;
    DzOrderRemark remark;
    double price;
    DzVolume volume;
    DzDirection direction;
    DzPriceType price_type;
    DzPositionEffect position_effect;
    char reserved[1];
});

DZ_DECLARE_ALIGNED_STRUCT(DzOrderCancelReq, {
    DzOrderId order_id;
    DzAccountId account_id;  ///< 账户标识（basic 帧下撤单路由依据，契约 12-td-order）
});

DZ_END_C_DECLS

namespace dztrader {
enum class SubscribeAction : int {
    Subscribe = 0,
    Unsubscribe = 1,
    UnsubscribeAll = 2,
};

NLOHMANN_JSON_SERIALIZE_ENUM(SubscribeAction,
                             {
                                 {SubscribeAction::Subscribe, 0},
                                 {SubscribeAction::Unsubscribe, 1},
                                 {SubscribeAction::UnsubscribeAll, 2},
                             })

struct SubscribeReq {
    std::string instance_id;
    SubscribeAction action = SubscribeAction::Subscribe;
    bool replace = false;
    std::vector<std::string> instruments;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SubscribeReq, instance_id, action, replace, instruments)
};

}  // namespace dztrader


#endif  // DZTRADER_CORE_CORE_STRUCT_H