# ============================================================
# dzcore: 基础设施（STATIC 库）
# 配置 + LastError + Exception
# 依赖策略接口层头文件获取类型/错误码
# ============================================================
add_library(dzcore STATIC)
target_sources(dzcore PRIVATE
    src/encoding.cpp
    src/env.cpp
    src/last_error.cpp
    src/path.cpp
    src/this_process.cpp
    src/timer_queue.cpp
)
target_include_directories(dzcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzcore
    PUBLIC strategy_api_headers
    PUBLIC Boost::process spdlog::spdlog toml11::toml11
           nlohmann_json::nlohmann_json magic_enum::magic_enum
)
# 可被链入 dzstrategy_sdk 动态库，静态库对象需 PIC（与 dzdb/dztdstore/dztrading 同）。
# 缺此设置时：SDK 动态库一旦引用本库某个含外部数据符号直接寻址的 TU（如 env.cpp），
# 共享链接会以 "relocation R_X86_64_PC32 ... recompile with -fPIC" 失败（ADR 0016）。
set_target_properties(dzcore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 测试共用的临时资源唯一化头（tests/test_temp_dir.h），全仓测试目标复用。
# 放 dzcore 而非各业务模块：唯一性只需 pid + 随机数，不含模块依赖（#136）。
# ============================================================
add_library(dztest_support INTERFACE)
target_include_directories(dztest_support INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB CORE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${CORE_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzcore_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzcore_${testname} PRIVATE dzcore dztest_support GTest::gtest)
    set_target_properties(dzcore_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dzcore_${testname} PROPERTIES LABELS fast)
endforeach()
