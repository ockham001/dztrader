#ifndef DZTRADER_WEBUI_TD_ACCOUNT_REGISTRY_H_
#define DZTRADER_WEBUI_TD_ACCOUNT_REGISTRY_H_

/**
 * @file td_account_registry.h
 * @brief 交易账户注册表：`account_id → gateway` 的平台侧投影（契约 td-config §5、
 *        rest §2.9.9）。
 *
 * 为什么需要它：`account_id` **全局唯一**（同一时刻只属于一个交易接口实例，跨接口类型也不
 * 冲突，ADR 0045）。UI 添加/编辑账户前必须能判断「这个 id 是否已被任何网关占用」，包括
 * **已禁用/离线**的账户与**网关停机期间**的账户——单网关的配置镜像拼不出全集。dzweb 因此
 * 维护这张表，读端点 `GET /api/td/accounts` 是账户配置页的唯一账户列表来源。
 *
 * 语义边界（契约只写这些，不要扩）：
 * - **内存优先，DB 尽力而为**：写入失败只记 ERROR、不抛、不阻塞账户操作（§5）。
 * - **帧为权威**：`TD_RTN_CONFIG` 到达时按网关**整组 reconcile**（自愈错误状态，§5）。
 * - **`enabled` 不持久化**：platform.db 只有 `account_id → gateway` 两列；重启恢复的条目
 *   `enabled` 未知 → 回 `true`（已知降级：重启窗口内禁用账户会显示为启用，比反过来危害小，
 *   rest §2.9.9）。随后该网关配置帧到达即 reconcile 出真实值。
 * - **网关被移除时清掉该网关条目**（内存 + platform.db，§5）。
 * - **注册表是投影不是真相源**：真相源是各 td 网关配置文件（td-config §6）；注册表丢失时
 *   账户全集的前两个来源（在线集合 ∪ 交易库四表）仍能兜底（rest §2.9.1）。
 *
 * 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问都在 IO 线程串行执行，
 * 不加锁（与 `MirrorStore` 同约定）。
 */

#include "repository.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {

/// 注册表持久层薄接口（契约 td-config §5「尽力而为」）。
///
/// 抽接口的唯一理由是可注入的失败路径：「DB 写失败只记 ERROR 不阻塞账户操作」是契约要求，
/// 而真 sqlite 句柄一旦打开就无法在单测里稳定制造写失败（删除/改名文件不影响已打开的 fd）。
/// 生产实现是 {@link RepositoryTdAccountRegistryStore}（唯一实现，不引入第二套存储）。
class TdAccountRegistryStore {
public:
    virtual ~TdAccountRegistryStore() = default;

    /// 读取全部持久化条目（启动加载用）。失败抛异常，由调用方按「从空开始」处理。
    virtual std::vector<TdAccountRegistryRow> load() = 0;

    /// 整组覆盖某网关的持久化条目（单事务）。返回 false = 写入失败。
    virtual bool replace_gateway(const std::string& gateway,
                                 const std::vector<std::string>& account_ids) = 0;

    /// 删除某网关全部持久化条目。返回 false = 无行可删或写入失败。
    virtual bool delete_gateway(const std::string& gateway) = 0;
};

/// platform.db 实现（表 `td_account_registry`，见 `db_init.cpp` 的 schema）。
class RepositoryTdAccountRegistryStore : public TdAccountRegistryStore {
public:
    explicit RepositoryTdAccountRegistryStore(Repository& repo)
        : repo_(repo) {}

    std::vector<TdAccountRegistryRow> load() override;
    bool replace_gateway(const std::string& gateway,
                         const std::vector<std::string>& account_ids) override;
    bool delete_gateway(const std::string& gateway) override;

private:
    Repository& repo_;
};

/// 账户配置的通用层子集（契约 td-config §2.2）：`ext`（柜台特有连接参数）不在注册表内——
/// 注册表只回答「哪个网关拥有它、是否启用」，连接参数读配置镜像。
struct TdAccountRegistryEntry {
    std::string account_id;  ///< 全局唯一 key
    std::string gateway;     ///< 拥有该账户的交易网关进程名（`dztd_*`）
    bool enabled = true;     ///< 来自最近一次配置帧；从 DB 恢复的条目恒 true（已知降级）
};

class TdAccountRegistry {
public:
    /// @param store 持久层（nullptr = 纯内存，测试/未装配 platform.db 时使用）
    explicit TdAccountRegistry(std::unique_ptr<TdAccountRegistryStore> store = nullptr);

    // 含 unique_ptr 成员，禁拷贝；不需移动
    TdAccountRegistry(const TdAccountRegistry&) = delete;
    TdAccountRegistry& operator=(const TdAccountRegistry&) = delete;
    TdAccountRegistry(TdAccountRegistry&&) = delete;
    TdAccountRegistry& operator=(TdAccountRegistry&&) = delete;

    /// 启动加载（main 在装配时调用一次）。失败 → 记 ERROR、从空开始、随各网关配置帧重建
    /// （契约 td-config §5：已知降级「最多少覆盖一个网关」）。
    /// @return 成功加载的条目数（失败为 0）
    size_t load();

    /// 配置帧到达时按网关**整组** reconcile（契约 td-config §5，帧为权威）：
    /// 先清掉该网关全部旧条目，再按 `accounts` 重建。跨网关重复的 `account_id` 以帧为准
    /// 改判归属（记 WARN——它意味着其他网关的配置与注册表不一致，等那个网关的帧自愈）。
    ///
    /// `enabled` 取帧内通用层 `enabled`（缺失/类型不符回 true）；不读 `ext`。
    ///
    /// @return true = 该网关的账户集合发生变化（供调用方广播 `data_changed{td}`）
    bool reconcile_gateway(const std::string& gateway,
                           const std::vector<TdAccountRegistryEntry>& accounts);

    /// 标记「已收到过配置帧」（契约 rest §2.9.9 的 `no_gateway_snapshot` 降级判定）。
    /// 配置帧 schema 异常（缺 `accounts` 数组）时只置位不 reconcile——用空集合 reconcile 会
    /// 把该网关账户从注册表抹掉，那比不更新更危险。
    void note_config_frame_received() noexcept;

    /// 是否已收到过任何配置帧。
    [[nodiscard]] bool has_snapshot() const noexcept { return has_snapshot_; }

    /// 注册表是否可用（契约 rest §2.9.9）：收到过配置帧 **或** 启动加载恢复出条目。
    /// 两者皆否时「无账户」不可信，读端点必须回降级而不是空数组。
    [[nodiscard]] bool available() const noexcept { return has_snapshot_ || !entries_.empty(); }

    /// 按 account_id 查找。
    [[nodiscard]] std::optional<TdAccountRegistryEntry> find(const std::string& account_id) const;

    /// 该账户是否已被任何网关占用（含已禁用/离线）。
    [[nodiscard]] bool contains(const std::string& account_id) const;

    /// 全部账户（按 account_id 升序，契约 rest §2.9.9）。
    [[nodiscard]] std::vector<TdAccountRegistryEntry> entries() const;

    /// 全部账户 ID（按 account_id 升序）——账户全集第三源（rest §2.9.1）。
    [[nodiscard]] std::vector<std::string> account_ids() const;

    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

    /// 网关被移除时清理：删除所有 `gateway ∉ live_gateways` 的条目（内存 + 持久层）。
    /// @return true = 至少删掉了一条（供调用方广播 `data_changed{td}`）
    bool prune_gateways_not_in(const std::vector<std::string>& live_gateways);

private:
    /// 尽力持久化某网关的整组条目：失败只记 ERROR，不影响内存状态（契约 td-config §5）。
    void persist_gateway(const std::string& gateway, const std::vector<std::string>& account_ids);

    /// 尽力删除某网关的持久化条目：失败只记 ERROR。
    void persist_delete_gateway(const std::string& gateway);

    std::unique_ptr<TdAccountRegistryStore> store_;
    std::map<std::string, TdAccountRegistryEntry> entries_;  ///< account_id -> entry（有序）
    bool has_snapshot_ = false;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ACCOUNT_REGISTRY_H_
