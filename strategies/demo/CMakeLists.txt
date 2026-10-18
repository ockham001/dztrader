# ============================================================
# stg_demo: 常驻示例策略 (StrategyBase + UI_INPUT/OUTPUT_UI)
#
# 关键约束: exe 名即策略ID (strategy_id = exe_stem), master 配置中
# strategy[].name 必须为 "stg_demo", 事件通道订阅者身份为 stg.stg_demo。
# 链接自包含动态 SDK (dzstrategy_sdk, ADR 0020): 示例即外部策略作者的构建
# 参照 —— 只 include 公开头文件 + 链接动态库 + 同目录放置动态库即可运行,
# 不接触任何第三方、不依赖任何网关/web 内部库。
# 行为核心在 stg_demo_core (静态库), 供进程入口与进程内集成测试共用。
# ============================================================
add_library(stg_demo_core STATIC
    stg_demo_strategy.cpp
)
target_include_directories(stg_demo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(stg_demo_core PUBLIC dzstrategy_sdk)

add_executable(stg_demo
    stg_demo.cpp
)
target_link_libraries(stg_demo PRIVATE stg_demo_core)
set_target_properties(stg_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
# 动态库与示例可执行同目录定位: Linux 用 $ORIGIN rpath (构建树与安装布局一致),
# Windows 由默认加载器搜索 exe 所在目录命中同目录 DLL。
if(UNIX)
    set_target_properties(stg_demo PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# ============================================================
# stg_demo_test: 进程内集成测试 (Seam A: 帧通道边界)
# 临时 DZTRADER_HOME + 真实事件/行情通道, 测试线程跑 run_strategy;
# 只经 UI_INPUT 写入 / OUTPUT_UI 读出 / TICK 与回报帧注入验证行为。
# 测试 exe 在 tests/ 下、动态库在上级目录: Linux 用 $ORIGIN/.. rpath,
# Windows 构建后把 DLL 复制到 tests/ (该 POST_BUILD 命令先于
# gtest_discover_tests 声明 —— 同一目标的 POST_BUILD 按声明顺序执行,
# 故构建期列举用例时 DLL 已就位)。
# 额外链 dzshm 仅因测试自身要用内部帧通道做探针 (见 tests/stg_demo_test.cpp);
# 策略侧 (stg_demo_core) 仍只经动态 SDK, 测试不是示例的构建参照。
# RUN_SERIAL: 机器全局 NamedSemaphore + 会话固定 DZTRADER_HOME,
# 不与并行测试混跑 (与 SDK API 测试同理)。
# ============================================================
add_executable(stg_demo_test
    tests/stg_demo_test.cpp
    tests/test_main.cpp
)
target_link_libraries(stg_demo_test PRIVATE stg_demo_core dzshm GTest::gtest)
set_target_properties(stg_demo_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
if(UNIX)
    set_target_properties(stg_demo_test PROPERTIES
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )
else()
    add_custom_command(TARGET stg_demo_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:dzstrategy_sdk> $<TARGET_FILE_DIR:stg_demo_test>
        COMMENT "Copy dzstrategy_sdk next to stg_demo_test"
    )
endif()
gtest_discover_tests(stg_demo_test PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

install(TARGETS stg_demo RUNTIME DESTINATION ${DZ_INSTALL_DIR})
install(TARGETS stg_demo_test RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
# Windows 安装树与构建树同布局: 加载器只搜索 exe 所在目录, 测试可执行同目录
# 需再装一份 DLL (Linux 由测试 exe 的 $ORIGIN/.. rpath 覆盖, 无需重复安装)
if(WIN32)
    install(TARGETS dzstrategy_sdk RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endif()
