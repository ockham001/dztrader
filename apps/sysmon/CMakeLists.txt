# ============================================================
# dzsysmon：内部系统监控进程（契约 sysmon）
#
# 职责：周期采集本机 CPU/系统信息，按 realtime.push_interval_sec 向事件通道
#       推送 RTN_SYSMON_SNAPSHOT；响应 SET_SYSMON_CONFIG/SET_LOG_CONFIG 等定向帧。
#
# dzsysmon_obj：OBJECT 库（可复用源文件），测试链接它而无需拉入 main.cpp。
# ============================================================
add_library(dzsysmon_obj OBJECT
    app.cpp
    clock_probe.cpp
    clock_thresholds.cpp
    config.cpp
    cpu_saturation.cpp
    cpu_usage.cpp
    disk_io.cpp
    history_recorder.cpp
    network.cpp
    ntp_codec.cpp
    ntp_transport.cpp
    probe_schedule.cpp
    snapshot_builder.cpp
    thresholds.cpp
)
target_include_directories(dzsysmon_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(dzsysmon_obj
    PUBLIC dzplatform_headers dzcore dzlog dzshm dzsysmonstore Boost::boost
)
if(WIN32)
    # std::getenv / _CRT 安全函数告警在 MSVC Release（/WX）下升级为错误
    target_compile_definitions(dzsysmon_obj PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# ============================================================
# dzsysmon：主可执行文件（薄壳，进程主体在 app.cpp / dzsysmon_obj 里）
# 输出到构建目录根（DZ_OUTPUT_DIR），输出名 dzsysmon
# ============================================================
add_executable(dzsysmon main.cpp)
target_link_libraries(dzsysmon PRIVATE dzsysmon_obj)
if(WIN32)
    target_compile_definitions(dzsysmon PRIVATE _CRT_SECURE_NO_WARNINGS)
    # 平台版本探测走 ntdll!RtlGetVersion（advapi32 为注册表读取 CPU 型号所需）
    target_link_libraries(dzsysmon PRIVATE advapi32)
endif()
set_target_properties(dzsysmon PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
install(TARGETS dzsysmon RUNTIME DESTINATION ${DZ_INSTALL_DIR})

# ============================================================
# dzsysmon_clock_e2e_runner：时钟探测进程级 e2e 的测试专用可执行文件（#299）
# 与生产 dzsysmon 同一份进程主体，只把 NTP 传输换成脚本化假实现（不依赖外网）。
# 与测试二进制同目录（tests/），clock_e2e_test 通过 this_process::exe_dir() 定位。
# **输出名 = `dzsysmon`**：进程名（instance_id / 配置文件名 / 日志名 / 快照 source）
# 由可执行文件 stem 决定，而快照校验要求 `source == dzsysmon`（契约 sysmon §5）——
# 假传输不得改变进程身份。生产 dzsysmon 在构建目录根，本 runner 在 tests/，不冲突。
# 先例：apps/master 的 test_worker（同为测试辅助可执行文件）。
# ============================================================
add_executable(dzsysmon_clock_e2e_runner
    tests/clock_e2e_runner.cpp
)
target_include_directories(dzsysmon_clock_e2e_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(dzsysmon_clock_e2e_runner PRIVATE dzsysmon_obj)
if(WIN32)
    target_compile_definitions(dzsysmon_clock_e2e_runner PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(dzsysmon_clock_e2e_runner PRIVATE advapi32)
endif()
set_target_properties(dzsysmon_clock_e2e_runner PROPERTIES
    OUTPUT_NAME dzsysmon
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译
# 纯函数/配置测试不依赖真实时钟长睡眠，全部 fast label。
# ============================================================
file(GLOB DZSYSMON_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${DZSYSMON_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    # 每轮重置（foreach 作用域变量会跨迭代保留，漏重置会让后续测试继承上一个的依赖）
    set(test_needs_daemon FALSE)
    set(test_needs_clock_runner FALSE)
    # 进程级 E2E（#77/#80/#81/#123）：起真实 dzsysmon 观察帧序（SET_SYSMON_CONFIG 后立即推送
    # 快照；#123 起资源越限不再出现 NOTIFY_UI；#81 运行期配置的全段生效/定时器重排/history 开关）。
    # 事件通道命名信号量是机器全局命名空间，与 dev 栈/其它测试实例互扰，必须串行；
    # 且需先构建 dzsysmon（测试从 <build>/tests 的上一级定位该可执行文件）。
    if(testname STREQUAL "network_config_e2e_test" OR testname STREQUAL "history_e2e_test" OR
       testname STREQUAL "alert_e2e_test" OR testname STREQUAL "config_e2e_test")
        set(test_needs_daemon TRUE)
        set(test_properties RUN_SERIAL TRUE)
    elseif(testname STREQUAL "clock_e2e_test")
        # #299：起测试专用可执行文件 dzsysmon_clock_e2e_runner（真实进程主体 + 假 NTP 传输），
        # 需先构建它。`TIMEOUT` 经 `gtest_discover_tests` 透传，作用域是**每个用例**而不是
        # 整个目标——用例数随票增长（#301 起 14 个），单例取值不受影响。
        # 单个用例里最长的等待是「下一个整分钟」：TZ 秒级偏移生效时约 8s，平台不支持时最多
        # ~60s，外加若干 5–10s 的条件等待，故单例 TIMEOUT 取 240 留足余量。
        set(test_needs_daemon FALSE)
        set(test_needs_clock_runner TRUE)
        set(test_properties RUN_SERIAL TRUE TIMEOUT 240)
    elseif(testname STREQUAL "collector_oracle_test")
        # 真机 oracle（#136 AC5）：读 ctest 注入的 DZTRADER_HOME/shm（全仓测试共享同一目录），
        # DiskDomainsHoldOnRealMachine 先 collect_shm_dir 再用 recursive_directory_iterator
        # 独立走第二遍同一目录，断言 page_files 与 oldest_file_age_sec「同源」（⟺）以及
        # 页文件字节 ≤ 目录总字节。两次读取之间若有并发进程增删页文件，两个视图取自不同
        # 时刻会让 ⟺ 与 ≤ 翻转 —— 必须与往该目录写 shm 的用例互斥。
        set(test_needs_daemon FALSE)
        set(test_properties RUN_SERIAL TRUE TIMEOUT 60)
    else()
        set(test_needs_daemon FALSE)
        set(test_properties "")
    endif()
    dz_add_test(dzsysmon_${testname}
        SOURCES ${testfile} ../master/tests/test_main.cpp
        LABELS fast
        PROPERTIES ${test_properties})
    target_link_libraries(dzsysmon_${testname} PRIVATE
        dzsysmon_obj
        dzshm_test_support
        GTest::gtest
    )
    target_include_directories(dzsysmon_${testname} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    # MSVC C4702（gtest + nlohmann::json）豁免已收敛到 cmake/DzTest.cmake 的 dz_add_test。
    if(test_needs_daemon)
        add_dependencies(dzsysmon_${testname} dzsysmon)
    endif()
    if(test_needs_clock_runner)
        add_dependencies(dzsysmon_${testname} dzsysmon_clock_e2e_runner)
    endif()
endforeach()
