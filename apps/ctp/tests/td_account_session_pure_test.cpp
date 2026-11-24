#include "td/td_account_session_pure.h"

#include <limits>

#include <gtest/gtest.h>

#include <dztrader/date_time/date.h>

using namespace dztrader::ctp;

// ============================================================================
// OrderRefMap
// ============================================================================

TEST(OrderRefMapTest, EmptyReturnsNullptr) {
    OrderRefMap m;
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.find_by_sys_id("12345"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, InsertAndFindByOrderRef) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    EXPECT_EQ(m.size(), 1u);
    const DzOrderId* p = m.find_by_order_ref("1");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 100);
}

TEST(OrderRefMapTest, InsertAndFindBySysId) {
    OrderRefMap m;
    m.insert_by_sys_id("12345", 200);
    const DzOrderId* p = m.find_by_sys_id("12345");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 200);
}

TEST(OrderRefMapTest, MultipleEntries) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_order_ref("2", 200);
    m.insert_by_order_ref("3", 300);
    EXPECT_EQ(m.size(), 3u);
}

TEST(OrderRefMapTest, RefAndSysIdIndependent) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_sys_id("12345", 100);
    EXPECT_EQ(*m.find_by_order_ref("1"), 100);
    EXPECT_EQ(*m.find_by_sys_id("12345"), 100);
}

TEST(OrderRefMapTest, EraseByOrderRef) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.erase_by_order_ref("1");
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, OverwriteByOrderRef) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_order_ref("1", 200);  // 覆盖
    EXPECT_EQ(*m.find_by_order_ref("1"), 200);
}

// C1: OrderRefMap 内部归一化到 12 位补零格式 (与 CTP 回传格式一致).
// place_order 用 std::to_string("1") insert, on_rtn_order 用 CTP 回传 "000000000001" find,
// 修复前两者格式不匹配导致本平台订单全被误判为外部订单.
TEST(OrderRefMapTest, NormalizeOrderRefInsertShortFindPadded) {
    OrderRefMap m;
    // place_order 用非补零格式登记
    m.insert_by_order_ref("1", 100);
    EXPECT_EQ(m.size(), 1u);
    // on_rtn_order 用 CTP 12 位补零格式查表, 应能找到
    const DzOrderId* p = m.find_by_order_ref("000000000001");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 100);
    // 反向也成立
    EXPECT_EQ(*m.find_by_order_ref("1"), 100);
}

TEST(OrderRefMapTest, NormalizeOrderRefInsertPaddedFindShort) {
    OrderRefMap m;
    // CTP 回传的外部订单回报用 12 位补零格式 insert
    m.insert_by_order_ref("000000000001", 200);
    // place_order 回滚用 std::to_string 格式 erase, 应能匹配
    EXPECT_EQ(*m.find_by_order_ref("1"), 200);
}

TEST(OrderRefMapTest, NormalizeOrderRefEraseByIdentity) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    // erase 用补零格式, 应能删除非补零格式登记的条目
    m.erase_by_order_ref("000000000001");
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.find_by_order_ref("000000000001"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, NormalizeOrderRefLargeNumber) {
    OrderRefMap m;
    // 大数也归一化 (CTP OrderRef 最大 12 位 9)
    m.insert_by_order_ref("999999999999", 300);
    EXPECT_EQ(*m.find_by_order_ref("999999999999"), 300);
}

TEST(OrderRefMapTest, NonNumericOrderRefPassedThrough) {
    // 外部订单可能传入非数字 OrderRef: 原样返回, 不做补零归一化
    OrderRefMap m;
    m.insert_by_order_ref("EXT-1", 400);
    EXPECT_EQ(m.size(), 1u);
    const DzOrderId* p = m.find_by_order_ref("EXT-1");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 400);
    // 非数字串不会被当成数字补零后匹配
    EXPECT_EQ(m.find_by_order_ref("00000000EXT-1"), nullptr);
    m.erase_by_order_ref("EXT-1");
    EXPECT_EQ(m.find_by_order_ref("EXT-1"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, ThirteenDigitOrderRefNotTruncated) {
    // 超出 12 位但仍在 stoll 范围内: 原样保留, 不截断
    OrderRefMap m;
    const std::string ref = "1234567890123";
    m.insert_by_order_ref(ref, 500);
    const DzOrderId* p = m.find_by_order_ref(ref);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 500);
    EXPECT_EQ(m.size(), 1u);
}

TEST(OrderRefMapTest, OrderRefBeyondStollRangePassedThrough) {
    // 20 位数字超出 long long 范围: stoll 抛异常后原样返回
    OrderRefMap m;
    const std::string ref = "99999999999999999999";
    m.insert_by_order_ref(ref, 600);
    const DzOrderId* p = m.find_by_order_ref(ref);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 600);
    m.erase_by_order_ref(ref);
    EXPECT_EQ(m.find_by_order_ref(ref), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, Clear) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_sys_id("12345", 100);
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.find_by_sys_id("12345"), nullptr);
    EXPECT_EQ(m.find_strategy(100), nullptr);
}

// ============================================================================
// strategy_id 映射 (DzOrderId -> 裸策略名, 回报回填用)
// ============================================================================

TEST(OrderRefMapTest, StrategyInsertAndFind) {
    OrderRefMap m;
    m.insert_strategy(100, "stg_a");
    const std::string* s = m.find_strategy(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "stg_a");
}

TEST(OrderRefMapTest, StrategyFindMissingReturnsNullptr) {
    OrderRefMap m;
    EXPECT_EQ(m.find_strategy(999), nullptr);
}

TEST(OrderRefMapTest, StrategyOverwrite) {
    OrderRefMap m;
    m.insert_strategy(100, "stg_a");
    m.insert_strategy(100, "stg_b");
    EXPECT_EQ(*m.find_strategy(100), "stg_b");
}

TEST(OrderRefMapTest, StrategyErase) {
    OrderRefMap m;
    m.insert_strategy(100, "stg_a");
    m.erase_strategy(100);
    EXPECT_EQ(m.find_strategy(100), nullptr);
}

TEST(OrderRefMapTest, StrategyEraseMissingNoOp) {
    OrderRefMap m;
    m.erase_strategy(999);
    EXPECT_EQ(m.find_strategy(999), nullptr);
}

// ============================================================================
// CancelContext (C4: cancel_order 反向查找)
// ============================================================================

TEST(OrderRefMapTest, CancelContextEmptyReturnsNullptr) {
    OrderRefMap m;
    EXPECT_EQ(m.find_cancel_context(100), nullptr);
}

TEST(OrderRefMapTest, InsertAndFindCancelContext) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "000000000001", .front_id = 0, .session_id = 0};
    m.insert_cancel_context(100, ctx);
    const CancelContext* p = m.find_cancel_context(100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->order_ref, "000000000001");
    EXPECT_EQ(p->front_id, 0);
    EXPECT_EQ(p->session_id, 0);
}

TEST(OrderRefMapTest, UpdateCancelContextFrontSessionId) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "000000000001", .front_id = 0, .session_id = 0};
    m.insert_cancel_context(100, ctx);
    // 模拟 on_rtn_order 收到 CTP 回报后更新 front_id/session_id
    m.update_cancel_context(100, 5, 1234);
    const CancelContext* p = m.find_cancel_context(100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->front_id, 5);
    EXPECT_EQ(p->session_id, 1234);
}

TEST(OrderRefMapTest, UpdateCancelContextZeroIgnored) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "000000000001", .front_id = 5, .session_id = 1234};
    m.insert_cancel_context(100, ctx);
    // 0 值应被忽略, 不覆盖已有有效值
    m.update_cancel_context(100, 0, 0);
    const CancelContext* p = m.find_cancel_context(100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->front_id, 5);
    EXPECT_EQ(p->session_id, 1234);
}

TEST(OrderRefMapTest, UpdateCancelContextNotFoundIgnored) {
    OrderRefMap m;
    // 未 insert 的 order_id, update 应 no-op
    m.update_cancel_context(999, 5, 1234);
    EXPECT_EQ(m.find_cancel_context(999), nullptr);
}

TEST(OrderRefMapTest, ClearAlsoClearsCancelContext) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "1", .front_id = 0, .session_id = 0};
    m.insert_cancel_context(100, ctx);
    m.clear();
    EXPECT_EQ(m.find_cancel_context(100), nullptr);
}

// ============================================================================
// sync_order_ref (设计 §9.4)
// ============================================================================

TEST(SyncOrderRefTest, CtpMaxLarger) { EXPECT_EQ(sync_order_ref(5, 100), 101); }

TEST(SyncOrderRefTest, CurrentLarger) { EXPECT_EQ(sync_order_ref(200, 100), 201); }

TEST(SyncOrderRefTest, Equal) { EXPECT_EQ(sync_order_ref(100, 100), 101); }

TEST(SyncOrderRefTest, BothZero) { EXPECT_EQ(sync_order_ref(0, 0), 1); }

TEST(SyncOrderRefTest, NegativeCtpMax) { EXPECT_EQ(sync_order_ref(50, -1), 51); }

// ============================================================================
// parse_max_order_ref
// ============================================================================

TEST(ParseMaxOrderRefTest, ValidNumber) {
    EXPECT_EQ(parse_max_order_ref("100"), 100);
    EXPECT_EQ(parse_max_order_ref("0"), 0);
    EXPECT_EQ(parse_max_order_ref("999999"), 999999);
}

TEST(ParseMaxOrderRefTest, EmptyReturnsZero) {
    EXPECT_EQ(parse_max_order_ref(""), 0);
    EXPECT_EQ(parse_max_order_ref(nullptr), 0);
}

TEST(ParseMaxOrderRefTest, NonDigitReturnsZero) {
    EXPECT_EQ(parse_max_order_ref("abc"), 0);
    EXPECT_EQ(parse_max_order_ref("12abc"), 0);
}

// ============================================================================
// alert_report_due (持续异常告警的通知限频: 未知枚举 / 外部单重放告警共用)
// ============================================================================

TEST(AlertReportDueTest, NeverReportedIsDue) {
    // 0 / 负值 = 尚未上报过 -> 首次必发 (否则计数器涨了但界面永远收不到通知).
    EXPECT_TRUE(alert_report_due(1000, 0, 300));
    EXPECT_TRUE(alert_report_due(1000, -5, 300));
}

TEST(AlertReportDueTest, WithinIntervalIsThrottled) {
    // 间隔内只计数不通知: 未知枚举可能每条回报都带, 逐条通知会刷爆前端通知环.
    EXPECT_FALSE(alert_report_due(1299, 1000, 300));
    EXPECT_FALSE(alert_report_due(1000, 1000, 300));
}

TEST(AlertReportDueTest, AtOrBeyondIntervalIsDue) {
    EXPECT_TRUE(alert_report_due(1300, 1000, 300));
    EXPECT_TRUE(alert_report_due(2000, 1000, 300));
}

// ============================================================================
// replay_external_only_alarm (外部单「只在登录重放出现」告警, ADR 0049)
// ============================================================================

TEST(ReplayExternalOnlyAlarmTest, RecordNotAfterLastLiveReportIsAlarmed) {
    // 该记录的柜台时间早于最后一条实时回报 → 那时本会话已在收实时流, 按投资者级假设
    // 它本应实时到达并落库；仍以新记录出现在重放里 = 假设可能失效.
    const int64_t last_live = 100000;  // 交易日当天 10:00 前后 (值域只要求可比)
    EXPECT_TRUE(
        replay_external_only_alarm(/*in_replay=*/true, /*record_epoch_s=*/99000, last_live));
    EXPECT_TRUE(replay_external_only_alarm(true, last_live, last_live))
        << "同一秒内的记录同样应已到达 (同秒边界不豁免)";
}

TEST(ReplayExternalOnlyAlarmTest, RecordAfterLastLiveReportIsNotAlarmed) {
    // 晚于最后实时回报的记录无法判定: 可能是断线窗口内别的终端下的单, 只能靠重放补发.
    EXPECT_FALSE(replay_external_only_alarm(true, /*record_epoch_s=*/100001, 100000));
    EXPECT_FALSE(replay_external_only_alarm(true, 110000, 100000));
}

TEST(ReplayExternalOnlyAlarmTest, RealTimeRecordIsNeverAlarmed) {
    // 实时流记录不是本告警的取证对象 (判据只针对「只出现在重放里」).
    EXPECT_FALSE(replay_external_only_alarm(/*in_replay=*/false, 100000, 200000));
}

TEST(ReplayExternalOnlyAlarmTest, UnknownTimestampsAreNotAlarmed) {
    // 柜台时间未知 (InsertTime 解析失败) 或尚未观测到实时回报 (进程刚启动 / 跨日清空)
    // 时无从比较, 保守不告警.
    EXPECT_FALSE(replay_external_only_alarm(true, /*record_epoch_s=*/0, 100000));
    EXPECT_FALSE(replay_external_only_alarm(true, /*record_epoch_s=*/-1, 100000));
    EXPECT_FALSE(replay_external_only_alarm(true, 100000, /*last_live_epoch_s=*/0));
    EXPECT_FALSE(replay_external_only_alarm(true, 100000, /*last_live_epoch_s=*/-1));
}

// ============================================================================
// trading_day_usable (登录响应的会话交易日可用性)
// ============================================================================

TEST(TradingDayUsableTest, AcceptsRealTradingDay) {
    // 2026-07-27 距纪元天数 (与 mapping 测试同一常量口径).
    EXPECT_TRUE(trading_day_usable(dztrader::Date{2026, 7, 27}.days_since_epoch()));
    EXPECT_TRUE(trading_day_usable(1));
    EXPECT_TRUE(trading_day_usable(20000));
}

TEST(TradingDayUsableTest, RejectsParseFailureSentinel) {
    // TdSpi::OnRspUserLogin 在 parse_ctp_date 返回 DZ_DATE_NA 时保留的哨兵.
    // 判为不可用 = 调用方按登录失败处置 (对齐 md 的 LoginParseError), 不进 LoggedIn.
    EXPECT_FALSE(trading_day_usable(std::numeric_limits<int32_t>::min()));
}

TEST(TradingDayUsableTest, RejectsEpochDayCollidingWithNa) {
    // 0 = DZ_DATE_NA; "19700101" 解析结果与哨兵重合, 业务上不存在 1970 年的真实交易日,
    // 同样判为不可用 —— 否则登录会把整会话数据锚在纪元日.
    EXPECT_FALSE(trading_day_usable(0));
    EXPECT_FALSE(trading_day_usable(-1));
}

// ============================================================================
// login_failed_message (登录失败通知正文, #407)
// ============================================================================

TEST(LoginFailedMessageTest, IncludesReasonConsequenceAndRecovery) {
    // 登录失败后状态回滚到 Authenticated/Connected (正常中间态), 前端无法与「正在登录」
    // 区分; 且本进程不自动重发登录 (#406). 正文必须给出「发生了什么」与「会怎样」,
    // 以及下一步——#409 落地后 WebUI 有「先登出、再登录」入口 (ADR 0051 决策 4).
    const std::string msg = login_failed_message("交易日无效（原始值 \"00000000\"）");
    EXPECT_EQ(msg,
              "交易登录失败（交易日无效（原始值 \"00000000\"）），本进程不会自动重试该账户；"
              "请先登出再重新登录");
    EXPECT_NE(msg.find("请先登出再重新登录"), std::string::npos)
        << "recovery path must be actionable since #409 landed";
}

TEST(LoginFailedMessageTest, PropagatesScenarioVerbatim) {
    // 三种失败场景的说明各自原样带入, 不被归一成同一句通用文案.
    for (const char* reason : {"柜台拒绝，错误码 3：CTP:用户不存在", "当日订单/成交基准装载失败",
                               "10 秒内未收到柜台的登录响应"}) {
        const std::string msg = login_failed_message(reason);
        EXPECT_NE(msg.find(reason), std::string::npos) << reason;
    }
}

TEST(LoginFailedMessageTest, EmptyReasonOmitsParentheses) {
    // 空原因不得产出「（）」这种残缺文案; 逐字锁定, 防止改成 ASCII () 后静默通过.
    EXPECT_EQ(login_failed_message(""),
              "交易登录失败，本进程不会自动重试该账户；请先登出再重新登录");
}

// ============================================================================
// position_query_retry_allowed / position_query_failed_message
// (基准持仓查询有限重试策略, ADR 0049 决策 3)
// ============================================================================

TEST(PositionQueryRetryTest, RetriesUntilBudgetExhausted) {
    // 语义: retries_done = 已重试次数 (0 = 首次尝试刚失败). 预算 = 3 次重试,
    // 即总尝试 4 次; 第 4 次失败后不再重试 (重试耗尽 → 登录失败).
    EXPECT_EQ(kPositionQueryMaxRetries, 3) << "ADR 0049: retry 3 times, 3s apart";
    EXPECT_TRUE(position_query_retry_allowed(0));
    EXPECT_TRUE(position_query_retry_allowed(1));
    EXPECT_TRUE(position_query_retry_allowed(kPositionQueryMaxRetries - 1));
    EXPECT_FALSE(position_query_retry_allowed(kPositionQueryMaxRetries));
    EXPECT_FALSE(position_query_retry_allowed(kPositionQueryMaxRetries + 1));
}

TEST(PositionQueryRetryTest, NegativeCounterIsNotTreatedAsUnlimited) {
    // 防御: 计数被写坏成负值时同样不再追加重试 (返回 false 由调用方判失败),
    // 不能因为 -1 < 3 就退化成无限重试.
    EXPECT_FALSE(position_query_retry_allowed(-1));
}

TEST(PositionQueryRetryTest, IntervalIsThreeSeconds) {
    // 3 秒节奏: ADR 0049 决策 3, 沿用旧版 DzTrader3 的节奏 (#413).
    EXPECT_EQ(kPositionQueryRetryInterval, std::chrono::seconds(3));
}

TEST(PositionQueryFailedMessageTest, StatesFactWithRetryCount) {
    // 文案只陈述事实: 查询失败 + 账户未就绪 + 已重试几次. 逐字锁定.
    const std::string msg = position_query_failed_message();
    EXPECT_EQ(msg, "持仓查询失败，账户未就绪（已重试 3 次）");
    EXPECT_NE(msg.find(std::to_string(kPositionQueryMaxRetries)), std::string::npos)
        << "retry count must follow the policy constant, not be hard-coded twice";
}

TEST(PositionQueryFailedMessageTest, DoesNotPromiseOperationsTheUiLacks) {
    // 不承诺「请登出后重新登录」: WebUI 没有 td 连接/断开入口 (#409)
    // —— 与 login_failed_message 同一口径, 不把用户指向界面上不存在的操作.
    for (const char* forbidden : {"请登出", "重新登录", "断开"}) {
        EXPECT_EQ(position_query_failed_message().find(forbidden), std::string::npos)
            << "must not tell the user to do something the UI cannot do (#409): " << forbidden;
    }
}

// ============================================================================
// judge_report_trading_day / report_trading_day_reason
// (回报交易日校验, ADR 0049 决策 5, #423)
// ============================================================================

namespace {
/// 会话交易日取 2026-10-09 的距纪元天数 (与真实日历解耦的固定锚).
int32_t session_day() { return dztrader::Date{2026, 10, 9}.days_since_epoch(); }
}  // namespace

TEST(ReportTradingDayVerdictTest, MatchWhenEqual) {
    // 唯一可接受的分支: 回报自证等于会话交易日.
    EXPECT_EQ(judge_report_trading_day(session_day(), "20261009"), ReportTradingDayVerdict::Match);
}

TEST(ReportTradingDayVerdictTest, MismatchWhenDifferentDay) {
    // 真跨日 (期货公司没在日切时断连): 回报交易日前进/后退都判死, 不引入方向.
    EXPECT_EQ(judge_report_trading_day(session_day(), "20261012"),
              ReportTradingDayVerdict::Mismatch);
    EXPECT_EQ(judge_report_trading_day(session_day(), "20261008"),
              ReportTradingDayVerdict::Mismatch);
}

TEST(ReportTradingDayVerdictTest, EmptyWhenReportDayMissing) {
    // 空串 = 回报没带交易日; nullptr 防御性同判 (事件结构体正常不会为 null).
    EXPECT_EQ(judge_report_trading_day(session_day(), ""), ReportTradingDayVerdict::Empty);
    EXPECT_EQ(judge_report_trading_day(session_day(), nullptr), ReportTradingDayVerdict::Empty);
}

TEST(ReportTradingDayVerdictTest, UnparsableForMalformedDay) {
    // 长度/字符/日历三类非法都必须判死 —— 拍板: 不退回会话交易日, 宁可多登出一次.
    for (const char* bad : {"2026109", "202610091", "2026-10-09", "2026/10/09", "abcdefgh",
                            "20261309", "20261032", "00000000"}) {
        EXPECT_EQ(judge_report_trading_day(session_day(), bad), ReportTradingDayVerdict::Unparsable)
            << bad;
    }
}

TEST(ReportTradingDayVerdictTest, SessionUnknownTakesPrecedence) {
    // 会话交易日未建立 (0 = DZ_DATE_NA, 负数 / INT32_MIN 哨兵): 无法自证, 先于报文内容判死.
    // 即便报文本身合法/空, 结论都是 SessionUnknown (会话状态异常是更可操作的证据).
    for (int32_t unknown : {0, -1, std::numeric_limits<int32_t>::min()}) {
        EXPECT_EQ(judge_report_trading_day(unknown, "20261009"),
                  ReportTradingDayVerdict::SessionUnknown);
        EXPECT_EQ(judge_report_trading_day(unknown, ""), ReportTradingDayVerdict::SessionUnknown);
    }
}

TEST(ReportTradingDayReasonTest, StableLowerSnakeStrings) {
    // 字符串是日志/检索口径 (契约 §7 的 reason), 与枚举名解耦: 逐字锁定, 改名不得静默漂移.
    EXPECT_EQ(std::string(report_trading_day_reason(ReportTradingDayVerdict::Match)), "match");
    EXPECT_EQ(std::string(report_trading_day_reason(ReportTradingDayVerdict::SessionUnknown)),
              "session_unknown");
    EXPECT_EQ(std::string(report_trading_day_reason(ReportTradingDayVerdict::Empty)), "empty");
    EXPECT_EQ(std::string(report_trading_day_reason(ReportTradingDayVerdict::Unparsable)),
              "unparsable");
    EXPECT_EQ(std::string(report_trading_day_reason(ReportTradingDayVerdict::Mismatch)),
              "mismatch");
}

// ============================================================================
// cross_day_logout_message (判死时的 UI 通知正文, #423)
// ============================================================================

TEST(CrossDayLogoutMessageTest, MismatchNamesBothDays) {
    // 不一致场景必须给出「会话哪一天 / 回报哪一天」两个可比对的事实, 否则运维无法区分
    // 真跨日与柜台脏数据。
    const std::string msg =
        cross_day_logout_message(ReportTradingDayVerdict::Mismatch, session_day(), "20261012");
    EXPECT_NE(msg.find("20261009"), std::string::npos) << msg;
    EXPECT_NE(msg.find("20261012"), std::string::npos) << msg;
    EXPECT_NE(msg.find("不一致"), std::string::npos) << msg;
    EXPECT_NE(msg.find("已主动登出"), std::string::npos) << msg;
}

TEST(CrossDayLogoutMessageTest, EachVerdictExplainsItsOwnCause) {
    EXPECT_NE(cross_day_logout_message(ReportTradingDayVerdict::Empty, session_day(), "")
                  .find("未携带交易日"),
              std::string::npos);
    EXPECT_NE(
        cross_day_logout_message(ReportTradingDayVerdict::Unparsable, session_day(), "2026100X")
            .find("2026100X"),
        std::string::npos);
    EXPECT_NE(cross_day_logout_message(ReportTradingDayVerdict::SessionUnknown, 0, "20261009")
                  .find("未建立"),
              std::string::npos);
}

TEST(CrossDayLogoutMessageTest, UnknownSessionDayIsNotRenderedAsEpoch) {
    // 会话交易日不可用时不得把 0 渲染成 1970-01-01 这种假证据。
    const std::string msg =
        cross_day_logout_message(ReportTradingDayVerdict::SessionUnknown, 0, "20261009");
    EXPECT_EQ(msg.find("19700101"), std::string::npos) << msg;
}

TEST(CrossDayLogoutMessageTest, DoesNotPromiseOperationsTheUiLacks) {
    // 与 login_failed_message 同一口径: WebUI 没有 td 连接/断开入口 (#409),
    // 只陈述「已主动登出 + 会等自动登录」, 不让用户去做做不到的操作。
    for (auto verdict :
         {ReportTradingDayVerdict::Mismatch, ReportTradingDayVerdict::Empty,
          ReportTradingDayVerdict::Unparsable, ReportTradingDayVerdict::SessionUnknown}) {
        const std::string msg = cross_day_logout_message(verdict, session_day(), "20261012");
        EXPECT_EQ(msg.find("请登出"), std::string::npos) << msg;
        EXPECT_EQ(msg.find("请重新登录"), std::string::npos) << msg;
        EXPECT_NE(msg.find("自动登录"), std::string::npos) << msg;
    }
}

// ============================================================================
// route_query_error (ADR 0055 / #457): OnRspError 按在途查询 request id 归位
// ============================================================================

TEST(RouteQueryErrorTest, RoutesByInFlightQueryRequestId) {
    EXPECT_EQ(route_query_error(11, 11, 12, 13), QueryErrorTarget::Instrument);
    EXPECT_EQ(route_query_error(12, 11, 12, 13), QueryErrorTarget::Position);
    EXPECT_EQ(route_query_error(13, 11, 12, 13), QueryErrorTarget::TradingAccount);
}

TEST(RouteQueryErrorTest, UnmatchedRequestIdIsNone) {
    EXPECT_EQ(route_query_error(99, 11, 12, 13), QueryErrorTarget::None);
}

TEST(RouteQueryErrorTest, InvalidRequestIdIsNone) {
    // 0 / 负数视为无效（测试与未登录会话的默认值），不参与匹配，避免把无关错误
    // 路由到默认 id=0 的查询链。
    EXPECT_EQ(route_query_error(0, 11, 12, 13), QueryErrorTarget::None);
    EXPECT_EQ(route_query_error(-1, 11, 12, 13), QueryErrorTarget::None);
    EXPECT_EQ(route_query_error(0, 0, 0, 0), QueryErrorTarget::None);
}

TEST(RouteQueryErrorTest, FirstMatchWinsInFixedOrder) {
    // 生产环境 request id 单调递增、绝不重复；若配置异常出现重复 id，按固定顺序
    // （持仓 -> 合约 -> 资金）取第一个，行为确定、可测试。
    EXPECT_EQ(route_query_error(7, 7, 7, 7), QueryErrorTarget::Position);
}
