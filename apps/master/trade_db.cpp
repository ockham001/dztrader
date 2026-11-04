#include "trade_db.h"

#include <filesystem>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/path.h>
#include <dztrader/shm/order_id_meta.h>

namespace dztrader::master {

TradeDbStartupResult initialize_trade_db() {
    TradeDbStartupResult result;
    const std::filesystem::path db_path = paths::trade_db();
    result.init = tdstore::ensure_trade_db(db_path);
    result.db_max_order_id = tdstore::read_max_order_id(db_path);

    // 全平台共享一个 order_id 计数器（策略与 td 同用 CHANNEL_NAME_ORDER_ID）。
    // master 先于子进程启动播种；ensure_at_least 单调不减，重复启动/多进程调用幂等。
    shm::OrderIdMeta counter =
        shm::OrderIdMeta::open_or_create(dztrader::CHANNEL_NAME_ORDER_ID, paths::shm());
    result.counter_before = counter.current();
    (void)counter.ensure_at_least(static_cast<DzOrderId>(result.db_max_order_id + 1));
    result.counter_after = counter.current();
    return result;
}

}  // namespace dztrader::master
