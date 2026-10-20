#include <dztrader/core/exception.h>
#include <dztrader/core/last_error.h>

#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <atomic>

#include <gtest/gtest.h>

using dztrader::Exception;
using dztrader::LastError;

// ── LastError 基本操作 ──

TEST(LastError, InitialStateIsOk) {
    LastError::clear();
    EXPECT_EQ(LastError::code(), DZ_EC_OK);
    EXPECT_STREQ(LastError::msg(), "");
}

TEST(LastError, SetWithNoArgs) {
    LastError::set(DZ_EC_INTERNAL, "something failed");
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_STREQ(LastError::msg(), "something failed");
}

TEST(LastError, SetWithFormatArgs) {
    LastError::set(DZ_EC_SHM_OPEN_FAILED, "path={} size={}", "/tmp/dz_shm", 4096);
    EXPECT_EQ(LastError::code(), DZ_EC_SHM_OPEN_FAILED);
    EXPECT_STREQ(LastError::msg(), "path=/tmp/dz_shm size=4096");
}

TEST(LastError, SetOverwritesPrevious) {
    LastError::set(DZ_EC_INTERNAL, "first");
    LastError::set(DZ_EC_SYSTEM, "second");
    EXPECT_EQ(LastError::code(), DZ_EC_SYSTEM);
    EXPECT_STREQ(LastError::msg(), "second");
}

TEST(LastError, ClearResetsToOk) {
    LastError::set(DZ_EC_INTERNAL, "error");
    LastError::clear();
    EXPECT_EQ(LastError::code(), DZ_EC_OK);
    EXPECT_STREQ(LastError::msg(), "");
}

TEST(LastError, SetEmptyFmt) {
    LastError::set(DZ_EC_OK, "");
    EXPECT_EQ(LastError::code(), DZ_EC_OK);
    EXPECT_STREQ(LastError::msg(), "");
}

// ── LastError 截断 ──

TEST(LastError, TruncatesLongMessage) {
    const std::string long_msg(3000, 'x');
    LastError::set(DZ_EC_INTERNAL, long_msg);
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
    EXPECT_EQ(std::string(LastError::msg(), 100), std::string(100, 'x'));
}

TEST(LastError, TruncatesLongFormattedMessage) {
    const std::string long_val(3000, 'y');
    LastError::set(DZ_EC_INTERNAL, "val={}", long_val);
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
}

// ── LastError str() 映射 ──

TEST(LastError, StrOk) { EXPECT_STREQ(LastError::str(DZ_EC_OK), "ok"); }

TEST(LastError, StrKnownCodes) {
    EXPECT_STREQ(LastError::str(DZ_EC_INTERNAL), "internal error");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_OPEN_FAILED), "shm open failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_MAP_FAILED), "shm map failed");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_SOURCE_NOT_CONFIGURED), "md source not configured");
}

TEST(LastError, StrUnknownCode) {
    EXPECT_STREQ(LastError::str(static_cast<DzErrorCode>(-99999)), "unknown error");
}

TEST(LastError, StrNeverReturnsNull) {
    EXPECT_NE(LastError::str(DZ_EC_OK), nullptr);
    EXPECT_NE(LastError::str(static_cast<DzErrorCode>(-99999)), nullptr);
}

// ── LastError 恰好 2047 字节边界 ──

TEST(LastError, Exact2047ByteMessage) {
    const std::string exact_2047(2047, 'a');
    LastError::set(DZ_EC_INTERNAL, exact_2047);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
    EXPECT_EQ(std::string(LastError::msg()), exact_2047);
}

TEST(LastError, Exact2048ByteMessageTruncated) {
    const std::string exact_2048(2048, 'b');
    LastError::set(DZ_EC_INTERNAL, exact_2048);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
    EXPECT_EQ(std::string(LastError::msg(), 2047), std::string(2047, 'b'));
}

// ── LastError str() 全码覆盖 ──

namespace {

struct CodeMapping {
    DzErrorCode code;
    const char* text;
};

/// error.h 中全部 79 个 DZ_EC_* 的映射。新增/删除错误码时必须同步本表
/// （static_assert 会在数量变化时先失败）。
constexpr CodeMapping kAllCodeMappings[] = {
    {DZ_EC_OK, "ok"},
    {DZ_EC_INTERNAL, "internal error"},
    {DZ_EC_INVALID_PARAM, "invalid param"},
    {DZ_EC_NULL_PTR, "null pointer"},
    {DZ_EC_BUFFER_TOO_SMALL, "buffer too small"},
    {DZ_EC_NOT_FOUND, "not found"},
    {DZ_EC_ALREADY_EXISTS, "already exists"},
    {DZ_EC_PERMISSION_DENIED, "permission denied"},
    {DZ_EC_TIMEOUT, "timeout"},
    {DZ_EC_SYSTEM, "system error"},
    {DZ_EC_NOT_IMPLEMENTED, "not implemented"},
    {DZ_EC_SHM_CREATE_FAILED, "shm create failed"},
    {DZ_EC_SHM_OPEN_FAILED, "shm open failed"},
    {DZ_EC_SHM_MAP_FAILED, "shm map failed"},
    {DZ_EC_SHM_UNMAP_FAILED, "shm unmap failed"},
    {DZ_EC_SHM_FILE_NOT_FOUND, "shm file not found"},
    {DZ_EC_SHM_FILE_TOO_SMALL, "shm file too small"},
    {DZ_EC_SHM_ALIGN_ERROR, "shm align error"},
    {DZ_EC_SHM_WRITE_OVERFLOW, "shm write overflow"},
    {DZ_EC_SHM_READ_INVALID, "shm read invalid"},
    {DZ_EC_SHM_CHANNEL_CLOSED, "shm channel closed"},
    {DZ_EC_SHM_CHANNEL_FULL, "shm channel full"},
    {DZ_EC_SHM_LOCK_FAILED, "shm lock failed"},
    {DZ_EC_SHM_LOCK_TIMEOUT, "shm lock timeout"},
    {DZ_EC_SHM_LOCK_ABANDONED, "shm lock abandoned"},
    {DZ_EC_SHM_SEM_CREATE_FAILED, "sem create failed"},
    {DZ_EC_SHM_SEM_WAIT_FAILED, "sem wait failed"},
    {DZ_EC_SHM_SEM_POST_FAILED, "sem post failed"},
    {DZ_EC_SHM_FRAME_INVALID, "frame invalid"},
    {DZ_EC_SHM_FRAME_SIZE_MISMATCH, "frame size mismatch"},
    {DZ_EC_SHM_VERSION_MISMATCH, "shm version mismatch"},
    {DZ_EC_SHM_INIT_FAILED, "shm init failed"},
    {DZ_EC_SHM_WRITE_FAILED, "shm write failed"},
    {DZ_EC_SHM_NO_SPACE, "shm no space"},
    {DZ_EC_SHM_SUBSCRIBER_FAILED, "shm subscriber failed"},
    {DZ_EC_SHM_PROCESS_LIST_FULL, "shm process list full"},
    {DZ_EC_SHM_SEM_OPEN_FAILED, "sem open failed"},
    {DZ_EC_SHM_FILE_REMOVE_FAILED, "shm file remove failed"},
    {DZ_EC_SHM_ORDER_ID_ACCESS_FAILED, "order id access failed"},
    {DZ_EC_STRATEGY_ALREADY_INITIALIZED, "strategy already initialized"},
    {DZ_EC_STRATEGY_INIT_FAILED, "strategy init failed"},
    {DZ_EC_STRATEGY_RELEASE_FAILED, "strategy release failed"},
    {DZ_EC_STRATEGY_NOTIFY_FAILED, "strategy notify failed"},
    {DZ_EC_STRATEGY_HOME_NOT_FOUND, "strategy home not found"},
    {DZ_EC_TIMER_NOT_FOUND, "timer not found"},
    {DZ_EC_TRADE_INSERT_FAILED, "trade insert failed"},
    {DZ_EC_TRADE_CANCEL_FAILED, "trade cancel failed"},
    {DZ_EC_TRADE_QUERY_FAILED, "trade query failed"},
    {DZ_EC_TRADE_ORDER_REJECTED, "order rejected"},
    {DZ_EC_TRADE_ORDER_NOT_FOUND, "order not found"},
    {DZ_EC_TRADE_ACCOUNT_NOT_FOUND, "account not found"},
    {DZ_EC_TRADE_ACCOUNT_DISABLED, "account disabled"},
    {DZ_EC_TRADE_INSUFFICIENT_MARGIN, "insufficient margin"},
    {DZ_EC_TRADE_INSUFFICIENT_VOLUME, "insufficient volume"},
    {DZ_EC_TRADE_PRICE_INVALID, "price invalid"},
    {DZ_EC_TRADE_VOLUME_INVALID, "volume invalid"},
    {DZ_EC_TRADE_POSITION_INVALID, "position invalid"},
    {DZ_EC_TRADE_GATEWAY_NOT_CONNECTED, "gateway not connected"},
    {DZ_EC_TRADE_GATEWAY_ERROR, "gateway error"},
    {DZ_EC_TRADE_RISK_LIMIT, "risk limit"},
    {DZ_EC_MD_SUBSCRIBE_FAILED, "subscribe failed"},
    {DZ_EC_MD_UNSUBSCRIBE_FAILED, "unsubscribe failed"},
    {DZ_EC_MD_QUERY_FAILED, "md query failed"},
    {DZ_EC_MD_INSTRUMENT_NOT_FOUND, "instrument not found"},
    {DZ_EC_MD_GATEWAY_NOT_CONNECTED, "md gateway not connected"},
    {DZ_EC_MD_GATEWAY_ERROR, "md gateway error"},
    {DZ_EC_MD_NO_DATA, "no data"},
    {DZ_EC_MD_SOURCE_NOT_CONFIGURED, "md source not configured"},
    {DZ_EC_MASTER_LOCK_FAILED, "master lock failed"},
    {DZ_EC_MASTER_ALREADY_RUNNING, "master already running"},
    {DZ_EC_MASTER_DB_FAILED, "master db failed"},
    {DZ_EC_DB_OPEN_FAILED, "db open failed"},
    {DZ_EC_DB_CLOSED, "db closed"},
    {DZ_EC_DB_QUERY_FAILED, "db query failed"},
    {DZ_EC_DB_WRITE_FAILED, "db write failed"},
    {DZ_EC_DB_MIGRATE_FAILED, "db migrate failed"},
    {DZ_EC_DB_UNSUPPORTED_BACKEND, "db unsupported backend"},
    {DZ_EC_DB_UNSUPPORTED_CAPABILITY, "db unsupported capability"},
    {DZ_EC_DB_TRANSACTION_FAILED, "db transaction failed"},
};

static_assert(std::size(kAllCodeMappings) == 79,
              "error.h DZ_EC_* count changed: sync kAllCodeMappings");

}  // namespace

TEST(LastError, StrMatchesEveryDefinedCode) {
    for (const auto& mapping : kAllCodeMappings) {
        SCOPED_TRACE(::testing::Message()
                     << "code=" << mapping.code << " expected=" << mapping.text);
        EXPECT_STREQ(LastError::str(mapping.code), mapping.text);
    }
}

// ── LastError 格式化降级 ──

TEST(LastError, FormatFallbackOnBadFormat) {
    LastError::set(DZ_EC_INTERNAL, "bad={", 42);
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_STREQ(LastError::msg(), "bad={");
}

// ── LastError 线程独立性 ──

TEST(LastError, ThreadLocalIsolation) {
    LastError::set(DZ_EC_INTERNAL, "main_thread");
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);

    std::thread t([] {
        // 子线程初始状态应为 OK
        EXPECT_EQ(LastError::code(), DZ_EC_OK);
        LastError::set(DZ_EC_SYSTEM, "child_thread");
        EXPECT_EQ(LastError::code(), DZ_EC_SYSTEM);
    });
    t.join();

    // 主线程不受影响
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_STREQ(LastError::msg(), "main_thread");
}

// ── Exception 基本操作 ──

TEST(Exception, ConstructAndReadCode) {
    Exception ex(DZ_EC_SHM_MAP_FAILED, "path=/tmp/dz size=4096");
    EXPECT_EQ(ex.code(), DZ_EC_SHM_MAP_FAILED);
    EXPECT_STREQ(ex.what(), "path=/tmp/dz size=4096");
}

TEST(Exception, WhatContainsMessage) {
    const char* msg = "name=ch_a";
    Exception ex(DZ_EC_SHM_OPEN_FAILED, msg);
    EXPECT_STREQ(ex.what(), msg);
}

TEST(Exception, IsRuntimeError) {
    Exception ex(DZ_EC_INTERNAL, "test");
    EXPECT_THROW(throw ex, std::runtime_error);
}
