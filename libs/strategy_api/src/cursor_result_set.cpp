#include "cursor_result_set.h"

#include <climits>
#include <cfloat>

namespace dztrader::strategy_api_internal {

CursorResultSet::CursorResultSet(std::vector<ColumnMeta> columns, int32_t status)
    : columns_(std::move(columns)), status_(status) {}

bool CursorResultSet::next() { return false; }

int32_t CursorResultSet::status() const { return status_; }

uint32_t CursorResultSet::column_count() const {
    return static_cast<uint32_t>(columns_.size());
}

DzColumnType CursorResultSet::column_type(uint32_t index) const {
    if (index >= columns_.size()) return DZ_COL_TYPE_NULL;
    return columns_[index].type;
}

const char* CursorResultSet::column_name(uint32_t index) const {
    if (index >= columns_.size()) return "";
    return columns_[index].name.c_str();
}

bool CursorResultSet::is_null(uint32_t index) const {
    if (!has_row_ || index >= current_row_.size()) return true;
    return std::holds_alternative<std::monostate>(current_row_[index]);
}

int64_t CursorResultSet::get_int64(uint32_t index) const {
    if (!has_row_ || index >= current_row_.size()) return INT64_MAX;
    if (auto* p = std::get_if<int64_t>(&current_row_[index])) return *p;
    return INT64_MAX;
}

double CursorResultSet::get_float64(uint32_t index) const {
    if (!has_row_ || index >= current_row_.size()) return DBL_MAX;
    if (auto* p = std::get_if<double>(&current_row_[index])) return *p;
    return DBL_MAX;
}

const char* CursorResultSet::get_string(uint32_t index) const {
    if (!has_row_ || index >= current_row_.size()) return "";
    if (auto* p = std::get_if<std::string>(&current_row_[index])) return p->c_str();
    return "";
}

bool CursorResultSet::get_bool(uint32_t index) const {
    if (!has_row_ || index >= current_row_.size()) return false;
    if (auto* p = std::get_if<bool>(&current_row_[index])) return *p;
    return false;
}

}  // namespace dztrader::strategy_api_internal
