#ifndef DZTRADER_CTP_TD_INSTRUMENT_QUERY_PENDING_H_
#define DZTRADER_CTP_TD_INSTRUMENT_QUERY_PENDING_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>

namespace dztrader::ctp {

/// 单合约刷新请求的"场所查询码 -> 平台 instrument_id"待回写映射.
///
/// 背景: CZCE 行 symbol != instrument_id (人工消歧), 刷新按库内 symbol 发起
/// ReqQryInstrument, 响应只带场所 InstrumentID; 无本映射则原行 (PK) 的
/// updated_at 不推进且新增重复行.
///
/// 线程约定: 与 query_instrument / on_rsp_qry_instrument 一致, 仅主线程访问,
/// 不加锁. 登录全量查询期间映射为空, 行为不变.
class InstrumentQueryPending {
public:
    /// 容量上限: 防无界增长. 新增 key 将超限时整体清空并告警, 再登记本条
    /// (保最新在途请求可回写); 测试可传小容量.
    static constexpr size_t kDefaultCapacity = 1024;

    explicit InstrumentQueryPending(size_t capacity = kDefaultCapacity)
        : capacity_(capacity) {}

    /// 登记待回写映射 (覆盖写: 同一 symbol 重复发起以最后一次为准).
    void add(std::string symbol, std::string instrument_id) {
        if (pending_.size() >= capacity_ && pending_.find(symbol) == pending_.end()) {
            SPDLOG_WARN("td instrument refresh pending overflow, cleared | size={} capacity={}",
                        pending_.size(), capacity_);
            pending_.clear();
        }
        pending_.insert_or_assign(std::move(symbol), std::move(instrument_id));
    }

    /// 命中即擦除并返回原 instrument_id; 未命中返回空串.
    std::string take(std::string_view symbol) {
        if (pending_.empty()) {
            return {};  // 登录全量查询逐行调用, 空表快速返回 (免每行构造 key)
        }
        auto it = pending_.find(std::string(symbol));
        if (it == pending_.end()) {
            return {};
        }
        std::string instrument_id = std::move(it->second);
        pending_.erase(it);
        return instrument_id;
    }

    size_t size() const noexcept { return pending_.size(); }

private:
    std::unordered_map<std::string, std::string> pending_;
    size_t capacity_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_INSTRUMENT_QUERY_PENDING_H_
