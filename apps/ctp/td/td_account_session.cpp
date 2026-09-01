#include "td/td_account_session.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/encoding.h>
#include <dztrader/core/string_util.h>
#include <dztrader/data_type.h>
#include <dztrader/date_time/date.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/struct.h>

namespace dztrader::ctp {

AccountSession::AccountSession(std::string account_id,
                               shm::OrderIdMeta& order_id_meta,
                               shm::MultiWriter& event_writer,
                               PersistWriter& persist_writer,
                               const MpmcQueuePtr& event_queue,
                               dztrader::core::TimerQueue& timer_queue)
    : account_id_(std::move(account_id)),
      order_id_meta_(order_id_meta),
      event_writer_(event_writer),
      persist_writer_(persist_writer),
      event_queue_(event_queue),
      timer_queue_(timer_queue),
      risk_gate_(false) {
    if (!event_queue_) {
        throw std::runtime_error("AccountSession: event_queue is null");
    }
}

AccountSession::~AccountSession() {
    try {
        disconnect();
    } catch (...) {
        // 析构不抛异常
    }
}

// ============================================================================
// 生命周期
// ============================================================================

void AccountSession::open(const std::string& flow_dir,
                          const std::vector<std::string>& front_addrs,
                          const std::string& broker_id,
                          const std::string& user_id,
                          const std::string& password,
                          const std::string& auth_code,
                          const std::string& app_id) {
    if (api_ != nullptr) {
        SPDLOG_WARN("td session already opened | account={}", account_id_);
        return;
    }
    broker_id_ = broker_id;
    user_id_ = user_id;
    password_ = password;
    auth_code_ = auth_code;
    app_id_ = app_id;
    front_addrs_ = front_addrs;
    flow_dir_ = flow_dir;

    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flow_dir.c_str());
    if (api_ == nullptr) {
        throw std::runtime_error("CreateFtdcTraderApi returned null");
    }

    // 保存 CTP API 版本, 用于 on_rsp_user_login 填充 sys_version (I1: 替代 CZCETime)
    api_version_ = CThostFtdcTraderApi::GetApiVersion();
    state_machine_.set_api_version(api_version_);

    spi_ = std::make_unique<TdSpi>(account_id_, event_queue_);
    api_->RegisterSpi(spi_.get());
    // RESTART: 重连后从断点续传, 用于回报补登 (设计 §5.3)
    api_->SubscribePrivateTopic(THOST_TERT_RESTART);
    api_->SubscribePublicTopic(THOST_TERT_RESTART);
    // 注册所有前置地址（CTP 支持多前置自动故障切换）
    for (const auto& addr : front_addrs) {
        std::string front_addr = addr;
        if (!front_addr.starts_with("tcp://") && !front_addr.starts_with("ssl://") &&
            !front_addr.starts_with("socks")) {
            front_addr = "tcp://" + front_addr;
        }
        api_->RegisterFront(const_cast<char*>(front_addr.c_str()));  // NOLINT
    }

    state_machine_.on_connect();
    SPDLOG_INFO("td connecting | account={} fronts={}", account_id_, front_addrs.size());

    // 30s 连接超时, 防止卡在 Connecting 状态
    uint64_t gen = generation_;
    connect_timer_id_ = timer_queue_.schedule_after(
        std::chrono::seconds(30),
        [this, gen]() {
            if (gen != generation_) return;  // 陈旧回调
            if (state_machine_.state() == TdState::Connecting) {
                SPDLOG_ERROR("td connect timeout | account={}", account_id_);
                state_machine_.on_connect_timeout();
            }
        });

    api_->Init();
}

void AccountSession::disconnect() {
    if (api_ == nullptr) return;
    cancel_connect_timer();
    cancel_login_timer();
    cancel_instruments_load_timer();
    ++generation_;  // 使已挂起定时器回调失效
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
    spi_.reset();
    state_machine_.on_disconnect();
    // I1: 清空缓冲, 防止重连后重放陈旧回报
    buffered_orders_.clear();
    buffered_trades_.clear();
    SPDLOG_INFO("td disconnected | account={}", account_id_);
}

// ============================================================================
// SPI 事件处理
// ============================================================================

void AccountSession::on_front_connected() {
    cancel_connect_timer();
    auto notif = state_machine_.on_front_connected();
    SPDLOG_INFO("td front connected | account={} state={}",
                account_id_, magic_enum::enum_name(state_machine_.state()));
    if (!auth_code_.empty()) {
        req_authenticate();
    } else {
        req_login();
    }
}

void AccountSession::on_front_disconnected(int reason) {
    state_machine_.on_front_disconnected(reason);
    cancel_connect_timer();
    cancel_login_timer();
    cancel_instruments_load_timer();
    ++generation_;  // 使已挂起定时器回调失效
    // 清空持仓 map, 重连后主动查询重建 (设计 §6)
    holdings_.clear();
    // 清空合约 -> 交易所映射, 重连后 req_qry_instrument 重新填充 (C2)
    instrument_exchange_map_.clear();
    // I1: 清空缓冲回报, 防止重连后重放陈旧回报导致状态错乱
    buffered_orders_.clear();
    buffered_trades_.clear();
    // order_ref_ 故意保留: 重连后 sync_order_ref 重新同步 (设计 §9.4)
    // order_ref_map_ 故意保留: RESTART 重传去重 (设计 §9.4)
    SPDLOG_WARN("td front disconnected | account={} reason={}", account_id_, reason);
}

void AccountSession::on_rsp_authenticate(const OnRspAuthenticateField& f) {
    cancel_login_timer();
    bool ok = f.rsp_info && f.rsp_info->ErrorID == 0;
    if (!ok) {
        SPDLOG_ERROR("td authenticate failed | account={} error_id={}",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1);
        state_machine_.on_authenticate_failed();
        return;
    }
    state_machine_.on_authenticate_success();
    SPDLOG_INFO("td authenticated | account={}", account_id_);
    req_login();
}

void AccountSession::on_rsp_user_login(const OnRspTdUserLoginField& f) {
    cancel_login_timer();
    if (!f.rsp_info || f.rsp_info->ErrorID != 0) {
        SPDLOG_ERROR("td login failed | account={} error_id={}",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1);
        state_machine_.on_login_failed();
        return;
    }
    // 同步 order_ref (设计 §9.4): new = max(local+1, ctp_max+1)
    if (f.rsp_user_login) {
        int64_t ctp_max = parse_max_order_ref(f.rsp_user_login->MaxOrderRef);
        order_ref_ = sync_order_ref(order_ref_, ctp_max);
    }
    // I4: 同步 trading_day, 解析失败 (INT32_MIN) 时记 WARN 但不中断登录
    if (f.days_since_epoch != std::numeric_limits<int32_t>::min()) {
        trading_day_ = f.days_since_epoch;
    } else {
        SPDLOG_WARN("td trading_day parse failed, keep previous | account={} trading_day={}",
                    account_id_, trading_day_);
    }
    // I1: sys_version 改用 CTP API 版本 (CZCETime 是郑商所时间, 不是系统版本)
    const std::string& sys_version = api_version_;
    std::string trading_day_str = f.rsp_user_login ? f.rsp_user_login->TradingDay : "";
    std::string login_time = f.rsp_user_login ? f.rsp_user_login->LoginTime : "";
    state_machine_.on_login_success(sys_version, trading_day_str, login_time);
    SPDLOG_INFO("td login success | account={} order_ref={} trading_day={}",
                account_id_, order_ref_, trading_day_str);
    req_settlement_confirm();
}

void AccountSession::on_rsp_settlement_confirm(const OnRspSettlementInfoConfirmField& f) {
    // I8: 统一 rsp_info 处理: 缺失视为失败 (与 on_rsp_authenticate / on_rsp_user_login 一致)
    if (!f.rsp_info || f.rsp_info->ErrorID != 0) {
        std::string err = f.rsp_info
            ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
            : "rsp_info is null";
        SPDLOG_ERROR("td settlement confirm failed | account={} error_id={} error=\"{}\"",
                     account_id_, f.rsp_info ? f.rsp_info->ErrorID : -1, err);
        // C6: 回退到 LoggedIn, 等待下个调度点重试 (不卡死在 Confirming)
        state_machine_.on_settlement_confirm_failed(err);
        return;
    }
    state_machine_.on_settlement_confirmed();
    SPDLOG_INFO("td settlement confirmed | account={}", account_id_);
    // C2: 启动合约查询 (设计 §7.2), 进入 LoadingInstruments 状态.
    // 查询完成 (on_rsp_qry_instrument is_last) 或失败时调 on_instruments_loaded 转 Ready.
    req_qry_instrument();
}

void AccountSession::on_rsp_qry_instrument(const OnRspQryInstrumentField& f) {
    // 先处理数据, 再判 is_last (避免 null instrument + is_last 时状态机卡死)
    if (f.instrument) {
        // C2: 存储 instrument_id -> exchange_id 映射, 供 place_order 查表获取 exchange_id
        instrument_exchange_map_[f.instrument->InstrumentID] = f.instrument->ExchangeID;
        // I6: 推 DZ_FRAME_TD_INSTRUMENT SHM 帧, 让策略进程拿到合约信息 (price_tick/乘数/期权字段)
        try {
            DzInstrumentInfo contract = to_dz_instrument(*f.instrument);
            platform::write_struct(event_writer_, DZ_FRAME_TD_INSTRUMENT, contract);
            // 持久化合约信息 (供审计/复盘)
            // update_day[9]: "YYYYMMDD" 文本 (从 DzDate 距纪元天数转换)
            InstrumentRecord rec;
            rec.base = contract;
            if (trading_day_ > 0) {
                dztrader::Date d{trading_day_};
                auto* end = std::format_to_n(rec.update_day, sizeof(rec.update_day) - 1,
                                             "{:04d}{:02d}{:02d}",
                                             d.year(), d.month(), d.day()).out;
                *end = '\0';
            } else {
                std::snprintf(rec.update_day, sizeof(rec.update_day), "00000000");
            }
            persist_writer_.enqueue(PersistTask{PersistTask::Kind::Instrument, rec});
        } catch (const std::exception& e) {
            SPDLOG_ERROR("td instrument push failed | account={} instrument={} error=\"{}\"",
                         account_id_, f.instrument->InstrumentID, e.what());
        }
        SPDLOG_DEBUG("td instrument | account={} instrument={}",
                     account_id_, f.instrument->InstrumentID);
    } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
        // CTP 查询失败 (流控超时/权限错误等): pInstrument=null, pRspInfo={ErrorID!=0}
        SPDLOG_ERROR("td qry instrument error | account={} error_id={} error_msg=\"{}\"",
                     account_id_, f.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
    }
    if (f.is_last) {
        // C1: 错误路径调 on_instruments_load_failed, 不进入 Ready (设计 §2.4.1)
        bool failed = (f.rsp_info && f.rsp_info->ErrorID != 0) || !f.instrument;
        if (failed) {
            std::string err = f.rsp_info
                ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
                : "instrument is null on is_last";
            cancel_instruments_load_timer();
            state_machine_.on_instruments_load_failed(err);
        } else {
            cancel_instruments_load_timer();
            state_machine_.on_instruments_loaded();
            SPDLOG_INFO("td instruments loaded | account={} count={}",
                        account_id_, instrument_exchange_map_.size());
            replay_buffered_reports();
        }
    }
}

void AccountSession::on_rtn_order(const OnRtnOrderField& f) {
    // LoadingInstruments 期间缓冲, Ready 后重放 (设计 §5.3)
    if (state_machine_.state() == TdState::LoadingInstruments) {
        buffer_order_rpt(f);
        return;
    }

    try {
        // CTP OrderField -> OrderRecord (含 DzOrderReport base + CTP 扩展字段)
        OrderRecord rpt = to_order_record(f.order, account_id_, trading_day_);

        // 外部订单识别 (设计 §9.3): OrderRef 在映射表中 = 本地发出, 否则外部订单
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.order.OrderRef);
        if (local != nullptr) {
            rpt.base.order_id = *local;
            rpt.is_external = 0;
            // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
            if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
                copy_string(rpt.base.strategy_id, sid->c_str(), true);
            }
            // C4: 收到 CTP 回报后更新 CancelContext 的 front_id/session_id
            // (place_order 时初始为 0, 这里填充实际值供后续 cancel_order 使用)
            order_ref_map_.update_cancel_context(*local, f.order.FrontID, f.order.SessionID);
        } else {
            rpt.base.order_id = order_id_meta_.generate();
            rpt.is_external = 1;
            order_ref_map_.insert_by_order_ref(f.order.OrderRef, rpt.base.order_id);
        }
        // OrderSysID 非空时建立反向映射 (供外部订单识别)
        if (f.order.OrderSysID[0] != '\0') {
            order_ref_map_.insert_by_sys_id(f.order.OrderSysID, rpt.base.order_id);
        }
        // 撤单数量推导: CTP OrderField 无 VolumeCanceled 字段, 按状态推导
        // M1: 用 std::max 防止 VolumeTraded > Volume 异常数据导致负值
        if (rpt.base.status == DZ_ORDER_CANCELLED) {
            rpt.volume_canceled = std::max(0, rpt.base.volume - rpt.base.volume_traded);
        }

        // 推 SHM (DzOrderReport 通用字段, 不含 CTP 特有)
        write_order_rpt(rpt.base);
        // 持久化 (OrderRecord 含 CTP 扩展字段)
        persist_order(rpt);

        SPDLOG_DEBUG("td rtn order | account={} order_id={} order_ref={} status={} traded={}",
                     account_id_, rpt.base.order_id, f.order.OrderRef,
                     magic_enum::enum_name(rpt.base.status), rpt.base.volume_traded);
    } catch (const std::exception& e) {
        // "宁肯乱码也不能崩溃": 不传播异常到 TdApi 主循环
        SPDLOG_ERROR("td rtn order process failed | account={} error=\"{}\" instrument={} order_ref={}",
                     account_id_, e.what(), f.order.InstrumentID, f.order.OrderRef);
    }
}

void AccountSession::on_rtn_trade(const OnRtnTradeField& f) {
    if (state_machine_.state() == TdState::LoadingInstruments) {
        buffer_trade_rpt(f);
        return;
    }

    try {
        // CTP TradeField -> TradeRecord (含 DzTradeReport base + 扩展字段)
        TradeRecord rpt = to_trade_record(f.trade, account_id_, trading_day_);

        // 通过 OrderRef 反查 DzOrderId, 填到 base.order_id
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.trade.OrderRef);
        if (local != nullptr) {
            rpt.base.order_id = *local;
            // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
            if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
                copy_string(rpt.base.strategy_id, sid->c_str(), true);
            }
        }

        // 推 SHM (DzTradeReport 通用字段)
        write_trade_rpt(rpt.base);
        // 持久化 (TradeRecord 含 commission/trade_time/trade_date 扩展)
        persist_trade(rpt);

        SPDLOG_INFO("td rtn trade | account={} instrument={} trade_id={} volume={} price={}",
                    account_id_, f.trade.InstrumentID, f.trade.TradeID, f.trade.Volume,
                    f.trade.Price);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td rtn trade process failed | account={} error=\"{}\" instrument={} trade_id={}",
                     account_id_, e.what(), f.trade.InstrumentID, f.trade.TradeID);
    }
}

// ============================================================================
// 业务接口
// ============================================================================

void AccountSession::place_order(const DzOrderReq& req) {
    if (!is_ready()) {
        SPDLOG_WARN("td place_order rejected, not ready | account={} state={}",
                    account_id_, magic_enum::enum_name(state_machine_.state()));
        // C3: 推 REJECTED 回报让策略进程感知, 避免静默丢单
        reject_order(req, std::format("交易未就绪: {}",
                                       magic_enum::enum_name(state_machine_.state())));
        return;
    }

    // 风控检查 (设计 §8)
    AccountContext ctx;
    ctx.account_id = account_id_;
    // TODO: 查询链路落地后, 从 holdings_ + 合约信息缓存填充 price_tick/long_pos/short_pos
    if (auto rej = risk_gate_.check_order(req, ctx)) {
        SPDLOG_WARN("td place_order rejected by risk gate | account={} rule={} reason={}",
                    account_id_, rej->rule_name, rej->reason);
        // C3: 推 REJECTED 回报 + 风控拒绝帧
        reject_order(req, std::format("风控拒绝: {}", rej->reason));
        write_risk_reject(account_id_, rej->rule_name, rej->reason);
        return;
    }

    // C2: 从 instrument_exchange_map_ 查表获取 exchange_id (替代 req.instrument_id)
    auto it = instrument_exchange_map_.find(req.instrument_id);
    if (it == instrument_exchange_map_.end()) {
        SPDLOG_WARN("td place_order rejected: instrument not found | account={} instrument={}",
                    account_id_, req.instrument_id);
        reject_order(req, std::format("未知合约: {}", req.instrument_id));
        return;
    }

    // order_ref 递增 (设计 §9.4)
    ++order_ref_;
    OrderBuildContext build_ctx{};
    build_ctx.account_id = user_id_;  // CTP InvestorID
    build_ctx.order_ref = order_ref_;
    build_ctx.request_id = ++request_id_;
    build_ctx.exchange_id = it->second;

    CThostFtdcInputOrderField input = to_input_order_field(req, build_ctx);
    // BrokerID 单独填 (CTP 要求, to_input_order_field 不填)
    copy_string(input.BrokerID, broker_id_.c_str(), true);

    // 映射表登记: order_ref -> DzOrderId (发单前登记, 防止 OnRtnOrder 先到)
    // C1: OrderRefMap 内部归一化为 12 位补零格式, 与 CTP 回传格式一致
    std::string order_ref_str = std::to_string(order_ref_);
    order_ref_map_.insert_by_order_ref(order_ref_str, req.order_id);
    // 回报回填: DzOrderId -> strategy_id (on_rtn_order/on_rtn_trade 回填用)
    order_ref_map_.insert_strategy(req.order_id, req.strategy_id);

    int ret = api_->ReqOrderInsert(&input, build_ctx.request_id);
    if (ret != 0) {
        SPDLOG_ERROR("td req order insert failed | account={} ret={} order_id={} order_ref={}",
                     account_id_, ret, req.order_id, order_ref_);
        // 失败回滚映射
        order_ref_map_.erase_by_order_ref(order_ref_str);
        order_ref_map_.erase_strategy(req.order_id);
        reject_order(req, std::format("CTP 下单失败: ret={}", ret));
        return;
    }

    // C4: 登记 DzOrderId -> CancelContext, 供 cancel_order 反向查找.
    // front_id/session_id 初始为 0, on_rtn_order 收到 CTP 回报后 update.
    // order_ref 用 12 位补零格式, 与 to_input_order_field 下单格式一致 (CTP 按字符串比较 OrderRef)
    std::string cancel_order_ref = std::format("{:012}", order_ref_);
    CancelContext cancel_ctx{.order_ref = cancel_order_ref, .front_id = 0, .session_id = 0};
    order_ref_map_.insert_cancel_context(req.order_id, cancel_ctx);

    SPDLOG_INFO("td order submitted | account={} order_id={} order_ref={} instrument={} volume={}",
                account_id_, req.order_id, order_ref_, req.instrument_id, req.volume);
}

bool AccountSession::cancel_order(DzOrderId order_id) {
    if (!is_ready()) {
        SPDLOG_WARN("td cancel_order rejected, not ready | account={} state={} order_id={}",
                    account_id_, magic_enum::enum_name(state_machine_.state()), order_id);
        return false;
    }
    // C4: 反向查找 order_ref + front_id + session_id
    const CancelContext* ctx = order_ref_map_.find_cancel_context(order_id);
    if (ctx == nullptr) {
        SPDLOG_WARN("td cancel_order rejected: order_id not found | account={} order_id={}",
                    account_id_, order_id);
        return false;
    }
    if (api_ == nullptr) {
        SPDLOG_ERROR("td cancel_order failed: api is null | account={} order_id={}",
                     account_id_, order_id);
        return false;
    }

    CThostFtdcInputOrderActionField action{};
    copy_string(action.BrokerID, broker_id_.c_str(), true);
    copy_string(action.InvestorID, user_id_.c_str(), true);
    // OrderRef + FrontID + SessionID 三元组定位订单 (CTP 撤单要求)
    copy_string(action.OrderRef, ctx->order_ref.c_str(), true);
    action.FrontID = ctx->front_id;
    action.SessionID = ctx->session_id;
    action.ActionFlag = THOST_FTDC_AF_Delete;

    int ret = api_->ReqOrderAction(&action, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req order action failed | account={} ret={} order_id={} order_ref={}",
                     account_id_, ret, order_id, ctx->order_ref);
        return false;
    }
    SPDLOG_INFO("td cancel submitted | account={} order_id={} order_ref={} front={} session={}",
                account_id_, order_id, ctx->order_ref, ctx->front_id, ctx->session_id);
    return true;
}

void AccountSession::set_trading_day(int32_t trading_day) {
    trading_day_ = trading_day;
    for (auto& [_, holding] : holdings_) {
        holding.set_trading_day(trading_day);
    }
    SPDLOG_INFO("td trading day updated | account={} trading_day={}", account_id_, trading_day);
}

void AccountSession::delete_event(Event& event) noexcept {
    if (event.data == nullptr) return;
    // td 关心的事件走 td_delete_event_data:
    //   - td 类型 (>=100)
    //   - OnFrontConnected / OnFrontDisconnected (td 进程用 td 版本 Field, 不可走 md 路径)
    // 其他 md 类型 (3, 4-13) 走 Event::delete_data (md 内联)
    if (static_cast<int16_t>(event.type) >= 100 ||
        event.type == EventType::OnFrontConnected ||
        event.type == EventType::OnFrontDisconnected) {
        td_delete_event_data(event);
    } else {
        event.delete_data();
    }
}

// ============================================================================
// 内部辅助: CTP 请求
// ============================================================================

void AccountSession::req_authenticate() {
    CThostFtdcReqAuthenticateField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    copy_string(f.UserID, user_id_.c_str(), true);
    copy_string(f.AppID, app_id_.c_str(), true);
    copy_string(f.AuthCode, auth_code_.c_str(), true);
    int ret = api_->ReqAuthenticate(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req authenticate failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_authenticate();
    // 10s 认证超时
    uint64_t gen = generation_;
    login_timer_id_ = timer_queue_.schedule_after(
        std::chrono::seconds(10),
        [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::Authenticating) {
                SPDLOG_ERROR("td authenticate timeout | account={}", account_id_);
                state_machine_.on_authenticate_failed();
            }
        });
}

void AccountSession::req_login() {
    CThostFtdcReqUserLoginField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    copy_string(f.UserID, user_id_.c_str(), true);
    copy_string(f.Password, password_.c_str(), true);
    int ret = api_->ReqUserLogin(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req login failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_login();
    uint64_t gen = generation_;
    login_timer_id_ = timer_queue_.schedule_after(
        std::chrono::seconds(10),
        [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoggingIn) {
                SPDLOG_ERROR("td login timeout | account={}", account_id_);
                state_machine_.on_login_failed();
            }
        });
}

void AccountSession::req_settlement_confirm() {
    CThostFtdcSettlementInfoConfirmField f{};
    copy_string(f.BrokerID, broker_id_.c_str(), true);
    copy_string(f.InvestorID, user_id_.c_str(), true);
    int ret = api_->ReqSettlementInfoConfirm(&f, ++request_id_);
    if (ret != 0) {
        SPDLOG_ERROR("td req settlement confirm failed | account={} ret={}", account_id_, ret);
        return;
    }
    state_machine_.on_req_settlement_confirm();
}

void AccountSession::req_qry_instrument() {
    // C2: 查询全部合约, 填充 instrument_exchange_map_ (设计 §7.2)
    CThostFtdcQryInstrumentField qry{};
    // InstrumentID 留空: 查询所有合约
    int ret = api_->ReqQryInstrument(&qry, ++request_id_);
    if (ret != 0) {
        if (ret == -3) {
            // I3: 流控 (-3), 1.5s 后重试 (参考 mdctp 流控队列模式)
            SPDLOG_WARN("td qry instrument flow control, retry in 1.5s | account={}", account_id_);
            uint64_t gen = generation_;
            timer_queue_.schedule_after(std::chrono::milliseconds(1500),
                [this, gen]() {
                    if (gen != generation_) return;
                    if (state_machine_.state() == TdState::LoadingInstruments) {
                        req_qry_instrument();
                    }
                });
            return;
        }
        // C2: 非 -3 错误, 调 on_instruments_load_failed 回退到 LoggedIn (设计 §2.4.1)
        SPDLOG_ERROR("td req qry instrument failed | account={} ret={}", account_id_, ret);
        state_machine_.on_instruments_load_failed(
            std::format("ReqQryInstrument ret={}", ret));
        return;
    }
    // I2: 排定 5 分钟超时, 防止 CTP 长期不回 is_last 导致卡死 (设计 §2.4.1)
    cancel_instruments_load_timer();
    uint64_t gen = generation_;
    instruments_load_timer_id_ = timer_queue_.schedule_after(
        std::chrono::minutes(5),
        [this, gen]() {
            if (gen != generation_) return;
            if (state_machine_.state() == TdState::LoadingInstruments) {
                SPDLOG_ERROR("td instruments load timeout | account={}", account_id_);
                state_machine_.on_instruments_load_failed("query timeout 5min");
            }
        });
}

void AccountSession::cancel_connect_timer() {
    if (connect_timer_id_ != 0) {
        timer_queue_.cancel(connect_timer_id_);
        connect_timer_id_ = 0;
    }
}

void AccountSession::cancel_login_timer() {
    if (login_timer_id_ != 0) {
        timer_queue_.cancel(login_timer_id_);
        login_timer_id_ = 0;
    }
}

void AccountSession::cancel_instruments_load_timer() {
    if (instruments_load_timer_id_ != 0) {
        timer_queue_.cancel(instruments_load_timer_id_);
        instruments_load_timer_id_ = 0;
    }
}

// ============================================================================
// 内部辅助: SHM 推送 + 持久化
// ============================================================================

void AccountSession::write_order_rpt(const DzOrderReport& rpt) {
    // DZ_FRAME_ORDER_REPORT (2000), payload=DzOrderReport 通用字段
    platform::write_struct(event_writer_, DZ_FRAME_ORDER_REPORT, rpt);
}

void AccountSession::write_trade_rpt(const DzTradeReport& rpt) {
    // DZ_FRAME_TRADE_REPORT (2001), payload=DzTradeReport 通用字段
    platform::write_struct(event_writer_, DZ_FRAME_TRADE_REPORT, rpt);
}

void AccountSession::reject_order(const DzOrderReq& req, const std::string& reason) {
    // C3: 推 REJECTED 回报让策略进程感知, 避免静默丢单 (设计 §11.1)
    // 异常不传播, 仅记日志 (符合 "宁肯乱码也不能崩溃")
    try {
        DzOrderReport rpt{};
        rpt.order_id = req.order_id;
        copy_string(rpt.strategy_id, req.strategy_id, true);
        copy_string(rpt.instrument_id, req.instrument_id, true);
        copy_string(rpt.account_id, req.account_id, true);
        rpt.direction = req.direction;
        rpt.price_type = req.price_type;
        rpt.position_effect = req.position_effect;
        rpt.status = DZ_ORDER_REJECTED;
        rpt.price = req.price;
        rpt.volume = req.volume;
        rpt.volume_traded = 0;
        rpt.date = trading_day_;
        rpt.time = 0;  // 拒单时间未定义, 留 0 (策略进程可按接收时刻处理)
        // exchange_id 从映射表查 (place_order 已校验存在, 这里兜底防异常)
        auto it = instrument_exchange_map_.find(req.instrument_id);
        if (it != instrument_exchange_map_.end()) {
            copy_string(rpt.exchange_id, it->second.c_str(), true);
        }
        copy_string(rpt.remark, reason.c_str(), true);

        write_order_rpt(rpt);

        // 持久化 OrderRecord (含 CTP 扩展字段, 留 0/空)
        OrderRecord rec{};
        rec.base = rpt;
        rec.is_external = 0;
        rec.volume_canceled = req.volume;
        rec.error_id = -1;  // 本地拒绝, 用 -1 区分 CTP 错误码
        copy_string(rec.error_msg, reason.c_str(), true);
        if (trading_day_ > 0) {
            dztrader::Date d{trading_day_};
            auto* end = std::format_to_n(rec.trading_day, sizeof(rec.trading_day) - 1,
                                         "{:04d}{:02d}{:02d}",
                                         d.year(), d.month(), d.day()).out;
            *end = '\0';
        }
        persist_order(rec);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td reject_order failed | account={} order_id={} error=\"{}\"",
                     account_id_, req.order_id, e.what());
    }
}

void AccountSession::write_risk_reject(const std::string& account_id,
                                       const std::string& rule_name,
                                       const std::string& reason) {
    // C3: 推 DZ_FRAME_TD_RISK_REJECT (2008), payload=DzRiskReject
    // 异常不传播 (设计 §8.2)
    try {
        DzRiskReject field{};
        copy_string(field.account_id, account_id.c_str(), true);
        copy_string(field.rule_name, rule_name.c_str(), true);
        copy_string(field.reason, reason.c_str(), true);
        field.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        platform::write_struct(event_writer_, DZ_FRAME_TD_RISK_REJECT, field);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td write_risk_reject failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void AccountSession::persist_order(const OrderRecord& r) {
    persist_writer_.enqueue(PersistTask{PersistTask::Kind::Order, r});
}

void AccountSession::persist_trade(const TradeRecord& r) {
    persist_writer_.enqueue(PersistTask{PersistTask::Kind::Trade, r});
}

// ============================================================================
// 内部辅助: 缓冲回报 (设计 §5.3)
// ============================================================================

void AccountSession::buffer_order_rpt(const OnRtnOrderField& f) {
    if (buffered_orders_.size() >= kMaxBuffered) {
        buffered_orders_.pop_front();
        SPDLOG_ERROR("td buffer overflow, drop oldest order | account={}", account_id_);
    }
    buffered_orders_.push_back(f);
}

void AccountSession::buffer_trade_rpt(const OnRtnTradeField& f) {
    if (buffered_trades_.size() >= kMaxBuffered) {
        buffered_trades_.pop_front();
        SPDLOG_ERROR("td buffer overflow, drop oldest trade | account={}", account_id_);
    }
    buffered_trades_.push_back(f);
}

void AccountSession::replay_buffered_reports() {
    if (buffered_orders_.empty() && buffered_trades_.empty()) return;
    SPDLOG_INFO("td replay buffered | account={} orders={} trades={}",
                account_id_, buffered_orders_.size(), buffered_trades_.size());
    while (!buffered_orders_.empty()) {
        on_rtn_order(buffered_orders_.front());
        buffered_orders_.pop_front();
    }
    while (!buffered_trades_.empty()) {
        on_rtn_trade(buffered_trades_.front());
        buffered_trades_.pop_front();
    }
}

// ============================================================================
// C5: 补齐缺失的 SPI 事件处理
// 设计原则: "宁肯乱码也不能崩溃", 所有 handler 异常不传播, 仅记日志
// ============================================================================

// === on_rsp_qry_order: 委托查询响应 (RESTART 崩溃恢复补登, 设计 §5.6) ===
// 仅记日志, 不修改 OrderRefMap (避免覆盖活跃订单). is_last 时记 INFO.
void AccountSession::on_rsp_qry_order(const OnRspQryOrderField& f) {
    try {
        if (f.order) {
            SPDLOG_DEBUG("td qry order | account={} instrument={} order_ref={} status={} is_last={}",
                         account_id_, f.order->InstrumentID, f.order->OrderRef,
                         magic_enum::enum_name(STATUS_CTP2VT(f.order->OrderStatus)), f.is_last);
        }
        if (f.is_last) {
            SPDLOG_INFO("td qry order done | account={}", account_id_);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_order failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_trading_account: 资金查询响应 ===
// 用 to_dz_trading_account 转换, 仅记日志 (无 SHM 帧类型)
void AccountSession::on_rsp_qry_trading_account(const OnRspQryTradingAccountField& f) {
    try {
        if (f.trading_account) {
            DzTradingAccount acct = to_dz_trading_account(*f.trading_account, account_id_, trading_day_);
            SPDLOG_INFO("td qry trading account | account={} balance={} available={}",
                        account_id_, acct.balance, acct.available);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry trading account error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_trading_account failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_investor_position: 持仓查询响应 ===
// 仅记日志 (PositionHolding 重建待查询链路落地后实现)
void AccountSession::on_rsp_qry_investor_position(const OnRspQryInvestorPositionField& f) {
    try {
        if (f.investor_position) {
            SPDLOG_DEBUG("td qry position | account={} instrument={} position={} long_frozen={} short_frozen={}",
                         account_id_, f.investor_position->InstrumentID,
                         f.investor_position->Position, f.investor_position->LongFrozen,
                         f.investor_position->ShortFrozen);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_investor_position failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_investor_position_detail: 持仓明细查询响应 ===
// TODO: PositionHolding 重建待查询链路落地后实现, 当前仅记日志
void AccountSession::on_rsp_qry_investor_position_detail(const OnRspQryInvestorPositionDetailField& f) {
    try {
        if (f.investor_position_detail) {
            SPDLOG_DEBUG("td qry position detail | account={} instrument={} direction={} volume={}",
                         account_id_, f.investor_position_detail->InstrumentID,
                         f.investor_position_detail->Direction,
                         f.investor_position_detail->Volume);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_investor_position_detail failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_instrument_margin_rate: 保证金率查询响应 ===
// 转 DzMarginRate (内联转换), 推 SHM + 持久化
void AccountSession::on_rsp_qry_instrument_margin_rate(const OnRspQryInstrumentMarginRateField& f) {
    try {
        if (f.margin_rate && (!f.rsp_info || f.rsp_info->ErrorID == 0)) {
            DzMarginRate rec{};
            copy_string(rec.account_id, account_id_.c_str(), true);
            copy_string(rec.instrument_id, f.margin_rate->InstrumentID, true);
            std::string product = normalize_to_product(f.margin_rate->InstrumentID);
            copy_string(rec.product_code, product.c_str(), true);
            copy_string(rec.exchange_id, f.margin_rate->ExchangeID, true);
            rec.hedge_flag = static_cast<int8_t>(f.margin_rate->HedgeFlag);
            rec.is_relative = static_cast<int8_t>(f.margin_rate->IsRelative);
            rec.long_margin_ratio_by_money = f.margin_rate->LongMarginRatioByMoney;
            rec.long_margin_ratio_by_volume = f.margin_rate->LongMarginRatioByVolume;
            rec.short_margin_ratio_by_money = f.margin_rate->ShortMarginRatioByMoney;
            rec.short_margin_ratio_by_volume = f.margin_rate->ShortMarginRatioByVolume;
            rec.date = trading_day_;

            platform::write_struct(event_writer_, DZ_FRAME_TD_MARGIN_RATE, rec);
            persist_writer_.enqueue(PersistTask{PersistTask::Kind::MarginRate, rec});

            SPDLOG_INFO("td qry margin rate | account={} instrument={} long={} short={}",
                        account_id_, f.margin_rate->InstrumentID,
                        rec.long_margin_ratio_by_money, rec.short_margin_ratio_by_money);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry margin rate error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_instrument_margin_rate failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_qry_instrument_commission_rate: 手续费率查询响应 ===
// 转 DzCommissionRate (内联转换), 推 SHM + 持久化
void AccountSession::on_rsp_qry_instrument_commission_rate(const OnRspQryInstrumentCommissionRateField& f) {
    try {
        if (f.commission_rate && (!f.rsp_info || f.rsp_info->ErrorID == 0)) {
            DzCommissionRate rec{};
            copy_string(rec.account_id, account_id_.c_str(), true);
            copy_string(rec.instrument_id, f.commission_rate->InstrumentID, true);
            std::string product = normalize_to_product(f.commission_rate->InstrumentID);
            copy_string(rec.product_code, product.c_str(), true);
            copy_string(rec.exchange_id, f.commission_rate->ExchangeID, true);
            rec.open_ratio_by_money = f.commission_rate->OpenRatioByMoney;
            rec.open_ratio_by_volume = f.commission_rate->OpenRatioByVolume;
            rec.close_ratio_by_money = f.commission_rate->CloseRatioByMoney;
            rec.close_ratio_by_volume = f.commission_rate->CloseRatioByVolume;
            rec.close_today_ratio_by_money = f.commission_rate->CloseTodayRatioByMoney;
            rec.close_today_ratio_by_volume = f.commission_rate->CloseTodayRatioByVolume;
            rec.date = trading_day_;

            platform::write_struct(event_writer_, DZ_FRAME_TD_COMMISSION_RATE, rec);
            persist_writer_.enqueue(PersistTask{PersistTask::Kind::CommissionRate, rec});

            SPDLOG_INFO("td qry commission rate | account={} instrument={} open_money={} close_money={}",
                        account_id_, f.commission_rate->InstrumentID,
                        rec.open_ratio_by_money, rec.close_ratio_by_money);
        } else if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            SPDLOG_ERROR("td qry commission rate error | account={} error_id={} error=\"{}\"",
                         account_id_, f.rsp_info->ErrorID,
                         dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg));
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_qry_instrument_commission_rate failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rtn_instrument_status: 合约交易状态回报 ===
// 转 DzInstrumentStatus (内联转换), 推 SHM
void AccountSession::on_rtn_instrument_status(const OnRtnInstrumentStatusField& f) {
    try {
        DzInstrumentStatus rec{};
        copy_string(rec.instrument_id, f.instrument_status.InstrumentID, true);
        copy_string(rec.exchange_id, f.instrument_status.ExchangeID, true);
        rec.status = static_cast<int8_t>(f.instrument_status.InstrumentStatus);
        rec.time = parse_ctp_time(f.instrument_status.EnterTime);

        platform::write_struct(event_writer_, DZ_FRAME_TD_INSTRUMENT_STATUS, rec);

        SPDLOG_DEBUG("td rtn instrument status | account={} instrument={} status={}",
                     account_id_, f.instrument_status.InstrumentID,
                     f.instrument_status.InstrumentStatus);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rtn_instrument_status failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_order_insert: 报单录入响应 (CTP 同步拒单, 设计 §11.1) ===
// rsp_info 有错误时, 查 OrderRefMap 找 order_id, 构建 REJECTED OrderRecord 推 SHM + 持久化
void AccountSession::on_rsp_order_insert(const OnRspOrderInsertField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID == 0) {
            // 无错误 (罕见), 仅记 DEBUG
            if (f.input_order) {
                SPDLOG_DEBUG("td rsp order insert (no error) | account={} instrument={} order_ref={}",
                             account_id_, f.input_order->InstrumentID, f.input_order->OrderRef);
            }
            return;
        }

        // I8: rsp_info 缺失视为失败 (与 on_rsp_authenticate / on_rsp_user_login 一致)
        int error_id = f.rsp_info ? f.rsp_info->ErrorID : -1;
        std::string error_msg = f.rsp_info
            ? dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg)
            : "rsp_info is null";

        if (!f.input_order) {
            SPDLOG_ERROR("td rsp order insert error, no input_order | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            return;
        }

        // 查 OrderRefMap 找 order_id
        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.input_order->OrderRef);
        if (local == nullptr) {
            SPDLOG_WARN("td rsp order insert error, order_ref not found | account={} order_ref={} error_id={} error=\"{}\"",
                        account_id_, f.input_order->OrderRef, error_id, error_msg);
            return;
        }

        // 内联构建 REJECTED OrderRecord
        OrderRecord rec{};
        rec.base.order_id = *local;
        // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
        if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
            copy_string(rec.base.strategy_id, sid->c_str(), true);
        }
        copy_string(rec.base.instrument_id, f.input_order->InstrumentID, true);
        copy_string(rec.base.account_id, account_id_.c_str(), true);
        copy_string(rec.base.exchange_id, f.input_order->ExchangeID, true);
        rec.base.direction = (f.input_order->Direction == THOST_FTDC_D_Buy) ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;
        switch (f.input_order->CombOffsetFlag[0]) {
            case THOST_FTDC_OF_Open:           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
            case THOST_FTDC_OF_Close:          rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE;          break;
            case THOST_FTDC_OF_CloseToday:     rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;    break;
            case THOST_FTDC_OF_CloseYesterday: rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;  break;
            default:                           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
        }
        switch (f.input_order->OrderPriceType) {
            case THOST_FTDC_OPT_AnyPrice:   rec.base.price_type = DZ_PRICE_MARKET; break;
            case THOST_FTDC_OPT_LimitPrice: rec.base.price_type = DZ_PRICE_LIMIT;  break;
            default:                        rec.base.price_type = DZ_PRICE_LIMIT;  break;
        }
        rec.base.status = DZ_ORDER_REJECTED;
        rec.base.price = f.input_order->LimitPrice;
        rec.base.volume = f.input_order->VolumeTotalOriginal;
        rec.base.volume_traded = 0;
        rec.base.date = trading_day_;
        rec.base.time = 0;
        copy_string(rec.base.remark, error_msg.c_str(), true);

        copy_string(rec.order_ref, f.input_order->OrderRef, true);
        rec.is_external = 0;
        rec.volume_canceled = f.input_order->VolumeTotalOriginal;
        rec.insert_time = 0;
        rec.update_time = 0;
        rec.error_id = error_id;
        copy_string(rec.error_msg, error_msg.c_str(), true);
        if (trading_day_ > 0) {
            dztrader::Date d{trading_day_};
            auto* end = std::format_to_n(rec.trading_day, sizeof(rec.trading_day) - 1,
                                         "{:04d}{:02d}{:02d}",
                                         d.year(), d.month(), d.day()).out;
            *end = '\0';
        }

        write_order_rpt(rec.base);
        persist_order(rec);

        SPDLOG_ERROR("td rsp order insert rejected | account={} order_id={} order_ref={} error_id={} error=\"{}\"",
                     account_id_, *local, f.input_order->OrderRef, error_id, error_msg);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_order_insert failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_order_action: 报单操作响应 (撤单同步拒绝) ===
// 撤单失败不影响订单状态 (CTP 会通过 OnRtnOrder 推 CANCELLED), 仅记 WARN
void AccountSession::on_rsp_order_action(const OnRspOrderActionField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            SPDLOG_WARN("td rsp order action error | account={} error_id={} error=\"{}\" order_ref={}",
                        account_id_, f.rsp_info->ErrorID, error_msg,
                        f.input_order_action ? f.input_order_action->OrderRef : "");
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_order_action failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_err_rtn_order_insert: 报单录入错误回报 (交易所拒单, 设计 §11.1) ===
// 类似 on_rsp_order_insert, 但 f.input_order 可能 null
void AccountSession::on_err_rtn_order_insert(const OnErrRtnOrderInsertField& f) {
    try {
        if (!f.rsp_info || f.rsp_info->ErrorID == 0) {
            return;  // 无错误, 忽略
        }

        int error_id = f.rsp_info->ErrorID;
        std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);

        if (!f.input_order) {
            SPDLOG_ERROR("td err rtn order insert, no input_order | account={} error_id={} error=\"{}\"",
                         account_id_, error_id, error_msg);
            return;
        }

        const DzOrderId* local = order_ref_map_.find_by_order_ref(f.input_order->OrderRef);
        if (local == nullptr) {
            SPDLOG_WARN("td err rtn order insert, order_ref not found | account={} order_ref={} error_id={} error=\"{}\"",
                        account_id_, f.input_order->OrderRef, error_id, error_msg);
            return;
        }

        // 内联构建 REJECTED OrderRecord (与 on_rsp_order_insert 一致)
        OrderRecord rec{};
        rec.base.order_id = *local;
        // 本地单: 回填 strategy_id (策略 SDK 按 strategy_id 定向过滤回报)
        if (const std::string* sid = order_ref_map_.find_strategy(*local)) {
            copy_string(rec.base.strategy_id, sid->c_str(), true);
        }
        copy_string(rec.base.instrument_id, f.input_order->InstrumentID, true);
        copy_string(rec.base.account_id, account_id_.c_str(), true);
        copy_string(rec.base.exchange_id, f.input_order->ExchangeID, true);
        rec.base.direction = (f.input_order->Direction == THOST_FTDC_D_Buy) ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;
        switch (f.input_order->CombOffsetFlag[0]) {
            case THOST_FTDC_OF_Open:           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
            case THOST_FTDC_OF_Close:          rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE;          break;
            case THOST_FTDC_OF_CloseToday:     rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_TODAY;    break;
            case THOST_FTDC_OF_CloseYesterday: rec.base.position_effect = DZ_POSITION_EFFECT_CLOSE_YESTDAY;  break;
            default:                           rec.base.position_effect = DZ_POSITION_EFFECT_OPEN;           break;
        }
        switch (f.input_order->OrderPriceType) {
            case THOST_FTDC_OPT_AnyPrice:   rec.base.price_type = DZ_PRICE_MARKET; break;
            case THOST_FTDC_OPT_LimitPrice: rec.base.price_type = DZ_PRICE_LIMIT;  break;
            default:                        rec.base.price_type = DZ_PRICE_LIMIT;  break;
        }
        rec.base.status = DZ_ORDER_REJECTED;
        rec.base.price = f.input_order->LimitPrice;
        rec.base.volume = f.input_order->VolumeTotalOriginal;
        rec.base.volume_traded = 0;
        rec.base.date = trading_day_;
        rec.base.time = 0;
        copy_string(rec.base.remark, error_msg.c_str(), true);

        copy_string(rec.order_ref, f.input_order->OrderRef, true);
        rec.is_external = 0;
        rec.volume_canceled = f.input_order->VolumeTotalOriginal;
        rec.insert_time = 0;
        rec.update_time = 0;
        rec.error_id = error_id;
        copy_string(rec.error_msg, error_msg.c_str(), true);
        if (trading_day_ > 0) {
            dztrader::Date d{trading_day_};
            auto* end = std::format_to_n(rec.trading_day, sizeof(rec.trading_day) - 1,
                                         "{:04d}{:02d}{:02d}",
                                         d.year(), d.month(), d.day()).out;
            *end = '\0';
        }

        write_order_rpt(rec.base);
        persist_order(rec);

        SPDLOG_ERROR("td err rtn order insert rejected | account={} order_id={} order_ref={} error_id={} error=\"{}\"",
                     account_id_, *local, f.input_order->OrderRef, error_id, error_msg);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_err_rtn_order_insert failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_err_rtn_order_action: 报单操作错误回报 (撤单被拒) ===
// 同 on_rsp_order_action, 仅记 WARN (撤单被拒不影响订单状态)
void AccountSession::on_err_rtn_order_action(const OnErrRtnOrderActionField& f) {
    try {
        if (f.rsp_info && f.rsp_info->ErrorID != 0) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            SPDLOG_WARN("td err rtn order action | account={} error_id={} error=\"{}\" order_ref={}",
                        account_id_, f.rsp_info->ErrorID, error_msg,
                        f.order_action ? f.order_action->OrderRef : "");
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_err_rtn_order_action failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_transfer: 出入金响应 ===
// 转 DzTransferRsp (内联转换), 推 SHM DZ_FRAME_TD_TRANSFER_RSP
void AccountSession::on_rsp_transfer(const OnRspFromBankToFutureByFutureField& f) {
    try {
        if (!f.req_transfer) {
            SPDLOG_WARN("td rsp transfer, no req_transfer | account={}", account_id_);
            return;
        }

        DzTransferRsp rec{};
        copy_string(rec.account_id, account_id_.c_str(), true);
        copy_string(rec.trade_code, f.req_transfer->TradeCode, true);
        rec.error_id = (f.rsp_info && f.rsp_info->ErrorID != 0) ? f.rsp_info->ErrorID : 0;
        if (f.rsp_info) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            copy_string(rec.error_msg, error_msg.c_str(), true);
        }
        rec.bank_balance = 0;  // ReqTransferField 无银行余额字段
        rec.trade_amount = f.req_transfer->TradeAmount;
        // TransferStatus 是单个 char, 直接赋值到 char[2] (首位 + null 终止)
        rec.transfer_status[0] = f.req_transfer->TransferStatus;
        rec.transfer_status[1] = '\0';
        rec.time = parse_ctp_time(f.req_transfer->TradeTime);

        platform::write_struct(event_writer_, DZ_FRAME_TD_TRANSFER_RSP, rec);

        SPDLOG_INFO("td rsp transfer | account={} trade_code={} amount={} error_id={}",
                    account_id_, f.req_transfer->TradeCode, f.req_transfer->TradeAmount, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_transfer failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rtn_transfer: 出入金实时通知 (银行权威结果) ===
// 转 DzTransferRsp, 推 SHM DZ_FRAME_TD_TRANSFER_RTN
void AccountSession::on_rtn_transfer(const OnRtnFromBankToFutureByFutureField& f) {
    try {
        DzTransferRsp rec{};
        copy_string(rec.account_id, account_id_.c_str(), true);
        copy_string(rec.trade_code, f.rsp_transfer.TradeCode, true);
        rec.error_id = f.rsp_transfer.ErrorID;
        std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_transfer.ErrorMsg);
        copy_string(rec.error_msg, error_msg.c_str(), true);
        rec.bank_balance = 0;  // RspTransferField 无明确银行余额字段
        rec.trade_amount = f.rsp_transfer.TradeAmount;
        // TransferStatus 是单个 char, 直接赋值到 char[2] (首位 + null 终止)
        rec.transfer_status[0] = f.rsp_transfer.TransferStatus;
        rec.transfer_status[1] = '\0';
        rec.time = parse_ctp_time(f.rsp_transfer.TradeTime);

        platform::write_struct(event_writer_, DZ_FRAME_TD_TRANSFER_RTN, rec);

        SPDLOG_INFO("td rtn transfer | account={} trade_code={} amount={} error_id={}",
                    account_id_, f.rsp_transfer.TradeCode, f.rsp_transfer.TradeAmount, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rtn_transfer failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_user_password_update: 修改登录密码响应 ===
// 转 DzPasswordUpdateRsp (password_type='U'), 推 SHM
void AccountSession::on_rsp_user_password_update(const OnRspUserPasswordUpdateField& f) {
    try {
        DzPasswordUpdateRsp rec{};
        copy_string(rec.account_id, account_id_.c_str(), true);
        rec.password_type = 'U';
        rec.error_id = (f.rsp_info && f.rsp_info->ErrorID != 0) ? f.rsp_info->ErrorID : 0;
        if (f.rsp_info) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            copy_string(rec.error_msg, error_msg.c_str(), true);
        }
        rec.time = 0;  // CTP 无时间字段, 留 0

        platform::write_struct(event_writer_, DZ_FRAME_TD_PASSWORD_UPDATE_RSP, rec);

        SPDLOG_INFO("td rsp user password update | account={} error_id={}",
                    account_id_, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_user_password_update failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// === on_rsp_trading_account_password_update: 修改资金密码响应 ===
// 转 DzPasswordUpdateRsp (password_type='A'), 推 SHM
void AccountSession::on_rsp_trading_account_password_update(const OnRspTradingAccountPasswordUpdateField& f) {
    try {
        DzPasswordUpdateRsp rec{};
        copy_string(rec.account_id, account_id_.c_str(), true);
        rec.password_type = 'A';
        rec.error_id = (f.rsp_info && f.rsp_info->ErrorID != 0) ? f.rsp_info->ErrorID : 0;
        if (f.rsp_info) {
            std::string error_msg = dztrader::to_utf8_from_gbk(f.rsp_info->ErrorMsg);
            copy_string(rec.error_msg, error_msg.c_str(), true);
        }
        rec.time = 0;  // CTP 无时间字段, 留 0

        platform::write_struct(event_writer_, DZ_FRAME_TD_PASSWORD_UPDATE_RSP, rec);

        SPDLOG_INFO("td rsp trading account password update | account={} error_id={}",
                    account_id_, rec.error_id);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td on_rsp_trading_account_password_update failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

}  // namespace dztrader::ctp
