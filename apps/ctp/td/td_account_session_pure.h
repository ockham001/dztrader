#ifndef DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
#define DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <dztrader/data_type.h>  // DzOrderId

namespace dztrader::ctp {

/// 撤单上下文 (C4: cancel_order 反向查找所需).
/// 主线程在 place_order 成功后 insert, on_rtn_order 收到 CTP 回报时 update front_id/session_id.
struct CancelContext {
    std::string order_ref;   ///< CTP OrderRef (12 位补零格式)
    int32_t front_id = 0;    ///< CTP FrontID (OnRtnOrder 回报填充)
    int32_t session_id = 0;  ///< CTP SessionID (OnRtnOrder 回报填充)
};

/// order_ref / OrderSysID -> DzOrderId 映射表 (设计 §9.3)
///
/// 线程模型: 仅主线程访问, 无锁
/// 生命周期: 进程生命周期内累积, 不清空 (设计 §9.4 RESTART 重传去重)
///
/// order_ref 归一化: 内部统一以 12 位补零格式存储 (与 CTP OrderRef 一致),
/// 调用方传入 "1" 或 "000000000001" 均能匹配, 避免格式不一致导致的查表失败.
class OrderRefMap {
public:
    /// 按 order_ref 查 DzOrderId, 未找到返回 nullptr
    const DzOrderId* find_by_order_ref(const std::string& order_ref) const;
    /// 按 OrderSysID 查 DzOrderId, 未找到返回 nullptr
    const DzOrderId* find_by_sys_id(const std::string& sys_id) const;
    /// 按 DzOrderId 查撤单上下文 (C4), 未找到返回 nullptr
    const CancelContext* find_cancel_context(DzOrderId order_id) const;

    void insert_by_order_ref(const std::string& order_ref, DzOrderId order_id);
    void insert_by_sys_id(const std::string& sys_id, DzOrderId order_id);
    void erase_by_order_ref(const std::string& order_ref);

    /// C4: 登记 DzOrderId -> CancelContext (place_order 成功后调用)
    void insert_cancel_context(DzOrderId order_id, const CancelContext& ctx);
    /// C4: 更新已有 CancelContext 的 front_id/session_id (on_rtn_order 收到回报时调用)
    /// 仅在 ctx 存在且新值非 0 时更新, 避免覆盖有效值.
    void update_cancel_context(DzOrderId order_id, int32_t front_id, int32_t session_id);

    /// 登记 DzOrderId -> 裸策略名 (place_order 时记录, 回报回填 strategy_id 用)
    void insert_strategy(DzOrderId order_id, const std::string& strategy_id);
    /// 按 DzOrderId 查策略名, 未找到 (外部单) 返回 nullptr
    const std::string* find_strategy(DzOrderId order_id) const;
    void erase_strategy(DzOrderId order_id);

    size_t size() const noexcept { return ref_to_id_.size(); }
    void clear() noexcept;

private:
    /// 将 order_ref 归一化为 12 位补零格式 (与 CTP TThostFtdcOrderRefType 一致).
    /// 非数字字符串原样返回 (外部订单可能传入非数字 OrderRef, 不强制归一化).
    static std::string normalize_order_ref(const std::string& order_ref);

    std::unordered_map<std::string, DzOrderId> ref_to_id_;
    std::unordered_map<std::string, DzOrderId> sys_to_id_;
    /// C4: DzOrderId -> CancelContext (撤单反向查找)
    std::unordered_map<DzOrderId, CancelContext> id_to_ctx_;
    /// DzOrderId -> 裸策略名 (回报回填 strategy_id; 外部单无条目)
    std::unordered_map<DzOrderId, std::string> id_to_strategy_;
};

/// OnRspError 路由结果 (ADR 0055 / #457): 通用错误按在途查询的 nRequestID 归位。
enum class QueryErrorTarget : uint8_t {
    None = 0,
    Instrument,
    Position,
    TradingAccount,
};

/// 按 request_id 匹配在途查询的 request id 表。生产环境 request id 从 1 递增,
/// 0 视为无效 (测试 / 未登录会话默认值), 不参与匹配; 未匹配返回 None。
/// @param runtime_account_request_id 运行期资金刷新的在途 id (与登录链共用
///        `TradingAccount` 目标, 调用方再按 id 区分登录 / 运行期)
QueryErrorTarget route_query_error(int request_id,
                                   int instrument_request_id,
                                   int position_request_id,
                                   int account_request_id,
                                   int runtime_account_request_id = 0) noexcept;

/// order_ref 同步公式 (设计 §9.4): new = max(current+1, ctp_max+1)
/// 解决重连后 CTP 端 MaxOrderRef 大于本地计数器的问题.
int64_t sync_order_ref(int64_t current_ref, int64_t ctp_max) noexcept;

/// 解析 CTP MaxOrderRef 字符串为 int64. 非数字/空返回 0.
int64_t parse_max_order_ref(const char* max_order_ref) noexcept;

/// 持续异常告警的**通知**限频判定 (未知枚举 / 外部单「只在登录重放出现」共用).
/// 这类异常可能每条回报都带: 逐条通知会刷爆前端通知环, 间隔内只计数与记日志.
/// @param now_s 当前单调时钟秒
/// @param last_report_s 上次上报时刻 (<=0 = 尚未上报过)
/// @param interval_s 最小间隔秒
/// @return true = 本次应当通知
bool alert_report_due(int64_t now_s, int64_t last_report_s, int64_t interval_s) noexcept;

/// 外部单「只在登录重放中出现」告警判据 (ADR 0049：私有流投资者级假设的运行期可观测性).
///
/// 该假设（同账号其他终端的单也实时推给本会话）无一手明文. 若柜台实际按**会话**推送,
/// 外部单只会在登录重放里出现, 持仓偏差持续到下次登录; 本判据把「假设失效」变成可观测事件.
///
/// 判据 = 记录来自登录重放, 且其柜台时间不晚于本会话最后一条**实时**回报的柜台时间:
/// 后者证明那一刻本会话已在接收实时流, 按投资者级假设该记录本应实时到达并落库
/// (落库后在重放过滤器里命中基准被吞, 走不到本判定) —— 走到这里即假设可能失效,
/// 或实时流出现缺口 (两者都值得告警).
/// 保守性 (有意): 柜台时间晚于最后实时回报的记录无法判定——它可能是断线窗口内别的终端
/// 下的单, 只能靠重放补发; 进程重启后基准为 0, 同样不告警. 因此本告警只会**漏报**, 不误报.
/// @param in_replay 该记录是否来自登录重放 (实时流记录恒 false)
/// @param record_epoch_s 记录柜台时间 (交易日 × 86400 + HH:MM:SS 秒; <=0 = 未知)
/// @param last_live_epoch_s 本会话最后一条实时回报的柜台时间 (<=0 = 尚未观测)
bool replay_external_only_alarm(bool in_replay,
                                int64_t record_epoch_s,
                                int64_t last_live_epoch_s) noexcept;

/// 登录响应的会话交易日是否可用 (纯判定便于断言).
///
/// 会话交易日是本会话全部业务数据的时间锚 (GLOSSARY「会话交易日」): 订单/成交/持仓/资金
/// 的 date、ACCOUNT_STATUS 帧的 date、回报交易日校验都取它。它缺失时若继续登录, 数据会盖上
/// 无效日期、日切感知 (status 帧 date 变化) 失真 —— 与 md 的 LoginParseError 同口径:
/// 判为登录失败, 不进 LoggedIn。
/// @param days_since_epoch OnRspTdUserLoginField::days_since_epoch (SPI 线程解析结果)
/// @return true = 有效交易日 (正数 DzDate); false = 解析失败或哨兵, 调用方应按登录失败处置。
///   注意排除的不只是 INT32_MIN 哨兵: 0 (DZ_DATE_NA, 与 "19700101" 的解析结果重合)
///   与全部负数同样判不可用, 因此实现是 `> 0` 而非仅比哨兵。
bool trading_day_usable(int32_t days_since_epoch) noexcept;

/// 登录失败通知正文 (纯构造便于断言, #407).
///
/// 为什么需要场景说明而不是直接转发状态机的通用文案: 登录失败后状态回滚到
/// Authenticated/Connected —— 两者都是**正常中间态**, 前端无法与「正在登录」区分,
/// 而本进程不会自动重发登录 (#406)。所以通知必须自己说清「发生了什么」, 只说
/// 「交易登录失败」等于让用户拿不到任何可用信息。
///
/// 正文给出「发生了什么 + 会怎样 + 下一步」: 恢复路径 = 先登出、再登录 (ADR 0051 决策 4;
/// #409 落地后 WebUI 已有该入口), 因此文案可以直接告诉用户怎么做。
/// @param reason 失败场景的中文说明 (如「交易日无效（原始值 "00000000"）」)
/// @return 形如「交易登录失败（交易日无效（原始值
/// "00000000"）），本进程不会自动重试该账户；请先登出再重新登录」
std::string login_failed_message(const std::string& reason);

// ============================================================================
// 登录链后段失败的通知正文 (结算确认 / 合约加载, #488)
// ============================================================================
// 与 login_failed_message 同一问题形态: 状态机把失败回滚到 LoggedIn (进度 6/10
// 「已登录」), 那是**正常中间态**——前端与用户无法与「登录成功、还在初始化」区分,
// 而本进程不会自动重试登录链 (#406)。因此正文同样要说清「发生了什么 + 会怎样 +
// 下一步」, 不能只转发状态机的通用文案「结算单确认失败: ...」。
//
// 两者的「会怎样」相同: 停在 6/10、不进 Ready、不接受下单; 恢复路径同样 = 先登出再登录。

/// 结算单确认的响应看门狗时长 (#488).
///
/// 柜台不回 `OnRspSettlementInfoConfirm` 时状态机会永久停在 `Confirming` (7/10),
/// 而本进程不自动重试登录链 —— 没有看门狗就是静默卡死。取值依据: 实测同一丢包前置上
/// 该响应耗时 13.5 s / 4.3 s (2026-11-26 诊断), 取 60 s 留 4.4 倍余量;
/// 误判代价 = 用户被迫重登且本进程不自动恢复, 故宁可偏长。
inline constexpr std::chrono::seconds kSettlementConfirmTimeout{60};

/// 结算单确认失败的通知正文 (纯构造便于断言, #488).
/// @param reason 失败场景的中文说明 (如「柜台拒绝，错误码 3：CTP:结算单确认失败」/
///   「60 秒内未收到柜台的结算单确认响应」)
/// @return 形如「结算单确认失败（<原因>），账户停在「已登录」不能交易，本进程不会自动重试；
///   请先登出再重新登录」; reason 为空时不产出残缺括号
std::string settlement_confirm_failed_message(const std::string& reason);

/// 合约加载失败的通知正文 (纯构造便于断言, #488).
/// @param reason 失败场景的中文说明 (如「柜台拒绝，错误码 3：CTP:未到合约加载时段」/
///   「查询 5 分钟未收到最后一批响应」)
/// @return 形如「合约加载失败（<原因>），账户停在「已登录」不能交易，本进程不会自动重试；
///   请先登出再重新登录」; reason 为空时不产出残缺括号
std::string instruments_load_failed_message(const std::string& reason);

// ============================================================================
// 基准持仓查询的有限重试策略 (ADR 0049 决策 3)
// ============================================================================
// 持仓基准是登录成功的前置 (没有基准就没有账): 单次查询失败先按本策略重试,
// 预算用尽即判本次登录失败 —— 停在当前进度、不转 Ready, 由用户处理.
// 只在登录 / 重连链上生效, 与已删除的周期查询/补查链无关.

/// 基准持仓查询最多重试次数 (首次尝试之外; 总尝试次数 = 1 + 本值).
inline constexpr int kPositionQueryMaxRetries = 3;

/// 基准持仓查询重试间隔 (ADR 0049 决策 3: 3 秒; 沿用旧版 DzTrader3 的节奏, 见 #413).
inline constexpr std::chrono::milliseconds kPositionQueryRetryInterval{3000};

/// 基准持仓查询本次失败后是否还应再重试一次.
/// @param retries_done 已重试次数 (0 = 首次尝试刚失败, 尚未重试过)
/// @return true = 还可用剩余重试预算; false = 预算已用尽, 调用方应判登录失败
bool position_query_retry_allowed(int retries_done) noexcept;

/// 基准持仓查询重试耗尽的通知正文 (纯构造便于断言).
///
/// 只陈述事实 (「查询失败 + 账户未就绪 + 已重试几次」), **不承诺** UI 上不存在的操作:
/// WebUI 目前没有 td 的连接/断开入口 (见 #409), 写「请登出后重新登录」等于让用户
/// 去做一件界面上做不到的事 (与 login_failed_message 同一口径).
/// @return 固定文案「持仓查询失败，账户未就绪（已重试 3 次）」(N = kPositionQueryMaxRetries)
std::string position_query_failed_message();

// ============================================================================
// 回报交易日校验 (ADR 0049 决策 5 / 契约 td-data-sync §3 §7)
// ============================================================================
// 「会话不得跨交易日」是持仓/统计正确性的前提 (期权行权/履约/到期、盘后自动组合零回调,
// 只能由次日登录的基准查询吸收; 会话内迁移 today→yd 会让这些缺口永久化). 运行期保证方式 =
// 回报自带的 `TradingDay` 必须能自证属于当前会话交易日; 判死即主动登出, 由下一次登录
// 重建会话与基准 (#423).

/// 回报自带 `TradingDay` 与会话交易日的一致性判定.
enum class ReportTradingDayVerdict : uint8_t {
    Match = 0,       ///< 解析成功且等于会话交易日: 正常处理
    SessionUnknown,  ///< 会话交易日尚未建立 (未登录 / 登录失败): 无法自证
    Empty,           ///< 回报未携带交易日: 无法自证
    Unparsable,      ///< 回报交易日无法解析: 无法自证
    Mismatch,        ///< 回报交易日与会话交易日不同 (真跨日): 不能自证
};

/// 判定回报交易日是否可接受: 只有 Match 可接受, 其余一律按「不能自证」处理 (记 ERROR +
/// 主动登出). 判定顺序 = 会话交易日未建立 → 空串 → 解析失败 → 不相等, 与日志 `reason` 一致.
///
/// 「无法解析」也判死而不是退回会话交易日: 日期字段变成垃圾说明数据模型已变, 此时继续用
/// 会话交易日给记录盖日期, 比多登出一次更危险 (issue #423 拍板: 空串/非法/不相等一律登出).
/// @param session_day 当前会话交易日 (DzDate 距纪元天数; 哨兵 / <=0 视为未建立)
/// @param report_day CTP 回报自带的 TradingDay (8 位 "YYYYMMDD"; 允许空 / nullptr)
ReportTradingDayVerdict judge_report_trading_day(int32_t session_day,
                                                 const char* report_day) noexcept;

/// 判定值的稳定字符串 (小写下划线; 与枚举名解耦, 便于日志检索).
/// Match→"match", SessionUnknown→"session_unknown", Empty→"empty",
/// Unparsable→"unparsable", Mismatch→"mismatch".
const char* report_trading_day_reason(ReportTradingDayVerdict verdict) noexcept;

/// 跨交易日主动登出的 UI 通知正文 (纯构造便于断言, #423).
///
/// 口径同 login_failed_message / position_query_failed_message: 只陈述事实 + 后续行为,
/// **不承诺** UI 上不存在的操作 (WebUI 无 td 连接/断开入口, 见 #409) —— 登出后由自动调度
/// 在下一个 `login_time` 重新登录 (契约 td-data-sync §7 的代价口径).
/// @param verdict judge_report_trading_day 的判定值 (只接受非 Match)
/// @param session_day 会话交易日 (DzDate 距纪元天数; <=0 时文案不引用具体日期)
/// @param report_day 回报自带原始 TradingDay (可空)
std::string cross_day_logout_message(ReportTradingDayVerdict verdict,
                                     int32_t session_day,
                                     const char* report_day);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
