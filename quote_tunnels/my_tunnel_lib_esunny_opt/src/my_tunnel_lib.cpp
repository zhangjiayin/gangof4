﻿#include "my_tunnel_lib.h"

#include <string>
#include <mutex>
#include <condition_variable>
#include <time.h>

#include "qtm_with_code.h"
#include "my_cmn_util_funcs.h"
#include "my_cmn_log.h"

#include "config_data.h"
#include "trade_log_util.h"
#include "esunny_trade_interface.h"
#include "esunny_position_interface.h"
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
        std::lock_guard<std::mutex> lock(s_init_sync);
        if (s_have_init)
        {
            return;
        }
        std::string log_file_name = "esunny_tunnel_log_" + my_cmn::GetCurrentDateTimeString();
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

void EsunnyTunnel::QueryPositionBeforeClose(const std::string &qry_time)
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
			pos_inf_ = new MYEsunnyPositionSpi(*lib_cfg_);
			sleep(20);
			break;
		} else {
			sleep(1);
		}
	}
}

void EsunnyTunnel::RespondHandleThread(EsunnyTunnel *ptunnel)
{
    MYEsunnyTradeSpi * p_esunny = static_cast<MYEsunnyTradeSpi *>(ptunnel->trade_inf_);
    if (!p_esunny)
    {
        TNL_LOG_ERROR("tunnel not init in RespondHandleThread.");
        return;
    }

    std::mutex &rsp_sync = p_esunny->rsp_sync;
    std::condition_variable &rsp_con = p_esunny->rsp_con;
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
void EsunnyTunnel::SendRespondAsync(int rsp_type, void *rsp)
{
    MYEsunnyTradeSpi * p_esunny = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_esunny)
    {
        TNL_LOG_ERROR("tunnel not init in RespondHandleThread.");
        return;
    }

    std::mutex &rsp_sync = p_esunny->rsp_sync;
    std::condition_variable &rsp_con = p_esunny->rsp_con;

    {
        std::unique_lock<std::mutex> lock(rsp_sync);
        pending_rsp_.push_back(std::make_pair(rsp_type, rsp));
    }
    rsp_con.notify_one();
}

EsunnyTunnel::EsunnyTunnel(const std::string &provider_config_file)
{
    trade_inf_ = NULL;
    exchange_code_ = ' ';
    InitOnce();

    // load config file
    std::string cfg_file("my_trade_tunnel_esunny.xml");
    if (!provider_config_file.empty())
    {
        cfg_file = provider_config_file;
    }

    TNL_LOG_INFO("create EsunnyTunnel object with configure file: %s", cfg_file.c_str());

    //TunnelConfigData cfg;
    lib_cfg_ = new TunnelConfigData();
    lib_cfg_->Load(cfg_file);
    exchange_code_ = lib_cfg_->Logon_config().exch_code.c_str()[0];
    tunnel_info_.account = lib_cfg_->Logon_config().investorid;
    pos_qry_time_ = lib_cfg_->Initial_policy_param().time_to_query_pos;

    char qtm_tmp_name[32];
    memset(qtm_tmp_name, 0, sizeof(qtm_tmp_name));
    sprintf(qtm_tmp_name, "esunny_opt_%s_%u", tunnel_info_.account.c_str(), getpid());
    tunnel_info_.qtm_name = qtm_tmp_name;
    TunnelUpdateState(tunnel_info_.qtm_name.c_str(), QtmState::INIT);

    if (pos_qry_time_.size() == 6) {
    	std::thread qry_pos(std::bind(&EsunnyTunnel::QueryPositionBeforeClose, this, pos_qry_time_));
    	qry_pos.detach();
    }

    // start log output thread
    LogUtil::Start("my_tunnel_lib_esunny", lib_cfg_->App_cfg().share_memory_key);

    int provider_type = lib_cfg_->App_cfg().provider_type;
    if (provider_type == TunnelProviderType::PROVIDERTYPE_ESUNNY)
    {
        InitInf(*lib_cfg_);
    }
    else
    {
        TNL_LOG_ERROR("not support tunnel provider type, please check config file.");
    }

    // init respond thread
    std::thread rsp_thread(&EsunnyTunnel::RespondHandleThread, this);
    rsp_thread.detach();
}

std::string EsunnyTunnel::GetClientID()
{
    return tunnel_info_.account;
}

bool EsunnyTunnel::InitInf(const TunnelConfigData &cfg)
{
    // 连接服务
    TNL_LOG_INFO("prepare to start ESUNNY tunnel server.");

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
    trade_inf_ = new MYEsunnyTradeSpi(cfg);

    return true;
}

void EsunnyTunnel::PlaceOrder(const T_PlaceOrder *p)
{
    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnPlaceOrder(p, tunnel_info_);
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);

    if (p_tunnel)
    {
        OrderRefDataType return_param;
        process_result = ComplianceCheck_TryReqOrderInsert(
            tunnel_info_.account.c_str(),
            p->serial_no,
            exchange_code_,
            p->stock_code,
            p->volume,
            p->limit_price,
            p->order_kind,
            p->speculator,
            p->direction,
            p->open_close,
            p->order_type,
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
                        p->stock_code).c_str());
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
                        p->stock_code).c_str());
                TNL_LOG_WARN("reach the warn threthold of cancel time, forbit open new position.");
            }
            else if (process_result == TUNNEL_ERR_CODE::CANCEL_REACH_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT, QtmComplianceState::CANCEL_TIME_OVER_MAXIMUN,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_OVER_MAXIMUN, tunnel_info_.account.c_str(),
                        p->stock_code).c_str());
                TNL_LOG_WARN("reach the maximum of cancel time, forbit open new position.");
            }
        }
        else
        {
            if (process_result == TUNNEL_ERR_CODE::OPEN_EQUAL_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::OPEN_POSITIONS_EQUAL_LIMITS,
                    GetComplianceDescriptionWithState(QtmComplianceState::OPEN_POSITIONS_EQUAL_LIMITS, tunnel_info_.account.c_str(),
                        p->stock_code).c_str());
                TNL_LOG_WARN("equal the warn threthold of open position.");
            }
            else if (process_result == TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT)
            {
                update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                    QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD,
                    GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                        p->stock_code).c_str());
                TNL_LOG_WARN("equal the warn threthold of cancel time.");
            }
            // 下单
            process_result = p_tunnel->ReqOrderInsert(p);

            // 发送失败，即时回滚
            if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
            {
                ComplianceCheck_OnOrderInsertFailed(
                    tunnel_info_.account.c_str(),
                    p->serial_no,
                    exchange_code_,
                    p->stock_code,
                    p->volume,
                    p->speculator,
                    p->open_close,
                    p->order_type);
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
        T_OrderRespond *rsp = new T_OrderRespond();
        ESUNNYPacker::OrderRespond(process_result, p->serial_no, 0, MY_TNL_OS_ERROR, *rsp);
        LogUtil::OnOrderRespond(rsp, tunnel_info_);

        SendRespondAsync(RespondDataType::kPlaceOrderRsp, rsp);
    }
}

void EsunnyTunnel::CancelOrder(const T_CancelOrder *p)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);

    int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnCancelOrder(p, tunnel_info_);

    if (p_tunnel)
    {
        process_result = ComplianceCheck_TryReqOrderAction(
            tunnel_info_.account.c_str(),
            p->stock_code,
            p->org_serial_no);

        if (process_result == TUNNEL_ERR_CODE::CANCEL_TIMES_REACH_WARN_THRETHOLD)
        {
            update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                QtmComplianceState::CANCEL_TIME_OVER_WARNING_THRESHOLD,
                GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_OVER_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                    p->stock_code).c_str());
            TNL_LOG_WARN("cancel time approaches threshold");
        }
        if (process_result == TUNNEL_ERR_CODE::CANCEL_REACH_LIMIT)
        {
            update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                QtmComplianceState::CANCEL_TIME_OVER_MAXIMUN,
                GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_OVER_MAXIMUN, tunnel_info_.account.c_str(),
                    p->stock_code).c_str());
            TNL_LOG_WARN("cancel time reach limitation");
        }
        if (process_result == TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT)
        {
            update_compliance(tunnel_info_.qtm_name.c_str(), tag_compl_type_enum::CANCEL_ORDER_LIMIT,
                QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD,
                GetComplianceDescriptionWithState(QtmComplianceState::CANCEL_TIME_EQUAL_WARNING_THRESHOLD, tunnel_info_.account.c_str(),
                    p->stock_code).c_str());
            TNL_LOG_WARN("cancel time equal threshold");
        }

        if ((process_result == TUNNEL_ERR_CODE::RESULT_SUCCESS)
            || (process_result == TUNNEL_ERR_CODE::CANCEL_TIMES_REACH_WARN_THRETHOLD)
            || (process_result == TUNNEL_ERR_CODE::CANCEL_EQUAL_LIMIT))
        {
            // cancel order
            process_result = p_tunnel->ReqOrderAction(p);

            // hanle fail event
            if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS)
            {
                ComplianceCheck_OnOrderCancelFailed(
                    tunnel_info_.account.c_str(),
                    p->stock_code,
                    p->org_serial_no);
            }
            else
            {
                ComplianceCheck_OnOrderPendingCancel(
                    tunnel_info_.account.c_str(),
                    p->org_serial_no);
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
        ESUNNYPacker::CancelRespond(process_result, p->serial_no, 0L, *cancle_order);
        LogUtil::OnCancelRespond(cancle_order, tunnel_info_);

        SendRespondAsync(RespondDataType::kCancelOrderRsp, cancle_order);
    }
}

void EsunnyTunnel::QueryPosition(const T_QryPosition *pQryPosition)
{
    LogUtil::OnQryPosition(pQryPosition, tunnel_info_);
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    T_PositionReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query position, please check configure file");

        ret.error_no = TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        if (QryPosReturnHandler_) QryPosReturnHandler_(&ret);
        LogUtil::OnPositionRtn(&ret, tunnel_info_);
        return;
    }

    TapAPIPositionQryReq qry_param;
    memset(&qry_param, 0, sizeof(TapAPICloseQryReq));

    int qry_result = p_tunnel->QryPosition(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryPosReturnHandler_) QryPosReturnHandler_(&ret);
        LogUtil::OnPositionRtn(&ret, tunnel_info_);
    }
}
void EsunnyTunnel::QueryOrderDetail(const T_QryOrderDetail *pQryParam)
{
    LogUtil::OnQryOrderDetail(pQryParam, tunnel_info_);
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    T_OrderDetailReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query order detail, please check configure file");

        ret.error_no = TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        if (QryOrderDetailReturnHandler_) QryOrderDetailReturnHandler_(&ret);
        LogUtil::OnOrderDetailRtn(&ret, tunnel_info_);
        return;
    }

    TapAPIOrderQryReq qry_param;
    memset(&qry_param, 0, sizeof(TapAPIOrderQryReq));
    qry_param.OrderQryType = TAPI_ORDER_QRY_TYPE_ALL;

    int qry_result = p_tunnel->QryOrderDetail(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryOrderDetailReturnHandler_) QryOrderDetailReturnHandler_(&ret);
        LogUtil::OnOrderDetailRtn(&ret, tunnel_info_);
    }
}
void EsunnyTunnel::QueryTradeDetail(const T_QryTradeDetail *pQryParam)
{
    LogUtil::OnQryTradeDetail(pQryParam, tunnel_info_);
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    T_TradeDetailReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query trade detail, please check configure file");

        ret.error_no = TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        if (QryTradeDetailReturnHandler_) QryTradeDetailReturnHandler_(&ret);
        LogUtil::OnTradeDetailRtn(&ret, tunnel_info_);
        return;
    }

    TapAPIFillQryReq qry_param;
    memset(&qry_param, 0, sizeof(TapAPIFillQryReq));

    int qry_result = p_tunnel->QryTradeDetail(&qry_param, 0);
    if (qry_result != 0)
    {
        ret.error_no = qry_result;
        if (QryTradeDetailReturnHandler_) QryTradeDetailReturnHandler_(&ret);
        LogUtil::OnTradeDetailRtn(&ret, tunnel_info_);
    }
}

void EsunnyTunnel::QueryContractInfo(const T_QryContractInfo *p)
{
    LogUtil::OnQryContractInfo(p, tunnel_info_);
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    T_ContractInfoReturn ret;
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when query contract info, please check configure file");

        ret.error_no = TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        if (QryContractInfoHandler_) QryContractInfoHandler_(&ret);
        LogUtil::OnContractInfoRtn(&ret, tunnel_info_);
        return;
    }

    // TODO for compatible ctp tunnel, return success
    int qry_result = p_tunnel->QryContractInfo(p);
	if (qry_result != 0) {
		ret.error_no = qry_result;
		if (QryContractInfoHandler_) QryContractInfoHandler_(&ret);
		LogUtil::OnContractInfoRtn(&ret, tunnel_info_);
	}
}

void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_OrderRespond *)> callback_handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
    OrderRespond_call_back_handler_ = callback_handler;
}

void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_CancelRespond *)> callback_handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
    CancelRespond_call_back_handler_ = callback_handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_OrderReturn *)> callback_handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_TradeReturn *)> callback_handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(callback_handler);
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_PositionReturn *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryPosReturnHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_OrderDetailReturn *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryOrderDetailReturnHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_TradeDetailReturn *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryTradeDetailReturnHandler_ = handler;
}

void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_ContractInfoReturn *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("tunnel not init when SetCallbackHandler");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    QryContractInfoHandler_ = handler;
}

// 新增做市接口, not support now 20151127
// 询价
void EsunnyTunnel::ReqForQuoteInsert(const T_ReqForQuote *p)
{
	int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnReqForQuote(p, tunnel_info_);
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);

    if (p_tunnel) {
    	process_result = p_tunnel->ReqForQuoteInsert(p);
    } else {
    	TNL_LOG_ERROR("not support tunnel, check configure file");
    }

    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS) {
    	TNL_LOG_WARN("req for quote insert failed, result=%d", process_result);
    	T_RspOfReqForQuote *rsp = new T_RspOfReqForQuote();
    	ESUNNYPacker::QuoteRequestRespond(p, process_result, *rsp);
    	if (RspOfReqForQuoteHandler_) {
    		RspOfReqForQuoteHandler_(rsp);
    	}
    	LogUtil::OnRspOfReqForQuote(rsp, tunnel_info_);

    	SendRespondAsync(RespondDataType::kInsertForQuoteRsp, rsp);
    }
}
///报价录入请求
void EsunnyTunnel::ReqQuoteInsert(const T_InsertQuote *p)
{
	int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnInsertQuote(p, tunnel_info_);
    MYEsunnyTradeSpi *p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);

    if (p_tunnel) {
    	process_result = p_tunnel->ReqQuoteInsert(p);
    } else {
    	TNL_LOG_ERROR("not support tunnel, check configure file");
    }

    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS) {
    	TNL_LOG_WARN("req quote insert failed, result=%d", process_result);
    	T_InsertQuoteRespond *rsp = new T_InsertQuoteRespond();
    	ESUNNYPacker::QuoteInsertRespond(p, process_result, *rsp);
    	if (InsertQuoteRespondHandler_) {
    		InsertQuoteRespondHandler_(rsp);
    	}
    	LogUtil::OnInsertQuoteRespond(rsp, tunnel_info_);

    	SendRespondAsync(RespondDataType::kInsertQuoteRsp, rsp);
    }
}
///报价操作请求
void EsunnyTunnel::ReqQuoteAction(const T_CancelQuote *p)
{
	int process_result = TUNNEL_ERR_CODE::RESULT_FAIL;
    LogUtil::OnCancelQuote(p, tunnel_info_);
    MYEsunnyTradeSpi *p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);

    if (p_tunnel) {
    	process_result = p_tunnel->ReqQuoteAction(p);
    } else {
    	TNL_LOG_ERROR("not support tunnel, check configure file");
    }

    if (process_result != TUNNEL_ERR_CODE::RESULT_SUCCESS) {
    	TNL_LOG_WARN("req quote action failed, result=%d", process_result);
    	T_CancelQuoteRespond *rsp = new T_CancelQuoteRespond();
    	ESUNNYPacker::QuoteActionRespond(p, process_result, *rsp);
    	if (CancelQuoteRespondHandler_) {
    		CancelQuoteRespondHandler_(rsp);
    	}
    	LogUtil::OnCancelQuoteRespond(rsp, tunnel_info_);

    	SendRespondAsync(RespondDataType::kCancelQuoteRsp, rsp);
    }
}

// 新增做市接口
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_RtnForQuote *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    RtnOfReqForQuoteHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_RspOfReqForQuote *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    RspOfReqForQuoteHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_InsertQuoteRespond *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    InsertQuoteRespondHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_CancelQuoteRespond *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    CancelQuoteRespondHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_QuoteReturn *)> handler)
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    RtnOfInsertQuoteHandler_ = handler;
}
void EsunnyTunnel::SetCallbackHandler(std::function<void(const T_QuoteTrade *)> handler)
{
    //TNL_LOG_WARN("tunnel not support function SetCallbackHandler(T_QuoteTrade)");
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
    if (!p_tunnel)
    {
        TNL_LOG_ERROR("not support tunnel when SetCallbackHandler, please check configure file");
        return;
    }
    p_tunnel->SetCallbackHandler(handler);
    RtnOfCancelQuoteHandler_ = handler;
}

EsunnyTunnel::~EsunnyTunnel()
{
    MYEsunnyTradeSpi * p_tunnel = static_cast<MYEsunnyTradeSpi *>(trade_inf_);
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
	return new EsunnyTunnel(tunnel_config_file);
}

void DestroyTradeTunnel(MYTunnelInterface * p)
{
    if (p) {
    	delete p;
    }
}
