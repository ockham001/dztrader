#ifndef DZTRADER_SYSMON_PROBE_SCHEDULE_H_
#define DZTRADER_SYSMON_PROBE_SCHEDULE_H_

/**
 * @file probe_schedule.h
 * @brief 探测定时排期：把 `clock.probe_times` 的「每天 HH:MM 列表」折算成到下一个本地
 *        墙钟时刻的单调时钟延时，并用 `steady_timer` 等待（契约 sysmon §2.7、§3.1）。
 *
 * 语义（全部来自契约 §2.7「探测调度」与 §3.1.1 第 2 步）：
 * - 排期基准是**进程本地时区**的墙钟时刻（`mktime` 按本地时区折算，`tm_isdst = -1` 由
 *   运行时判定夏令时），换算成延时后交给 `steady_timer`——等待本身用单调时钟，
 *   免疫 NTP 步进/回拨；
 * - 每次完成后**重算**下一个时刻；进程停顿期间错过的时刻被直接跳过、**不补采**
 *   （只排「严格晚于当前时刻」的最近一个，错过的自然不会产生多次探测）。若停顿恰好
 *   跨过一个目标时刻，恢复后**只会迟到触发一次**（已到期的等待先回调），随后立即按
 *   当前时刻重算——不会连续补采多个错过的时刻；
 * - 不区分周末与节假日（契约明文）；
 * - 空列表 / 全部非法元素 = 不排期（关闭定时探测，只保留启动首探与手动查询）。
 *
 * 本模块是纯函数 + 一层薄定时器包装：时刻折算（`next_probe_delay`）不读时钟、不碰 I/O，
 * 测试可用构造出来的墙钟时刻直接断言；`ProbeSchedule` 只负责把延时交给 asio 定时器，
 * 不读配置、不发探测——调用方（`SysmonApp`）决定到点后做什么。
 */

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::sysmon {

/**
 * @brief 从 @p now_wall 起下一个（**严格晚于**）本地 HH:MM 时刻。
 *
 * 多个时刻取最近的一个；跨午夜与夏令时由 `mktime` 归一化处理。进程恰好在某个 HH:MM:SS.0
 * 之后排期时，该时刻已算「过去」——下一次出现是次日（启动首探覆盖的正是这个当下时刻，
 * 定时探测不得在同一分钟内再补一次）。
 *
 * @param now_wall 当前墙钟时刻（`system_clock`）
 * @param probe_times 每天探测时刻列表（`clock.probe_times`）
 * @return 下一个墙钟时刻；列表为空或全部非法时 `std::nullopt`
 */
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> next_probe_wall_time(
    std::chrono::system_clock::time_point now_wall,
    const std::vector<std::string>& probe_times) noexcept;

/**
 * @brief 到下一个探测时刻的延时（供 `steady_timer::expires_after` 使用）。
 *
 * 纯函数：只做「墙钟时刻差 → 单调时钟时长」的换算，不读时钟。墙钟差为负（调用方传入的
 * 时刻异常）时钳制为 0——立即到点，不倒退成「永远等不到」。
 *
 * @return 延时；列表为空或全部非法时 `std::nullopt`
 */
[[nodiscard]] std::optional<std::chrono::steady_clock::duration> next_probe_delay(
    std::chrono::system_clock::time_point now_wall,
    const std::vector<std::string>& probe_times) noexcept;

/**
 * @brief 定时探测的等待器：持有唯一一个 `steady_timer`，负责「排到下一个时刻 / 取消」。
 *
 * 生命周期与线程模型：与 dzsysmon 的 io_context 单线程一致，只在那一线程上调用；
 * 非线程安全。析构时若仍有 pending 等待，调用方应已停止 io_context（进程退出路径
 * `ioc_.stop()` 之后才析构）。
 *
 * 到点后**只回调一次**：回调里发起探测，探测完成后由调用方再次 `reschedule()` 重算下一个
 * 时刻（「每次重算」是契约语义的一部分，错过的时刻因此不会被补采）。
 */
class ProbeSchedule {
public:
    /// 到点回调（由调用方在回调里发起一轮探测，并在完成后再 `reschedule()`）。
    using Handler = std::function<void()>;

    explicit ProbeSchedule(boost::asio::io_context& io);

    ProbeSchedule(const ProbeSchedule&) = delete;
    ProbeSchedule& operator=(const ProbeSchedule&) = delete;
    ProbeSchedule(ProbeSchedule&&) = delete;
    ProbeSchedule& operator=(ProbeSchedule&&) = delete;

    ~ProbeSchedule() = default;

    /**
     * @brief 从 @p now_wall 重排到下一个探测时刻（先取消当前 pending 等待）。
     *
     * 列表为空或全部非法时退化为「停止」：不排期、`armed()` 为 false。
     *
     * @param now_wall    当前墙钟时刻（调用方读 `system_clock`；测试注入构造时刻）
     * @param probe_times 每天探测时刻列表（`clock.probe_times`）
     * @param on_due      到点回调
     */
    void reschedule(std::chrono::system_clock::time_point now_wall,
                    const std::vector<std::string>& probe_times,
                    Handler on_due);

    /// 取消 pending 等待（配置关闭 / 进程退出路径）。幂等。
    void stop();

    /// @return 是否有 pending 的到点等待。
    [[nodiscard]] bool armed() const noexcept { return armed_; }

private:
    /// 把延时交给定时器并登记到点回调。
    void arm(std::chrono::steady_clock::duration delay);

    boost::asio::steady_timer timer_;
    Handler on_due_;
    /// 等待代次：`stop()` / 重排都会自增。asio 的定时器取消后旧 handler 仍可能以
    /// `operation_aborted` 或「已到期但尚未执行」的形态被投递——代次不符即丢弃，
    /// 避免旧等待的 handler 误触发新排期的回调。
    uint64_t generation_ = 0;
    bool armed_ = false;
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_PROBE_SCHEDULE_H_
