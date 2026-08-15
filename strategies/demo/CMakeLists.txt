# ============================================================
# stg_demo: 命令行示例策略 (info/md/order/cancel, 见 stg_demo.cpp 头注释)
#
# 关键约束: exe 名即策略ID (strategy_id = exe_stem), master 配置中
# strategy[].name 必须为 "stg_demo", 事件通道订阅者身份为 stg.stg_demo。
# 仅链接策略 SDK (dzstrategy_sdk_static), 不依赖任何网关/web 内部库。
# ============================================================
add_executable(stg_demo
    stg_demo.cpp
)
target_link_libraries(stg_demo PRIVATE dzstrategy_sdk_static)
set_target_properties(stg_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
install(TARGETS stg_demo RUNTIME DESTINATION ${DZ_INSTALL_DIR})
