# ============================================================
# dztrading: 交易领域共享逻辑（STATIC 库）
# 当前承载 TD 数据消费端 ingest 门（SDK 与 dzweb 共用）。
# 纯逻辑，无 core/shm/db 依赖；平台进程与策略 SDK 均可按需 PRIVATE 链接。
# ============================================================
add_library(dztrading STATIC
    src/td_ingest.cpp
)
target_include_directories(dztrading PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# 可被链入 dzstrategy_sdk 动态库，静态库对象需 PIC。
set_target_properties(dztrading PROPERTIES POSITION_INDEPENDENT_CODE ON)
