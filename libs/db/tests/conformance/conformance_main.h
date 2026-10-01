#ifndef DZTRADER_DB_TESTS_CONFORMANCE_MAIN_H_
#define DZTRADER_DB_TESTS_CONFORMANCE_MAIN_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <dztrader/db/database.h>

namespace db_test {

using Factory = std::function<std::unique_ptr<dztrader::db::Database>(
    const std::filesystem::path&, std::span<const dztrader::db::ResourceSchema>)>;

/// 用例收尾钩子：sqlite 为空表示按 path 删文件；mysql 用 path.stem() 推导库名并 DROP
using Cleanup = std::function<void(const std::filesystem::path&)>;

struct Driver {
    std::string name;
    Factory make;
    Cleanup cleanup;               ///< 空 = 删除 path_ 指向的文件（sqlite）
    bool requires_server = false;  ///< true 且未配置 DZ_TEST_MYSQL_* 时该参数化用例整体 skip
};

const std::vector<Driver>& drivers();
const std::vector<dztrader::db::ResourceSchema>& conformance_schemas();

}  // namespace db_test

#endif
