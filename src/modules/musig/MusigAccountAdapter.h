#pragma once

#include "interfaces/account.h"

#include <QByteArray>
#include <cstdint>

class MusigAccountController;

// The IAccount, named apart from the Rust opaque ::MusigAccount.
class MusigAccountAdapter final : public IAccount {
    Q_OBJECT

public:
    explicit MusigAccountAdapter(MusigAccountController *controller);
    [[nodiscard]] auto implemented() const -> bool override;
    auto name() -> ReqId override;
    auto info() -> ReqId override;
    auto coins() -> ReqId override;
    auto balance() -> ReqId override;
    auto newReceiveAddress() -> ReqId override;
    auto newChangeAddress() -> ReqId override;
    auto raw(const QByteArray &request) -> ReqId override;

public slots:
    void onControllerCoinsUpdate();
    void onControllerBalanceUpdate(uint64_t balance);

protected:
    auto accountReady(ReqId req_id) -> bool;
    auto nextReqId() -> ReqId;
    void emitCoins(ReqId req_id);
    void emitBalance(ReqId req_id);

private:
    MusigAccountController *m_controller = nullptr;
    ReqId m_next_req_id = 0;
};
