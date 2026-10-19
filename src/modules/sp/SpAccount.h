#pragma once

#include "interfaces/account.h"

#include <QObject>

class AccountController;

class SpAccount final : public IAccount {
    Q_OBJECT

public:
    explicit SpAccount(AccountController *controller);

    [[nodiscard]] bool implemented() const override;
    ReqId name() override;
    ReqId info() override;
    ReqId coins() override;
    ReqId balance() override;
    ReqId newReceiveAddress() override;
    ReqId newChangeAddress() override;
    ReqId descriptor() override;
    ReqId raw(const QByteArray &request) override;

private slots:
    void onControllerCoinsUpdate();
    void onControllerBalanceUpdate(uint64_t balance);

protected:
    bool accountReady(ReqId req_id);

private:
    ReqId nextReqId();
    void emitCoins(ReqId req_id);
    void emitBalance(ReqId req_id);

    AccountController *m_controller = nullptr;
    ReqId m_next_req_id = 0;
};
