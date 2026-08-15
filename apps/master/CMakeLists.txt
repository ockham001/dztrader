# ============================================================
# dztraderd_obj: 主进程 OBJECT 库
# 进程管理 + 配置 + 共享内存管理
# OBJECT 库：编译一次，无 .lib 产物，可执行与测试共享对象文件
# ============================================================
add_library(dztraderd_obj OBJECT
    config.cpp
    process_registry.cpp
    child_process.cpp
    orphan_guard.cpp
    shm_manager.cpp
    shm_manager_subscribers.cpp
    shm_manager_maintenance.cpp
    shm_manager_frame.cpp
    process_supervisor.cpp
    process_config.cpp
)
target_include_directories(dztraderd_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dztraderd_obj
    PUBLIC dzcore dzlog dzshm dzprocess
    PUBLIC dzplatform_headers
    PUBLIC spdlog::spdlog magic_enum::magic_enum Boost::boost SQLiteCpp
)
if(WIN32)
    target_compile_definitions(dztraderd_obj PUBLIC
        _WIN32_WINNT=0x0602 _CRT_SECURE_NO_WARNINGS
    )
endif()

# ============================================================
# test_worker: 测试辅助进程
# 被 child_process_test 等调用，与测试二进制同目录（tests/）
# child_process_test 通过 this_process::exe_dir() / "test_worker" 查找
# ============================================================
add_executable(test_worker tests/worker.cpp)
set_target_properties(test_worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
install(TARGETS test_worker RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)

# ============================================================
# dztraderd: 主进程可执行文件
# 输出到构建目录根（DZ_OUTPUT_DIR），输出名 dztraderd
# ============================================================
add_executable(dztraderd main.cpp)
set_target_properties(dztraderd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
target_link_libraries(dztraderd PRIVATE dztraderd_obj)
install(TARGETS dztraderd RUNTIME DESTINATION ${DZ_INSTALL_DIR})

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译
# 输出到 tests/，test_worker 也在同目录
# 测试需要 DZTRADER_HOME 的，在 SetUp 里创建临时目录（不再用 .config）
# ============================================================
file(GLOB MAIN_ENGINE_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${MAIN_ENGINE_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dztraderd_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dztraderd_${testname} PRIVATE
        dztraderd_obj GTest::gtest
    )
    set_target_properties(dztraderd_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dztraderd_${testname})
    install(TARGETS dztraderd_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()

# ============================================================
# dz_smoke_test: 集成冒烟测试 (master + dzmd_ctp 真实进程组合验证)
# 独立于上方 glob (源码在 tests/smoke/ 子目录), RUN_SERIAL:
# spawn 真实 dztraderd/dzmd_ctp + 机器全局 NamedSemaphore, 不与并行测试混跑
# ============================================================
add_executable(dz_smoke_test tests/smoke/smoke_test.cpp tests/test_main.cpp)
target_link_libraries(dz_smoke_test PRIVATE dztraderd_obj GTest::gtest)
set_target_properties(dz_smoke_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
gtest_discover_tests(dz_smoke_test PROPERTIES RUN_SERIAL TRUE)
install(TARGETS dz_smoke_test RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
