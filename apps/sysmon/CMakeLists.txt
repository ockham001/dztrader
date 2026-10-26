# ============================================================
# dzsysmon：内部系统监控进程（契约 sysmon）
#
# 职责：周期采集本机 CPU/系统信息，按 realtime.push_interval_sec 向事件通道
#       推送 RTN_SYSMON_SNAPSHOT；响应 SET_SYSMON_CONFIG/SET_LOG_CONFIG 等定向帧。
#
# dzsysmon_obj：OBJECT 库（可复用源文件），测试链接它而无需拉入 main.cpp。
# ============================================================
add_library(dzsysmon_obj OBJECT
    config.cpp
    cpu_saturation.cpp
    cpu_usage.cpp
    disk_io.cpp
    history_recorder.cpp
    network.cpp
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
# dzsysmon：主可执行文件
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
# 单元测试：每个 *_test.cpp 独立编译
# 纯函数/配置测试不依赖真实时钟长睡眠，全部 fast label。
# ============================================================
file(GLOB DZSYSMON_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${DZSYSMON_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzsysmon_${testname} ${testfile} ../master/tests/test_main.cpp)
    target_link_libraries(dzsysmon_${testname} PRIVATE
        dzsysmon_obj
        dzshm_test_support
        GTest::gtest
    )
    target_include_directories(dzsysmon_${testname} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    set_target_properties(dzsysmon_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    if(MSVC)
        # gtest 1.17 gtest-printers.h + nlohmann::json 在 MSVC /O2 下偶发 C4702
        # （unreachable code），Release /WX 升级为错误；仅豁免测试目标。
        target_compile_options(dzsysmon_${testname} PRIVATE /wd4702)
    endif()
    # 进程级 E2E（#77/#80/#81/#123）：起真实 dzsysmon 观察帧序（SET_SYSMON_CONFIG 后立即推送
    # 快照；#123 起资源越限不再出现 NOTIFY_UI；#81 运行期配置的全段生效/定时器重排/history 开关）。
    # 事件通道命名信号量是机器全局命名空间，与 dev 栈/其它测试实例互扰，必须串行；
    # 且需先构建 dzsysmon（测试从 <build>/tests 的上一级定位该可执行文件）。
    if(testname STREQUAL "network_config_e2e_test" OR testname STREQUAL "history_e2e_test" OR
       testname STREQUAL "alert_e2e_test" OR testname STREQUAL "config_e2e_test")
        add_dependencies(dzsysmon_${testname} dzsysmon)
        gtest_discover_tests(dzsysmon_${testname} PROPERTIES
            LABELS fast
            RUN_SERIAL TRUE
        )
    elseif(testname STREQUAL "collector_oracle_test")
        # 真机 oracle（#136 AC5）：读 ctest 注入的 DZTRADER_HOME/shm（全仓测试共享同一目录），
        # DiskDomainsHoldOnRealMachine 先 collect_shm_dir 再用 recursive_directory_iterator
        # 独立走第二遍同一目录，断言 page_files 与 oldest_file_age_sec「同源」（⟺）以及
        # 页文件字节 ≤ 目录总字节。两次读取之间若有并发进程增删页文件，两个视图取自不同
        # 时刻会让 ⟺ 与 ≤ 翻转 —— 必须与往该目录写 shm 的用例互斥。
        gtest_discover_tests(dzsysmon_${testname} PROPERTIES
            LABELS fast
            RUN_SERIAL TRUE
            TIMEOUT 60
        )
    else()
        gtest_discover_tests(dzsysmon_${testname} PROPERTIES LABELS fast)
    endif()
    install(TARGETS dzsysmon_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
