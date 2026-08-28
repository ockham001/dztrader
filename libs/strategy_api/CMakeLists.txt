# ============================================================
# 策略接口 SDK: 同一源码,两种链接方式
#   dzstrategy_sdk_static -> 链接进平台各进程 + 当前示例策略 stg_demo
#                            (DZ_API_COMPILE_STATIC; 策略自包含单文件, 无部署依赖)
#   dzstrategy_sdk        -> dzstrategy_sdk.dll/.so (DZ_API_EXPORTS)
#                            预留给"分发编译后策略"的形态: ABI 稳定边界 + 独立更新,
#                            默认不构建 (策略需要时 --target dzstrategy_sdk)
# 测试直接测试内部类（result_set 等），不链接 api.cpp
# ============================================================

# 共享源码列表（static / shared 复用）
set(SDK_SOURCES
    src/api.cpp
    src/cursor_result_set.cpp
    src/vector_result_set.cpp
)

# 内部头文件目录（src 下的 .h 是内部实现头）
set(SDK_INTERNAL_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ============================================================
# dzstrategy_sdk_static: 静态库，链接进平台进程
# ============================================================
add_library(dzstrategy_sdk_static STATIC ${SDK_SOURCES})
target_include_directories(dzstrategy_sdk_static PUBLIC ${SDK_INTERNAL_INCLUDES})
target_compile_definitions(dzstrategy_sdk_static PUBLIC DZ_API_COMPILE_STATIC)
target_link_libraries(dzstrategy_sdk_static
    PUBLIC strategy_api_headers dzcore dzshm
    PUBLIC Boost::boost nlohmann_json::nlohmann_json
)

# ============================================================
# dzstrategy_sdk: 动态库 dzstrategy_sdk.dll/.so
# 默认不构建（策略才需要），通过 --target dzstrategy_sdk 单独构建
# ============================================================
add_library(dzstrategy_sdk SHARED ${SDK_SOURCES})
target_include_directories(dzstrategy_sdk PRIVATE ${SDK_INTERNAL_INCLUDES})
target_compile_definitions(dzstrategy_sdk PRIVATE DZ_API_EXPORTS)
set_target_properties(dzstrategy_sdk PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    # 输出到 build 根:Windows DLL 用 RUNTIME_OUTPUT_DIRECTORY,Linux .so 用 LIBRARY_OUTPUT_DIRECTORY
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
target_link_libraries(dzstrategy_sdk
    PUBLIC strategy_api_headers dzcore dzshm
    PUBLIC Boost::boost nlohmann_json::nlohmann_json
)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译
# 测试只编译内部类（不含 api.cpp，避免导出符号；测试 result_set 等）
# 例外：notify_ui_test / trade_api_test 链接真实 SDK (api.cpp)，
#       覆盖 dz_* C 接口的完整写入/错误路径。
# ============================================================
set(SDK_TEST_SOURCES
    src/vector_result_set.cpp
    src/cursor_result_set.cpp
)

# 链接 api.cpp 的测试 (覆盖 C 接口完整路径)
set(SDK_API_TESTS
    notify_ui_test
    trade_api_test
    md_api_test
    schedule_api_test
)
file(GLOB SDK_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${SDK_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzstrategy_sdk_${testname}
        ${testfile}
        tests/test_main.cpp
        ${SDK_TEST_SOURCES})
    target_include_directories(dzstrategy_sdk_${testname} PRIVATE ${SDK_INTERNAL_INCLUDES})
    target_link_libraries(dzstrategy_sdk_${testname} PRIVATE
        strategy_api_headers dzcore dzshm GTest::gtest
        Boost::boost nlohmann_json::nlohmann_json)
    if(testname IN_LIST SDK_API_TESTS)
        target_sources(dzstrategy_sdk_${testname} PRIVATE src/api.cpp)
        # api.cpp 直接编入测试可执行文件（自包含, 不经 DLL 导出）,
        # 语义同 dzstrategy_sdk_static: DZ_API 展开为空。
        # 缺此定义时 MSVC 落到 dllimport 分支, 库内定义函数体报 C2491。
        target_compile_definitions(dzstrategy_sdk_${testname} PRIVATE DZ_API_COMPILE_STATIC)
    endif()
    set_target_properties(dzstrategy_sdk_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    # SDK API 测试 (md/notify/trade) 使用固定 DZTRADER_HOME + 机器全局 SHM 通道名,
    # gtest_discover_tests 把每个用例注册为独立 ctest 测试, -j 并行时同二进制
    # 多进程共用同一通道文件互相踩踏 -> RUN_SERIAL 串行化。
    if(testname IN_LIST SDK_API_TESTS)
        gtest_discover_tests(dzstrategy_sdk_${testname} PROPERTIES RUN_SERIAL TRUE)
    else()
        gtest_discover_tests(dzstrategy_sdk_${testname})
    endif()
    install(TARGETS dzstrategy_sdk_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
