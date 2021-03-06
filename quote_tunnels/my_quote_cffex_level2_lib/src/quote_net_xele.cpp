﻿#include "quote_net_xele.h"
#include <iomanip>
#include <vector>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>

#include "quote_cmn_config.h"
#include "quote_cmn_utility.h"

using namespace my_cmn;
using namespace std;

NetXeleDataHandler::NetXeleDataHandler(const SubscribeContracts *subscribe_contracts, const ConfigData &cfg)
    : cfg_(cfg), p_save_(NULL), qry_md_thread_(NULL), running_flag_(true)
{
    if (subscribe_contracts)
    {
        subscribe_contracts_ = *subscribe_contracts;
    }

    sprintf(qtm_name_, "net_xele_%s_%u", cfg.Logon_config().account.c_str(), getpid());
    QuoteUpdateState(qtm_name_, QtmState::INIT);

    // 初始化
    api_ = NULL;
    p_save_ = new QuoteDataSave<CFfexFtdcDepthMarketData>(cfg_, qtm_name_, "cffex_level2", GTA_UDP_CFFEX_QUOTE_TYPE);

    api_ = CXeleMdApi::CreateMdApi(this);
    if (api_)
    {
        MY_LOG_INFO("CXeleMdApi::GetVersion:%s", api_->GetVersion());
    }
    else
    {
        QuoteUpdateState(qtm_name_, QtmState::QUOTE_INIT_FAIL);
        MY_LOG_ERROR("CXeleMdApi::CreateMDApi failed");
        return;
    }

    CXeleMdFtdcReqUserLoginField login_field;
    strcpy(login_field.UserID, cfg_.Logon_config().account.c_str());
    strcpy(login_field.Password, cfg_.Logon_config().password.c_str());
    strcpy(login_field.ProtocolInfo, "");

    int ret = api_->LoginInit(cfg_.Logon_config().quote_provider_addrs.front().c_str(),
        cfg_.Logon_config().trade_server_addr.c_str(),
        cfg_.Logon_config().broker_id.c_str(),
        &login_field);

    if (ret == XELEAPI_SUCCESS)
    {
        QuoteUpdateState(qtm_name_, QtmState::API_READY);
        qry_md_thread_ = new boost::thread(&NetXeleDataHandler::QryDepthMarketData, this);
    }
    else
    {
        QuoteUpdateState(qtm_name_, QtmState::LOG_ON_FAIL);
        MY_LOG_ERROR("CXeleMdApi::LoginInit failed, return:%d", ret);
    }
}

NetXeleDataHandler::~NetXeleDataHandler(void)
{
    if (qry_md_thread_ && qry_md_thread_->joinable())
    {
        running_flag_ = false;
        qry_md_thread_->join();
    }

    if (api_)
    {
        api_->Release();
        api_ = NULL;
    }
}

void NetXeleDataHandler::OnFrontUserLoginSuccess()
{

}

void NetXeleDataHandler::OnFrontDisconnected(int nReason)
{

}

void NetXeleDataHandler::QryDepthMarketData()
{
    try
    {
        MarketDataTick mdtick;
        bool have_data = false;
        while (running_flag_)
        {
            have_data = api_->RecvMarketDataTick(&mdtick);

            if (!have_data)
            {
                continue;
            }

            const CXeleMdFtdcDepthMarketDataField *p = &mdtick.data;
            timeval t;
            gettimeofday(&t, NULL);

            CXeleMdFtdcDepthMarketDataField d(*p);
            RalaceInvalidValue(d);
            CFfexFtdcDepthMarketData q_cffex_level2 = Convert(d);

            if (quote_data_handler_
                && (subscribe_contracts_.empty() || subscribe_contracts_.find(d.InstrumentID) != subscribe_contracts_.end()))
            {
                quote_data_handler_(&q_cffex_level2);
            }

            // 存起来
            p_save_->OnQuoteData(t.tv_sec * 1000000 + t.tv_usec, &q_cffex_level2);
        }
    }
    catch (...)
    {
        MY_LOG_FATAL("xele - Unknown exception in OnRtnDepthMarketData.");
    }

}

void NetXeleDataHandler::SetQuoteDataHandler(boost::function<void(const CFfexFtdcDepthMarketData *)> quote_data_handler)
{
    quote_data_handler_ = quote_data_handler;
}

void NetXeleDataHandler::RalaceInvalidValue(CXeleMdFtdcDepthMarketDataField &d)
{
    d.Turnover = InvalidToZeroD(d.Turnover);
    d.LastPrice = InvalidToZeroD(d.LastPrice);
    d.UpperLimitPrice = InvalidToZeroD(d.UpperLimitPrice);
    d.LowerLimitPrice = InvalidToZeroD(d.LowerLimitPrice);
    d.HighestPrice = InvalidToZeroD(d.HighestPrice);
    d.LowestPrice = InvalidToZeroD(d.LowestPrice);
    d.OpenPrice = InvalidToZeroD(d.OpenPrice);
    d.ClosePrice = InvalidToZeroD(d.ClosePrice);
    //d.PreClosePrice = InvalidToZeroD(d.PreClosePrice);
    d.OpenInterest = InvalidToZeroD(d.OpenInterest);
    //d.PreOpenInterest = InvalidToZeroD(d.PreOpenInterest);

    d.BidPrice1 = InvalidToZeroD(d.BidPrice1);
    d.BidPrice2 = InvalidToZeroD(d.BidPrice2);
    d.BidPrice3 = InvalidToZeroD(d.BidPrice3);
    d.BidPrice4 = InvalidToZeroD(d.BidPrice4);
    d.BidPrice5 = InvalidToZeroD(d.BidPrice5);

    d.AskPrice1 = InvalidToZeroD(d.AskPrice1);
    d.AskPrice2 = InvalidToZeroD(d.AskPrice2);
    d.AskPrice3 = InvalidToZeroD(d.AskPrice3);
    d.AskPrice4 = InvalidToZeroD(d.AskPrice4);
    d.AskPrice5 = InvalidToZeroD(d.AskPrice5);

    d.SettlementPrice = InvalidToZeroD(d.SettlementPrice);
    //d.PreSettlementPrice = InvalidToZeroD(d.PreSettlementPrice);

    //d.PreDelta = InvalidToZeroD(d.PreDelta);
    d.CurrDelta = InvalidToZeroD(d.CurrDelta);
}

CFfexFtdcDepthMarketData NetXeleDataHandler::Convert(const CXeleMdFtdcDepthMarketDataField &xele_data)
{
    CFfexFtdcDepthMarketData q2;
    memset(&q2, 0, sizeof(CFfexFtdcDepthMarketData));

//    memcpy(q2.szTradingDay, xele_data.TradingDay, 9);
//    memcpy(q2.szSettlementGroupID, xele_data.SettlementGroupID, 9);
//    q2.nSettlementID = xele_data.SettlementID;
//    q2.dPreSettlementPrice = xele_data.PreSettlementPrice;
//    q2.dPreClosePrice = xele_data.PreClosePrice;
//    q2.dPreOpenInterest = xele_data.PreOpenInterest;
    q2.dLastPrice = xele_data.LastPrice;
    q2.dOpenPrice = xele_data.OpenPrice;
    q2.dHighestPrice = xele_data.HighestPrice;
    q2.dLowestPrice = xele_data.LowestPrice;
    q2.nVolume = xele_data.Volume;
    q2.dTurnover = xele_data.Turnover;
    q2.dOpenInterest = xele_data.OpenInterest;
    q2.dClosePrice = xele_data.ClosePrice;
    q2.dSettlementPrice = xele_data.SettlementPrice;
    q2.dUpperLimitPrice = xele_data.UpperLimitPrice;
    q2.dLowerLimitPrice = xele_data.LowerLimitPrice;
    //q2.dPreDelta = xele_data.PreDelta;
    q2.dCurrDelta = xele_data.CurrDelta;
    memcpy(q2.szUpdateTime, xele_data.UpdateTime, 9);
    q2.nUpdateMillisec = xele_data.UpdateMillisec;
    memcpy(q2.szInstrumentID, xele_data.InstrumentID, 31);
    q2.dBidPrice1 = xele_data.BidPrice1;
    q2.nBidVolume1 = xele_data.BidVolume1;
    q2.dAskPrice1 = xele_data.AskPrice1;
    q2.nAskVolume1 = xele_data.AskVolume1;
    q2.dBidPrice2 = xele_data.BidPrice2;
    q2.nBidVolume2 = xele_data.BidVolume2;
    q2.dAskPrice2 = xele_data.AskPrice2;
    q2.nAskVolume2 = xele_data.AskVolume2;
    q2.dBidPrice3 = xele_data.BidPrice3;
    q2.nBidVolume3 = xele_data.BidVolume3;
    q2.dAskPrice3 = xele_data.AskPrice3;
    q2.nAskVolume3 = xele_data.AskVolume3;
    q2.dBidPrice4 = xele_data.BidPrice4;
    q2.nBidVolume4 = xele_data.BidVolume4;
    q2.dAskPrice4 = xele_data.AskPrice4;
    q2.nAskVolume4 = xele_data.AskVolume4;
    q2.dBidPrice5 = xele_data.BidPrice5;
    q2.nBidVolume5 = xele_data.BidVolume5;
    q2.dAskPrice5 = xele_data.AskPrice5;
    q2.nAskVolume5 = xele_data.AskVolume5;

    return q2;
}
