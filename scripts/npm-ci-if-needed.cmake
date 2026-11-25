# npm-ci-if-needed.cmake — 前端依赖安装门控（跨平台，经 `cmake -P` 调用）
#
# 背景：原先无条件 `npm ci` 会先删 node_modules，实测并行构建 / type-check 期间
# 出现 `vue-tsc not found`（exit 127），且每次前端源码变更都重复安装（#134）。
#
# 判定为"需要安装"（任一成立）：
#   1. `${FRONTEND_DIR}/node_modules/.package-lock.json` 不存在（npm 安装成功才写该隐藏
#      lockfile）——不只看目录存在：中断的半装目录会被判为未装完
#   2. 戳文件 `${BUILD_DIR}/.npm-ci.hash` 不存在
#   3. package-lock.json 的 SHA256 与戳文件记录不一致
#   4. package-lock.json 比戳文件新（touch / 切换分支后也重装）
# 安装前先删旧戳文件、成功后才写新戳：失败/中断的安装不会被下一次误判为"已装好"。
# CI 全新 build 目录没有戳文件，仍会完整 `npm ci`。
#
# 并发注意：安装窗口内会短时重建 node_modules，并发的 `npm run type-check` 可能读到
# 半装状态（#134 原症状）。正常流程经 `gate.sh build|test|preflight`（共用一把 flock），
# 不会与安装窗口重叠；绕过 gate 直接并发跑 npm/构建不在保证范围内。
#
# 调用方通过 -D 传入（见 apps/webui/CMakeLists.txt）：
#   NPM_EXECUTABLE  npm 可执行文件
#   FRONTEND_DIR    前端目录（含 package.json / package-lock.json / node_modules）
#   BUILD_DIR       构建目录（戳文件所在目录）

foreach(_var NPM_EXECUTABLE FRONTEND_DIR BUILD_DIR)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "npm-ci-if-needed: missing required variable ${_var}")
    endif()
endforeach()

set(_lock_file "${FRONTEND_DIR}/package-lock.json")
set(_stamp_file "${BUILD_DIR}/.npm-ci.hash")
set(_node_modules "${FRONTEND_DIR}/node_modules")

if(NOT EXISTS "${_lock_file}")
    message(FATAL_ERROR "npm-ci-if-needed: package-lock.json not found: ${_lock_file}")
endif()

# 符号链接的 node_modules 必须硬拒（#480）。为什么不是"跳过一次安装"就够了：
# `npm ci` 会先递归删除 node_modules 内容再重建，遇到符号链接时**删的是目标目录里的
# 东西**，然后把它替换成真实目录——于是「worktree 里 ln -s 主仓 node_modules 图快」
# 会静默清空**主工作区**的 299 个包（#471 实测：主仓 `.package-lock.json` 消失、
# node_modules 归零，破坏面跨会话，主工作区下一个会话的 preflight 直接红）。
# 判据只看 `_node_modules` 本身是否 IS_SYMLINK：目录内容里出现符号链接（pnpm 布局）
# 不受影响。worktree 的正确做法是本地 `npm ci`（约 1 分钟），不能像 conan 缓存那样共享。
if(IS_SYMLINK "${_node_modules}")
    message(FATAL_ERROR
        "npm-ci-if-needed: ${_node_modules} is a symlink; refusing to run npm ci "
        "(it would empty the link target, e.g. the main workspace's node_modules - "
        "see #480). Install frontend deps locally instead: "
        "cd ${FRONTEND_DIR} && npm ci")
endif()

file(SHA256 "${_lock_file}" _lock_hash)
set(_need_install FALSE)

if(NOT EXISTS "${_node_modules}/.package-lock.json")
    set(_need_install TRUE)
    set(_reason "node_modules missing or incomplete")
elseif(NOT EXISTS "${_stamp_file}")
    set(_need_install TRUE)
    set(_reason "install stamp missing")
else()
    file(READ "${_stamp_file}" _installed_hash)
    string(STRIP "${_installed_hash}" _installed_hash)
    if(NOT _installed_hash STREQUAL _lock_hash)
        set(_need_install TRUE)
        set(_reason "package-lock.json content changed")
    elseif("${_lock_file}" IS_NEWER_THAN "${_stamp_file}")
        set(_need_install TRUE)
        set(_reason "package-lock.json touched since last install")
    endif()
endif()

if(_need_install)
    message(STATUS "npm ci: installing deps (${_reason})")
    # 先删旧戳：安装失败/被中断时不能留下"已完成"的证据
    file(REMOVE "${_stamp_file}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "CI=true" "${NPM_EXECUTABLE}" ci
        WORKING_DIRECTORY "${FRONTEND_DIR}"
        RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "npm ci failed (exit code ${_result})")
    endif()
    file(WRITE "${_stamp_file}" "${_lock_hash}\n")
else()
    message(STATUS "npm ci: skipped (package-lock.json unchanged, node_modules present)")
endif()
