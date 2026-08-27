/**
 * @file last_error.cpp
 * @brief 线程局部错误存储实现
 */

#include <dztrader/core/last_error.h>

namespace dztrader {

thread_local DzErrorCode LastError::code_{DZ_EC_OK};
thread_local std::array<char, 2048> LastError::msg_ = {};

const char* LastError::str(DzErrorCode code) noexcept
{
    switch (code) {
    /* ── 通用 ── */
    case DZ_EC_OK:                return "ok";
    case DZ_EC_INTERNAL:          return "internal error";
    case DZ_EC_INVALID_PARAM:     return "invalid param";
    case DZ_EC_NULL_PTR:          return "null pointer";
    case DZ_EC_BUFFER_TOO_SMALL:  return "buffer too small";
    case DZ_EC_NOT_FOUND:         return "not found";
    case DZ_EC_ALREADY_EXISTS:    return "already exists";
    case DZ_EC_PERMISSION_DENIED: return "permission denied";
    case DZ_EC_TIMEOUT:           return "timeout";
    case DZ_EC_SYSTEM:            return "system error";
    /* ── 共享内存 ── */
    case DZ_EC_SHM_CREATE_FAILED:       return "shm create failed";
    case DZ_EC_SHM_OPEN_FAILED:         return "shm open failed";
    case DZ_EC_SHM_MAP_FAILED:          return "shm map failed";
    case DZ_EC_SHM_UNMAP_FAILED:        return "shm unmap failed";
    case DZ_EC_SHM_FILE_NOT_FOUND:      return "shm file not found";
    case DZ_EC_SHM_FILE_TOO_SMALL:      return "shm file too small";
    case DZ_EC_SHM_ALIGN_ERROR:         return "shm align error";
    case DZ_EC_SHM_WRITE_OVERFLOW:      return "shm write overflow";
    case DZ_EC_SHM_READ_INVALID:        return "shm read invalid";
    case DZ_EC_SHM_CHANNEL_CLOSED:      return "shm channel closed";
    case DZ_EC_SHM_CHANNEL_FULL:        return "shm channel full";
    case DZ_EC_SHM_LOCK_FAILED:         return "shm lock failed";
    case DZ_EC_SHM_LOCK_TIMEOUT:        return "shm lock timeout";
    case DZ_EC_SHM_LOCK_ABANDONED:      return "shm lock abandoned";
    case DZ_EC_SHM_SEM_CREATE_FAILED:   return "sem create failed";
    case DZ_EC_SHM_SEM_WAIT_FAILED:     return "sem wait failed";
    case DZ_EC_SHM_SEM_POST_FAILED:     return "sem post failed";
    case DZ_EC_SHM_FRAME_INVALID:       return "frame invalid";
    case DZ_EC_SHM_FRAME_SIZE_MISMATCH: return "frame size mismatch";
    case DZ_EC_SHM_VERSION_MISMATCH:    return "shm version mismatch";
    case DZ_EC_SHM_INIT_FAILED:         return "shm init failed";
    case DZ_EC_SHM_WRITE_FAILED:        return "shm write failed";
    case DZ_EC_SHM_NO_SPACE:            return "shm no space";
    case DZ_EC_SHM_SUBSCRIBER_FAILED:   return "shm subscriber failed";
    case DZ_EC_SHM_PROCESS_LIST_FULL:   return "shm process list full";
    case DZ_EC_SHM_SEM_OPEN_FAILED:     return "sem open failed";
    case DZ_EC_SHM_FILE_REMOVE_FAILED:  return "shm file remove failed";
    /* ── 策略接口 ── */
    case DZ_EC_STRATEGY_ALREADY_INITIALIZED: return "strategy already initialized";
    case DZ_EC_STRATEGY_INIT_FAILED:         return "strategy init failed";
    case DZ_EC_STRATEGY_RELEASE_FAILED:      return "strategy release failed";
    case DZ_EC_STRATEGY_NOTIFY_FAILED:       return "strategy notify failed";
    case DZ_EC_STRATEGY_HOME_NOT_FOUND:      return "strategy home not found";
    /* ── 交易 ── */
    case DZ_EC_TRADE_INSERT_FAILED:        return "trade insert failed";
    case DZ_EC_TRADE_CANCEL_FAILED:        return "trade cancel failed";
    case DZ_EC_TRADE_QUERY_FAILED:         return "trade query failed";
    case DZ_EC_TRADE_ORDER_REJECTED:       return "order rejected";
    case DZ_EC_TRADE_ORDER_NOT_FOUND:      return "order not found";
    case DZ_EC_TRADE_ACCOUNT_NOT_FOUND:    return "account not found";
    case DZ_EC_TRADE_ACCOUNT_DISABLED:     return "account disabled";
    case DZ_EC_TRADE_INSUFFICIENT_MARGIN:  return "insufficient margin";
    case DZ_EC_TRADE_INSUFFICIENT_VOLUME:  return "insufficient volume";
    case DZ_EC_TRADE_PRICE_INVALID:        return "price invalid";
    case DZ_EC_TRADE_VOLUME_INVALID:       return "volume invalid";
    case DZ_EC_TRADE_POSITION_INVALID:     return "position invalid";
    case DZ_EC_TRADE_GATEWAY_NOT_CONNECTED: return "gateway not connected";
    case DZ_EC_TRADE_GATEWAY_ERROR:        return "gateway error";
    case DZ_EC_TRADE_RISK_LIMIT:           return "risk limit";
    /* ── 行情 ── */
    case DZ_EC_MD_SUBSCRIBE_FAILED:      return "subscribe failed";
    case DZ_EC_MD_UNSUBSCRIBE_FAILED:    return "unsubscribe failed";
    case DZ_EC_MD_QUERY_FAILED:          return "md query failed";
    case DZ_EC_MD_INSTRUMENT_NOT_FOUND:  return "instrument not found";
    case DZ_EC_MD_GATEWAY_NOT_CONNECTED: return "md gateway not connected";
    case DZ_EC_MD_GATEWAY_ERROR:         return "md gateway error";
    case DZ_EC_MD_NO_DATA:               return "no data";
    /* ── Master ── */
    case DZ_EC_MASTER_LOCK_FAILED:       return "master lock failed";
    case DZ_EC_MASTER_ALREADY_RUNNING:   return "master already running";
    case DZ_EC_MASTER_DB_FAILED:         return "master db failed";
    default: return "unknown error";
    }
}

}  // namespace dztrader
