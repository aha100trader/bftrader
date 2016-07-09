#include "tdsm.h"
#include "ThostFtdcTraderApi.h"
#include "ctputils.h"
#include "encode_utils.h"
#include "file_utils.h"
#include "gatewaymgr.h"
#include "servicemgr.h"
#include <QDir>

///////////
class TdSmSpi : public CThostFtdcTraderSpi {
public:
    explicit TdSmSpi(TdSm* sm)
        : sm_(sm)
    {
    }

private:
    void OnFrontConnected() override
    {
        BfLog(__FUNCTION__);
        emit sm()->statusChanged(TDSM_CONNECTED);
    }

    // 客户端与服务端连接断开后，交易接口会自动尝试重新连接，频率是每 5 秒一次。(CTPSDK)
    // 如果网络异常，会直接调用OnFrontDisconnected，需要重置状态数据=
    // 网络错误当再次恢复时候，会自动重连重新走OnFrontConnected
    // logout也会导致一次disconnected+connected，0x1001
    void OnFrontDisconnected(int nReason) override
    {
        BfLog("TdSmSpi::OnFrontDisconnected,nReason=0x%x", nReason);

        emit sm()->statusChanged(TDSM_DISCONNECTED);
    }

    // errorId=3，msg=CTP:不合法的登陆=
    // errorId=7，msg=CTP:还没有初始化=
    // errorId=8,msg=CTP:前置不活跃=
    // 1. 并不是connected就可以登陆的=
    // 2. 如果connected后不能登录，对于7，会过一会来一个disconnected+connected，所以不用处理=
    // 3. 发现对于errorid=7，不一定会disconnect后再connected，需要自己去发包=
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        if (bIsLast) {
            BfLog(__FUNCTION__);
            if (isErrorRsp(pRspInfo, nRequestID)) {
                emit sm()->statusChanged(TDSM_LOGINFAIL);
            } else {
                orderRef_ = 0;
                frontId_ = pRspUserLogin->FrontID;
                sessionId_ = pRspUserLogin->SessionID;
                sysid2bfid_.clear();

                emit sm()->statusChanged(TDSM_LOGINED);
            }
        }
    }

    // logout不一定有logout的rep的，可能是直接disconnect+connect，所以在connect里面判断一下是否需要auologin，如果不是
    // 就是手动停止了，stop掉=
    void OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        if (bIsLast) {
            BfLog(__FUNCTION__);
            if (isErrorRsp(pRspInfo, nRequestID)) {
                emit sm()->statusChanged(TDSM_LOGOUTFAIL);
            } else {
                emit sm()->statusChanged(TDSM_LOGOUTED);
            }
        }
    }

    //出现了一次queryinstruments错误，打印详细信息=
    //TODO(hege):这个需要做一个判断，然后自动queryinstruments，不然就不能自动登录了=
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        if (bIsLast) {
            BfLog(__FUNCTION__);
            isErrorRsp(pRspInfo, nRequestID);
        }
    }

    // 可能有多次回调=
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        if (!symbolPrefixList_.length()) {
            QString prefixes = sm()->symbolPrefixes_;
            symbolPrefixList_ = prefixes.split(";", QString::SkipEmptyParts);
        }
        if (!isErrorRsp(pRspInfo, nRequestID) && pInstrument) {
            QString symbol = pInstrument->InstrumentID;

            // 全部合约，由于持仓的部分可能不是订阅的，持仓需要用合约信息，这里要保留全部合约信息=
            // https://github.com/sunwangme/bftrader/issues/5
            CThostFtdcInstrumentField* contract = new CThostFtdcInstrumentField();
            memcpy(contract, pInstrument, sizeof(CThostFtdcInstrumentField));
            g_sm->gatewayMgr()->insertContract(symbol, contract);
            symbols_all_ << symbol;

            // 订阅合约，如果过滤套利合约可以设置长度<=6
            //if (symbol.length() <= 6) {
            if (1) {
                QString symbol_low = symbol.toLower();
                for (int i = 0; i < symbolPrefixList_.length(); i++) {
                    QString prefix = symbolPrefixList_.at(i);
                    if (symbol_low.startsWith(prefix)) {
                        symbols_my_ << symbol;
                        break;
                    }
                }
            }
        }

        if (bIsLast) {
            QString symbols;
            for (auto symbol : symbols_my_) {
                symbols = symbols + symbol + ";";
            }
            BfLog("total got symbol:%d,reqId=%d,prefixes=(%s),symbols=(%s)", symbols_my_.length(), nRequestID, qPrintable(sm()->symbolPrefixes_), qPrintable(symbols));
            g_sm->gatewayMgr()->initRingBuffer(sizeof(CThostFtdcDepthMarketDataField), symbols_my_);
            emit g_sm->gatewayMgr()->gotContracts(symbols_my_, symbols_all_);
        }
    }

    // 请求查询资金账户响应=
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        BfLog(__FUNCTION__);
        if (bIsLast) {
            if (isErrorRsp(pRspInfo, nRequestID)) {
                return;
            } else {
                BfAccountData account;
                //账户代码=
                account.set_accountid(pTradingAccount->AccountID);
                //数值相关=
                account.set_prebalance(pTradingAccount->PreBalance);
                double balance = pTradingAccount->PreBalance - pTradingAccount->PreCredit - pTradingAccount->PreMortgage + pTradingAccount->Mortgage - pTradingAccount->Withdraw + pTradingAccount->Deposit + pTradingAccount->CloseProfit + pTradingAccount->PositionProfit + pTradingAccount->CashIn - pTradingAccount->Commission;
                account.set_balance(balance);
                //account.set_balance(pTradingAccount->Balance);
                account.set_available(pTradingAccount->Available);
                account.set_commission(pTradingAccount->Commission);
                double frozenMargin = pTradingAccount->FrozenMargin;
                if ((frozenMargin > 0) && (balance > 0)) {
                    frozenMargin = (frozenMargin * 100.0) / balance;
                }
                account.set_frozenmargin(frozenMargin);
                account.set_closeprofit(pTradingAccount->CloseProfit);
                account.set_positionprofit(pTradingAccount->PositionProfit);

                emit g_sm->gatewayMgr()->gotAccount(account);
            }
        }
    }

    // 请求查询投资者持仓响应，可能有多个回调=
    // 需要累加=由上层完成=
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        if (!isErrorRsp(pRspInfo, nRequestID) && pInvestorPosition) {
            BfPositionData pos;

            // 保存代码=
            pos.set_symbol(pInvestorPosition->InstrumentID);
            pos.set_direction(CtpUtils::translatePosiDirection(pInvestorPosition->PosiDirection));

            // 冻结=
            if (pos.direction() == DIRECTION_LONG || pos.direction() == DIRECTION_NET) {
                pos.set_frozen(pInvestorPosition->LongFrozen);
            } else if (pos.direction() == DIRECTION_SHORT) {
                pos.set_frozen(pInvestorPosition->ShortFrozen);
            }

            // 持仓量=
            // 持仓查询记录中的昨持仓是今天开盘前的一个初始值，不会因为平昨或者平仓而减少
            // 当前时侯的昨持仓=总持仓-今持仓。YdPosition := Position - TodayPosition
            pos.set_position(pInvestorPosition->Position);
            pos.set_ydposition(pInvestorPosition->Position - pInvestorPosition->TodayPosition);

            // 均价=
            if (pos.position() > 0) {
                pos.set_price(pInvestorPosition->PositionCost / pos.position());
            }

            BfLog("position: symbol=%s direction=%s position=%d ydposition=%d todayposition=%d",
                pos.symbol().c_str(),
                CtpUtils::formatDirection(pos.direction()).toStdString().c_str(),
                pos.position(),
                pos.ydposition(),
                pInvestorPosition->TodayPosition);

            // notification: begin query position
            if (queryPositionReqId_ == 0) {
                queryPositionReqId_ = nRequestID;

                BfNotificationData note;
                note.set_type(NOTIFICATION_BEGINQUERYPOSITION);
                emit g_sm->gatewayMgr()->gotNotification(note);
            }
            emit g_sm->gatewayMgr()->gotPosition(pos);
        }

        if (bIsLast) {
            BfLog(__FUNCTION__);

            // notification: end query position
            if (queryPositionReqId_ != 0) {
                queryPositionReqId_ = 0;

                BfNotificationData note;
                note.set_type(NOTIFICATION_ENDQUERYPOSITION);
                emit g_sm->gatewayMgr()->gotNotification(note);
            }
        }
    }

    // 投资者结算结果确认响应=
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        BfLog(__FUNCTION__);
        if (bIsLast) {
            if (isErrorRsp(pRspInfo, nRequestID)) {
                return;
            } else {
            }
        }
    }

    // 请求查询报单响应，可能有多次回调=
    virtual void OnRspQryOrder(CThostFtdcOrderField* pOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        if (!isErrorRsp(pRspInfo, nRequestID) && pOrder) {
            int orderRef = QString(pOrder->OrderRef).toInt();
            QString bfOrderId = CtpUtils::formatBfOrderId(pOrder->FrontID, pOrder->SessionID, orderRef);
            BfLog("%s: bfOrderId=(%s)", __FUNCTION__, qPrintable(bfOrderId));
            if (QString(pOrder->OrderSysID).trimmed().length() != 0 && QString(pOrder->ExchangeID).trimmed().length() != 0) {
                QString sysOrderId = QString().sprintf("%s.%s", pOrder->ExchangeID, pOrder->OrderSysID);
                sysid2bfid_[pOrder->OrderSysID] = bfOrderId;
                BfLog("%s: bfOrderId=(%s)<--sysOrderId=(%s)", __FUNCTION__, qPrintable(bfOrderId), qPrintable(sysOrderId));
            }

            BfOrderData order;
            // 保存代码和报单号=
            order.set_exchange(pOrder->ExchangeID);
            order.set_symbol(pOrder->InstrumentID);
            order.set_bforderid(bfOrderId.toStdString());
            order.set_direction(CtpUtils::translateDirection(pOrder->Direction)); //方向=
            order.set_offset(CtpUtils::translateOffset(pOrder->CombOffsetFlag[0])); //开平=
            order.set_status(CtpUtils::translateStatus(pOrder->OrderStatus)); //状态=

            //价格、报单量等数值=
            order.set_price(pOrder->LimitPrice);
            order.set_totalvolume(pOrder->VolumeTotalOriginal);
            order.set_tradedvolume(pOrder->VolumeTraded);
            order.set_insertdate(pOrder->InsertDate);
            order.set_inserttime(pOrder->InsertTime);
            order.set_canceltime(pOrder->CancelTime);

            // notification: begin query orders
            if (queryOrdersReqId_ == 0) {
                queryOrdersReqId_ = nRequestID;

                BfNotificationData note;
                note.set_type(NOTIFICATION_BEGINQUERYORDERS);
                emit g_sm->gatewayMgr()->gotNotification(note);
            }
            emit g_sm->gatewayMgr()->gotOrder(order);
        }

        if (bIsLast) {
            BfLog("%s: finished", __FUNCTION__);

            // notification: end query orders
            if (queryOrdersReqId_ != 0) {
                queryOrdersReqId_ = 0;

                BfNotificationData note;
                note.set_type(NOTIFICATION_ENDQUERYORDERS);
                emit g_sm->gatewayMgr()->gotNotification(note);
            }
        }
    }

    // 报单录入请求响应=
    // 发单错误（柜台）=
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        BfLog(__FUNCTION__);
        if (bIsLast) {
            if (isErrorRsp(pRspInfo, nRequestID) && pInputOrder) {
                int orderRef = QString(pInputOrder->OrderRef).toInt();
                QString bfOrderId = CtpUtils::formatBfOrderId(frontId_, sessionId_, orderRef);
                BfLog("OnRspOrderInsert: bfOrderId = %s", bfOrderId.toStdString().c_str());
                return;
            } else {
            }
        }
    }

    // 报单录入错误回报=
    // 发单错误回报（交易所）=
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo) override
    {
        BfLog(__FUNCTION__);
        if (true) {
            if (isErrorRsp(pRspInfo, 0) && pInputOrder) {
                int orderRef = QString(pInputOrder->OrderRef).toInt();
                QString bfOrderId = CtpUtils::formatBfOrderId(frontId_, sessionId_, orderRef);
                BfLog("OnErrRtnOrderInsert: bfOrderId = %s", bfOrderId.toStdString().c_str());
                return;
            } else {
            }
        }
    }

    // 报单操作请求响应=
    // 撤单错误（柜台）=
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override
    {
        BfLog(__FUNCTION__);
        if (bIsLast) {
            if (isErrorRsp(pRspInfo, nRequestID) && pInputOrderAction) {
                int orderRef = QString(pInputOrderAction->OrderRef).toInt();
                QString bfOrderId = CtpUtils::formatBfOrderId(pInputOrderAction->FrontID, pInputOrderAction->SessionID, orderRef);
                BfLog("OnRspOrderAction: bfOrderId = %s", bfOrderId.toStdString().c_str());
                return;
            } else {
            }
        }
    }

    // 报单操作错误回报=
    // 撤单错误回报（交易所）=
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction, CThostFtdcRspInfoField* pRspInfo) override
    {
        BfLog(__FUNCTION__);
        if (true) {
            if (isErrorRsp(pRspInfo, 0) && pOrderAction) {
                int orderRef = QString(pOrderAction->OrderRef).toInt();
                QString bfOrderId = CtpUtils::formatBfOrderId(pOrderAction->FrontID, pOrderAction->SessionID, orderRef);
                BfLog("OnErrRtnOrderAction: bfOrderId = %s", bfOrderId.toStdString().c_str());
                return;
            } else {
            }
        }
    }

    // 报单通知= 多客户端时候也得到其他客户端的回报，可以通过frontid+sessionid过滤出自己的报单=
    // 参考：综合交易平台交易API特别说明.pdf
    virtual void OnRtnOrder(CThostFtdcOrderField* pOrder)
    {
        int orderRef = QString(pOrder->OrderRef).toInt();
        QString bfOrderId = CtpUtils::formatBfOrderId(pOrder->FrontID, pOrder->SessionID, orderRef);
        BfLog("%s: bfOrderId=(%s)", __FUNCTION__, qPrintable(bfOrderId));
        if (QString(pOrder->OrderSysID).trimmed().length() != 0 && QString(pOrder->ExchangeID).trimmed().length() != 0) {
            QString sysOrderId = QString().sprintf("%s.%s", pOrder->ExchangeID, pOrder->OrderSysID);
            sysid2bfid_[sysOrderId] = bfOrderId;
            BfLog("%s: bfOrderId=(%s)<--sysOrderId=(%s)", __FUNCTION__, qPrintable(bfOrderId), qPrintable(sysOrderId));
        }

        BfOrderData order;
        // 保存代码和报单号=
        order.set_exchange(pOrder->ExchangeID);
        order.set_symbol(pOrder->InstrumentID);
        order.set_bforderid(bfOrderId.toStdString());
        order.set_direction(CtpUtils::translateDirection(pOrder->Direction)); //方向=
        order.set_offset(CtpUtils::translateOffset(pOrder->CombOffsetFlag[0])); //开平=
        order.set_status(CtpUtils::translateStatus(pOrder->OrderStatus)); //状态=

        //价格、报单量等数值=
        order.set_price(pOrder->LimitPrice);
        order.set_totalvolume(pOrder->VolumeTotalOriginal);
        order.set_tradedvolume(pOrder->VolumeTraded);
        order.set_insertdate(pOrder->InsertDate);
        order.set_inserttime(pOrder->InsertTime);
        order.set_canceltime(pOrder->CancelTime);

        emit g_sm->gatewayMgr()->gotOrder(order);
    }

    // 成交通知=成交可以使另外一个session的，这里要做sysid到bforderid的隐射，需要先拿到所有的working order
    // 比如：挂单->ctp断网->ctp联网->成交=
    // 重连之后，要重新查一下order，也可以重建隐射=
    virtual void OnRtnTrade(CThostFtdcTradeField* pTrade)
    {
        // TODO(hege):trim OrderSysID
        QString sysOrderId = QString().sprintf("%s.%s", pTrade->ExchangeID, pTrade->OrderSysID);
        BfLog("%s: sysOrderId=(%s)", __FUNCTION__, qPrintable(sysOrderId));
        if (!sysid2bfid_.contains(sysOrderId)) {
            BfLog("%s: can not find order,so ignore the trade,tradeId=(%s),sysOrderId=(%s)", __FUNCTION__, pTrade->TradeID, qPrintable(sysOrderId));
            return;
        }
        QString bfOrderId = sysid2bfid_[sysOrderId];
        BfLog("%s: bfOrderId=(%s)<--sysOrderId=(%s)", __FUNCTION__, qPrintable(bfOrderId), qPrintable(sysOrderId));

        BfTradeData trade;

        // 保存代码和报单号=
        trade.set_exchange(pTrade->ExchangeID);
        trade.set_symbol(pTrade->InstrumentID);
        trade.set_bforderid(bfOrderId.toStdString());
        // TODO(hege):trim tradeId
        trade.set_tradeid(pTrade->TradeID);
        trade.set_direction(CtpUtils::translateDirection(pTrade->Direction)); //方向=
        trade.set_offset(CtpUtils::translateOffset(pTrade->OffsetFlag)); //开平=

        //价格、报单量等数值=
        trade.set_price(pTrade->Price);
        trade.set_volume(pTrade->Volume);
        trade.set_tradedate(pTrade->TradeDate);
        trade.set_tradetime(pTrade->TradeTime);

        emit g_sm->gatewayMgr()->gotTrade(trade);
    }

private:
    bool isErrorRsp(CThostFtdcRspInfoField* pRspInfo, int reqId)
    {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            BfLog("reqid=%d,errorId=%d，msg=%s", reqId, pRspInfo->ErrorID, gbk2utf16(pRspInfo->ErrorMsg).toUtf8().constData());
            emit g_sm->gatewayMgr()->gotGatewayError(pRspInfo->ErrorID, gbk2utf16(pRspInfo->ErrorMsg), QString().sprintf("reqId=%d", reqId));
            return true;
        }
        return false;
    }

    TdSm* sm()
    {
        return sm_;
    }

    void resetData()
    {
        g_sm->checkCurrentOn(ServiceMgr::BLOGIC);

        symbols_my_.clear();
        symbols_all_.clear();
        symbolPrefixList_.clear();
        g_sm->gatewayMgr()->freeContracts();
    }

    int getOrderRef()
    {
        orderRef_++;
        return orderRef_;
    }

    int getFrontId() { return frontId_; }
    int getSessionId() { return sessionId_; }

private:
    TdSm* sm_;
    QStringList symbols_my_;
    QStringList symbols_all_;
    QStringList symbolPrefixList_;

    std::atomic_int32_t orderRef_ = 0;
    int frontId_ = 0;
    int sessionId_ = 0;
    QMap<QString, QString> sysid2bfid_;

    int queryPositionReqId_ = 0;
    int queryOrdersReqId_ = 0;

    friend TdSm;
};

///////////
TdSm::TdSm(QObject* parent)
    : QObject(parent)
{
}

TdSm::~TdSm()
{
}

bool TdSm::init(QString userId, QString password, QString brokerId, QString frontTd, QString flowPathTd, QString symbolPrefixes)
{
    userId_ = userId;
    password_ = password;
    brokerId_ = brokerId;
    frontTd_ = frontTd;
    flowPathTd_ = flowPathTd;
    symbolPrefixes_ = symbolPrefixes;

    // check
    if (userId_.length() == 0 || password_.length() == 0
        || brokerId_.length() == 0 || frontTd_.length() == 0 || flowPathTd_.length() == 0
        || symbolPrefixes.length() == 0) {
        return false;
    }

    return true;
}

void TdSm::start()
{
    BfLog(__FUNCTION__);

    if (tdapi_ != nullptr) {
        qFatal("tdapi_!=nullptr");
        return;
    }

    QDir dir;
    dir.mkpath(flowPathTd_);
    tdapi_ = CThostFtdcTraderApi::CreateFtdcTraderApi(flowPathTd_.toStdString().c_str());
    tdspi_ = new TdSmSpi(this);
    tdapi_->RegisterSpi(tdspi_);
    tdapi_->RegisterFront((char*)qPrintable(frontTd_));
    tdapi_->SubscribePublicTopic(THOST_TERT_QUICK);
    tdapi_->SubscribePrivateTopic(THOST_TERT_QUICK);
    tdapi_->Init();
}

void TdSm::stop()
{
    BfLog(__FUNCTION__);

    if (tdapi_ == nullptr) {
        qFatal("tdapi_==nullptr");
        return;
    }

    tdapi_->RegisterSpi(nullptr);
    tdapi_->Release();

    tdapi_ = nullptr;
    delete tdspi_;
    tdspi_ = nullptr;
    emit statusChanged(TDSM_STOPPED);
}

QString TdSm::version()
{
    return CThostFtdcTraderApi::GetApiVersion();
}

void TdSm::resetData()
{
    tdspi_->resetData();
}

QString TdSm::genBfOrderId()
{
    int orderRef = tdspi_->getOrderRef();
    QString bfOrderId = CtpUtils::formatBfOrderId(tdspi_->getFrontId(), tdspi_->getSessionId(), orderRef);
    return bfOrderId;
}

// 登录填userID，其他填investorid=
void TdSm::login(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcReqUserLoginField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.UserID, userId_.toStdString().c_str(), sizeof(req.UserID) - 1);
        strncpy(req.Password, password_.toStdString().c_str(), sizeof(req.Password) - 1);

        int result = tdapi_->ReqUserLogin(&req, reqId);
        BfLog("CmdTdLogin,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

//目前，通过 ReqUserLogout 登出系统的话，会先将现有的连接断开，再重新建立一个新的连接(CTPSDK)
// logout之后会有一个disconnect/connect...先disableautologin
void TdSm::logout(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcUserLogoutField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.UserID, userId_.toStdString().c_str(), sizeof(req.UserID) - 1);

        int result = tdapi_->ReqUserLogout(&req, reqId);
        BfLog("CmdTdLogout,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::queryInstrument(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        // 重置gatewaymgr相关内存=
        g_sm->gatewayMgr()->resetData();

        CThostFtdcQryInstrumentField req;
        memset(&req, 0, sizeof(req));

        int result = tdapi_->ReqQryInstrument(&req, reqId);
        BfLog("CmdTdReqQryInstrument,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::queryAccount(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcQryTradingAccountField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.InvestorID, userId_.toStdString().c_str(), sizeof(req.InvestorID) - 1);

        int result = tdapi_->ReqQryTradingAccount(&req, reqId);
        BfLog("CmdTdReqQryTradingAccount,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::reqSettlementInfoConfirm(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcSettlementInfoConfirmField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.InvestorID, userId_.toStdString().c_str(), sizeof(req.InvestorID) - 1);

        int result = tdapi_->ReqSettlementInfoConfirm(&req, reqId);
        BfLog("CmdTdReqSettlementInfoConfirm,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::queryPosition(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcQryInvestorPositionField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.InvestorID, userId_.toStdString().c_str(), sizeof(req.InvestorID) - 1);

        int result = tdapi_->ReqQryInvestorPosition(&req, reqId);
        BfLog("CmdTdReqQryInvestorPosition,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::sendOrder(unsigned int delayTick, const BfSendOrderReq& bfReq)
{
    QString bfOrderId = genBfOrderId();
    sendOrder(delayTick, bfOrderId, bfReq);
}

void TdSm::sendOrder(unsigned int delayTick, QString bfOrderId, const BfSendOrderReq& bfReq)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcInputOrderField req;
        memset(&req, 0, sizeof(req));

        int orderRef, frontId, sessionId;
        CtpUtils::translateBfOrderId(bfOrderId, frontId, sessionId, orderRef);

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.InvestorID, userId_.toStdString().c_str(), sizeof(req.InvestorID) - 1);
        strncpy(req.OrderRef, QString::number(orderRef).toStdString().c_str(), sizeof(req.OrderRef) - 1);
        strncpy(req.InstrumentID, bfReq.symbol().c_str(), sizeof(req.InstrumentID) - 1);

        req.Direction = CtpUtils::translateDirection(bfReq.direction());
        req.CombOffsetFlag[0] = CtpUtils::translateOffset(bfReq.offset());
        req.OrderPriceType = CtpUtils::translatePriceType(bfReq.pricetype());
        req.LimitPrice = bfReq.price();
        req.VolumeTotalOriginal = bfReq.volume();

        req.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation; // 投机单=
        req.ContingentCondition = THOST_FTDC_CC_Immediately; // 立即发单=
        req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose; // 非强平=
        req.IsAutoSuspend = 0; // 非自动挂起=
        req.TimeCondition = THOST_FTDC_TC_GFD; // 今日有效=
        req.VolumeCondition = THOST_FTDC_VC_AV; // 任意成交量=
        req.MinVolume = 1; // 最小成交量为1=

        int result = tdapi_->ReqOrderInsert(&req, reqId);
        BfLog("CmdTdReqOrderInsert,bfOrderId=%s,reqId=%d,result=%d", bfOrderId.toStdString().c_str(), reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::cancelOrder(unsigned int delayTick, const BfCancelOrderReq& bfReq)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcInputOrderActionField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.InvestorID, userId_.toStdString().c_str(), sizeof(req.InvestorID) - 1);

        strncpy(req.InstrumentID, bfReq.symbol().c_str(), sizeof(req.InstrumentID) - 1);
        strncpy(req.ExchangeID, bfReq.exchange().c_str(), sizeof(req.InstrumentID) - 1);

        // frontid+sessionid+orderref || exchangeid + ordersysid
        QString bfOrderId = bfReq.bforderid().c_str();
        int frontId, sessionId, orderRef;
        CtpUtils::translateBfOrderId(bfOrderId, frontId, sessionId, orderRef);

        strncpy(req.OrderRef, QString::number(orderRef).toStdString().c_str(), sizeof(req.OrderRef) - 1);
        req.FrontID = frontId;
        req.SessionID = sessionId;

        req.ActionFlag = THOST_FTDC_AF_Delete;

        int result = tdapi_->ReqOrderAction(&req, reqId);
        BfLog("CmdTdReqOrderAction,bfOrderId=%s,reqId=%d,result=%d", bfOrderId.toStdString().c_str(), reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}

void TdSm::queryOrders(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcQryOrderField req;
        memset(&req, 0, sizeof(req));

        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.InvestorID, userId_.toStdString().c_str(), sizeof(req.InvestorID) - 1);

        int result = tdapi_->ReqQryOrder(&req, reqId);
        BfLog("CmdTdReqQryOrder,reqId=%d,result=%d", reqId, result);
        if (result == 0) {
            emit g_sm->gatewayMgr()->requestSent(reqId);
        }
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}
