/**
 * @file cursor_result_set.h
 * @brief CursorResultSet — 懒加载游标结果集（预留骨架）
 */
#ifndef DZTRADER_STRATEGY_API_CURSOR_RESULT_SET_H_
#define DZTRADER_STRATEGY_API_CURSOR_RESULT_SET_H_

#include "result_set_impl.h"

#include <vector>

namespace dztrader::strategy_api_internal {

class CursorResultSet : public ResultSetImpl {
public:
    explicit CursorResultSet(std::vector<ColumnMeta> columns, int32_t status = 0);

    bool next() override;
    int32_t status() const override;
    uint32_t column_count() const override;
    DzColumnType column_type(uint32_t index) const override;
    const char* column_name(uint32_t index) const override;
    bool is_null(uint32_t index) const override;
    int64_t get_int64(uint32_t index) const override;
    double get_float64(uint32_t index) const override;
    const char* get_string(uint32_t index) const override;
    bool get_bool(uint32_t index) const override;

private:
    std::vector<ColumnMeta> columns_;
    Row current_row_;
    bool has_row_ = false;
    int32_t status_;
};

}  // namespace dztrader::strategy_api_internal

#endif  // DZTRADER_STRATEGY_API_CURSOR_RESULT_SET_H_
