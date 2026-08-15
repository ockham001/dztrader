/**
 * @file vector_result_set.h
 * @brief VectorResultSet — 全量预加载结果集
 */
#ifndef DZTRADER_STRATEGY_API_VECTOR_RESULT_SET_H_
#define DZTRADER_STRATEGY_API_VECTOR_RESULT_SET_H_

#include "result_set_impl.h"

#include <cstddef>
#include <vector>

namespace dztrader::strategy_api_internal {

class VectorResultSet : public ResultSetImpl {
public:
    VectorResultSet(std::vector<ColumnMeta> columns,
                    std::vector<Row> rows,
                    int32_t status = 0);

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
    const Row* current_row() const;

    std::vector<ColumnMeta> columns_;
    std::vector<Row> rows_;
    size_t cursor_ = 0;
    int32_t status_;
};

}  // namespace dztrader::strategy_api_internal

#endif  // DZTRADER_STRATEGY_API_VECTOR_RESULT_SET_H_
