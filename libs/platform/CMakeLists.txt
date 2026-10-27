# ============================================================
# dzplatform_headers: 高层平台服务库（STATIC，含 notify_ui.cpp 等实现）
#
# 职责：组合 log/shm/db 等底层库，提供帧适配层（frame_writer）
#       及未来的 FrameRouter / ConfigPipeline 等。
#
# 依赖方向（自下而上）：
#   strategy_api_headers (契约)
#        ↑
#   core / log / shm / db / process   (低层基础设施，互不依赖)
#        ↑
#   platform                          (本库：组合底层服务)
#        ↑
#   apps                              (master / ctp / webui / ...)
#
# 设计约束：
#   - log 与 shm 同层不可互相依赖，本层负责组合二者
#   - 编译进 dztrader_sdk.dll 的代码禁止依赖 dzplatform
#   - 本层只服务平台进程，不进入策略进程
# ============================================================
add_library(dzplatform_headers STATIC)
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(dzplatform_headers PRIVATE ${PLATFORM_SOURCES})
target_include_directories(dzplatform_headers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzplatform_headers PUBLIC
    dzcore dzlog dzshm
    strategy_api_headers
    nlohmann_json::nlohmann_json magic_enum::magic_enum
    spdlog::spdlog
)
if(WIN32)
    # sysmon 的 Windows 负载代理走 PDH（\System\Processor Queue Length，契约 sysmon §2.7）；
    # 内存与页文件口径走 GetPerformanceInfo（psapi，契约 sysmon §2.4）；
    # 每网卡累计计数器走 GetIfTable2 / FreeMibTable（iphlpapi，契约 sysmon §2.4）。
    target_link_libraries(dzplatform_headers PUBLIC pdh psapi iphlpapi)
endif()

# ============================================================
# 单元测试
# ============================================================
file(GLOB PLATFORM_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${PLATFORM_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzplatform_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzplatform_${testname} PRIVATE
        dzplatform_headers dzshm dzlog dzcore dzshm_test_support GTest::gtest
        spdlog::spdlog nlohmann_json::nlohmann_json)
    # 平台单测直接覆盖 src/ 下的内部接缝（Linux /proc/meminfo 解析的异常值防御，
    # 契约 sysmon §2.7）；内部头文件不安装、不进入其它目标，仅该系统采集测试可见。
    if(testname STREQUAL "system_collector_test")
        target_include_directories(dzplatform_${testname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    endif()
    # 测试用指定初始化器只填部分字段，豁免 -Wmissing-field-initializers
    # (与 apps/ctp 测试目标的处理一致; MSVC 无此警告)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dzplatform_${testname} PRIVATE -Wno-missing-field-initializers)
    endif()
    set_target_properties(dzplatform_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    if(MSVC AND testname STREQUAL "sysmon_test")
        # gtest 1.17 gtest-printers.h 在 MSVC /O2 下对 nlohmann::json 相关模板实例化
        # 偶发 C4702（unreachable code），Release /WX 会升级为错误；仅豁免测试目标。
        target_compile_options(dzplatform_${testname} PRIVATE /wd4702)
    endif()
    gtest_discover_tests(dzplatform_${testname} PROPERTIES LABELS fast)
endforeach()
