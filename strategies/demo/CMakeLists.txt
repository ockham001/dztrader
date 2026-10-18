# ============================================================
# stg_demo: 常驻示例策略 (StrategyBase + UI_INPUT/OUTPUT_UI)
#
# 关键约束: exe 名即策略ID (strategy_id = exe_stem), master 配置中
# strategy[].name 必须为 "stg_demo", 事件通道订阅者身份为 stg.stg_demo。
# 仅链接策略 SDK (dzstrategy_sdk_static), 不依赖任何网关/web 内部库。
# 行为核心在 stg_demo_core (静态库), 供进程入口与进程内集成测试共用。
# ============================================================
add_library(stg_demo_core STATIC
    stg_demo_strategy.cpp
)
target_include_directories(stg_demo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(stg_demo_core PUBLIC dzstrategy_sdk_static)

add_executable(stg_demo
    stg_demo.cpp
)
target_link_libraries(stg_demo PRIVATE stg_demo_core)
set_target_properties(stg_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)

# ============================================================
# stg_demo_test: 进程内集成测试 (Seam A: 帧通道边界)
# 临时 DZTRADER_HOME + 真实事件/行情通道, 测试线程跑 run_strategy;
# 只经 UI_INPUT 写入 / OUTPUT_UI 读出 / TICK 与回报帧注入验证行为。
# RUN_SERIAL: 机器全局 NamedSemaphore + 会话固定 DZTRADER_HOME,
# 不与并行测试混跑 (与 SDK API 测试同理)。
# ============================================================
add_executable(stg_demo_test
    tests/stg_demo_test.cpp
    tests/test_main.cpp
)
target_link_libraries(stg_demo_test PRIVATE stg_demo_core GTest::gtest)
set_target_properties(stg_demo_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
gtest_discover_tests(stg_demo_test PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

install(TARGETS stg_demo RUNTIME DESTINATION ${DZ_INSTALL_DIR})
install(TARGETS stg_demo_test RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
