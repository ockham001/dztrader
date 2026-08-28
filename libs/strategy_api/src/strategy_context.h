/**
 * @file strategy_context.h
 * @brief 策略运行时上下文（DzContext 内部实现，不暴露给 C API 头文件）
 */
#ifndef DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_
#define DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/string_util.h>
#include <dztrader/core/this_process.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/path.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/error.h>
#include <filesystem>
#include <format>
#include <set>
#include <string>

namespace dztrader {

/// 策略进程统一身份名: stg.<strategy_id>（无 pid 后缀, 重启复用同名, 身份稳定）。
/// 与 master make_subscriber_name (process_supervisor.cpp) 的 Strategy 分支一致,
/// 是事件通道 reader/writer/信号量名与行情订阅 instance_id 的唯一构造点。
inline std::string strategy_identity(const std::string& strategy_id) {
    return std::format("{}.{}", STRATEGY_PREFIX, strategy_id);
}

}  // namespace dztrader

/// 策略运行环境（api.h 不透明句柄 DzContext 的实现体）。
/// 全局作用域 tag: C typedef `typedef struct DzContext DzContext` 必须指向本类型。
struct DzContext {
    DzContext(const std::string& strategy_id,
              const std::string& md_source_name,
              const std::string& chn_event_name,
              const std::filesystem::path& chn_event_dir,
              const std::string& chn_order_id_name,
              const std::filesystem::path& chn_order_id_dir,
              const std::filesystem::path& chn_md_dir)
        : sem{dztrader::strategy_identity(strategy_id)},
          md_reader{dztrader::shm::Reader::create(
              checked_md_source_name(md_source_name), chn_md_dir,
              dztrader::strategy_identity(strategy_id))},
          md_source_name{},
          event_reader{dztrader::shm::Reader::create(chn_event_name, chn_event_dir,
                                                      dztrader::strategy_identity(strategy_id))},
          event_writer{dztrader::shm::MultiWriter::create(chn_event_name, chn_event_dir,
                                                           dztrader::strategy_identity(strategy_id))},
          order_id{dztrader::shm::OrderIdMeta::open_or_create(chn_order_id_name, chn_order_id_dir)} {
        dztrader::copy_string(this->md_source_name, checked_md_source_name(md_source_name).c_str(), true);
        dztrader::copy_string(this->strategy_id, strategy_id.c_str(), true);
        strategy_home = dztrader::this_process::exe_dir().string();
    }

    DzContext()
        : DzContext(dztrader::this_process::exe_stem(),
                    default_md_source(),
                    dztrader::CHANNEL_NAME_EVENT,
                    dztrader::paths::shm(),
                    dztrader::CHANNEL_NAME_ORDER_ID,
                    dztrader::paths::shm(),
                    dztrader::paths::shm()) {}

    // ── 第一热区：等待 + 行情 ────────────────────────────────
    dztrader::shm::NamedSemaphore sem;
    dztrader::shm::Reader md_reader;
    DzInstanceId md_source_name{};

    // ── 第二热区：事件读取 ──────────────────────────────────
    dztrader::shm::Reader event_reader;

    // ── 第三热区：事件写入 / 交易 ────────────────────────────
    dztrader::shm::MultiWriter event_writer;
    dztrader::shm::OrderIdMeta order_id;
    DzStrategyId strategy_id{};

    // ── 冷区 ────────────────────────────────────────────────
    std::string strategy_home;
    std::set<std::string> md_desired_instruments;

private:
    static const std::string& checked_md_source_name(const std::string& name) {
        if (name.empty()) {
            throw dztrader::Exception(DZ_EC_MD_SOURCE_NOT_CONFIGURED,
                                      "DZTRADER_MD_SOURCE is empty");
        }
        if (name.size() >= sizeof(DzInstanceId)) {
            throw dztrader::Exception(DZ_EC_INVALID_PARAM,
                                      "market source name too long (max 63)");
        }
        for (char c : name) {
            if (c == '/' || c == '\\' || (static_cast<unsigned char>(c) < 0x20)) {
                throw dztrader::Exception(DZ_EC_INVALID_PARAM,
                                          "market source name contains invalid character");
            }
        }
        return name;
    }

    static std::string default_md_source() {
        auto value = dztrader::env::get("DZTRADER_MD_SOURCE");
        if (!value || value->empty()) {
            throw dztrader::Exception(DZ_EC_MD_SOURCE_NOT_CONFIGURED,
                                      "DZTRADER_MD_SOURCE is not set");
        }
        return *value;
    }
};

#endif  // DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_
