/**
 * @file scaling.h
 * @brief 挂靠账户缩放：账户表类型 + `L_i = sign(P) × f(scale_i × |P|)` 的纯函数
 *        （ADR 0028/0030，组件文档 §3/§6.2）
 *
 * 一个策略进程只有一个逻辑账户（虚拟台账，B 可见的唯一持仓口径）；实体账户 0..N 个按
 * 挂靠比率 `scale` 镜像逻辑持仓。账户 `i` 在某品种上的**逻辑持仓**：
 *
 *     L_i = sign(P) × f(scale_i × |P|)
 *     f = nearest（默认四舍五入）| floor（向少）| ceil（向多），可选「目标非零时至少 1 手」
 *
 * 手数换算只在这里实现一次：正常单的每账户手数（`qty_i = L_i(P_new) − L_i(P_old)`）、
 * 生命周期命令的同步单差额、删除清零后的目标值、回放结束的批量上报，全部由同一个
 * `logical_position()` 得出——两条路径各写一份换算就一定会有一处口径漂移。
 *
 * **取整误差不随逐单累积**（ADR 0030 决策 8 的关键性质）：取整发生在**目标持仓**上而不是
 * 每笔单量上，且 `L_i` 是无状态纯函数（P 与 scale 决定一切），因此第 n 笔单之后账户逻辑
 * 持仓恒等于 `f(scale × |P_n|)`，与历史路径无关。逐单独立取整（每笔手数 = `f(scale × 本单量)`）
 * 会让账户持仓长期漂移，ADR 0030 已否决。
 *
 * 本头是 SDK 公开头依赖链的一部分，但**不依赖配置层的 JSON**（`config.h` 反过来包含本头）：
 * `InstrumentContext` 只需要这几个值类型与纯函数，不应该为它们把内置 nlohmann 单头拉进
 * 每个策略 TU（ADR 0026 的隔离口径）。
 */
#ifndef DZTRADER_BAR_STRATEGY_SCALING_H_
#define DZTRADER_BAR_STRATEGY_SCALING_H_

#include <dztrader/data_type.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace dztrader {

/**
 * @brief 挂靠账户（`accounts` 数组元素）：平台账户标识 + 挂靠比率（ADR 0028/0030）
 *
 * 挂靠比率 `scale` 是「逻辑持仓 → 该账户目标持仓」的比例系数：`L_i = f(scale × |P|)`。
 * 账户的登录/凭据/交易接口归属属于平台 td 配置，框架只持有标识与比率。
 *
 * 配置层（`config.h`）按 JSON 解析出本类型；该层负责 `scale ∈ (0, max_scale]` 与 id 唯一的
 * 语义校验（组件文档 §7），本头不做校验——到了执行路径，值已经过校验。
 */
struct AttachedAccount {
    std::string account_id;  ///< 平台账户标识（非空、配置内唯一）
    double scale = 1.0;      ///< 挂靠比率（`(0, max_scale]`）
};

/**
 * @brief 挂靠比率的取整方向（ADR 0030 决策 2）：`L_i = f(scale_i × |P|)`
 *
 * 进程级取整策略，配置里写作字符串 `"nearest"` / `"floor"` / `"ceil"`：
 *   - `Nearest`：四舍五入（默认）
 *   - `Floor`：向少取整
 *   - `Ceil`：向多取整
 *
 * 枚举的 wire 名映射表在配置层（`config_internal::SCALE_ROUNDING_NAMES`），不放在这里：
 * 本头被 `InstrumentContext` 依赖，而 ctx 不读也不写配置文件。
 */
enum class ScaleRounding {
    Nearest,  ///< 四舍五入（默认）
    Floor,    ///< 向少取整
    Ceil,     ///< 向多取整
    // 新增取值时必须同步 config_internal::SCALE_ROUNDING_NAMES（唯一映射表与日志/解析共用）
};

/**
 * @brief 进程级缩放配置（生效值；组件文档 §3）
 *
 * 由配置层的三个可选字段解析/取默认后得到：`max_scale` 启动加载一次、盘中不可变
 * （运行期编辑无效，重启生效，ADR 0027 决策 11）；`rounding` / `min_one` 是挂靠
 * 手数换算策略（ADR 0030）。`ctx` 按值拷贝一份，随 `InstrumentContext` 分发给各实例。
 *
 * 落盘字段的「省略 vs 显式默认值」在 `BarStrategyConfig` 里保留（写回时原样保留省略状态），
 * 本结构只描述生效值。
 */
struct ScalingConfig {
    double max_scale = 1.0;                           ///< 挂靠比率上限（> 0）
    ScaleRounding rounding = ScaleRounding::Nearest;  ///< 取整方向
    bool min_one = false;                             ///< 目标非零时至少 1 手
};

/// @brief 挂靠账户表（A 持有；运行期命令可增删改，ctx 只持只读指针）
///
/// 用 `std::vector` 而不是 map：账户量级是个位数到几十，热路径上唯一的遍历发生在交易意图
/// 与命令冷路径（每笔意图 N 次换算，N ≤ 3 量级），而**按 id 查找**只出现在命令层。表对象
/// 地址在 manager 生命周期内稳定（元素地址随增删改变化，因此任何代码都不缓存元素指针）。
using AccountTable = std::vector<AttachedAccount>;

namespace scaling_internal {

/// @brief `LargeVolume` 能表示的最大值（double 形式；换算越界时饱和到这里而不是 UB）
inline constexpr double LARGE_VOLUME_MAX =
    static_cast<double>(std::numeric_limits<LargeVolume>::max());

/**
 * @brief 把已缩放的幅值按取整策略落成手数（唯一实现，三种策略各一处）
 *
 * 输入保证非负、有限（调用方对 `|P| × scale` 做过守卫）。越界饱和到 `LARGE_VOLUME_MAX`：
 * `static_cast<LargeVolume>(double)` 在 double 超出 int64 范围时是 UB，而 `|P|` 本身可以
 * 是 `int64`、`scale` 又由配置给出——不能假设乘积一定可表示。饱和值随后在发单出口被按
 * `DzVolume` 上限拒绝，不会变成一张错误手数的单。
 *
 * `Nearest` 用 `floor(m + 0.5)` 而不是 `std::round`：本函数只处理非负幅值，两者等价，但
 * `floor(m + 0.5)` 对 .5 的进位方向是显式的（四舍五入 = 向大），不依赖 `std::round` 的
 * 「远离零」语义——那条语义在符号被拆出去之后读起来会误导。
 *
 * @param magnitude 非负的缩放幅值（`scale × |P|`）
 * @param rounding  取整方向
 * @return 非负手数（饱和到 `LARGE_VOLUME_MAX`）
 */
[[nodiscard]] inline LargeVolume round_magnitude(double magnitude,
                                                 ScaleRounding rounding) noexcept {
    double rounded = 0;
    switch (rounding) {
        case ScaleRounding::Floor:
            rounded = std::floor(magnitude);
            break;
        case ScaleRounding::Ceil:
            rounded = std::ceil(magnitude);
            break;
        case ScaleRounding::Nearest:
            rounded = std::floor(magnitude + 0.5);
            break;
    }
    if (!(rounded < LARGE_VOLUME_MAX)) [[unlikely]] {
        return std::numeric_limits<LargeVolume>::max();  // 含 NaN（比较为假）与上溢
    }
    return static_cast<LargeVolume>(rounded);
}

/**
 * @brief 账户逻辑持仓 `L_i = sign(P) × f(scale × |P|)`（组件文档 §6.2 的唯一实现）
 *
 * 几个刻意的口径：
 *
 *   - **`P == 0` 返回 0**（而不是 `sign(0) × f(0) = 0` 顺推）：符号在 0 处无定义，显式短路
 *     让「空仓 → 账户目标恒为 0」成为读得出的规则；
 *   - **`min_one` 只抬到 1、不放宽取整方向**：目标是抑平「比率很小但策略有仓 → 账户完全
 *     不跟」的情形，而不是把 `floor` 变成 `ceil`；
 *   - **符号在取整之后才贴回去**：`f` 定义在幅值上，因此 `floor(0.5) = 0` 对多空两个方向
 *     对称（`sign` 提前参与取整会让 `floor(-0.5) = -1`，与「向少」矛盾）；
 *   - **`scale` 非法（非正/NaN）返回 0**：配置层已保证 `scale ∈ (0, max_scale]`，这里的守卫
 *     只防止执行路径上出现一个漏校验的比率就把 NaN 传进 int64 转换（UB）。返回 0 而不是
 *     抛异常：本函数在交易意图路径上、框架热路径不抛异常。
 *
 * @param position 策略逻辑持仓 P（多正空负）
 * @param scale    该账户的挂靠比率
 * @param scaling  进程级缩放配置（取整方向 + `min_one`）
 * @return 该账户的逻辑持仓（与 `position` 同号或 0）
 */
[[nodiscard]] inline LargeVolume logical_position(LargeVolume position,
                                                  double scale,
                                                  const ScalingConfig& scaling) noexcept {
    if (position == 0) {
        return 0;
    }
    if (!(scale > 0.0)) [[unlikely]] {
        return 0;  // 含 NaN：不参与镜像，避免 NaN 传给 int64 转换
    }
    // 先转 double 再取符号：`-position` 在 INT64_MIN 上是 UB，而 double 能表示它的幅值
    // （精度会丢，但那是 9.2e18 手这种不可能出现的量级，且随后被饱和/拒绝）。
    const double signed_size = static_cast<double>(position);
    const double magnitude = (signed_size < 0.0 ? -signed_size : signed_size) * scale;
    LargeVolume units = round_magnitude(magnitude, scaling.rounding);
    if (units == 0 && scaling.min_one) {
        units = 1;
    }
    return position > 0 ? units : -units;
}

}  // namespace scaling_internal

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_SCALING_H_
