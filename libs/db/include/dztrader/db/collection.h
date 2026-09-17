/**
 * @file collection.h
 * @brief Collection：Session 之上的非虚便利句柄
 */
#ifndef DZTRADER_DB_COLLECTION_H_
#define DZTRADER_DB_COLLECTION_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <dztrader/db/database.h>

namespace dztrader::db {

class Collection {
public:
    Collection(Session& session, std::string name)
        : session_(&session), name_(std::move(name)) {}

    void upsert(std::span<const Row> rows) { session_->upsert(name_, rows); }
    uint64_t remove(const Filter& filter) { return session_->remove(name_, filter); }
    [[nodiscard]] ResultSet find(const Filter& filter = {},
                                 const FindOptions& options = {}) const {
        return session_->find(name_, filter, options);
    }
    /// 保留 sort，忽略 offset/limit，取首行
    [[nodiscard]] std::optional<Row> find_one(const Filter& filter = {},
                                              const FindOptions& options = {}) const {
        FindOptions one = options;
        one.offset = 0;
        one.limit = 1;
        ResultSet result = session_->find(name_, filter, one);
        if (result.empty()) {
            return std::nullopt;
        }
        return result.rows().front();
    }
    [[nodiscard]] ResultSet aggregate(const Aggregation& aggregation) const {
        return session_->aggregate(name_, aggregation);
    }
    [[nodiscard]] uint64_t count(const Filter& filter = {}) const {
        Aggregation aggregation;
        aggregation.op = AggregateOp::Count;
        aggregation.filter = filter;
        ResultSet result = session_->aggregate(name_, aggregation);
        if (result.empty() || result.rows().front().size() == 0) {
            return 0;
        }
        return static_cast<uint64_t>(result.rows().front().get<int64_t>(0));
    }

private:
    Session* session_;
    std::string name_;
};

}  // namespace dztrader::db

#endif  // DZTRADER_DB_COLLECTION_H_
