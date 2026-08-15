#ifndef DZTRADER_WEBUI_DB_INIT_H_
#define DZTRADER_WEBUI_DB_INIT_H_

#include <filesystem>
#include <string>
struct sqlite3;

namespace dztrader::webui {

/// Create all tables and seed default config on an open database connection.
/// Also enables foreign keys. Called by init_database and Repository constructor.
/// @throws std::runtime_error if schema creation or config seeding fails.
void run_schema(sqlite3* db);

/// Initialize database: open connection, create schema, seed config.
/// Creates db_path file if it does not exist.
/// @throws std::runtime_error if open or schema creation fails.
void init_database(const std::filesystem::path& db_path, sqlite3*& out_db);

/// Seed admin user if the users table is empty.
void seed_admin_user(sqlite3* db, const std::string& username, const std::string& password);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_DB_INIT_H_
