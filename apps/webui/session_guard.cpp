#include "session_guard.h"

#include <spdlog/spdlog.h>

namespace dztrader::webui {

SessionState check_session(Repository& repo, const std::string& username, int64_t token_version) {
    try {
        auto user = repo.get_user_by_username(username);
        if (!user.has_value()) {
            return SessionState::InvalidToken;
        }
        // 禁用账户的既有 token 立即失效（契约 rest §2.2）
        if (user->status == "disabled") {
            return SessionState::InvalidToken;
        }
        // ver 与 DB 现值不等 → 改密 / 重置 / 禁用后签发点已失效（契约 rest §1）
        if (user->token_version != token_version) {
            return SessionState::InvalidToken;
        }
        if (user->must_change_password != 0) {
            return SessionState::PasswordChangeRequired;
        }
        return SessionState::Valid;
    } catch (const std::exception& e) {
        // 吞因：查询失败按无效会话处理（保守拒绝优于放行），不向调用方抛 500。
        // WARN 而非 DEBUG：SPDLOG_DEBUG 在 Release 编译掉后 catch 参数 e 不再被引用，
        // MSVC /WX 报 C4101；且会话判定失败会让用户被踢下线，值得运维看见
        SPDLOG_WARN("session check failed, treat as invalid | user={} error=\"{}\"", username,
                    e.what());
        return SessionState::InvalidToken;
    }
}

bool is_path_allowed_before_password_change(const std::string& path) {
    // 改密本身与「我是谁」（前端据此刷新 isAdmin / 展示账户信息）必须放行，
    // 否则强制改密弹窗无法工作（契约 rest §1「强制改密」）
    return path == "/api/auth/change-password" || path == "/api/user/me";
}

}  // namespace dztrader::webui
