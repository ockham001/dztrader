/**
 * @file md_source.h
 * @brief 行情源内部实现（C API opaque 类型 DzMdSource 的定义）
 */
#ifndef DZTRADER_STRATEGY_API_MD_SOURCE_H_
#define DZTRADER_STRATEGY_API_MD_SOURCE_H_

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/core/path.h>
#include <string>

struct DzMdSource {
    DzMdSource(const std::string& name,
               const std::string& channel_name,
               const std::filesystem::path& shm_dir,
               const std::string& reader_name)
        : name(name),
          reader{dztrader::shm::Reader::create(channel_name, shm_dir, reader_name)} {}

    DzMdSource(const std::string& name, const std::string& reader_name)
        : DzMdSource(name, name, dztrader::paths::shm(), reader_name) {}

    std::string name;
    dztrader::shm::Reader reader;
};

#endif  // DZTRADER_STRATEGY_API_MD_SOURCE_H_
