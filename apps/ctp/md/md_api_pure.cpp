#include <dztrader/platform/ctp_md_config.h>
#include "md/md_batch_check.h"

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

namespace dztrader::ctp {

// md_api_pure.cpp: 纯函数实现, 不依赖 MdApi 类和 CTP 头文件
// 供单元测试直接编译 (无需链接 CTP 库)

// on_batch_complete 决策: queue_nonempty 优先 (滞留批次非重试), 语义见 md_batch_check.h
BatchCheckAction decide_batch_check_action(bool queue_nonempty, bool has_pending,
                                           int retry_count, int max_retry) {
    if (queue_nonempty) {
        return BatchCheckAction::ContinueSend;
    }
    if (!has_pending) {
        return BatchCheckAction::Done;
    }
    return (retry_count + 1 > max_retry) ? BatchCheckAction::GiveUp : BatchCheckAction::Retry;
}

// 将合约列表分批为多个批次 (纯函数, 供单元测试)。
// batch_size <= 0 兜底为 1 (validate 已校验, 此处兜底防死循环)。
std::deque<std::vector<std::string>> make_batches(std::vector<std::string> instruments,
                                                  int batch_size) {
    std::deque<std::vector<std::string>> batches;
    if (batch_size <= 0) {
        batch_size = 1;
    }
    for (size_t offset = 0; offset < instruments.size(); offset += batch_size) {
        auto count = std::min<size_t>(batch_size, instruments.size() - offset);
        batches.emplace_back(instruments.begin() + offset,
                             instruments.begin() + offset + count);
    }
    return batches;
}

// 在 brokers 中查找 name 对应的 broker (纯函数, 供单元测试)。
// name 为空或未找到时返回 nullptr。
const dztrader::platform::CtpBrokerEntry* find_current_broker_in(
    const std::vector<dztrader::platform::CtpBrokerEntry>& brokers,
    const std::string& name) {
    if (name.empty()) {
        return nullptr;
    }
    for (const auto& b : brokers) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

}  // namespace dztrader::ctp
