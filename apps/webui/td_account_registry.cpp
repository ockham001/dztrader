#include "td_account_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace dztrader::webui {

// ---------------------------------------------------------------------------
// RepositoryTdAccountRegistryStore：platform.db 实现（表 td_account_registry）
// ---------------------------------------------------------------------------

std::vector<TdAccountRegistryRow> RepositoryTdAccountRegistryStore::load() {
    return repo_.list_td_account_registry();
}

bool RepositoryTdAccountRegistryStore::replace_gateway(
    const std::string& gateway, const std::vector<std::string>& account_ids) {
    return repo_.replace_td_account_registry_for_gateway(gateway, account_ids);
}

bool RepositoryTdAccountRegistryStore::delete_gateway(const std::string& gateway) {
    return repo_.delete_td_account_registry_by_gateway(gateway);
}

// ---------------------------------------------------------------------------
// TdAccountRegistry
// ---------------------------------------------------------------------------

TdAccountRegistry::TdAccountRegistry(std::unique_ptr<TdAccountRegistryStore> store)
    : store_(std::move(store)) {}

size_t TdAccountRegistry::load() {
    if (!store_) {
        return 0;
    }
    std::vector<TdAccountRegistryRow> rows;
    try {
        rows = store_->load();
    } catch (const std::exception& e) {
        // 契约 td-config §5：启动加载失败 → 从空开始，随各网关配置回报重建。
        // 已知降级：最多少覆盖一个网关（该网关的账户在收到它的首帧前不在全集里）。
        SPDLOG_ERROR("td account registry load failed, starting empty | error=\"{}\"", e.what());
        return 0;
    }
    for (const auto& row : rows) {
        if (row.account_id.empty() || row.gateway.empty()) {
            continue;
        }
        // enabled 未持久化（契约 rest §2.9.9）：回 true，不把账户误标为禁用；
        // 该网关配置帧到达后 reconcile 出真实值。
        entries_[row.account_id] = TdAccountRegistryEntry{
            .account_id = row.account_id, .gateway = row.gateway, .enabled = true};
    }
    SPDLOG_INFO("td account registry loaded | accounts={}", entries_.size());
    return entries_.size();
}

bool TdAccountRegistry::reconcile_gateway(const std::string& gateway,
                                          const std::vector<TdAccountRegistryEntry>& accounts) {
    if (gateway.empty()) {
        return false;
    }
    // 帧为权威的整组替换：先按网关收拢旧集合，用来判定"集合是否变化"（决定是否广播）。
    // enabled 变化也算变化——读端点的 enabled 列会跟着变，前端列表需重拉。
    std::map<std::string, bool> before;
    for (const auto& [account_id, entry] : entries_) {
        if (entry.gateway == gateway) {
            before[account_id] = entry.enabled;
        }
    }

    // 删掉该网关的旧条目（配置帧是全量，条目消失 = 账户已从该网关移除）。
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.gateway == gateway) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    std::map<std::string, bool> after;
    std::vector<std::string> account_ids;
    account_ids.reserve(accounts.size());
    for (const auto& account : accounts) {
        if (account.account_id.empty()) {
            continue;  // 空 account_id 无意义（网关侧校验失败会回旧值，不应走到这里）
        }
        auto it = entries_.find(account.account_id);
        if (it != entries_.end() && it->second.gateway != gateway) {
            // 跨网关重复：契约 td-config §5 规定 account_id 全局唯一，帧为权威 → 改判归属。
            // 另一个网关的配置与注册表因此不一致，等它的帧到达时自愈。
            SPDLOG_WARN(
                "td account already owned by another gateway, reassigning | "
                "account={} old_gateway={} new_gateway={}",
                account.account_id, it->second.gateway, gateway);
        }
        TdAccountRegistryEntry entry = account;
        entry.gateway = gateway;
        entries_[account.account_id] = entry;
        after[account.account_id] = entry.enabled;
        account_ids.push_back(account.account_id);
    }

    size_t added = 0;
    for (const auto& item : after) {
        if (before.find(item.first) == before.end()) {
            ++added;
        }
    }
    size_t removed = 0;
    for (const auto& item : before) {
        if (after.find(item.first) == after.end()) {
            ++removed;
        }
    }
    const bool changed = before != after;
    if (changed) {
        SPDLOG_INFO("td account registry reconciled | gateway={} accounts={} added={} removed={}",
                    gateway, after.size(), added, removed);
    } else {
        SPDLOG_DEBUG("td account registry reconciled | gateway={} accounts={}", gateway,
                     after.size());
    }

    persist_gateway(gateway, account_ids);
    return changed;
}

void TdAccountRegistry::note_config_frame_received() noexcept { has_snapshot_ = true; }

std::optional<TdAccountRegistryEntry> TdAccountRegistry::find(const std::string& account_id) const {
    auto it = entries_.find(account_id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool TdAccountRegistry::contains(const std::string& account_id) const {
    return entries_.find(account_id) != entries_.end();
}

std::vector<TdAccountRegistryEntry> TdAccountRegistry::entries() const {
    std::vector<TdAccountRegistryEntry> result;
    result.reserve(entries_.size());
    for (const auto& [account_id, entry] : entries_) {
        result.push_back(entry);
    }
    return result;
}

std::vector<std::string> TdAccountRegistry::account_ids() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& [account_id, entry] : entries_) {
        result.push_back(account_id);
    }
    return result;
}

bool TdAccountRegistry::prune_gateways_not_in(const std::vector<std::string>& live_gateways) {
    const std::set<std::string> live(live_gateways.begin(), live_gateways.end());
    std::set<std::string> removed_gateways;
    bool removed = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (live.find(it->second.gateway) == live.end()) {
            removed_gateways.insert(it->second.gateway);
            it = entries_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    // 持久层同清：否则重启后又被加载回来（契约 td-config §5「网关被移除时清掉该网关条目」
    // 说的是注册表整体，包含持久层）。清失败只记 ERROR。
    for (const auto& gateway : removed_gateways) {
        persist_delete_gateway(gateway);
    }
    if (removed) {
        SPDLOG_INFO("td account registry pruned departed gateways | gateways={}",
                    removed_gateways.size());
    }
    return removed;
}

void TdAccountRegistry::persist_gateway(const std::string& gateway,
                                        const std::vector<std::string>& account_ids) {
    if (!store_) {
        return;
    }
    try {
        if (store_->replace_gateway(gateway, account_ids)) {
            return;
        }
    } catch (const std::exception& e) {
        // 契约 td-config §5：DB 写失败只记 ERROR、不阻塞账户操作（内存已是最新值）。
        SPDLOG_ERROR("td account registry persist failed | gateway={} error=\"{}\"", gateway,
                     e.what());
        return;
    }
    SPDLOG_ERROR("td account registry persist failed | gateway={} reason=\"store rejected write\"",
                 gateway);
}

void TdAccountRegistry::persist_delete_gateway(const std::string& gateway) {
    if (!store_) {
        return;
    }
    try {
        if (store_->delete_gateway(gateway)) {
            return;
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td account registry persist delete failed | gateway={} error=\"{}\"", gateway,
                     e.what());
        return;
    }
    // 无行可删（内存有条目但库里没有）是正常状态，不记 ERROR。
    SPDLOG_DEBUG("td account registry persist delete no rows | gateway={}", gateway);
}

}  // namespace dztrader::webui
