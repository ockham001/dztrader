/**
 * @file result_set.h
 * @brief DzResultSet 不透明结构体定义（C API 内部实现）
 */
#ifndef DZTRADER_STRATEGY_API_RESULT_SET_H_
#define DZTRADER_STRATEGY_API_RESULT_SET_H_

#include <memory>

#include "result_set_impl.h"

struct DzResultSet {
    std::unique_ptr<dztrader::strategy_api_internal::ResultSetImpl> impl;
};

#endif  // DZTRADER_STRATEGY_API_RESULT_SET_H_
