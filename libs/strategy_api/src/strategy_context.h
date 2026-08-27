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
#include <dztrader/core/string_util.h>
#include <dztrader/core/this_process.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/path.h>
#include <dztrader/core/core_data_type.h>
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
/// 全局作用域 tag: C typedef `typedef struct DzContext DzContext` 必须指向本类型
/// （与 DzMdSource/DzResultSet 的既有模式一致）。
struct DzContext {
    DzContext(const std::string& strategy_id,
              const std::string& chn_event_name,
              const std::filesystem::path& chn_event_dir,
              const std::string& chn_order_id_name,
              const std::filesystem::path& chn_order_id_dir)
        : writer{dztrader::shm::MultiWriter::create(chn_event_name, chn_event_dir,
                                                     dztrader::strategy_identity(strategy_id))},
          reader{dztrader::shm::Reader::create(chn_event_name, chn_event_dir,
                                                dztrader::strategy_identity(strategy_id))},
          sem{dztrader::strategy_identity(strategy_id)},
          order_id{dztrader::shm::OrderIdMeta::open_or_create(chn_order_id_name, chn_order_id_dir)} {
        dztrader::copy_string(this->strategy_id, strategy_id.c_str(), true);
        strategy_home = dztrader::this_process::exe_dir().string();
    }

    DzContext()
        : DzContext(dztrader::this_process::exe_stem(),
                    dztrader::CHANNEL_NAME_EVENT,
                    dztrader::paths::shm(),
                    dztrader::CHANNEL_NAME_ORDER_ID,
                    dztrader::paths::shm()) {}

    DzStrategyId strategy_id{};
    dztrader::shm::MultiWriter writer;
    dztrader::shm::Reader reader;
    dztrader::shm::NamedSemaphore sem;
    std::string strategy_home;
    dztrader::shm::OrderIdMeta order_id;
    std::set<std::string> md_sources;
};

#endif  // DZTRADER_STRATEGY_API_STRATEGY_CONTEXT_H_
