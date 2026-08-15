#include "vector_result_set.h"

#include <climits>
#include <cfloat>

namespace dztrader::strategy_api_internal {

VectorResultSet::VectorResultSet(std::vector<ColumnMeta> columns,
                                 std::vector<Row> rows,
                                 int32_t status)
    : columns_(std::move(columns)),
      rows_(std::move(rows)),
      status_(status) {}

const Row* VectorResultSet::current_row() const {
    if (cursor_ == 0 || cursor_ > rows_.size()) return nullptr;
    return &rows_[cursor_ - 1];
}

bool VectorResultSet::next() {
    if (cursor_ >= rows_.size()) return false;
    ++cursor_;
    return true;
}

int32_t VectorResultSet::status() const { return status_; }

uint32_t VectorResultSet::column_count() const {
    return static_cast<uint32_t>(columns_.size());
}

DzColumnType VectorResultSet::column_type(uint32_t index) const {
    if (index >= columns_.size()) return DZ_COL_TYPE_NULL;
    return columns_[index].type;
}

const char* VectorResultSet::column_name(uint32_t index) const {
    if (index >= columns_.size()) return "";
    return columns_[index].name.c_str();
}

bool VectorResultSet::is_null(uint32_t index) const {
    auto* row = current_row();
    if (!row || index >= row->size()) return true;
    return std::holds_alternative<std::monostate>((*row)[index]);
}

int64_t VectorResultSet::get_int64(uint32_t index) const {
    auto* row = current_row();
    if (!row || index >= row->size()) return INT64_MAX;
    if (auto* p = std::get_if<int64_t>(&(*row)[index])) return *p;
    return INT64_MAX;
}

double VectorResultSet::get_float64(uint32_t index) const {
    auto* row = current_row();
    if (!row || index >= row->size()) return DBL_MAX;
    if (auto* p = std::get_if<double>(&(*row)[index])) return *p;
    return DBL_MAX;
}

const char* VectorResultSet::get_string(uint32_t index) const {
    auto* row = current_row();
    if (!row || index >= row->size()) return "";
    if (auto* p = std::get_if<std::string>(&(*row)[index])) return p->c_str();
    return "";
}

bool VectorResultSet::get_bool(uint32_t index) const {
    auto* row = current_row();
    if (!row || index >= row->size()) return false;
    if (auto* p = std::get_if<bool>(&(*row)[index])) return *p;
    return false;
}

}  // namespace dztrader::strategy_api_internal
