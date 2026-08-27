#include <dztrader/core/exception.h>
#include <dztrader/core/last_error.h>

#include <cstring>
#include <string>
#include <thread>
#include <atomic>

#include <gtest/gtest.h>

using dztrader::Exception;
using dztrader::LastError;

// ── LastError 基本操作 ──

TEST(LastError, InitialStateIsOk)
{
    LastError::clear();
    EXPECT_EQ(LastError::code(), DZ_EC_OK);
    EXPECT_STREQ(LastError::msg(), "");
}

TEST(LastError, SetWithNoArgs)
{
    LastError::set(DZ_EC_INTERNAL, "something failed");
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_STREQ(LastError::msg(), "something failed");
}

TEST(LastError, SetWithFormatArgs)
{
    LastError::set(DZ_EC_SHM_OPEN_FAILED, "path={} size={}", "/tmp/dz_shm", 4096);
    EXPECT_EQ(LastError::code(), DZ_EC_SHM_OPEN_FAILED);
    EXPECT_STREQ(LastError::msg(), "path=/tmp/dz_shm size=4096");
}

TEST(LastError, SetOverwritesPrevious)
{
    LastError::set(DZ_EC_INTERNAL, "first");
    LastError::set(DZ_EC_TIMEOUT, "second");
    EXPECT_EQ(LastError::code(), DZ_EC_TIMEOUT);
    EXPECT_STREQ(LastError::msg(), "second");
}

TEST(LastError, ClearResetsToOk)
{
    LastError::set(DZ_EC_INTERNAL, "error");
    LastError::clear();
    EXPECT_EQ(LastError::code(), DZ_EC_OK);
    EXPECT_STREQ(LastError::msg(), "");
}

TEST(LastError, SetEmptyFmt)
{
    LastError::set(DZ_EC_OK, "");
    EXPECT_EQ(LastError::code(), DZ_EC_OK);
    EXPECT_STREQ(LastError::msg(), "");
}

// ── LastError 截断 ──

TEST(LastError, TruncatesLongMessage)
{
    const std::string long_msg(3000, 'x');
    LastError::set(DZ_EC_INTERNAL, long_msg);
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
    EXPECT_EQ(std::string(LastError::msg(), 100), std::string(100, 'x'));
}

TEST(LastError, TruncatesLongFormattedMessage)
{
    const std::string long_val(3000, 'y');
    LastError::set(DZ_EC_INTERNAL, "val={}", long_val);
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
}

// ── LastError str() 映射 ──

TEST(LastError, StrOk)
{
    EXPECT_STREQ(LastError::str(DZ_EC_OK), "ok");
}

TEST(LastError, StrKnownCodes)
{
    EXPECT_STREQ(LastError::str(DZ_EC_INTERNAL), "internal error");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_OPEN_FAILED), "shm open failed");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_INSERT_FAILED), "trade insert failed");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_SUBSCRIBE_FAILED), "subscribe failed");
}

TEST(LastError, StrUnknownCode)
{
    EXPECT_STREQ(LastError::str(static_cast<DzErrorCode>(-99999)), "unknown error");
}

TEST(LastError, StrNeverReturnsNull)
{
    EXPECT_NE(LastError::str(DZ_EC_OK), nullptr);
    EXPECT_NE(LastError::str(static_cast<DzErrorCode>(-99999)), nullptr);
}

// ── LastError 恰好 2047 字节边界 ──

TEST(LastError, Exact2047ByteMessage)
{
    const std::string exact_2047(2047, 'a');
    LastError::set(DZ_EC_INTERNAL, exact_2047);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
    EXPECT_EQ(std::string(LastError::msg()), exact_2047);
}

TEST(LastError, Exact2048ByteMessageTruncated)
{
    const std::string exact_2048(2048, 'b');
    LastError::set(DZ_EC_INTERNAL, exact_2048);
    EXPECT_EQ(std::strlen(LastError::msg()), 2047u);
    EXPECT_EQ(std::string(LastError::msg(), 2047), std::string(2047, 'b'));
}

// ── LastError str() 全码覆盖 ──

TEST(LastError, StrAllGeneralCodes)
{
    EXPECT_STREQ(LastError::str(DZ_EC_OK), "ok");
    EXPECT_STREQ(LastError::str(DZ_EC_INTERNAL), "internal error");
    EXPECT_STREQ(LastError::str(DZ_EC_INVALID_PARAM), "invalid param");
    EXPECT_STREQ(LastError::str(DZ_EC_NULL_PTR), "null pointer");
    EXPECT_STREQ(LastError::str(DZ_EC_BUFFER_TOO_SMALL), "buffer too small");
    EXPECT_STREQ(LastError::str(DZ_EC_NOT_FOUND), "not found");
    EXPECT_STREQ(LastError::str(DZ_EC_ALREADY_EXISTS), "already exists");
    EXPECT_STREQ(LastError::str(DZ_EC_PERMISSION_DENIED), "permission denied");
    EXPECT_STREQ(LastError::str(DZ_EC_TIMEOUT), "timeout");
    EXPECT_STREQ(LastError::str(DZ_EC_SYSTEM), "system error");
}

TEST(LastError, StrAllShmCodes)
{
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_CREATE_FAILED), "shm create failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_OPEN_FAILED), "shm open failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_MAP_FAILED), "shm map failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_UNMAP_FAILED), "shm unmap failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_FILE_NOT_FOUND), "shm file not found");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_FILE_TOO_SMALL), "shm file too small");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_ALIGN_ERROR), "shm align error");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_WRITE_OVERFLOW), "shm write overflow");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_READ_INVALID), "shm read invalid");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_CHANNEL_CLOSED), "shm channel closed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_CHANNEL_FULL), "shm channel full");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_LOCK_FAILED), "shm lock failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_LOCK_TIMEOUT), "shm lock timeout");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_LOCK_ABANDONED), "shm lock abandoned");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_SEM_CREATE_FAILED), "sem create failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_SEM_WAIT_FAILED), "sem wait failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_SEM_POST_FAILED), "sem post failed");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_FRAME_INVALID), "frame invalid");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_FRAME_SIZE_MISMATCH), "frame size mismatch");
    EXPECT_STREQ(LastError::str(DZ_EC_SHM_VERSION_MISMATCH), "shm version mismatch");
}

TEST(LastError, StrAllStrategyCodes)
{
    EXPECT_STREQ(LastError::str(DZ_EC_STRATEGY_ALREADY_INITIALIZED), "strategy already initialized");
    EXPECT_STREQ(LastError::str(DZ_EC_STRATEGY_INIT_FAILED), "strategy init failed");
    EXPECT_STREQ(LastError::str(DZ_EC_STRATEGY_RELEASE_FAILED), "strategy release failed");
    EXPECT_STREQ(LastError::str(DZ_EC_STRATEGY_NOTIFY_FAILED), "strategy notify failed");
    EXPECT_STREQ(LastError::str(DZ_EC_STRATEGY_HOME_NOT_FOUND), "strategy home not found");
}

TEST(LastError, StrAllTradeCodes)
{
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_INSERT_FAILED), "trade insert failed");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_CANCEL_FAILED), "trade cancel failed");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_QUERY_FAILED), "trade query failed");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_ORDER_REJECTED), "order rejected");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_ORDER_NOT_FOUND), "order not found");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_ACCOUNT_NOT_FOUND), "account not found");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_ACCOUNT_DISABLED), "account disabled");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_INSUFFICIENT_MARGIN), "insufficient margin");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_INSUFFICIENT_VOLUME), "insufficient volume");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_PRICE_INVALID), "price invalid");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_VOLUME_INVALID), "volume invalid");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_POSITION_INVALID), "position invalid");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_GATEWAY_NOT_CONNECTED), "gateway not connected");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_GATEWAY_ERROR), "gateway error");
    EXPECT_STREQ(LastError::str(DZ_EC_TRADE_RISK_LIMIT), "risk limit");
}

TEST(LastError, StrAllMdCodes)
{
    EXPECT_STREQ(LastError::str(DZ_EC_MD_SUBSCRIBE_FAILED), "subscribe failed");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_UNSUBSCRIBE_FAILED), "unsubscribe failed");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_QUERY_FAILED), "md query failed");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_INSTRUMENT_NOT_FOUND), "instrument not found");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_GATEWAY_NOT_CONNECTED), "md gateway not connected");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_GATEWAY_ERROR), "md gateway error");
    EXPECT_STREQ(LastError::str(DZ_EC_MD_NO_DATA), "no data");
}

// ── LastError 格式化降级 ──

TEST(LastError, FormatFallbackOnBadFormat)
{
    LastError::set(DZ_EC_INTERNAL, "bad={", 42);
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_STREQ(LastError::msg(), "bad={");
}

// ── LastError 线程独立性 ──

TEST(LastError, ThreadLocalIsolation)
{
    LastError::set(DZ_EC_INTERNAL, "main_thread");
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);

    std::thread t([] {
        // 子线程初始状态应为 OK
        EXPECT_EQ(LastError::code(), DZ_EC_OK);
        LastError::set(DZ_EC_TIMEOUT, "child_thread");
        EXPECT_EQ(LastError::code(), DZ_EC_TIMEOUT);
    });
    t.join();

    // 主线程不受影响
    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_STREQ(LastError::msg(), "main_thread");
}

// ── Exception 基本操作 ──

TEST(Exception, ConstructAndReadCode)
{
    Exception ex(DZ_EC_SHM_MAP_FAILED, "path=/tmp/dz size=4096");
    EXPECT_EQ(ex.code(), DZ_EC_SHM_MAP_FAILED);
    EXPECT_STREQ(ex.what(), "path=/tmp/dz size=4096");
}

TEST(Exception, WhatContainsMessage)
{
    const char* msg = "name=ch_a";
    Exception ex(DZ_EC_SHM_OPEN_FAILED, msg);
    EXPECT_STREQ(ex.what(), msg);
}

TEST(Exception, IsRuntimeError)
{
    Exception ex(DZ_EC_INTERNAL, "test");
    EXPECT_THROW(throw ex, std::runtime_error);
}

// ── Exception → LastError 桥接 ──

TEST(Bridge, CatchExceptionSetsLastError)
{
    try {
        throw Exception(DZ_EC_SHM_CHANNEL_FULL, "name=ch_a");
    }
    catch (const Exception& e) {
        LastError::set(e.code(), "name={} err={}", "ch_a", e.what());
    }

    EXPECT_EQ(LastError::code(), DZ_EC_SHM_CHANNEL_FULL);
    const std::string m = LastError::msg();
    EXPECT_NE(m.find("name=ch_a"), std::string::npos);
    EXPECT_NE(m.find("err="), std::string::npos);
}

TEST(Bridge, CatchStdExceptionSetsLastError)
{
    try {
        throw std::runtime_error("unexpected failure");
    }
    catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, "err={}", e.what());
    }

    EXPECT_EQ(LastError::code(), DZ_EC_INTERNAL);
    EXPECT_NE(std::string(LastError::msg()).find("unexpected failure"), std::string::npos);
}
