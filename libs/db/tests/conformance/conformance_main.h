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

struct Driver {
    std::string name;
    Factory make;
};

const std::vector<Driver>& drivers();
const std::vector<dztrader::db::ResourceSchema>& conformance_schemas();

}  // namespace db_test

#endif
