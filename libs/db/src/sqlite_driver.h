/**
 * @file sqlite_driver.h
 * @brief SQLite 驱动（Database/Session/Transaction/Snapshot 实现）
 */
#ifndef DZTRADER_DB_SRC_SQLITE_DRIVER_H_
#define DZTRADER_DB_SRC_SQLITE_DRIVER_H_

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SQLiteCpp/Database.h>

#include <dztrader/db/database.h>

namespace dztrader::db {

class SqliteDatabaseImpl final : public Database {
public:
    SqliteDatabaseImpl(std::string path, std::map<std::string, std::string, std::less<>> options,
                       std::span<const ResourceSchema> schemas);

    std::unique_ptr<Session> session(bool read_only = false) override;
    [[nodiscard]] Capability capabilities() const noexcept override;
    void migrate() override;

    [[nodiscard]] const ResourceSchema* find_schema(std::string_view name) const;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& options() const noexcept {
        return options_;
    }

private:
    std::string path_;
    std::map<std::string, std::string, std::less<>> options_;
    std::vector<ResourceSchema> schemas_;
    std::unordered_map<std::string_view, const ResourceSchema*> schema_by_name_;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_SRC_SQLITE_DRIVER_H_
