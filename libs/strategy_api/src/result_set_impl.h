/**
 * @file result_set_impl.h
 * @brief DzResultSet 内部实现基类与类型定义
 */
#ifndef DZTRADER_STRATEGY_API_RESULT_SET_IMPL_H_
#define DZTRADER_STRATEGY_API_RESULT_SET_IMPL_H_

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <dztrader/data_type.h>

namespace dztrader::strategy_api_internal {

using ColumnValue = std::variant<
    std::monostate,
    bool,
    int64_t,
    double,
    std::string
>;

using Row = std::vector<ColumnValue>;

struct ColumnMeta {
    DzColumnType type;
    std::string name;
};

inline DzColumnType variant_to_col_type(const ColumnValue& val) {
    if (std::holds_alternative<std::monostate>(val)) return DZ_COL_TYPE_NULL;
    if (std::holds_alternative<bool>(val)) return DZ_COL_TYPE_BOOL;
    if (std::holds_alternative<int64_t>(val)) return DZ_COL_TYPE_INT64;
    if (std::holds_alternative<double>(val)) return DZ_COL_TYPE_FLOAT64;
    if (std::holds_alternative<std::string>(val)) return DZ_COL_TYPE_STRING;
    return DZ_COL_TYPE_NULL;
}

class ResultSetImpl {
public:
    virtual ~ResultSetImpl() = default;

    virtual bool next() = 0;
    virtual int32_t status() const = 0;
    virtual uint32_t column_count() const = 0;
    virtual DzColumnType column_type(uint32_t index) const = 0;
    virtual const char* column_name(uint32_t index) const = 0;
    virtual bool is_null(uint32_t index) const = 0;
    virtual int64_t get_int64(uint32_t index) const = 0;
    virtual double get_float64(uint32_t index) const = 0;
    virtual const char* get_string(uint32_t index) const = 0;
    virtual bool get_bool(uint32_t index) const = 0;
};

}  // namespace dztrader::strategy_api_internal

#endif  // DZTRADER_STRATEGY_API_RESULT_SET_IMPL_H_
