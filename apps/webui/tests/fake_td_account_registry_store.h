#ifndef DZTRADER_WEBUI_TESTS_FAKE_TD_ACCOUNT_REGISTRY_STORE_H_
#define DZTRADER_WEBUI_TESTS_FAKE_TD_ACCOUNT_REGISTRY_STORE_H_

#include <stdexcept>
#include <string>
#include <vector>

#include "td_account_registry.h"

namespace dztrader::webui {

/// 测试用注册表持久层假实现：内存里保存「gateway → 行（account_id + account_name）」整组，
/// 可注入失败。
///
/// 为什么需要它（而不是只用真 Repository）：契约 td-config §5 要求「DB 写失败只记 ERROR、
/// 不阻塞账户操作」，真 sqlite 句柄一旦打开就无法在单测里稳定制造写失败（删文件不影响已
/// 打开的 fd），因此失败路径必须靠可注入的假实现覆盖。**生产路径仍只有一实现**
/// （`RepositoryTdAccountRegistryStore`），本文件只在 tests/ 下。
class FakeTdAccountRegistryStore : public TdAccountRegistryStore {
public:
    struct GatewayRows {
        std::string gateway;
        std::vector<TdAccountRegistryRow> rows;
        bool replace_called = false;  // 该网关的整组替换被调用过（无论成败）
    };

    /// 启动加载用行（与 platform.db 的 account_id → gateway / account_name 同形）。
    std::vector<TdAccountRegistryRow> stored_rows;
    /// 已持久化的整组（gateway → rows 有序）；replace_gateway 覆盖写。
    std::vector<GatewayRows> gateways;

    bool fail_load = false;
    bool fail_replace = false;
    bool fail_delete = false;

    std::vector<TdAccountRegistryRow> load() override {
        if (fail_load) {
            throw std::runtime_error("fake load failure");
        }
        return stored_rows;
    }

    bool replace_gateway(const std::string& gateway,
                         const std::vector<TdAccountRegistryRow>& rows) override {
        GatewayRows entry{.gateway = gateway, .rows = rows, .replace_called = true};
        for (auto& existing : gateways) {
            if (existing.gateway == gateway) {
                existing = entry;
                return !fail_replace;
            }
        }
        gateways.push_back(entry);
        return !fail_replace;
    }

    bool delete_gateway(const std::string& gateway) override {
        if (fail_delete) {
            return false;
        }
        for (auto it = gateways.begin(); it != gateways.end(); ++it) {
            if (it->gateway == gateway) {
                gateways.erase(it);
                return true;
            }
        }
        return false;
    }

    /// 某网关的整组替换是否被调用过（无论成败）——用于断言「失败路径确实被走到」，
    /// 避免只写 `EXPECT_NO_THROW` 时即便 reconcile 完全没执行也恒过。
    bool replaced_called(const std::string& gateway) const {
        for (const auto& row : gateways) {
            if (row.gateway == gateway) {
                return row.replace_called;
            }
        }
        return false;
    }

    /// 某网关已持久化的账户 ID 集合（未替换过 = 空）。
    std::vector<std::string> persisted(const std::string& gateway) const {
        std::vector<std::string> ids;
        for (const auto& row : gateways) {
            if (row.gateway != gateway) {
                continue;
            }
            ids.reserve(row.rows.size());
            for (const auto& item : row.rows) {
                ids.push_back(item.account_id);
            }
        }
        return ids;
    }

    /// 某网关已持久化的某账户展示名（未找到 = 空串）。
    std::string persisted_name(const std::string& gateway, const std::string& account_id) const {
        for (const auto& row : gateways) {
            if (row.gateway != gateway) {
                continue;
            }
            for (const auto& item : row.rows) {
                if (item.account_id == account_id) {
                    return item.account_name;
                }
            }
        }
        return {};
    }
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TESTS_FAKE_TD_ACCOUNT_REGISTRY_STORE_H_
