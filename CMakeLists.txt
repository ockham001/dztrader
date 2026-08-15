# ============================================================
# dztrader: 高性能量化交易系统
# 构建系统：CMake + Conan
# ============================================================
cmake_minimum_required(VERSION 3.28)

project(dztrader
    VERSION 0.0.1
    DESCRIPTION "高性能量化交易系统"
    LANGUAGES CXX)

# ============================================================
# 全局策略
# ============================================================

# C++20（非扩展模式）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 关闭 C++20 module 扫描 (CMAKE_CXX_SCAN_FOR_MODULES=OFF)。
# 必需：CMake 3.28+ + C++20 + MSVC 会默认启用 CXX_SCAN 规则，即便项目无 module 声明。
# 启用后 CMake 不再生成 deps=msvc /showIncludes 头文件依赖追踪规则，
# 导致 .ninja_deps 中所有 .obj #deps 0，头文件改动不触发重编译。
# 典型症状: 头文件加字段后旧 obj 链接新 obj, sizeof 不一致, 运行时 AV 崩溃。
# 项目无 C++20 module (无 file_set CXX_MODULES), 关闭扫描回到传统 /showIncludes 依赖追踪。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 显式指定 MSVC /showIncludes 输出前缀（中文 Windows 必需）。
# 必需：CMake 自动检测 CMAKE_CL_SHOWINCLUDES_PREFIX 时按系统 ANSI 代码页 (ACP=936/GBK)
# 解码 cl.exe 输出，但 cl.exe 实际按控制台输出代码页 (chcp 65001=UTF-8) 编码，
# 两者不匹配导致检测到乱码前缀 (UTF-8 字节被当 GBK 解码)，写入 rules.ninja 的
# msvc_deps_prefix 是乱码 UTF-8 字节，ninja 字节级比较与 cl.exe 实际输出不匹配，
# 头文件依赖追踪失效 (#deps 0)，头文件改动不触发重编译。
# 显式设置正确前缀后，CMake 用 Encoding::ConsoleOutput (UTF-8) 写入 rules.ninja，
# 与 cl.exe UTF-8 输出字节一致，ninja 能正确匹配。
# 前提：configure 和 build 时控制台代码页必须一致 (Activate-MSVC.ps1 强制 chcp 65001)。
# VSLANG=1033 在 VS 18 (2026) 上对 cl.exe /showIncludes 输出无效，不能用于强制英文。
if(MSVC)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
endif()

# Windows: 静态 CRT（MT/MTd），避免运行时依赖 vcredist
# conan profile 用 compiler.runtime=static
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 警告：全部开启，Release 视为错误
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    # C4100: 未引用参数（接口/回调常见噪音）
    add_compile_options(/wd4100)
    add_compile_options($<$<CONFIG:Release>:/WX>)
    # 启用 PDB 生成（Release 模式下保留符号，便于 crash dump 分析；不影响运行时性能）
    add_compile_options(/Zi)
    add_link_options(/DEBUG)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options($<$<CONFIG:Release>:-Werror>)
endif()

# compile_commands.json（clangd 用）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 自动复制到项目根目录供 clangd 使用
add_custom_target(copy_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_SOURCE_DIR}/compile_commands.json
    COMMENT "Copying compile_commands.json to source root for clangd")

# 启用 CTest（必须在顶层，子目录的 gtest_discover_tests 才能注册）
enable_testing()
include(GoogleTest)

# 默认构建类型
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ============================================================
# 输出目录布局：build/<plat>/<arch>/<config>/
# 主进程/网关/webui/CTP DLL 均在根目录（扁平化），tests/ 子目录
# Ninja 单配置：binaryDir 已含 config（CMakePresets 里 binaryDir = build/<plat>/<arch>/<config>），
# DZ_OUTPUT_DIR 直接用 CMAKE_BINARY_DIR，不再追加 $<CONFIG>（否则 Release/Release 重复）
# ============================================================
set(DZ_OUTPUT_DIR ${CMAKE_BINARY_DIR})
set(DZ_TESTS_DIR ${DZ_OUTPUT_DIR}/tests)

# ============================================================
# 安装布局：install/<plat>/<arch>/<config>/（与 build 同结构）
# install(TARGETS) 只拷贝最终二进制，自动排除 .obj/CMakeFiles/.pdb 等中间产物
# Ninja 单配置：binaryDir 已含 config，install 不需 --config
# ============================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(DZ_PLATFORM "windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DZ_PLATFORM "linux")
else()
    set(DZ_PLATFORM "${CMAKE_SYSTEM_NAME}")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    set(DZ_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(DZ_ARCH "arm64")
else()
    set(DZ_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
# 相对于 CMAKE_INSTALL_PREFIX 的路径（install 命令 DESTINATION 用相对路径）
set(DZ_INSTALL_DIR ${DZ_PLATFORM}/${DZ_ARCH}/$<CONFIG>)
# 默认安装前缀：项目根/install（用户未显式指定时）
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install prefix" FORCE)
endif()

# ============================================================
# 第三方依赖（由 conan install 生成的 CMakeDeps 提供）
# 全部用 CONFIG 模式 + 导入目标，符合现代 CMake 最佳实践
# ============================================================
find_package(Boost CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# spdlog header_only 模式下 conan 生成 spdlog::spdlog_header_only 而非 spdlog::spdlog
# 创建 ALIAS 保持现有代码 spdlog::spdlog 引用不变（CMake 3.18+ 支持 ALIAS 引用 IMPORTED target）
if(TARGET spdlog::spdlog_header_only AND NOT TARGET spdlog::spdlog)
    add_library(spdlog::spdlog ALIAS spdlog::spdlog_header_only)
endif()
find_package(toml11 CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# ============================================================
# version: 生成 version.h（header-only）
# 单一真相源为上方 project(VERSION)，通过变量映射保持 version.h.in 模板不改
# 生成到源码树 libs/strategy_api/include/dztrader/version.h（与 version.h.in 同目录），
# 该目录已在 strategy_api_headers 的 include 路径中，无需额外 target_include_directories。
# 生成物在 .gitignore 中，不会污染源码仓库。
# ============================================================
add_library(version INTERFACE)
set(VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(VERSION_ALTER ${PROJECT_VERSION_PATCH})  # 兼容 version.h.in 模板变量名
set(VERSION ${PROJECT_VERSION})
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/strategy_api/include/dztrader/version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/strategy_api/include/dztrader/version.h)

# ============================================================
# strategy_api_headers: header-only 策略接口头文件
# 零内部依赖，所有内部模块的头文件依赖此契约
# ============================================================
add_library(strategy_api_headers INTERFACE)
target_include_directories(strategy_api_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/strategy_api/include)
target_link_libraries(strategy_api_headers INTERFACE version)

# ============================================================
# 子模块（按依赖顺序逐步 add_subdirectory）
# ============================================================
add_subdirectory(libs/core)
add_subdirectory(libs/process)
add_subdirectory(libs/log)
add_subdirectory(libs/shm)
add_subdirectory(libs/db)
add_subdirectory(libs/strategy_api)
add_subdirectory(libs/platform)
add_subdirectory(apps/master)
add_subdirectory(apps/ctp)
add_subdirectory(apps/webui)
add_subdirectory(strategies/demo)
