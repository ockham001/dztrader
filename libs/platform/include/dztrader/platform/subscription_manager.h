#ifndef DZTRADER_PLATFORM_SUBSCRIPTION_MANAGER_H_
#define DZTRADER_PLATFORM_SUBSCRIPTION_MANAGER_H_

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <dztrader/core/core_data_type.h>    // DzFrameType
#include <dztrader/core/core_struct.h>       // SubscribeReq, SubscribeAction
#include <dztrader/core/json_enum.h>         // DZ_JSON_ENUM
#include <dztrader/platform/notify_ui.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform {

// ---- Types migrated from apps/ctp/md/md_state.h ----

/// 合约订阅状态 (CTP 侧)。
enum class SubState {
    NotRequested,  ///< 未发送订阅请求
    Pending,       ///< 已发送, 待 CTP 确认
    Subscribed     ///< CTP 确认订阅成功
};

/// 基于 magic_enum 自动派生 SubState 的 JSON 序列化。
DZ_JSON_ENUM(SubState)

/// 查询返回条数上限（编译期固定常量，不可配置）。
inline constexpr int SUBSCRIPTION_QUERY_MAX = 32;

/// 单条订阅详情 (DZ_FRAME_RTN_MD_SUBSCRIPTIONS 响应元素)。
/// 跨进程契约: md 进程构造, dzweb 透传, UI 消费。
struct SubscriptionDetail {
    std::string instrument;                              ///< 合约代码
    SubState sub_state = SubState::NotRequested;         ///< 订阅状态
    std::vector<std::string> subscribers;                ///< 订阅者 instance_id 列表

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SubscriptionDetail, instrument, sub_state, subscribers)
};

/// 查询订阅响应 (DZ_FRAME_RTN_MD_SUBSCRIPTIONS payload)。
/// 成功路径填充 subscriptions/计数; 错误路径仅填充 error, 其余为默认值。
struct RtnMdSubscriptionsRsp {
    std::vector<SubscriptionDetail> subscriptions;       ///< 详情列表 (可能截断)
    int returned_count = 0;                              ///< 实际返回条数
    int total_matched = 0;                               ///< 匹配总数
    bool truncated = false;                              ///< 是否因超 max_n 截断
    std::optional<std::string> error;                    ///< 错误路径填充, 成功路径为 nullopt

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RtnMdSubscriptionsRsp,
                                               subscriptions, returned_count,
                                               total_matched, truncated, error)
};

/// 合约订阅详情（从 MdStateMachine 内嵌类型提升为独立公开类型，逻辑不变）。
/// 跨进程契约无关，仅进程内订阅状态管理使用。
struct InstrumentSubInfo {
    std::set<std::string> subscribers;  ///< 订阅者 instance_id 集合
    SubState sub_state = SubState::NotRequested;
};

// ---- Result types ----

/// handle_subscribe_req 返回值
struct SubscribeActionResult {
    std::string instance_id;                  ///< 解码出的请求方（日志用；解码失败为空）
    std::vector<std::string> to_subscribe;    ///< 需要发 API 订阅的合约
    std::vector<std::string> to_unsubscribe;  ///< 需要发 API 退订的合约
};

/// on_logged_in 返回值
struct LoginSubPlan {
    std::vector<std::string> to_unsubscribe;  ///< 需 API 退订的孤儿合约
    std::vector<std::string> to_resubscribe;  ///< 需 mark_pending + 批次重订的合约
};

// ---- SubscriptionManager ----

class SubscriptionManager {
public:
    SubscriptionManager(const std::string& name,
                        shm::MultiWriter& event_writer,
                        NotifyUi& notify_ui);

    // 禁止拷贝/移动（引用成员）
    SubscriptionManager(const SubscriptionManager&) = delete;
    SubscriptionManager& operator=(const SubscriptionManager&) = delete;
    SubscriptionManager(SubscriptionManager&&) = delete;
    SubscriptionManager& operator=(SubscriptionManager&&) = delete;

    // --- 1. 帧处理 ---
    SubscribeActionResult handle_subscribe_req(const shm::FrameView& view);
    void handle_query_md_subscriptions(const shm::FrameView& view);

    // --- 2. 手动入口 ---
    std::vector<std::string> subscribe(const std::string& instance_id,
                                       std::vector<std::string> instruments);
    std::vector<std::string> unsubscribe(const std::string& instance_id,
                                         std::vector<std::string> instruments);
    std::vector<std::string> unsubscribe_all(const std::string& instance_id);

    // --- 3. 状态机操作 ---
    void mark_pending(const std::vector<std::string>& instruments);
    /// 处理 API 订阅确认。
    /// @return true = 调用方需发起 API 退订: 确认成功但已无订阅者 (确认后乐观退订,
    ///         状态先置 NotRequested, 保证退订在途窗口内新订阅会重新触发订阅)。
    ///         确认失败且无订阅者时条目直接 erase (CTP 侧确定无订阅), 返回 false。
    bool on_sub_confirmed(const std::string& instrument, bool success);
    void on_unsub_confirmed(const std::string& instrument);
    std::vector<std::string> check_subscribe_status();
    void reset_pending_state();

    LoginSubPlan on_logged_in();
    void on_idle();
    void on_session_lost();

    // --- 4. 统计与查询 ---
    size_t expected_subscribe_count() const;
    size_t subscribed_count() const;
    bool is_instrument_subscribed(const std::string& instrument) const;
    const InstrumentSubInfo* find_instrument(const std::string& instrument) const;
    const std::map<std::string, InstrumentSubInfo>& all_instruments() const;

private:
    std::string name_;
    shm::MultiWriter& event_writer_;
    NotifyUi& notify_ui_;
    std::map<std::string, InstrumentSubInfo> instruments_sub_info_;
    size_t cached_subscribed_count_ = 0;

    // 仅供内部组合使用
    std::vector<std::string> cleanup_orphan_instruments();
    std::vector<std::string> get_instruments_to_resubscribe() const;
    void reset_all_sub_state();
    void send_query_error_rsp(const std::string& error);
    void query_subscriptions(const nlohmann::json& req);
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_SUBSCRIPTION_MANAGER_H_