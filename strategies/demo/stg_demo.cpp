/**
 * @file stg_demo.cpp
 * @brief stg_demo — 命令行示例策略
 *
 * 目的: 以最直接的方式演示策略 SDK 的核心闭环, 也是人工观察
 * 下单/撤单/行情订阅/回报延迟的调试工具。
 *
 * 子命令:
 *   stg_demo info
 *       打印策略身份与 SDK 版本, 观察策略进程生命周期。
 *   stg_demo md <合约...>
 *       使用 DZTRADER_MD_SOURCE 绑定的行情源订阅合约 → 观察窗口内打印 tick。
 *   stg_demo order <账户> <合约> <价格> <手数> [long|short]
 *       下限价开仓单并等待回报; 账户未连接时 td 网关回 REJECTED (空账户拒单闭环)。
 *   stg_demo cancel <账户> <订单ID>
 *       发撤单请求并等待回报 (账户未连接时 td 静默丢弃撤单, 观察超时路径)。
 *
 * 退出码 (供脚本/冒烟测试机器断言):
 *   0 = 成功 (info 完成 / md 观察窗口结束 / order、cancel 收到回报)
 *   2 = 用法或初始化错误
 *   3 = order 收到 REJECTED 回报 (空账户拒单闭环)
 *   4 = order/cancel 在等待窗口内未收到回报
 *
 * 运行前提: dztraderd 已启动且 DZTRADER_HOME 一致。master 负责把本策略注册为
 * 事件通道订阅者 (stg.<exe名>), 网关回报帧经事件通道广播送达。
 * strategy_id 由平台取可执行文件名 (stg_demo), 与 master 配置 strategy[].name 一致。
 *
 * 句柄式用法: DzContext* ctx = dz_init(); 所有会话函数首参传 ctx,
 * dz_release(ctx) 释放; 重复 dz_init 返回 NULL (查 dz_errcode)。
 */
#include <dztrader/api.h>
#include <dztrader/struct.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitRejected = 3;
constexpr int kExitNoReport = 4;

constexpr uint32_t kPollStepMs = 500;  // 每次等待/轮询步长
constexpr auto kOrderWait = 5s;        // 下单/撤单等待回报窗口
constexpr auto kMdWindow = 10s;        // 行情观察窗口

void print_usage() {
    std::puts(
        "stg_demo — dztrader 命令行示例策略\n"
        "\n"
        "用法:\n"
        "  stg_demo info\n"
        "  stg_demo md <合约...>\n"
        "  stg_demo order <账户> <合约> <价格> <手数> [long|short]\n"
        "  stg_demo cancel <账户> <订单ID>\n"
        "\n"
        "退出码: 0 成功 | 2 用法/初始化错误 | 3 下单被拒 (空账户拒单闭环) | 4 等待回报超时");
}

void print_identity(DzContext* ctx) {
    std::puts(std::format("策略ID: {} | 行情源: {} | SDK: {} | 目录: {}", dz_strategy_id(ctx),
                          dz_md_source_name(ctx), dz_version_string(), dz_strategy_home(ctx))
                  .c_str());
}

const char* order_status_name(DzOrderStatus status) {
    switch (status) {
        case DZ_ORDER_SUBMITTING:
            return "提交中";
        case DZ_ORDER_NOT_TRADED:
            return "未成交";
        case DZ_ORDER_PART_TRADED:
            return "部分成交";
        case DZ_ORDER_ALL_TRADED:
            return "全部成交";
        case DZ_ORDER_CANCELLED:
            return "已撤销";
        case DZ_ORDER_REJECTED:
            return "拒单";
        default:
            return "未知";
    }
}

const char* direction_name(DzDirection direction) {
    switch (direction) {
        case DZ_DIRECTION_LONG:
            return "买";
        case DZ_DIRECTION_SHORT:
            return "卖";
        default:
            return "净";
    }
}

// basic 帧 payload 紧跟 DzFrameHeader; 非目标帧类型返回 nullptr
const DzOrderReport* as_order_report(const void* frame) {
    const auto* hdr = static_cast<const DzFrameHeader*>(frame);
    if (hdr->frame_type != DZ_FRAME_TD_ORDER_RPT) {
        return nullptr;
    }
    return reinterpret_cast<const DzOrderReport*>(reinterpret_cast<const std::byte*>(hdr) +
                                                  sizeof(DzFrameHeader));
}

const DzTick* as_md_tick(const void* frame) {
    const auto* hdr = static_cast<const DzFrameHeader*>(frame);
    if (hdr->frame_type != DZ_FRAME_RTN_MD_TICK) {
        return nullptr;
    }
    return reinterpret_cast<const DzTick*>(reinterpret_cast<const std::byte*>(hdr) +
                                           sizeof(DzFrameHeader));
}

void print_report(const DzOrderReport& rpt) {
    std::puts(
        std::format("回报: order_id={} strategy={} account={} instrument={} direction={} "
                    "price_type={} price={:.3f} volume={} traded={} status={}({}) remark=\"{}\"",
                    rpt.order_id, rpt.strategy_id, rpt.account_id, rpt.instrument_id,
                    direction_name(rpt.direction), static_cast<int>(rpt.price_type), rpt.price,
                    rpt.volume, rpt.volume_traded, static_cast<int>(rpt.status),
                    order_status_name(rpt.status), rpt.remark)
            .c_str());
}

void print_tick(const DzTick& tick) {
    const int32_t t = tick.time;
    std::puts(std::format("tick: {} {:02d}:{:02d}:{:02d}.{:06d} last={:.3f} vol={} "
                          "bid1={:.3f}/{} ask1={:.3f}/{} oi={}",
                          tick.instrument_id, t / 3600, t % 3600 / 60, t % 60, tick.subseconds,
                          tick.last_price, tick.volume, tick.bid_price[0], tick.bid_volume[0],
                          tick.ask_price[0], tick.ask_volume[0], tick.open_interest)
                  .c_str());
}

int cmd_info(DzContext* ctx) {
    print_identity(ctx);
    return kExitOk;
}

int cmd_md(DzContext* ctx, int argc, char** argv) {
    // 用法: md <合约...>
    if (argc < 3) {
        print_usage();
        return kExitUsage;
    }
    print_identity(ctx);

    std::vector<const char*> instruments;
    for (int i = 2; i < argc; ++i) {
        instruments.push_back(argv[i]);
    }
    if (!dz_subscribe(ctx, instruments.data(), static_cast<uint32_t>(instruments.size()), true)) {
        std::puts(std::format("订阅失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        return kExitUsage;
    }
    std::puts(std::format("已订阅 {} 个合约, 观察 {}s (无行情连接时 0 tick 为预期)",
                          instruments.size(), kMdWindow.count())
                  .c_str());

    uint64_t tick_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + kMdWindow;
    // 观察窗口内的轮询步长: 周期定时器唤醒 dz_wait (演示 dz_schedule_every)
    const DzTimerId poll_timer = dz_schedule_every(ctx, static_cast<int32_t>(kPollStepMs));
    if (poll_timer == DZ_TIMER_INVALID) {
        std::puts(std::format("定时器创建失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        return kExitUsage;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        // 无论是否被唤醒都排空一次: 兜底订阅者快照过期导致的漏唤醒
        dz_wait(ctx);
        // 排干事件通道: 定时器帧等在此丢弃 (本命令只观察行情)
        while (dz_next_event(ctx) != nullptr) {
        }
        const void* frame = nullptr;
        while ((frame = dz_next_md(ctx)) != nullptr) {
            const DzTick* tick = as_md_tick(frame);
            if (tick == nullptr) {
                continue;
            }
            ++tick_count;
            print_tick(*tick);
        }
    }
    (void)dz_schedule_cancel(ctx, poll_timer);
    std::puts(std::format("观察窗口结束: 共收到 {} 个 tick", tick_count).c_str());
    return kExitOk;
}

int cmd_order(DzContext* ctx, int argc, char** argv) {
    // 用法: order <账户> <合约> <价格> <手数> [long|short]
    if (argc < 6 || argc > 7) {
        print_usage();
        return kExitUsage;
    }
    print_identity(ctx);
    const char* account = argv[2];
    const char* instrument = argv[3];
    double price = 0.0;
    DzVolume volume = 0;
    try {
        price = std::stod(argv[4]);
        volume = static_cast<DzVolume>(std::stoll(argv[5]));
    } catch (const std::exception&) {
        std::puts("价格/手数解析失败 (应为数字)");
        return kExitUsage;
    }
    if (volume <= 0) {
        std::puts("手数必须 > 0");
        return kExitUsage;
    }
    DzDirection direction = DZ_DIRECTION_LONG;
    if (argc == 7) {
        const std::string_view d = argv[6];
        if (d == "short") {
            direction = DZ_DIRECTION_SHORT;
        } else if (d != "long") {
            std::puts("方向只能是 long 或 short");
            return kExitUsage;
        }
    }

    // 固定限价 + 开仓 (演示工具; 市价/平仓可后续加参数)
    const DzOrderId order_id = dz_place_order(ctx, account, instrument, direction, DZ_PRICE_LIMIT,
                                              price, volume, DZ_POSITION_EFFECT_OPEN);
    if (order_id < 0) {
        std::puts(std::format("下单失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        return kExitUsage;
    }
    std::puts(std::format("订单已发送: order_id={} account={} instrument={} direction={} "
                          "price_type=限价 price={:.3f} volume={} 开平=开仓",
                          order_id, account, instrument, direction_name(direction), price, volume)
                  .c_str());

    // 等待本订单回报 (td 广播 TD_ORDER_RPT, 按 order_id 过滤)
    const auto deadline = std::chrono::steady_clock::now() + kOrderWait;
    const DzTimerId poll_timer = dz_schedule_every(ctx, static_cast<int32_t>(kPollStepMs));
    if (poll_timer == DZ_TIMER_INVALID) {
        std::puts(std::format("定时器创建失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        return kExitUsage;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        dz_wait(ctx);
        const void* frame = nullptr;
        while ((frame = dz_next_event(ctx)) != nullptr) {
            const DzOrderReport* rpt = as_order_report(frame);
            if (rpt == nullptr || rpt->order_id != order_id) {
                continue;
            }
            print_report(*rpt);
            (void)dz_schedule_cancel(ctx, poll_timer);
            return (rpt->status == DZ_ORDER_REJECTED) ? kExitRejected : kExitOk;
        }
    }
    (void)dz_schedule_cancel(ctx, poll_timer);
    std::puts(std::format("等待回报超时 ({}s): 未收到 order_id={} 的回报 (td 未运行?)",
                          kOrderWait.count(), order_id)
                  .c_str());
    return kExitNoReport;
}

int cmd_cancel(DzContext* ctx, int argc, char** argv) {
    // 用法: cancel <账户> <订单ID>
    if (argc != 4) {
        print_usage();
        return kExitUsage;
    }
    print_identity(ctx);
    const char* account = argv[2];
    DzOrderId order_id = 0;
    try {
        order_id = std::stoll(argv[3]);
    } catch (const std::exception&) {
        std::puts("订单ID 解析失败 (应为整数)");
        return kExitUsage;
    }
    if (!dz_cancel_order(ctx, account, order_id)) {
        std::puts(std::format("撤单请求发送失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        return kExitUsage;
    }
    std::puts(std::format("撤单请求已发送: account={} order_id={}", account, order_id).c_str());

    const auto deadline = std::chrono::steady_clock::now() + kOrderWait;
    const DzTimerId poll_timer = dz_schedule_every(ctx, static_cast<int32_t>(kPollStepMs));
    if (poll_timer == DZ_TIMER_INVALID) {
        std::puts(std::format("定时器创建失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        return kExitUsage;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        dz_wait(ctx);
        const void* frame = nullptr;
        while ((frame = dz_next_event(ctx)) != nullptr) {
            const DzOrderReport* rpt = as_order_report(frame);
            if (rpt == nullptr) {
                continue;
            }
            print_report(*rpt);
            (void)dz_schedule_cancel(ctx, poll_timer);
            return kExitOk;
        }
    }
    (void)dz_schedule_cancel(ctx, poll_timer);
    std::puts(
        std::format("等待回报超时 ({}s): 账户未连接时 td 不回应撤单 (预期)", kOrderWait.count())
            .c_str());
    return kExitNoReport;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 控制台输出 UTF-8 (与 /utf-8 编译选项一致, 否则中文输出乱码)
    (void)SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        print_usage();
        return kExitUsage;
    }
    DzContext* ctx = dz_init();
    if (ctx == nullptr) {
        std::puts(std::format("dz_init 失败: {} ({})", dz_errmsg(), dz_errcode()).c_str());
        std::puts("提示: 请确认 dztraderd 已启动且 DZTRADER_HOME 指向同一运行目录");
        return kExitUsage;
    }
    const std::string_view cmd = argv[1];
    int rc = kExitUsage;
    if (cmd == "info") {
        rc = cmd_info(ctx);
    } else if (cmd == "md") {
        rc = cmd_md(ctx, argc, argv);
    } else if (cmd == "order") {
        rc = cmd_order(ctx, argc, argv);
    } else if (cmd == "cancel") {
        rc = cmd_cancel(ctx, argc, argv);
    } else {
        print_usage();
    }
    dz_release(ctx);
    return rc;
}