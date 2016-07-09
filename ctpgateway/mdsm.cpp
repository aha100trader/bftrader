#include "mdsm.h"
#include "ThostFtdcMdApi.h"
#include "encode_utils.h"
#include "file_utils.h"
#include "gatewaymgr.h"
#include "servicemgr.h"
#include <QDateTime>
#include <QDir>

///////////
class MdSmSpi : public CThostFtdcMdSpi {
public:
    explicit MdSmSpi(MdSm* sm)
        : sm_(sm)
    {
    }

private:
    void OnFrontConnected() override
    {
        BfLog(__FUNCTION__);
        emit sm()->statusChanged(MDSM_CONNECTED);
    }

    // 如果网络异常，会直接调用OnFrontDisconnected，需要重置状态数据=
    // 网络错误当再次恢复时候，会自动重连重新走OnFrontConnected
    void OnFrontDisconnected(int nReason) override
    {
        BfLog("MdSmSpi::OnFrontDisconnected,nReason=0x%x", nReason);

        emit sm()->statusChanged(MDSM_DISCONNECTED);
    }

    // errorId=7，msg=CTP:还没有初始化=
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override
    {
        BfLog(__FUNCTION__);
        if (bIsLast) {
            if (isErrorRsp(pRspInfo, nRequestID)) {
                emit sm()->statusChanged(MDSM_LOGINFAIL);
            } else {
                emit sm()->statusChanged(MDSM_LOGINED);
            }
        }
    }

    // logout在tdapi里面是有效的,mdapi无效不用处理=
    void OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override {}

    void OnRspError(CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override
    {
        if (bIsLast) {
            BfLog(__FUNCTION__);
            isErrorRsp(pRspInfo, nRequestID);
        }
    }

    // 订阅成功了也会调用,目前是不管啥都返回订阅成功=
    void OnRspSubMarketData(
        CThostFtdcSpecificInstrumentField* pSpecificInstrument,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override
    {
        if (!isErrorRsp(pRspInfo, nRequestID) && pSpecificInstrument) {
            QString symbol = pSpecificInstrument->InstrumentID;
            got_symbols_ << symbol;
        }

        if (bIsLast) {
            QString symbols;
            for (auto symbol : got_symbols_) {
                symbols = symbols + symbol + ";";
            }
            BfLog("total subscribled symbols:%d,reqId=%d,(%s)", got_symbols_.length(), nRequestID, qPrintable(symbols));
        }
    }

    void OnRspUnSubMarketData(
        CThostFtdcSpecificInstrumentField* pSpecificInstrument,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override {}

    void OnRtnDepthMarketData(
        CThostFtdcDepthMarketDataField* pDepthMarketData) override
    {
        QString id = pDepthMarketData->InstrumentID;
        auto rb = g_sm->gatewayMgr()->getRingBuffer(id);
        if (!isValidTick(rb, pDepthMarketData)) {
            return;
        }
        auto preTick = (CThostFtdcDepthMarketDataField*)rb->get(rb->head());
        void* curTick = rb->put(pDepthMarketData);
        emit g_sm->gatewayMgr()->gotTick(curTick, preTick);
    }

private:
    bool isErrorRsp(CThostFtdcRspInfoField* pRspInfo, int reqId)
    {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            BfLog("reqid=%d,errorId=%d，msg=%s", reqId, pRspInfo->ErrorID,
                gbk2utf16(pRspInfo->ErrorMsg).toUtf8().constData());
            emit g_sm->gatewayMgr()->gotGatewayError(pRspInfo->ErrorID, gbk2utf16(pRspInfo->ErrorMsg), QString().sprintf("reqId=%d", reqId));
            return true;
        }
        return false;
    }

    MdSm* sm() { return sm_; }

    void resetData()
    {
        g_sm->checkCurrentOn(ServiceMgr::BLOGIC);

        got_symbols_.clear();
        g_sm->gatewayMgr()->freeRingBuffer();
    }

    // 每次收盘/断网/崩溃/退出等等,都会清空ringbufer，需要重来一下下面的逻辑=
    // 1.确定第一个有效tick：交易日期+时间，和当前时间偏离在3分钟之外：无效=
    // 2.确定后续tick是否有效：和上一个tick相比，总成交量不变的,如果偏移在3分钟之外：无效=
    // 规则2是过滤收盘后没断开网络之前发来的东西（这个可以改成前tick存在，而vol=0，校验时间偏移）
    // 规则1是过滤开盘前发来的之前的交易数据

    bool isValidTick(RingBuffer* rb, CThostFtdcDepthMarketDataField* curTick)
    {
        // 对于一些特殊的sim如周末回放tick的，不做过滤=
        if (!sm_->filterTick_) {
            return true;
        }

        auto preTick = (CThostFtdcDepthMarketDataField*)rb->get(rb->head());
        if (!preTick || preTick->Volume == curTick->Volume) {
            // ActionDay 指当时的系统日期
            // TradingDay 是指当时的交易日期，夜盘算下一个交易的=
            QDateTime curDateTime = QDateTime::currentDateTime();
            QString tickDateTimeStr = QString().sprintf("%s %s.%03d", curTick->ActionDay, curTick->UpdateTime, curTick->UpdateMillisec);
            QDateTime tickDateTime = QDateTime::fromString(tickDateTimeStr, "yyyyMMdd hh:mm:ss.zzz");
            qint64 delta = qAbs(curDateTime.msecsTo(tickDateTime));
            if (delta >= 5 * 60 * 1000) {
                return false;
            }
        }
        return true;
    }

private:
    MdSm* sm_;
    QStringList got_symbols_;

    friend MdSm;
};

///////////
MdSm::MdSm(QObject* parent)
    : QObject(parent)
{
}

MdSm::~MdSm() {}

bool MdSm::init(QString userId,
    QString password,
    QString brokerId,
    QString frontMd,
    QString flowPathMd,
    bool filterTick)
{
    userId_ = userId;
    password_ = password;
    brokerId_ = brokerId;
    frontMd_ = frontMd;
    flowPathMd_ = flowPathMd;
    filterTick_ = filterTick;

    // check
    if (userId_.length() == 0 || password_.length() == 0 || brokerId_.length() == 0 || frontMd_.length() == 0 || flowPathMd_.length() == 0) {
        return false;
    }
    return true;
}

void MdSm::start()
{
    BfLog(__FUNCTION__);

    if (mdapi_ != nullptr) {
        qFatal("mdapi_!=nullptr");
        return;
    }

    QDir dir;
    dir.mkpath(flowPathMd_);
    mdapi_ = CThostFtdcMdApi::CreateFtdcMdApi(flowPathMd_.toStdString().c_str());
    mdspi_ = new MdSmSpi(this);
    mdapi_->RegisterSpi(mdspi_);
    mdapi_->RegisterFront((char*)qPrintable(frontMd_));
    mdapi_->Init();
}

void MdSm::stop()
{
    BfLog(__FUNCTION__);

    if (mdapi_ == nullptr) {
        qFatal("mdapi_==nullptr");
        return;
    }

    this->mdapi_->RegisterSpi(nullptr);
    this->mdapi_->Release();

    this->mdapi_ = nullptr;
    delete this->mdspi_;
    this->mdspi_ = nullptr;
    emit this->statusChanged(MDSM_STOPPED);
}

QString MdSm::version()
{
    return CThostFtdcMdApi::GetApiVersion();
}

void MdSm::resetData()
{
    this->mdspi_->resetData();
}

void MdSm::login(unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        CThostFtdcReqUserLoginField req;
        memset(&req, 0, sizeof(req));
        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.UserID, userId_.toStdString().c_str(), sizeof(req.UserID) - 1);
        strncpy(req.Password, password_.toStdString().c_str(), sizeof(req.Password) - 1);
        int result = mdapi_->ReqUserLogin(&req, reqId);
        BfLog("CmdMdLogin,reqId=%d,result=%d", reqId, result);
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

void MdSm::subscrible(QStringList symbols, unsigned int delayTick)
{
    BfLog(__FUNCTION__);

    std::function<int(int)> fn = [=](int reqId) -> int {
        (void)reqId;
        QList<std::string> std_symbols;
        char** c_symbols = new char*[symbols.length()];
        for (int i = 0; i < symbols.length(); i++) {
            std_symbols.append(symbols.at(i).toStdString());
            c_symbols[i] = (char*)std_symbols.at(i).c_str();
        }
        int result = mdapi_->SubscribeMarketData(c_symbols, symbols.length());
        delete[] c_symbols;
        BfLog("CmdMdSubscrible,result=%d", result);
        return result;
    };

    CtpCmd* cmd = new CtpCmd;
    cmd->fn = fn;
    cmd->delayTick = delayTick;
    g_sm->gatewayMgr()->runCmd(cmd);
}
