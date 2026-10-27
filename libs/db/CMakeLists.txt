# ============================================================
# dzdb: 数据库统一接口 + 驱动（SQLite 恒有；MySQL 由 DZ_WITH_MYSQL 决定，ADR 0016）
# 依赖 SQLiteCpp (Conan 提供) + dzcore；DZ_WITH_MYSQL=ON 时加 Boost/OpenSSL（静态）
# 并发模型: Database 共享, Session 一线程一个（每会话独立连接）
# ============================================================
add_library(dzdb STATIC)
file(GLOB DB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# DZ_WITH_MYSQL=OFF 时整文件排除 mysql 驱动（源码内以 DZ_WITH_MYSQL 宏决定注册与否）
if(NOT DZ_WITH_MYSQL)
    list(FILTER DB_SOURCES EXCLUDE REGEX "/mysql_[^/]*\\.cpp$")
endif()
target_sources(dzdb PRIVATE ${DB_SOURCES})
target_include_directories(dzdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dzdb
    PUBLIC dzcore
    PUBLIC SQLiteCpp
)
if(DZ_WITH_MYSQL)
    # Boost.MySQL 为 header-only；编译期需 Boost 头 + OpenSSL 头，
    # 链接期需 boost_charconv/OpenSSL（静态）——SDK 单文件分发不得有运行时依赖
    target_compile_definitions(dzdb PUBLIC DZ_WITH_MYSQL=1)
    target_link_libraries(dzdb
        PUBLIC Boost::headers Boost::charconv
        PUBLIC OpenSSL::SSL OpenSSL::Crypto
    )
endif()
if(WIN32)
    target_compile_definitions(dzdb PUBLIC _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()
# 可被链入 dzstrategy_sdk 动态库，静态库对象需 PIC。
set_target_properties(dzdb PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB DB_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${DB_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzdb_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzdb_${testname} PRIVATE dzdb dztest_support GTest::gtest)
    target_include_directories(dzdb_${testname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)
    set_target_properties(dzdb_${testname} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})
    gtest_discover_tests(dzdb_${testname} PROPERTIES LABELS fast)
endforeach()
