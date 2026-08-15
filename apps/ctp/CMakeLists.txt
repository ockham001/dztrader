# ============================================================
# CTP 行情网关（dzmd_ctp）
# 第三方 CTP API 在 third_party/ctp/<plat>/，用 IMPORTED 目标封装
# 输出到构建目录根（DZ_OUTPUT_DIR），运行时 CTP DLL/SO 在同目录
# ============================================================

# 平台相关 CTP 目录
if(WIN32)
    set(CTP_DIR ${CMAKE_SOURCE_DIR}/third_party/ctp/windows)
    set(CTP_MD_LIB ${CTP_DIR}/thostmduserapi_se.lib)
    set(CTP_MD_DLL ${CTP_DIR}/thostmduserapi_se.dll)
else()
    set(CTP_DIR ${CMAKE_SOURCE_DIR}/third_party/ctp/linux)
    set(CTP_MD_SO ${CTP_DIR}/thostmduserapi_se.so)
endif()

# IMPORTED 目标：CTP 行情 API（thostmduserapi_se）
# 提供 include 目录 + 链接 import lib
add_library(dzmd_ctp_api SHARED IMPORTED)
if(WIN32)
    set_target_properties(dzmd_ctp_api PROPERTIES
        IMPORTED_LOCATION "${CTP_MD_DLL}"
        IMPORTED_IMPLIB "${CTP_MD_LIB}"
    )
else()
    set_target_properties(dzmd_ctp_api PROPERTIES
        IMPORTED_LOCATION "${CTP_MD_SO}"
    )
endif()
target_include_directories(dzmd_ctp_api INTERFACE ${CTP_DIR})

# ============================================================
# IMPORTED 目标：CTP 交易 API (thosttraderapi_se)
# ============================================================
if(WIN32)
    set(CTP_TD_LIB ${CTP_DIR}/thosttraderapi_se.lib)
    set(CTP_TD_DLL ${CTP_DIR}/thosttraderapi_se.dll)
else()
    set(CTP_TD_SO ${CTP_DIR}/thosttraderapi_se.so)
endif()

add_library(dztd_ctp_api SHARED IMPORTED)
if(WIN32)
    set_target_properties(dztd_ctp_api PROPERTIES
        IMPORTED_LOCATION "${CTP_TD_DLL}"
        IMPORTED_IMPLIB "${CTP_TD_LIB}"
    )
else()
    set_target_properties(dztd_ctp_api PROPERTIES
        IMPORTED_LOCATION "${CTP_TD_SO}"
    )
endif()
target_include_directories(dztd_ctp_api INTERFACE ${CTP_DIR})

# ============================================================
# dzmd_ctp: 行情网关可执行文件
# 输出到构建目录根（DZ_OUTPUT_DIR），与主进程同目录
# ============================================================
add_executable(dzmd_ctp
    md/md_main.cpp
    md/md_api.cpp
    md/md_api_config.cpp
    md/md_api_subscribe.cpp
    md/md_api_ctp.cpp
    md/md_api_scheduled.cpp
    md/md_api_pure.cpp
    md/md_spi.cpp
    md/md_state.cpp
    common/trading_calendar.cpp
)
target_include_directories(dzmd_ctp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dzmd_ctp PRIVATE dzlog dzshm dzcore dzplatform_headers dzmd_ctp_api)
if(WIN32)
    target_compile_definitions(dzmd_ctp PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()

set_target_properties(dzmd_ctp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
# Linux: rpath $ORIGIN，运行时加载同目录的 .so
if(UNIX)
    set_target_properties(dzmd_ctp PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()
install(TARGETS dzmd_ctp RUNTIME DESTINATION ${DZ_INSTALL_DIR})

# 构建后拷贝 CTP DLL/SO 到输出目录（运行时依赖）
if(WIN32)
    add_custom_command(TARGET dzmd_ctp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_MD_DLL}" "${DZ_OUTPUT_DIR}"
        COMMENT "Copying CTP MD DLL to output directory")
    install(FILES ${CTP_MD_DLL} DESTINATION ${DZ_INSTALL_DIR})
else()
    add_custom_command(TARGET dzmd_ctp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_MD_SO}" "${DZ_OUTPUT_DIR}"
        COMMENT "Copying CTP MD SO to output directory")
    install(FILES ${CTP_MD_SO} DESTINATION ${DZ_INSTALL_DIR})
endif()

# ============================================================
# dztd_ctp: 交易网关可执行文件 (占位, 后续 plan 实现)
# ============================================================
add_executable(dztd_ctp
    td/td_main.cpp
    td/td_state.cpp
    td/td_config.cpp
    td/td_api.cpp
    td/td_api_config.cpp
    td/td_api_scheduled.cpp
    td/td_api_pure.cpp
    td/td_persist_writer.cpp
    td/td_schema.cpp
    td/td_offset_converter.cpp
    td/td_ctp_mapping.cpp
    td/td_risk_gate.cpp
    td/td_account_session_pure.cpp
    td/td_account_session.cpp
    td/td_spi.cpp
    common/trading_calendar.cpp
)
target_include_directories(dztd_ctp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dztd_ctp PRIVATE dzlog dzshm dzcore dzdb dzplatform_headers dztd_ctp_api)
if(WIN32)
    target_compile_definitions(dztd_ctp PRIVATE _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()
set_target_properties(dztd_ctp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
if(UNIX)
    set_target_properties(dztd_ctp PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()
install(TARGETS dztd_ctp RUNTIME DESTINATION ${DZ_INSTALL_DIR})

# 构建后拷贝 CTP TD DLL/SO 到输出目录
if(WIN32)
    add_custom_command(TARGET dztd_ctp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_TD_DLL}" "${DZ_OUTPUT_DIR}"
        COMMENT "Copying CTP TD DLL to output directory")
    install(FILES ${CTP_TD_DLL} DESTINATION ${DZ_INSTALL_DIR})
else()
    add_custom_command(TARGET dztd_ctp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CTP_TD_SO}" "${DZ_OUTPUT_DIR}"
        COMMENT "Copying CTP TD SO to output directory")
    install(FILES ${CTP_TD_SO} DESTINATION ${DZ_INSTALL_DIR})
endif()

# ============================================================
# 单元测试: md 和 td 分离 glob, 避免编译冲突
# ============================================================
# md 测试 (dzmd_ctp_<testname>): 编译 md 源文件, 不依赖 CTP 库
file(GLOB MD_CTP_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/md_*_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/auto_sched_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_types_test.cpp"
)
foreach(testfile ${MD_CTP_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzmd_ctp_${testname}
        ${testfile}
        tests/test_main.cpp
        md/md_state.cpp
        md/md_api_pure.cpp
        common/trading_calendar.cpp
    )
    target_include_directories(dzmd_ctp_${testname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(dzmd_ctp_${testname} PRIVATE
        dzcore dzplatform_headers GTest::gtest
        spdlog::spdlog magic_enum::magic_enum nlohmann_json::nlohmann_json
    )
    # 测试用指定初始化器只填部分字段，豁免 -Wmissing-field-initializers
    # 该警告选项仅 GCC/Clang 支持，MSVC 不识别需跳过
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dzmd_ctp_${testname} PRIVATE -Wno-missing-field-initializers)
    endif()
    if(WIN32)
        target_compile_definitions(dzmd_ctp_${testname} PRIVATE
            _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
    endif()
    set_target_properties(dzmd_ctp_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dzmd_ctp_${testname})
    install(TARGETS dzmd_ctp_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()

# td 测试 (dztd_ctp_<testname>): 编译 td 源文件, 不依赖 CTP 库
# td_state/td_config/td_api_pure 是纯逻辑, 无 CTP 依赖
file(GLOB TD_CTP_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/td_*_test.cpp"
)
foreach(testfile ${TD_CTP_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dztd_ctp_${testname}
        ${testfile}
        tests/test_main.cpp
        td/td_state.cpp
        td/td_config.cpp
        td/td_api_pure.cpp
        common/trading_calendar.cpp
    )
    target_include_directories(dztd_ctp_${testname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(dztd_ctp_${testname} PRIVATE
        dzcore GTest::gtest
        spdlog::spdlog magic_enum::magic_enum nlohmann_json::nlohmann_json
        # td_config.h include <dztrader/platform/ctp_md_config.h> (复用 BrokerEntry 等类型)
        dzplatform_headers
    )
    # td_events_test / td_struct_test / td_ctp_mapping_test 需要 CTP 头 (ThostFtdcUserApiStruct.h)
    if(testname STREQUAL "td_events_test" OR testname STREQUAL "td_struct_test" OR
       testname STREQUAL "td_ctp_mapping_test")
        target_link_libraries(dztd_ctp_${testname} PRIVATE dztd_ctp_api)
    endif()
    # td_events_test 还需要 dzshm 头 (ctp_events.h include dztrader/shm/named_semaphore.h)
    if(testname STREQUAL "td_events_test")
        target_link_libraries(dztd_ctp_${testname} PRIVATE dzshm)
    endif()
    # 测试用指定初始化器只填部分字段，豁免 -Wmissing-field-initializers
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dztd_ctp_${testname} PRIVATE -Wno-missing-field-initializers)
    endif()
    if(WIN32)
        target_compile_definitions(dztd_ctp_${testname} PRIVATE
            _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
    endif()
    set_target_properties(dztd_ctp_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    # td_schema_test / td_persist_writer_test 需要 dzdb (SQLite) + td_schema.cpp
    if(testname STREQUAL "td_schema_test" OR testname STREQUAL "td_persist_writer_test")
        target_sources(dztd_ctp_${testname} PRIVATE td/td_schema.cpp)
        target_link_libraries(dztd_ctp_${testname} PRIVATE dzdb)
    endif()
    # td_persist_writer_test 还需要 td_persist_writer.cpp
    if(testname STREQUAL "td_persist_writer_test")
        target_sources(dztd_ctp_${testname} PRIVATE td/td_persist_writer.cpp)
    endif()
    # td_offset_converter_test 需要 td_offset_converter.cpp (纯逻辑, 不需要 dzdb)
    if(testname STREQUAL "td_offset_converter_test")
        target_sources(dztd_ctp_${testname} PRIVATE td/td_offset_converter.cpp)
    endif()
    # td_ctp_mapping_test 需要 td_ctp_mapping.cpp + td_schema.cpp (OrderRecord 组合 DzOrderReport)
    # 仅依赖 CTP 头 struct 定义, 不链接 .lib; 但 td_schema.h 引用 dztrader/db/migration.h 需链 dzdb
    if(testname STREQUAL "td_ctp_mapping_test")
        target_sources(dztd_ctp_${testname} PRIVATE td/td_ctp_mapping.cpp td/td_schema.cpp)
        target_link_libraries(dztd_ctp_${testname} PRIVATE dzdb)
    endif()
    # td_risk_gate_test 需要 td_risk_gate.cpp (纯逻辑, 不依赖 CTP)
    if(testname STREQUAL "td_risk_gate_test")
        target_sources(dztd_ctp_${testname} PRIVATE td/td_risk_gate.cpp)
    endif()
    # td_account_session_pure_test 需要 td_account_session_pure.cpp (OrderRefMap + sync_order_ref 纯逻辑)
    if(testname STREQUAL "td_account_session_pure_test")
        target_sources(dztd_ctp_${testname} PRIVATE td/td_account_session_pure.cpp)
    endif()
    gtest_discover_tests(dztd_ctp_${testname})
    install(TARGETS dztd_ctp_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
