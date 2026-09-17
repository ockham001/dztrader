/**
 * @file types.h
 * @brief 数据库统一接口基础类型（值/过滤/排序/聚合/结果/schema）
 */
#ifndef DZTRADER_DB_TYPES_H_
#define DZTRADER_DB_TYPES_H_

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

namespace dztrader::db {

using Value = std::variant<std::monostate, bool, int64_t, double, std::string>;

enum class ValueType { Null, Bool, Int64, Float64, String };

/// 比较算子；In 使用多值，其余取 values[0]
enum class CompareOp { Eq, Ne, Gt, Gte, Lt, Lte, In };

struct Condition {
    std::string field;
    CompareOp op = CompareOp::Eq;
    std::vector<Value> values;
};

/// 条件集合（隐式 AND；不支持逻辑嵌套）
class Filter {
public:
    Filter() = default;
    Filter(std::initializer_list<Condition> conditions) : conditions_(conditions) {}
    /// 单条件隐式转换（find/remove/count 直接接 filters::eq(...)）
    Filter(Condition condition) : conditions_{std::move(condition)} {}

    [[nodiscard]] bool empty() const noexcept { return conditions_.empty(); }
    [[nodiscard]] std::span<const Condition> conditions() const noexcept { return conditions_; }

    Filter& add(Condition condition) {
        conditions_.push_back(std::move(condition));
        return *this;
    }

private:
    std::vector<Condition> conditions_;
};

/// 可映射为 Value 的标量（concept 约束规避 variant 的 const char*→bool 静默误选）
template <typename T>
concept ScalarValue = (std::integral<T> && !std::same_as<T, bool>) ||
                      std::floating_point<T> || std::same_as<T, bool> ||
                      std::convertible_to<T, std::string_view>;

namespace filters {
namespace detail {

template <ScalarValue T>
[[nodiscard]] Value to_value(const T& value) {
    if constexpr (std::same_as<T, bool>) {
        return value;
    } else if constexpr (std::integral<T>) {
        return static_cast<int64_t>(value);
    } else if constexpr (std::floating_point<T>) {
        return static_cast<double>(value);
    } else {
        return std::string(value);
    }
}

template <ScalarValue T>
[[nodiscard]] Condition make(std::string field, CompareOp op, T value) {
    return Condition{std::move(field), op, {to_value(value)}};
}

}  // namespace detail

template <ScalarValue T> [[nodiscard]] Condition eq(std::string field, T value) {
    return detail::make(std::move(field), CompareOp::Eq, value);
}
template <ScalarValue T> [[nodiscard]] Condition ne(std::string field, T value) {
    return detail::make(std::move(field), CompareOp::Ne, value);
}
template <ScalarValue T> [[nodiscard]] Condition gt(std::string field, T value) {
    return detail::make(std::move(field), CompareOp::Gt, value);
}
template <ScalarValue T> [[nodiscard]] Condition gte(std::string field, T value) {
    return detail::make(std::move(field), CompareOp::Gte, value);
}
template <ScalarValue T> [[nodiscard]] Condition lt(std::string field, T value) {
    return detail::make(std::move(field), CompareOp::Lt, value);
}
template <ScalarValue T> [[nodiscard]] Condition lte(std::string field, T value) {
    return detail::make(std::move(field), CompareOp::Lte, value);
}
template <ScalarValue T>
[[nodiscard]] Condition in(std::string field, std::initializer_list<T> values) {
    std::vector<Value> converted;
    converted.reserve(values.size());
    for (const auto& value : values) {
        converted.push_back(detail::to_value(value));
    }
    return Condition{std::move(field), CompareOp::In, std::move(converted)};
}
template <ScalarValue T>
[[nodiscard]] Condition in(std::string field, std::vector<T> values) {
    std::vector<Value> converted;
    converted.reserve(values.size());
    for (const auto& value : values) {
        converted.push_back(detail::to_value(value));
    }
    return Condition{std::move(field), CompareOp::In, std::move(converted)};
}

}  // namespace filters

enum class SortOrder { Ascending, Descending };

struct SortSpec {
    std::string field;
    SortOrder order = SortOrder::Ascending;
};

/// limit < 0 表示不限
struct FindOptions {
    std::vector<SortSpec> sort;
    int64_t offset = 0;
    int64_t limit = -1;
};

enum class AggregateOp { Count, Max, Min, Sum, Avg };

struct Aggregation {
    AggregateOp op = AggregateOp::Count;
    std::string field;  ///< Count 时忽略
    std::vector<std::string> group_by;
    Filter filter;
    std::vector<SortSpec> sort;
};

struct Column {
    std::string name;
    ValueType type;
};

/// 位置化行（字段序 = ResultSet 列序）
class Row {
public:
    Row() = default;
    explicit Row(std::vector<Value> values) : values_(std::move(values)) {}

    template <typename T>
    [[nodiscard]] T get(size_t index) const {
        if (index >= values_.size()) {
            throw Exception(DZ_EC_INVALID_PARAM, "row index out of range: index={}", index);
        }
        const T* value = std::get_if<T>(&values_[index]);
        if (value == nullptr) {
            throw Exception(DZ_EC_INVALID_PARAM, "row type mismatch: index={}", index);
        }
        return *value;
    }

    [[nodiscard]] std::span<const Value> values() const noexcept { return values_; }
    [[nodiscard]] size_t size() const noexcept { return values_.size(); }

private:
    std::vector<Value> values_;
};

class ResultSet {
public:
    ResultSet() = default;
    ResultSet(std::vector<Column> columns, std::vector<Row> rows)
        : columns_(std::move(columns)), rows_(std::move(rows)) {}

    [[nodiscard]] std::span<const Column> columns() const noexcept { return columns_; }
    [[nodiscard]] std::span<const Row> rows() const noexcept { return rows_; }

    [[nodiscard]] std::optional<size_t> column_index(std::string_view name) const noexcept {
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return rows_.size(); }

private:
    std::vector<Column> columns_;
    std::vector<Row> rows_;
};

/// 资源 schema：由域层（tdstore）声明；实例须在进程存续期内有效且不变
struct FieldSchema {
    std::string_view name;
    ValueType type;
    bool nullable = false;
    bool primary_key = false;
    bool auto_increment = false;
};

struct IndexSchema {
    std::string_view name;
    std::vector<std::string_view> fields;
    bool unique = false;
};

struct ResourceSchema {
    std::string_view name;
    std::vector<FieldSchema> fields;   ///< 序 = Row 位置序
    std::vector<IndexSchema> indexes;  ///< 含业务唯一键（unique = true）
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_TYPES_H_
