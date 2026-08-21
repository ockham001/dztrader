#ifndef DZTRADER_WEBUI_TESTS_FAKE_DATA_CHANGE_NOTIFIER_H_
#define DZTRADER_WEBUI_TESTS_FAKE_DATA_CHANGE_NOTIFIER_H_

#include <string>
#include <vector>
#include "data_change_notifier.h"

namespace dztrader::webui {

/// 测试用 DataChangeNotifier 假实现：记录广播 scope 与踢人 username，供单测注入断言。
class FakeDataChangeNotifier : public DataChangeNotifier {
public:
    std::vector<std::string> scopes;     // broadcast_data_changed 记录（按调用顺序）
    std::vector<std::string> kicked;     // kick_user 记录（按调用顺序）

    void broadcast_data_changed(const std::string& scope) override { scopes.push_back(scope); }
    void kick_user(const std::string& username) override { kicked.push_back(username); }
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TESTS_FAKE_DATA_CHANGE_NOTIFIER_H_