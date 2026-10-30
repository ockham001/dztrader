# ============================================================
# dzweb: webui 后端可执行文件（drogon + msgpack + shm）
# 输出到构建目录根（DZ_OUTPUT_DIR），输出名 dzweb
# ============================================================
find_package(Drogon CONFIG REQUIRED)
find_package(msgpack-cxx CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
# Conan's sqlite3 recipe doesn't propagate include dirs on the imported target
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)

# ============================================================
# dzweb_obj: OBJECT 库（编码器等可复用源文件）
# 测试可链接此 OBJECT 库，无需拉入 main.cpp
# ============================================================
add_library(dzweb_obj OBJECT
    msgpack_encoder.cpp
    config.cpp
    system_info.cpp
    jwt.cpp
    event_monitor.cpp
    event_preloader.cpp
    auth_controller.cpp
    ws_controller.cpp
    ws_log_tail.cpp
    db_init.cpp
    repository.cpp
    shm_writer.cpp
    user_controller.cpp
    security_controller.cpp
    market_source_controller.cpp
    market_source_query.cpp
    market_source_ops.cpp
    log_service.cpp
    log_controller.cpp
    process_controller.cpp
    process_mirror.cpp
    notify_controller.cpp
    settings_controller.cpp
    td_data_service.cpp
    sysmon_controller.cpp
    sysmon_history_service.cpp
)
target_link_libraries(dzweb_obj PUBLIC
    Drogon::Drogon
    msgpack-cxx
    dzshm
    dzprocess
    dzsysmonstore
    dztrading
    strategy_api_headers
    dzcore
    dzlog
    dzplatform_headers
    toml11::toml11
    OpenSSL::Crypto
    SQLite::SQLite3
    spdlog::spdlog
)
target_include_directories(dzweb_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SQLITE3_INCLUDE_DIR}
)

# ============================================================
# dzweb: 主可执行文件
# ============================================================
add_executable(dzweb main.cpp)
target_link_libraries(dzweb PRIVATE dzweb_obj)
set_target_properties(dzweb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
install(TARGETS dzweb RUNTIME DESTINATION ${DZ_INSTALL_DIR})

# ============================================================
# 单元测试
# ============================================================
# 共用 test-base 注入的 DZTRADER_HOME/shm 中**固定名** dzevent 通道的用例。
# paths::shm() 对所有用例是同一目录，dzevent 是真实通道名（不是每用例唯一），
# 因此两个测试二进制并发时会写同一份 meta/页文件，并各自断言「下一次读到的
# 就是自己刚写的那一帧」——必须串行（#136，AC5）。
# 判据：读的是自己的帧且用共享通道名；用唯一 channel 名的测试不在此列。
# ============================================================
set(WEBUI_SHARED_CHANNEL_TESTS
    frame_router_test
    ws_log_report_test
)
file(GLOB WEBUI_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${WEBUI_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzweb_${testname} ${testfile} ../master/tests/test_main.cpp)
    target_link_libraries(dzweb_${testname} PRIVATE
        dzweb_obj
        dzshm_test_support
        GTest::gtest
    )
    target_include_directories(dzweb_${testname} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    set_target_properties(dzweb_${testname} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})
    if(MSVC AND testname MATCHES "^sysmon_")
        # gtest 1.17 gtest-printers.h 在 MSVC /O2 下对 nlohmann::json 相关模板实例化
        # 偶发 C4702（unreachable code），Release /WX 会升级为错误；仅豁免测试目标。
        target_compile_options(dzweb_${testname} PRIVATE /wd4702)
    endif()
    if(testname IN_LIST WEBUI_SHARED_CHANNEL_TESTS)
        # 见上方 WEBUI_SHARED_CHANNEL_TESTS 注释：共享 dzevent 通道且断言自己刚写的帧
        gtest_discover_tests(dzweb_${testname}
            PROPERTIES LABELS fast RUN_SERIAL TRUE TIMEOUT 60)
    else()
        gtest_discover_tests(dzweb_${testname} PROPERTIES LABELS fast)
    endif()
endforeach()

# ============================================================
# 前端构建集成：npm + Vite -> frontend/dist/
# dzweb 可执行文件依赖前端构建产物
# ============================================================
find_program(NPM_EXECUTABLE NAMES npm.cmd npm)
if(NPM_EXECUTABLE)
    set(FRONTEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
    set(FRONTEND_DIST ${FRONTEND_DIR}/dist)

    # 前端源文件（CONFIGURE_DEPENDS: 新增/删除文件时自动重生成构建系统）
    # 必须包含在 DEPENDS 中，否则只改 .vue/.ts/.css 源码时 CMake 会认为 dist 已是最新而跳过构建
    # 注意: 不能用 GLOB_RECURSE 匹配 ${FRONTEND_DIR}/index.html，否则会递归匹配到 dist/index.html (OUTPUT 自身) 形成依赖循环
    file(GLOB_RECURSE FRONTEND_SRC_SOURCES CONFIGURE_DEPENDS
        "${FRONTEND_DIR}/src/*.vue"
        "${FRONTEND_DIR}/src/*.ts"
        "${FRONTEND_DIR}/src/*.js"
        "${FRONTEND_DIR}/src/*.css"
    )
    file(GLOB FRONTEND_TOP_SOURCES CONFIGURE_DEPENDS
        "${FRONTEND_DIR}/index.html"
        "${FRONTEND_DIR}/vite.config.*"
        "${FRONTEND_DIR}/tsconfig*.json"
    )
    set(FRONTEND_SOURCES ${FRONTEND_SRC_SOURCES} ${FRONTEND_TOP_SOURCES})

    add_custom_command(
        OUTPUT ${FRONTEND_DIST}/index.html
        # 依赖安装门控：lockfile hash + node_modules 存在时跳过 npm ci（#134，脚本跨平台）
        COMMAND ${CMAKE_COMMAND}
            "-DNPM_EXECUTABLE=${NPM_EXECUTABLE}"
            "-DFRONTEND_DIR=${FRONTEND_DIR}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/scripts/npm-ci-if-needed.cmake"
        COMMAND ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${FRONTEND_DIR}
        DEPENDS ${FRONTEND_DIR}/package.json ${FRONTEND_DIR}/package-lock.json ${FRONTEND_SOURCES}
        COMMENT "Building Vue frontend (npm ci if needed + vite build)"
        VERBATIM
    )
    add_custom_target(dzweb_frontend ALL DEPENDS ${FRONTEND_DIST}/index.html)
    add_dependencies(dzweb dzweb_frontend)

    # 构建后把前端 dist 复制到 build 根 web/（与 dzweb.exe 同级）
    add_custom_command(TARGET dzweb_frontend POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FRONTEND_DIST}" "${DZ_OUTPUT_DIR}/web"
        COMMENT "Copying frontend dist to output directory")
    # install 规则：前端 dist 安装到 install 根 web/
    install(DIRECTORY "${FRONTEND_DIST}/"
        DESTINATION ${DZ_INSTALL_DIR}/web)
else()
    message(WARNING "npm not found - frontend will not be built. Install Node.js to enable frontend build.")
endif()
