# ============================================================
# dzsdkdb: 策略 SDK 私有的数据库层（端口 + 驱动；SQLite 恒有，MySQL 由 DZ_WITH_MYSQL 决定）
#
# ADR 0044：本层由平台公共端口 `dzdb`（原 `libs/db`）私有化进 SDK。
# **平台进程（td / tdstore / dzweb）不得链接本目标**——平台侧交易数据一律直连 SQLite
# （ADR 0041）；MySQL 驱动仅为将来行情/归档数据保留（ADR 0019），本轮不删。
# 对外不可见：不安装、不导出符号（SDK 只导出 dz_* C ABI，ADR 0038），头文件也不在
# `strategy_api_headers` 的公开 include 面内。
#
# 依赖 SQLiteCpp (Conan 提供) + dzcore；DZ_WITH_MYSQL=ON 时加 Boost/OpenSSL（静态）
# 并发模型: Database 共享, Session 一线程一个（每会话独立连接）
# ============================================================
add_library(dzsdkdb STATIC)
file(GLOB DB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# DZ_WITH_MYSQL=OFF 时整文件排除 mysql 驱动（源码内以 DZ_WITH_MYSQL 宏决定注册与否）
if(NOT DZ_WITH_MYSQL)
    list(FILTER DB_SOURCES EXCLUDE REGEX "/mysql_[^/]*\\.cpp$")
endif()
target_sources(dzsdkdb PRIVATE ${DB_SOURCES})
target_include_directories(dzsdkdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dzsdkdb
    PUBLIC dzcore
    PUBLIC SQLiteCpp
)
if(DZ_WITH_MYSQL)
    # Boost.MySQL 为 header-only；编译期需 Boost 头 + OpenSSL 头，
    # 链接期需 boost_charconv/OpenSSL（静态）——SDK 单文件分发不得有运行时依赖
    target_compile_definitions(dzsdkdb PUBLIC DZ_WITH_MYSQL=1)
    target_link_libraries(dzsdkdb
        PUBLIC Boost::headers Boost::charconv
        PUBLIC OpenSSL::SSL OpenSSL::Crypto
    )
endif()
if(WIN32)
    target_compile_definitions(dzsdkdb PUBLIC _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()
# 可被链入 dztrader_sdk 动态库，静态库对象需 PIC。
set_target_properties(dzsdkdb PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB DB_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${DB_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    dz_add_test(dzsdkdb_${testname} SOURCES ${testfile} tests/test_main.cpp LABELS fast)
    target_link_libraries(dzsdkdb_${testname} PRIVATE dzsdkdb dztest_support GTest::gtest)
    target_include_directories(dzsdkdb_${testname} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)
endforeach()
