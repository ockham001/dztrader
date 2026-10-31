#include "password_policy.h"

namespace dztrader::webui {

bool is_password_acceptable(const std::string& password) {
    return password.size() >= kMinPasswordLength;
}

}  // namespace dztrader::webui
