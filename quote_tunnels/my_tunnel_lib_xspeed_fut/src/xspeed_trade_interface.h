﻿#ifndef XSpeed_TRADE_INTERFACE_H_
#define XSpeed_TRADE_INTERFACE_H_

#include <string>
#include <sstream>
#include <list>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "DFITCTraderApi.h"
#include "my_cmn_util_funcs.h"
#include "my_cmn_log.h"
#include "config_data.h"
#include "trade_data_type.h"
#include "xspeed_trade_context.h"
#include "my_tunnel_lib.h"
#include "tunnel_cmn_utility.h"
#include "trade_log_util.h"
#include "field_convert.h"
#include "xspeed_data_formater.h"

struct OriginalReqInfo;

enum QueryStatus {
    QUERY_INIT = 0,
    QUERY_PENDING,
    QUERY_ERROR,
    QUERY_SUCCESS,
};

struct QueryInfo {
    QueryStatus query_position_status;
    QueryStatus query_order_status;
    QueryStatus query_trade_status;
    QueryStatus query_contract_status;

    T_PositionReturn position_return;
    T_OrderDetailReturn order_return;
    T_TradeDetailReturn trade_return;
    T_ContractInfoReturn contract_return;

    QueryInfo() {
        query_position_status = QueryStatus::QUERY_INIT;
        query_order_status = QueryStatus::QUERY_INIT;
        query_trade_status = QueryStatus::QUERY_INIT;
        query_contract_status = QueryStatus::QUERY_INIT;
    }
};



class MYXSpeedSpi: public DFITCXSPEEDAPI::DFITCTraderSpi
{
public:
    MYXSpeedSpi(const TunnelConfigData &cfg, const Tunnel_Info &tunnel_info);
    virtual ~MYXSpeedSpi(void);

    void SetCallbackHandler(std::function<void(const T_OrderRespond *)> handler)
    {
        OrderRespond_call_back_handler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_CancelRespond *)> handler)
    {
        CancelRespond_call_back_handler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_OrderReturn *)> handler)
    {
        OrderReturn_call_back_handler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_TradeReturn *)> handler)
    {
        TradeReturn_call_back_handler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_PositionReturn *)> handler)
    {
        QryPosReturnHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_OrderDetailReturn *)> handler)
    {
        QryOrderDetailReturnHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_TradeDetailReturn *)> handler)
    {
        QryTradeDetailReturnHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_ContractInfoReturn *)> handler)
    {
    	QryContractInfoHandler_ = handler;
    }

    void SetCallbackHandler(std::function<void(const T_RspOfReqForQuote *)> handler)
    {
        RspOfReqForQuoteHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_RtnForQuote *)> handler)
    {
        RtnForQuoteHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_InsertQuoteRespond *)> handler)
    {
        InsertQuoteRespondHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_CancelQuoteRespond *)> handler)
    {
        CancelQuoteRespondHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_QuoteReturn *)> handler)
    {
        QuoteReturnHandler_ = handler;
    }
    void SetCallbackHandler(std::function<void(const T_QuoteTrade *)> handler)
    {
        QuoteTradeHandler_ = handler;
    }

    ///当客户端与交易后台建立起通信连接时（还未登录前），该方法被调用。
    virtual void OnFrontConnected();

    ///当客户端与交易后台通信连接断开时，该方法被调用。当发生这个情况后，API会自动重新连接，客户端可不做处理。
    ///@param nReason 错误原因
    ///        0x1001 网络读失败
    ///        0x1002 网络写失败
    ///        0x2001 接收心跳超时
    ///        0x2002 发送心跳失败
    ///        0x2003 收到错误报文
    virtual void OnFrontDisconnected(int nReason);

    //客户请求登录响应
    virtual void OnRspUserLogin(struct DFITCUserLoginInfoRtnField *pf, struct DFITCErrorRtnField *pe);
    //客户退出请求响应
    virtual void OnRspUserLogout(struct DFITCUserLogoutInfoRtnField *pf, struct DFITCErrorRtnField *pe);
    //委托下单响应
    virtual void OnRspInsertOrder(struct DFITCOrderRspDataRtnField *pf, struct DFITCErrorRtnField *pe);
    //委托撤单响应
    virtual void OnRspCancelOrder(struct DFITCOrderRspDataRtnField *pf, struct DFITCErrorRtnField *pe);
    //持仓查询响应
    virtual void OnRspQryPosition(struct DFITCPositionInfoRtnField *pf, struct DFITCErrorRtnField *pe, bool bIsLast);
    virtual void OnRspQryPositionDetail(struct DFITCPositionDetailRtnField * pf, struct DFITCErrorRtnField * pe, bool bIsLast);
    //客户资金查询响应
    virtual void OnRspCustomerCapital(struct DFITCCapitalInfoRtnField *pf, struct DFITCErrorRtnField *pe, bool bIsLast);
    //交易所合约查询响应
    virtual void OnRspQryExchangeInstrument(struct DFITCExchangeInstrumentRtnField *pf, struct DFITCErrorRtnField *pe, bool bIsLast);
    // 账单确认响应
    virtual void OnRspBillConfirm(struct DFITCBillConfirmRspField * pf, struct DFITCErrorRtnField * pe);
    //错误回报
    virtual void OnRtnErrorMsg(struct DFITCErrorRtnField *pf);
    //成交回报
    virtual void OnRtnMatchedInfo(struct DFITCMatchRtnField *pf);
    //委托回报
    virtual void OnRtnOrder(struct DFITCOrderRtnField *pf);
    //撤单回报
    virtual void OnRtnCancelOrder(struct DFITCOrderCanceledRtnField *pf);

    virtual void OnRspQryOrderInfo(struct DFITCOrderCommRtnField *pf, struct DFITCErrorRtnField * pe, bool bIsLast);
    virtual void OnRspQryMatchInfo(struct DFITCMatchedRtnField *pf, struct DFITCErrorRtnField * pe, bool bIsLast);
    //交易日确认响应:用于接收交易日信息。
    virtual void OnRspTradingDay(struct DFITCTradingDayRtnField * pf);

    // added on 20141216 for support market making
    /**
     * 询价通知订阅响应
     * @param pRspQuoteSubscribeData:指向询价通知响应地址的指针。
     */
    virtual void OnRspQuoteSubscribe(struct DFITCQuoteSubscribeRspField * pf, struct DFITCErrorRtnField * pe){};

    /**
     * 询价通知订阅回报
     * @param pRtnQuoteSubscribeData:指向询价通知回报地址的指针。
     */
    virtual void OnRtnQuoteSubscribe(struct DFITCQuoteSubscribeRtnField * pf){};

    /**
     * 询价通知退订响应
     * @param pRspQuoteUnSubscribeData:指向询价通知退订响应地址的指针。
     */
    virtual void OnRspUnQuoteSubscribe(struct DFITCQuoteUnSubscribeRspField * pf, struct DFITCErrorRtnField * pe){};

    /**
     * 做市商报单响应
     * @param pRspQuoteData:指向做市商报单响应地址的指针。
     */
    virtual void OnRspQuoteInsert(struct DFITCQuoteRspField * pf, struct DFITCErrorRtnField * pe){};

    /**
     * 做市商报单回报
     * @param pRtnQuoteData:指向做市商报单回报地址的指针。
     */
    virtual void OnRtnQuoteInsert(struct DFITCQuoteRtnField * pf){};

    /**
     * 做市商成交回报
     * @param pRtnQuoteMatchedData:指向做市商成交回报地址的指针。
     */
    virtual void OnRtnQuoteMatchedInfo(struct DFITCQuoteMatchRtnField * pf){};

    /**
     * 做市商撤单响应
     * @param pRspQuoteCanceledData:指向做市商撤单响应地址的指针。
     */
    virtual void OnRspQuoteCancel(struct DFITCQuoteRspField * pf, struct DFITCErrorRtnField * pe){};

    /**
     * 做市商撤单回报
     * @param pRtnQuoteCanceledData:指向做市商撤单回报地址的指针。
     */
    virtual void OnRtnQuoteCancel(struct DFITCQuoteCanceledRtnField * pf){};

    /**
     * 交易所状态通知
     * @param pRtnExchangeStatusData:指向交易所状态通知地址的指针。
     */
    virtual void OnRtnExchangeStatus(struct DFITCExchangeStatusRtnField * pf);

    /**
     * 批量撤单响应
     * @param pRspStripCancelOrderData:指向批量撤单响应地址的指针。
     */
    virtual void OnRspCancelAllOrder(struct DFITCCancelAllOrderRspField *pf, struct DFITCErrorRtnField * pe){};

    /**
     * 询价响应
     * @param pRspForQuoteData:询价请求结构地址。
     * @return 0 - 请求发送成功 -1 - 请求发送失败  -2 -检测异常。
     */
    virtual void OnRspForQuote(struct DFITCForQuoteRspField * pf, struct DFITCErrorRtnField * pe){};

    /**
     * 询价回报
     * @param pRspForQuoteData:询价请求结构地址。
     * @return 0 - 请求发送成功 -1 - 请求发送失败  -2 -检测异常。
     */
    virtual void OnRtnForQuote(struct DFITCForQuoteRtnField * pf){};

public:
    // 下发指令接口
    int ReqOrderInsert(DFITCInsertOrderField *p)
    {
        if (!TunnelIsReady())
        {
            TNL_LOG_WARN("ReqOrderInsert when tunnel not ready");
            return TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        }
        int ret = TUNNEL_ERR_CODE::RESULT_FAIL;

        try
        {
            ret = api_->ReqInsertOrder(p);
            TNL_LOG_DEBUG("ReqInsertOrder - ret=%d - %s", ret, XSpeedDatatypeFormater::ToString(p).c_str());
            if (ret != 0)
            {
                // -2，表示未处理请求超过许可数；
                // -3，表示每秒发送请求数超过许可数。
                if (ret == -2)
                {
                    return TUNNEL_ERR_CODE::CFFEX_OVER_REQUEST;
                }
                if (ret == -3)
                {
                    return TUNNEL_ERR_CODE::CFFEX_OVER_REQUEST_PER_SECOND;
                }
                return TUNNEL_ERR_CODE::RESULT_FAIL;
            }
        }
        catch (...)
        {
            TNL_LOG_ERROR("unknown exception in ReqOrderInsert.");
        }

        return ret;
    }

    //报单操作请求
    int ReqOrderAction(DFITCCancelOrderField *p)
    {
        if (!TunnelIsReady())
        {
            TNL_LOG_WARN("ReqOrderAction when tunnel not ready");
            return TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        }
        int ret = TUNNEL_ERR_CODE::RESULT_FAIL;

        try
        {
            ret = api_->ReqCancelOrder(p);
            TNL_LOG_DEBUG("ReqCancelOrder - ret=%d - %s", ret, XSpeedDatatypeFormater::ToString(p).c_str());
            if (ret != 0)
            {
                // -2，表示未处理请求超过许可数；
                // -3，表示每秒发送请求数超过许可数。
                if (ret == -2)
                {
                    return TUNNEL_ERR_CODE::CFFEX_OVER_REQUEST;
                }
                if (ret == -3)
                {
                    return TUNNEL_ERR_CODE::CFFEX_OVER_REQUEST_PER_SECOND;
                }
                return TUNNEL_ERR_CODE::RESULT_FAIL;
            }
        }
        catch (...)
        {
            TNL_LOG_ERROR("unknown exception in ReqCancelOrder.");
        }

        return ret;
    }

    int QryPosition(DFITCPositionDetailField *p)
    {
        if (!TunnelIsReady())
        {
            TNL_LOG_WARN("QryPosition when tunnel not ready");
            return TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        }

        QryPosReturnHandler_(&query_info_.position_return);
        LogUtil::OnPositionRtn(&query_info_.position_return, tunnel_info_);
        return TUNNEL_ERR_CODE::RESULT_SUCCESS;
    }
    int QryOrderDetail(DFITCOrderField *p)
    {
        if (!TunnelIsReady())
        {
            TNL_LOG_WARN("QryOrderDetail when tunnel not ready");
            return TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        }

        if (QryOrderDetailReturnHandler_) QryOrderDetailReturnHandler_(&query_info_.order_return);
        LogUtil::OnOrderDetailRtn(&query_info_.order_return, tunnel_info_);
        return TUNNEL_ERR_CODE::RESULT_SUCCESS;
    }
    int QryTradeDetail(DFITCMatchField *p)
    {
        if (!TunnelIsReady())
        {
            TNL_LOG_WARN("QryTradeDetail when tunnel not ready");
            return TUNNEL_ERR_CODE::NO_VALID_CONNECT_AVAILABLE;
        }

        if (QryTradeDetailReturnHandler_) QryTradeDetailReturnHandler_(&query_info_.trade_return);
        LogUtil::OnTradeDetailRtn(&query_info_.trade_return, tunnel_info_);
        return TUNNEL_ERR_CODE::RESULT_SUCCESS;
    }

    int ReqForQuoteInsert(DFITCForQuoteField *p)
    {
	    return 0;
    }

    ///报价录入请求
    int ReqQuoteInsert(DFITCQuoteInsertField *p)
    {
	    return 0;
	}
    ///报价操作请求
    int ReqQuoteAction(DFITCCancelOrderField *p)
    {
	    return 0;
    }

    bool TunnelIsReady()
    {
        return logoned_ && have_handled_unterminated_orders_ && is_ready_ && query_is_ready_;
    }
    int Session_id() const
    {
        return session_id_;
    }

    bool IsReady()
    {
        return is_ready_;
    }

private:
    bool ParseConfig();
    void ReqLogin();

public:
    XSpeedTradeContext xspeed_trade_context_;
    std::mutex client_sync;
    // 外部接口对象使用，为避免修改接口，新增对象放到此处
    std::mutex rsp_sync;
    std::condition_variable rsp_con;

private:
    DFITCXSPEEDAPI::DFITCTraderApi *api_;
    long session_id_;
    long max_order_ref_;
    DFITCDateType trade_day;

    Tunnel_Info tunnel_info_;
    std::string pswd_;
    std::string quote_addr_;
    std::string exchange_code_;

    std::function<void(const T_OrderRespond *)> OrderRespond_call_back_handler_;
    std::function<void(const T_CancelRespond *)> CancelRespond_call_back_handler_;
    std::function<void(const T_OrderReturn *)> OrderReturn_call_back_handler_;
    std::function<void(const T_TradeReturn *)> TradeReturn_call_back_handler_;

    std::function<void(const T_PositionReturn *)> QryPosReturnHandler_;
    std::function<void(const T_OrderDetailReturn *)> QryOrderDetailReturnHandler_;
    std::function<void(const T_TradeDetailReturn *)> QryTradeDetailReturnHandler_;
    std::function<void(const T_ContractInfoReturn *)> QryContractInfoHandler_;

    // added for support market making interface
    std::function<void(const T_RspOfReqForQuote *)> RspOfReqForQuoteHandler_;
    std::function<void(const T_RtnForQuote *)> RtnForQuoteHandler_;
    std::function<void(const T_InsertQuoteRespond *)> InsertQuoteRespondHandler_;
    std::function<void(const T_CancelQuoteRespond *)> CancelQuoteRespondHandler_;
    std::function<void(const T_QuoteReturn *)> QuoteReturnHandler_;
    std::function<void(const T_QuoteTrade *)> QuoteTradeHandler_;

    // 配置数据对象
    TunnelConfigData cfg_;
    volatile bool connected_;
    std::atomic_bool logoned_;
    QueryInfo query_info_;

    void CheckAndSaveYestodayPosition();
    void LoadYestodayPositionFromFile(const std::string &file);
    void SaveYestodayPositionToFile(const std::string &file);

    // variables and functions for cancel all unterminated orders automatically
    std::atomic_bool have_handled_unterminated_orders_;
    std::mutex cancel_sync_;
    std::condition_variable qry_order_finish_cond_;
    std::thread *cancel_t_;
    //void CancelUnterminatedOrders(){};

    volatile bool is_ready_;
    volatile bool query_is_ready_;
    struct timeval timer_start_, timer_end_;

    std::mutex cancel_times_sync_;
    std::map<std::string, int> cancel_times_of_contract;

    void CalcCancelTimes(const struct DFITCOrderCommRtnField* const pf, const struct DFITCErrorRtnField* const pe, const bool bIsLast);
    void QueryAllBeforeReady();
    void ReportErrorState(int api_error_no, const std::string &error_msg);

    volatile bool in_init_state_; // clear after login
};

#endif //
