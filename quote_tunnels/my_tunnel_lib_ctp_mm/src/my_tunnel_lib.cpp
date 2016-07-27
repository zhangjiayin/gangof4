﻿#include "my_tunnel_lib.h"

#include <string>
#include <mutex>
#include <thread>

#include "qtm_with_code.h"
#include "my_cmn_util_funcs.h"
#include "my_cmn_log.h"

#include "config_data.h"
#include "trade_log_util.h"
#include "ctp_trade_interface.h"
#include "field_convert.h"

#include "my_protocol_packager.h"
#include "check_schedule.h"

using namespace std;
using namespace my_cmn;

static void InitOnce()
{
    static volatile bool s_have_init = false;
    static std::mutex s_init_sync;

    if (s_have_init)
    {
        return;
    }
    else
    {
        lock_guard<mutex> lock(s_init_sync);
        if (s_have_init)
        {
            return;
        }
        std::string log_file_name = "my_tunnel_lib_" + my_cmn::GetCurrentDateTimeString();
        (void) my_log::instance(log_file_name.c_str());
        TNL_LOG_INFO("start init tunnel library.");

        // initialize tunnel monitor
        qtm_init(TYPE_TCA);

        s_have_init = true;
    }
}

enum RespondDataType
{
    kPlaceOrderRsp = 1,
    kCancelOrderRsp = 2,
    kInsertForQuoteRsp = 3,
    kInsertQuoteRsp = 4,
    kCancelQuoteRsp = 5,
};

void CtpTunnel::QueryPositionBeforeClose(const std::string &qry_time)
{
	time_t t;
	struct tm *cur_time;
	char second[3];
	char minute[3];
	char hour[3];
	strcpy(second, qry_time.substr(4, 2).c_str());
	strcpy(minute, qry_time.substr(2, 2).c_str());
	strcpy(hour, qry_time.substr(0, 2).c_str());
	struct tm localtime_result;
	while (1) {
		t = time(NULL);
		cur_time = localtime_r(&t, &localtime_result);
		if (cur_time->tm_sec == atoi(second) && cur_time->tm_min == atoi(minute) && cur_time->tm_hour == atoi(hour)) {
			MyCtpTradeSpi * p_ctp = static_cast<MyCtpTradeSpi *>(trade_inf_);
			p_ctp->StartPositionBackUp();
			sleep(20);
			break;
		} else {
			sleep(1);
		}
	}
}

void CtpTunnel::RespondHandleThread(CtpTunnel *ptunnel)
{
    MyCtpTradeSpi * p_ctp = static_cast<MyCtpTradeSpi *>(ptunnel->trade_inf_);
    if (!p_ctp)
    {
        TNL_LOG_ERROR("tunnel not init in RespondHandleThread.");
        return;
    }

    std::mutex &rsp_sync = p_ctp->rsp_sync;
    std::condition_variable &rsp_con = p_ctp->rsp_con;
    std::vector<std::pair<int, void *> > rsp_tmp;

    while (true)
    {
        std::unique_lock<std::mutex> lock(rsp_sync);
        while (ptunnel->pending_rsp_.empty())
        {
            rsp_con.wait(lock);
        }

        ptunnel->pending_rsp_.swap(rsp_tmp);
        for (std::pair<int, void *> &v : rsp_tmp)
        {
            switch (v.first)
            {
                case RespondDataType::kPlaceOrderRsp:
                    {
                    T_OrderRespond *p = (T_OrderRespond *) v.second;
                    if (ptunnel->OrderRespond_call_back_handler_) ptunnel->OrderRespond_call_back_handler_(p);
                    delete p;
                }
                    break;
                case RespondDataType::kCancelOrderRsp:
                    {
                    T_CancelRespond *p = (T_CancelRespond *) v.second;
                    if (ptunnel->CancelRespond_call_back_handler_) ptunnel->CancelRespond_call_back_handler_(p);
                    delete p;
                }
                    break;
                case RespondDataType::kInsertForQuoteRsp:
                    {
                    T_RspOfReqForQuote *p = (T_RspOfReqForQuote *) v.second;
                    if (ptunnel->RspOfReqForQuoteHandler_) ptunnel->RspOfReqForQuoteHandler_(p);
                    delete p;
                }
                    break;
                case RespondDataType::kInsertQuoteRsp:
                    {
                    T_InsertQuoteRespond *p = (T_InsertQuoteRespond *) v.second;
                    if (ptunnel->InsertQuoteRespondHandler_) ptunnel->InsertQuoteRespondHandler_(p);
                    delete p;
                }
                    break;
                case RespondDataType::kCancelQuoteRsp:
                    {
                    T_CancelQuoteRespond *p = (T_CancelQuoteRespond *) v.second;
                    if (ptunnel->CancelQuoteRespondHandler_) ptunnel->CancelQuoteRespondHandler_(p);
                    delete p;
                }
                    break;
                default:
                    TNL_LOG_ERROR("unknown type of respond message: %d", v.first);
                    break;
            }
        }

        rsp_tmp.clear();
    }
}
void CtpTunnel::SendRespondAsync(int rsp_type, void *rsp)
{
    MyCtpTradeSpi * p_ctp = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_ctp)
    {
        TNL_LOG_ERROR("tunnel not init in RespondHandleThread.");
        return;
    }

    std::mutex &rsp_sync = p_ctp->rsp_sync;
    std::condition_variable &rsp_con = p_ctp->rsp_con;
    {
        std::unique_lock<std::mutex> lock(rsp_sync);
        pending_rsp_.push_back(std::make_pair(rsp_type, rsp));
    }
    rsp_con.notify_one();
}

CtpTunnel::CtpTunnel(const std::string &provider_config_file)
{
    trade_inf_ = NULL;
    exchange_code_ = ' ';
    InitOnce();

    std::string cfg_file("my_trade_tunnel_ctp.xml");
    if (!provider_config_file.empty())
    {
        cfg_file = provider_config_file;
    }

    TNL_LOG_INFO("create CtpTunnel object with configure file: %s", cfg_file.c_str());

    //TunnelConfigData cfg;
    lib_cfg_ = new TunnelConfigData();
    lib_cfg_->Load(cfg_file);
    exchange_code_ = lib_cfg_->Logon_config().exch_code.c_str()[0];
    tunnel_info_.account = lib_cfg_->Logon_config().investorid;

    char qtm_tmp_name[32];
    memset(qtm_tmp_name, 0, sizeof(qtm_tmp_name));
    sprintf(qtm_tmp_name, "ctp_%s_%d_%u", tunnel_info_.account.c_str(), lib_cfg_->App_cfg().orderref_prefix_id, getpid());
    tunnel_info_.qtm_name = qtm_tmp_name;
    TunnelUpdateState(tunnel_info_.qtm_name.c_str(), QtmState::INIT);

    // init tunnel log object
    LogUtil::Start("my_tunnel_lib_ctp", lib_cfg_->App_cfg().share_memory_key);

    int provider_type = lib_cfg_->App_cfg().provider_type;
    if (provider_type == TunnelProviderType::PROVIDERTYPE_CTP)
    {
        InitInf(*lib_cfg_);
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel provider type, please check config file.");
    }

    query_pos_time_ = lib_cfg_->Initial_policy_param().time_to_query_pos;
    if (query_pos_time_.size() == 6) {
    	std::thread qry_pos(std::bind(&CtpTunnel::QueryPositionBeforeClose, this, query_pos_time_));
    	qry_pos.detach();
    }

    // init respond thread
    std::thread rsp_thread(&CtpTunnel::RespondHandleThread, this);
    rsp_thread.detach();
}

std::string CtpTunnel::GetClientID()
{
    return tunnel_info_.account;
}

bool CtpTunnel::InitInf(const TunnelConfigData &cfg)
{
    // 连接服务
    TNL_LOG_INFO("prepare to start CTP tunnel server.");

    const ComplianceCheckParam &param = cfg.Compliance_check_param();
    ComplianceCheck_Init(
        tunnel_info_.account.c_str(),
        param.cancel_warn_threshold,
        param.cancel_upper_limit,
        param.max_open_of_speculate,
        param.max_open_of_arbitrage,
        param.max_open_of_total,
        param.switch_mask.c_str());

    char init_msg[127];
    sprintf(init_msg, "%s: Init compliance check", tunnel_info_.account.c_str());
    update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::OPEN_ORDER_LIMIT, QtmComplianceState::INIT_COMPLIANCE_CHECK, init_msg);
    update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT, QtmComplianceState::INIT_COMPLIANCE_CHECK, init_msg);
    trade_inf_ = new MyCtpTradeSpi(cfg);
    return true;
}

void CtpTunnel::PlaceOrder(const T_PlaceOrder *pPlaceOrder)
{
    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnPlaceOrder(pPlaceOrder, tunnel_info_);
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);

    OrderRefDataType return_param;
    if (p_tunnel)
    {
        // 获取OrderRef和往柜台报单，需要串行化处理，以免先获取OrderRef的线程，后报单到柜台，造成报单重复错误
        lock_guard<mutex> lock(p_tunnel->client_sync);

        OrderRefDataType order_ref = p_tunnel->ctp_trade_context_.GetNewOrderRef();
        TNL_LOG_DEBUG("serial_no: %ld map order_ref: %lld", pPlaceOrder->serial_no, order_ref);
        int request_id = p_tunnel->ctp_trade_context_.GetRequestID();

        process_result = ComplianceCheck_TryReqOrderInsert(
            tunnel_info_.account.c_str(),
            order_ref,
            exchange_code_,
            pPlaceOrder->stock_code,
            pPlaceOrder->volume,
            pPlaceOrder->limit_price,
            pPlaceOrder->order_kind,
            pPlaceOrder->speculator,
            pPlaceOrder->direction,
            pPlaceOrder->open_close,
            pPlaceOrder->order_type,
            &return_param);

        if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS &&
            process_result != TUNNEL_ERR_CODE::OPEN_EQUAL_LIMIT &&
            process_result != TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT)
        {
            // 日志
            if (process_result == TUNNEL_ERR_CODE::CFFEX_EXCEED_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::OPEN_ORDER_LIMIT,
                    QtmComplianceState::OPEN_POSITIONS_EXCEED_LIMITS,
                    GetComplianceDescriptionWithState(QtmComplianceState::OPEN_POSITIONS_EXCEED_LIMITS, tunnel_info_.account.c_str(),
                        pPlaceOrder->stock_code).c_str());
                TNL_LOG_WARN("forbid open because current open volumn: %lld", return_param);
            }
            else if (process_result == TUNNEL_ERR_CODE::POSSIBLE_SELF_TRADE)
            {
                TNL_LOG_WARN("possible trade with order: %lld (order ref)", return_param);
            }
            else if (process_result == TUNNEL_ERR_CODE::CANCEL_TIMES_REACH_WARN_THRETHOLD)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::CANCEL_TIME_OVER_WARNING_THRESHOLD,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_OVER_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                        pPlaceOrder->stock_code).c_str());
                TNL_LOG_WARN("reach the warn threthold of cancel time, forbit open new position.");
            }
        }
        else
        {
            if (process_result == TUNNEL_ERR_CODE::OPEN_EQUAL_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::OPEN_POSITIONS_EQUAL_LIMITS,
                    GetComplianceDescriptionWithState(QtmComplianceState::OPEN_POSITIONS_EQUAL_LIMITS, tunnel_info_.account.c_str(),
                        pPlaceOrder->stock_code).c_str());
                TNL_LOG_WARN("equal the warn threthold of open position.");
            }
            else if (process_result == TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                        pPlaceOrder->stock_code).c_str());
                TNL_LOG_WARN("equal the warn threthold of cancel time.");
            }
            CThostFtdcInputOrderField counter_req;
            CTPPacker::OrderRequest(*lib_cfg_, pPlaceOrder, order_ref, counter_req);
            p_tunnel->ctp_trade_context_.SaveSerialNoToOrderRef(order_ref,
                OriginalReqInfo(pPlaceOrder->serial_no,
                    p_tunnel->Front_id(), p_tunnel->Session_id(),
                    exchange_code_, pPlaceOrder->direction, pPlaceOrder->speculator, pPlaceOrder->open_close,
                    pPlaceOrder->order_type,
                    pPlaceOrder->volume, pPlaceOrder->order_kind, pPlaceOrder->stock_code, ""));
            process_result = p_tunnel->ReqOrderInsert(&counter_req, request_id);

            // 下单
            // 发送失败，即时回滚
            if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
            {
                ComplianceCheck_OnOrderInsertFailed(
                    tunnel_info_.account.c_str(),
                    order_ref,
                    exchange_code_,
                    pPlaceOrder->stock_code,
                    pPlaceOrder->volume,
                    pPlaceOrder->speculator,
                    pPlaceOrder->open_close,
                    pPlaceOrder->order_type);
            }
        }
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel, check configure file");
    }

    // 请求失败后即时回报，否则，等柜台回报
    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
    {
        TNL_LOG_WARN("place order failed, result=%d", process_result);

        // 应答包
        T_OrderRespond *pOrderRespond = new T_OrderRespond();
        CTPPacker::OrderRespond(process_result, pPlaceOrder->serial_no, 0, 0, *pOrderRespond);
        LogUtil::OnOrderRespond(pOrderRespond, tunnel_info_);

        SendRespondAsync(RespondDataType::kPlaceOrderRsp, pOrderRespond);
    }
}

void CtpTunnel::CancelOrder(const T_CancelOrder *pCancelOrder)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);

    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnCancelOrder(pCancelOrder, tunnel_info_);

    if (p_tunnel)
    {
        // 获取OrderRef和往柜台报单，需要串行化处理，以免先获取OrderRef的线程，后报单到柜台，造成报单重复错误
        lock_guard<mutex> lock(p_tunnel->client_sync);

        OrderRefDataType order_ref = p_tunnel->ctp_trade_context_.GetNewOrderRef();
        TNL_LOG_INFO("cancel_serial_no: %ld map action_ref: %lld", pCancelOrder->serial_no, order_ref);
        int request_id = p_tunnel->ctp_trade_context_.GetRequestID();
        OrderRefDataType org_order_ref = p_tunnel->ctp_trade_context_.GetOrderRefBySerialNo(pCancelOrder->org_serial_no);
        const OriginalReqInfo * org_order_info = p_tunnel->ctp_trade_context_.GetOrderInfoByOrderRef(org_order_ref);

        // not find valid place order in this tunnel
        if (org_order_info)
        {
            process_result = ComplianceCheck_TryReqOrderAction(
                tunnel_info_.account.c_str(),
                pCancelOrder->stock_code,
                org_order_ref);

            if (process_result == TUNNEL_ERR_CODE::CANCEL_TIMES_REACH_WARN_THRETHOLD)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::CANCEL_TIME_OVER_WARNING_THRESHOLD,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_OVER_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                        pCancelOrder->stock_code).c_str());
                TNL_LOG_WARN("cancel time approaches threshold");
            }
            if (process_result == TUNNEL_ERR_CODE::CANCEL_REACH_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::CANCEL_TIME_OVER_MAXIMUN,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_OVER_MAXIMUN, tunnel_info_.account.c_str(),
                        pCancelOrder->stock_code).c_str());
                TNL_LOG_WARN("cancel time reach limitation");
            }
            if (process_result == TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                        pCancelOrder->stock_code).c_str());
                TNL_LOG_WARN("cancel time equal threshold");
            }

            if ((process_result == TUNNEL_ERR_CODE::RESULT_SUCCESS)
                || (process_result == TUNNEL_ERR_CODE::CANCEL_TIMES_REACH_WARN_THRETHOLD)
                || (process_result == TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT))
            {

                CThostFtdcInputOrderActionField counter_req;
                CTPPacker::CancelRequest(*lib_cfg_, pCancelOrder, order_ref, org_order_ref, org_order_info, counter_req);
                char cancel_sn[64];
                sprintf(cancel_sn, "%ld", pCancelOrder->serial_no);
                p_tunnel->ctp_trade_context_.UpdateCancelOrderRef(org_order_ref, cancel_sn);

                process_result = p_tunnel->ReqOrderAction(&counter_req, request_id);

                if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
                {
                    ComplianceCheck_OnOrderCancelFailed(
                        tunnel_info_.account.c_str(),
                        pCancelOrder->stock_code,
                        org_order_ref);
                }
                else
                {
                    ComplianceCheck_OnOrderPendingCancel(
                        tunnel_info_.account.c_str(),
                        org_order_ref);
                }
            }
        }
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel when cancel order, please check configure file");
    }

    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS
        && process_result != TUNNEL_ERR_CODE::CANCEL_TIMES_REACH_WARN_THRETHOLD
        && process_result != TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT)
    {
        // 应答包
        T_CancelRespond *cancle_order = new T_CancelRespond();
        CTPPacker::CancelRespond(process_result, pCancelOrder->serial_no, 0L, *cancle_order);
        LogUtil::OnCancelRespond(cancle_order, tunnel_info_);

        SendRespondAsync(RespondDataType::kCancelOrderRsp, cancle_order);
    }
}

void CtpTunnel::QueryPosition(const T_QryPosition *pQryParam)
{
    LogUtil::OnQryPosition(pQryParam, tunnel_info_);
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    T_PositionReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query position, please check configure file");

        ret.error_no = 2;
        if (QryPosReturnHandler_) QryPosReturnHandler_(&ret);
        LogUtil::OnPositionRtn(&ret, tunnel_info_);
        return;
    }

    CThostFtdcQryInvestorPositionDetailField qry_param;
    memset(&qry_param, 0, sizeof(CThostFtdcQryInvestorPositionDetailField));
    ///经纪公司编号
    strncpy(qry_param.BrokerID, lib_cfg_->Logon_config().brokerid.c_str(), sizeof(TThostFtdcBrokerIDType));
    ///投资者编号
    strncpy(qry_param.InvestorID, tunnel_info_.account.c_str(), sizeof(TThostFtdcInvestorIDType));

    int qry_result = p_tunnel->QryPosition(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryPosReturnHandler_) QryPosReturnHandler_(&ret);
        LogUtil::OnPositionRtn(&ret, tunnel_info_);
    }
}
void CtpTunnel::QueryOrderDetail(const T_QryOrderDetail *pQryParam)
{
    LogUtil::OnQryOrderDetail(pQryParam, tunnel_info_);
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    T_OrderDetailReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query order detail, please check configure file");

        ret.error_no = 2;
        if (QryOrderDetailReturnHandler_) QryOrderDetailReturnHandler_(&ret);
        LogUtil::OnOrderDetailRtn(&ret, tunnel_info_);
        return;
    }

    CThostFtdcQryOrderField qry_param;
    memset(&qry_param, 0, sizeof(CThostFtdcQryOrderField));
    ///经纪公司编号
    strncpy(qry_param.BrokerID, lib_cfg_->Logon_config().brokerid.c_str(), sizeof(TThostFtdcBrokerIDType));
    ///交易所代码
    strncpy(qry_param.ExchangeID, CTPFieldConvert::ExchCodeToExchName(exchange_code_), sizeof(TThostFtdcExchangeIDType));
    ///投资者编号
    strncpy(qry_param.InvestorID, lib_cfg_->Logon_config().investorid.c_str(), sizeof(TThostFtdcInvestorIDType));

    int qry_result = p_tunnel->QryOrderDetail(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryOrderDetailReturnHandler_) QryOrderDetailReturnHandler_(&ret);
        LogUtil::OnOrderDetailRtn(&ret, tunnel_info_);
    }
}
void CtpTunnel::QueryTradeDetail(const T_QryTradeDetail *pQryParam)
{
    LogUtil::OnQryTradeDetail(pQryParam, tunnel_info_);
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    T_TradeDetailReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query trade detail, please check configure file");

        ret.error_no = 2;
        if (QryTradeDetailReturnHandler_) QryTradeDetailReturnHandler_(&ret);
        LogUtil::OnTradeDetailRtn(&ret, tunnel_info_);
        return;
    }

    CThostFtdcQryTradeField qry_param;
    memset(&qry_param, 0, sizeof(CThostFtdcQryTradeField));
    ///经纪公司编号
    strncpy(qry_param.BrokerID, lib_cfg_->Logon_config().brokerid.c_str(), sizeof(TThostFtdcBrokerIDType));
    ///交易所代码
    strncpy(qry_param.ExchangeID, CTPFieldConvert::ExchCodeToExchName(exchange_code_), sizeof(TThostFtdcExchangeIDType));
    ///投资者编号
    strncpy(qry_param.InvestorID, lib_cfg_->Logon_config().investorid.c_str(), sizeof(TThostFtdcInvestorIDType));
    ///成交编号
    //TUstpFtdcTradeIDType    TradeID;
    ///合约代码
    //strncpy(qry_param.InstrumentID, pQryPosition->stock_code, sizeof(TUstpFtdcInstrumentIDType));

    int qry_result = p_tunnel->QryTradeDetail(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryTradeDetailReturnHandler_) QryTradeDetailReturnHandler_(&ret);
        LogUtil::OnTradeDetailRtn(&ret, tunnel_info_);
    }
}

void CtpTunnel::QueryContractInfo(const T_QryContractInfo *pQryParam)
{
    LogUtil::OnQryContractInfo(pQryParam, tunnel_info_);
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    T_ContractInfoReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query contract info, please check configure file");

        ret.error_no = 2;
        if (QryContractInfoHandler_) QryContractInfoHandler_(&ret);
        LogUtil::OnContractInfoRtn(&ret, tunnel_info_);
        return;
    }

    CThostFtdcQryInstrumentField qry_param;
    memset(&qry_param, 0, sizeof(qry_param));
    const char * ex = CTPFieldConvert::ExchCodeToExchName(exchange_code_);
    memcpy(qry_param.InstrumentID, "", strlen(""));
    memcpy(qry_param.ExchangeID, ex, strlen(ex));
    memcpy(qry_param.ExchangeInstID, "", strlen(""));
    memcpy(qry_param.ProductID, "", strlen(""));

    int qry_result = p_tunnel->QryInstrument(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryContractInfoHandler_) QryContractInfoHandler_(&ret);
        LogUtil::OnContractInfoRtn(&ret, tunnel_info_);
    }
}

//added for mm, on 20141218
void CtpTunnel::ReqForQuoteInsert(const T_ReqForQuote *p)
{
    LogUtil::OnReqForQuote(p, tunnel_info_);

    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);

    if (p_tunnel)
    {
        CThostFtdcInputForQuoteField input_for_quote;
        memset(&input_for_quote, 0, sizeof(input_for_quote));

        OrderRefDataType order_ref = p_tunnel->ctp_trade_context_.GetNewOrderRef();
        int request_id = p_tunnel->ctp_trade_context_.GetRequestID();

        strncpy(input_for_quote.BrokerID, lib_cfg_->Logon_config().brokerid.c_str(), sizeof(input_for_quote.BrokerID));
        strncpy(input_for_quote.InvestorID, lib_cfg_->Logon_config().clientid.c_str(), sizeof(input_for_quote.InvestorID));
        strncpy(input_for_quote.UserID, lib_cfg_->Logon_config().clientid.c_str(), sizeof(input_for_quote.UserID));
        strncpy(input_for_quote.InstrumentID, p->stock_code, sizeof(input_for_quote.InstrumentID));
        snprintf(input_for_quote.ForQuoteRef, sizeof(TThostFtdcOrderRefType), "%lld", order_ref);
        CTPFieldConvert::GetMacAndIPAddress("eth0", input_for_quote.MacAddress, input_for_quote.IPAddress);
        process_result = p_tunnel->ReqForQuoteInsert(&input_for_quote, request_id);

        // insert quote success, save original request infomation
        if (process_result == TUNNEL_ERR_CODE::RESULT_SUCCESS)
        {
            p_tunnel->ctp_trade_context_.SaveForquoteSerialNoOfOrderRef(order_ref, p->serial_no);
        }
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel, check configure file");
    }

    // 请求失败后即时回报，否则，等柜台回报
    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
    {
        TNL_LOG_WARN("insert failed, result=%d", process_result);

        // 应答包
        T_RspOfReqForQuote *rsp = new T_RspOfReqForQuote();
        rsp->serial_no = p->serial_no;
        rsp->error_no = process_result;
        LogUtil::OnRspOfReqForQuote(rsp, tunnel_info_);

        SendRespondAsync(RespondDataType::kInsertForQuoteRsp, rsp);
    }
}

///报价录入请求
void CtpTunnel::ReqQuoteInsert(const T_InsertQuote *p)
{
    LogUtil::OnInsertQuote(p, tunnel_info_);

    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);

    if (p_tunnel)
    {
        // 获取OrderRef和往柜台报单，需要串行化处理，以免先获取OrderRef的线程，后报单到柜台，造成报单重复错误
        lock_guard<mutex> lock(p_tunnel->client_sync);

        OrderRefDataType quote_ref = p_tunnel->ctp_trade_context_.GetNewOrderRefForInsertQuote();
        TNL_LOG_INFO("insert_quote serial_no: %ld map order_ref: %lld", p->serial_no, quote_ref);
        int request_id = p_tunnel->ctp_trade_context_.GetRequestID();

        {
            CThostFtdcInputQuoteField insert_quote_req;
            memset(&insert_quote_req, 0, sizeof(insert_quote_req));

            insert_quote_req.RequestID = request_id;
            char exch_code = lib_cfg_->Logon_config().exch_code.c_str()[0];
            strncpy(insert_quote_req.BrokerID, lib_cfg_->Logon_config().brokerid.c_str(), sizeof(insert_quote_req.BrokerID));
            strncpy(insert_quote_req.InvestorID, lib_cfg_->Logon_config().clientid.c_str(), sizeof(insert_quote_req.InvestorID));
            strncpy(insert_quote_req.UserID, lib_cfg_->Logon_config().clientid.c_str(), sizeof(insert_quote_req.UserID));
            strncpy(insert_quote_req.InstrumentID, p->stock_code, sizeof(insert_quote_req.InstrumentID));
            insert_quote_req.BidOffsetFlag = CTPFieldConvert::GetCTPOCFlag(exch_code, p->buy_open_close);
            insert_quote_req.BidHedgeFlag = CTPFieldConvert::GetCTPHedgeType(p->buy_speculator);
            insert_quote_req.BidPrice = p->buy_limit_price;
            insert_quote_req.BidVolume = p->buy_volume;
            insert_quote_req.AskOffsetFlag = CTPFieldConvert::GetCTPOCFlag(exch_code, p->sell_open_close);
            insert_quote_req.AskHedgeFlag = CTPFieldConvert::GetCTPHedgeType(p->sell_speculator);
            insert_quote_req.AskPrice = p->sell_limit_price;
            insert_quote_req.AskVolume = p->sell_volume;

            snprintf(insert_quote_req.QuoteRef, sizeof(TThostFtdcOrderRefType), "%lld", quote_ref);
            snprintf(insert_quote_req.BidOrderRef, sizeof(TThostFtdcOrderRefType), "%lld", quote_ref + 2);
            snprintf(insert_quote_req.AskOrderRef, sizeof(TThostFtdcOrderRefType), "%lld", quote_ref + 1);

            process_result = p_tunnel->ReqQuoteInsert(&insert_quote_req, request_id);

            // insert quote success, save original request infomation
            if (process_result == TUNNEL_ERR_CODE::RESULT_SUCCESS)
            {
                p_tunnel->ctp_trade_context_.SaveQuoteInfo(quote_ref,
                    CTPQuoteInfo(p->serial_no, p_tunnel->Front_id(), p_tunnel->Session_id()));
            }
        }
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel, check configure file");
    }

    // 请求失败后即时回报，否则，等柜台回报
    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
    {
        TNL_LOG_WARN("insert failed, result=%d", process_result);

        // 应答包
        T_InsertQuoteRespond *rsp = new T_InsertQuoteRespond();
        memset(rsp, 0, sizeof(T_InsertQuoteRespond));
        rsp->entrust_no = 0;
        rsp->entrust_status = 'e';
        rsp->serial_no = p->serial_no;
        rsp->error_no = process_result;

        LogUtil::OnInsertQuoteRespond(rsp, tunnel_info_);

        SendRespondAsync(RespondDataType::kInsertQuoteRsp, rsp);
    }
}
///报价操作请求
void CtpTunnel::ReqQuoteAction(const T_CancelQuote *p)
{
    LogUtil::OnCancelQuote(p, tunnel_info_);

    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);

    if (p_tunnel)
    {
        // 获取OrderRef和往柜台报单，需要串行化处理，以免先获取OrderRef的线程，后报单到柜台，造成报单重复错误
        lock_guard<mutex> lock(p_tunnel->client_sync);

        OrderRefDataType quote_action_ref = p_tunnel->ctp_trade_context_.GetNewOrderRef();
        TNL_LOG_INFO("cancel_serial_no: %ld map quote_action_ref: %lld", p->serial_no, quote_action_ref);
        int request_id = p_tunnel->ctp_trade_context_.GetRequestID();
        OrderRefDataType org_quote_ref = p_tunnel->ctp_trade_context_.GetQuoteRefBySerialNo(p->org_serial_no);
        const CTPQuoteInfo * org_quote_info = p_tunnel->ctp_trade_context_.GetQuoteInfoByOrderRef(org_quote_ref);

        process_result = TUNNEL_ERR_CODE::ORDER_NOT_FOUND;
        if (org_quote_info)
        {
            // create request object
            CThostFtdcInputQuoteActionField req;
            memset(&req, 0, sizeof(req));

            req.RequestID = request_id;
            char exch_code = lib_cfg_->Logon_config().exch_code.c_str()[0];

            strncpy(req.BrokerID, lib_cfg_->Logon_config().brokerid.c_str(), sizeof(TThostFtdcBrokerIDType));
            strncpy(req.InvestorID, lib_cfg_->Logon_config().clientid.c_str(), sizeof(TThostFtdcInvestorIDType));
            strncpy(req.UserID, lib_cfg_->Logon_config().clientid.c_str(), sizeof(TThostFtdcInvestorIDType));
            strncpy(req.InstrumentID, p->stock_code, sizeof(TThostFtdcInstrumentIDType));
            req.QuoteActionRef = quote_action_ref;
            req.ActionFlag = THOST_FTDC_AF_Delete;

            // 原报单引用
            if (org_quote_info)
            {
                snprintf(req.QuoteRef, sizeof(TThostFtdcOrderRefType), "%lld", org_quote_ref);
                req.FrontID = org_quote_info->front_id;
                req.SessionID = org_quote_info->session_id;
            }

            // 原报单交易所标识
            strncpy(req.ExchangeID, CTPFieldConvert::ExchCodeToExchName(exch_code), sizeof(TThostFtdcExchangeIDType));
            CTPFieldConvert::SysOrderIDToCTPFormat(p->entrust_no, req.QuoteSysID);

            process_result = p_tunnel->ReqQuoteAction(&req, request_id);

            if (process_result == TUNNEL_ERR_CODE::RESULT_SUCCESS)
            {
                char cancel_sn[64];
                sprintf(cancel_sn, "%ld", p->serial_no);
                p_tunnel->ctp_trade_context_.UpdateCancelInfoOfQuoteRef(org_quote_ref, cancel_sn);
            }
        }
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel when cancel quote, please check configure file");
    }

    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
    {
        // 应答包
        T_CancelQuoteRespond *cancel_quote_rsp = new T_CancelQuoteRespond();
        memset(cancel_quote_rsp, 0, sizeof(T_CancelQuoteRespond));
        cancel_quote_rsp->entrust_no = p->entrust_no;
        cancel_quote_rsp->serial_no = p->serial_no;
        cancel_quote_rsp->error_no = process_result;
        cancel_quote_rsp->entrust_status = 'e';

        LogUtil::OnCancelQuoteRespond(cancel_quote_rsp, tunnel_info_);

        SendRespondAsync(RespondDataType::kCancelQuoteRsp, cancel_quote_rsp);
    }
}

void CtpTunnel::SetCallbackHandler(std::function<void(const T_OrderRespond *)> callback_handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
    OrderRespond_call_back_handler_ = callback_handler;
}

void CtpTunnel::SetCallbackHandler(std::function<void(const T_CancelRespond *)> callback_handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
    CancelRespond_call_back_handler_ = callback_handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_OrderReturn *)> callback_handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_TradeReturn *)> callback_handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_PositionReturn *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryPosReturnHandler_ = handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_OrderDetailReturn *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryOrderDetailReturnHandler_ = handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_TradeDetailReturn *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryTradeDetailReturnHandler_ = handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_ContractInfoReturn *)> handler)
{
    TNL_LOG_DEBUG("CtpTunnel::SetCallbackHandler - QryContractInfoHandler_");
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryContractInfoHandler_ = handler;
}

// added for market making
void CtpTunnel::SetCallbackHandler(std::function<void(const T_RtnForQuote *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);

    // TODO return of ForQuote, ctp will return from quote interface
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_RspOfReqForQuote *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    RspOfReqForQuoteHandler_ = handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_InsertQuoteRespond *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    InsertQuoteRespondHandler_ = handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_CancelQuoteRespond *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    CancelQuoteRespondHandler_ = handler;
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_QuoteReturn *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
}
void CtpTunnel::SetCallbackHandler(std::function<void(const T_QuoteTrade *)> handler)
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
}

CtpTunnel::~CtpTunnel()
{
    MyCtpTradeSpi * p_tunnel = static_cast<MyCtpTradeSpi *>(trade_inf_);
    if (p_tunnel)
    {
        delete p_tunnel;
        trade_inf_ = NULL;
    }
    qtm_finish();
    LogUtil::Stop();
}

MYTunnelInterface *CreateTradeTunnel(const std::string &tunnel_config_file)
{
    return new CtpTunnel(tunnel_config_file);
}
void DestroyTradeTunnel(MYTunnelInterface * p)
{
    if (p) delete p;
}