#pragma once

#include "account/services/AccountServices.h"
#include "interfaces/backend/types.h"
#include "modules/multikey/MusigAccountController.h"

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QtGlobal>
#include <silent.h>

namespace backend {
class IBlockchain;
} // namespace backend

namespace musig {

// A Multikey account's service context. A broadcast marks the saved transaction it
// belongs to and ends the signing session it finalized, so the controller runs it
// (MusigAccountController::startBroadcast()), refusing it as its own broadcast was.
// Once attachChain() hands it a blockchain backend plugin's service, the account syncs
// from that service instead of its Electrum scanner: what the service reports reaches the
// Rust account on every coinsChanged() and stateChanged(), a receive address is revealed to
// it before it is handed out, and a broadcast it accepts is recorded.
class MusigServices final : public account::AccountServices<MusigAccountController, MusigAccount> {
    Q_OBJECT

public:
    MusigServices(const QString &account, MusigAccountController *controller);

    void broadcast(backend::RequestId token, QByteArray transaction) override;
    void newAddress(backend::RequestId token, QString kind) override;
    // Syncs the account from `chain`, a blockchain backend plugin's service watching it with
    // gap limit `gap_limit`, or from the Electrum scanner again with `chain` null. A receive
    // address still waiting for the old service's reveal fails.
    void attachChain(backend::IBlockchain *chain, quint32 gap_limit);
    // Hands the account what the attached service reports, or takes it back to the
    // Electrum scanner with none attached: done on every change of the service, and again
    // once the account comes back from an endpoint change.
    void feedChain();

public slots:
    void onChainChanged();
    // The reveal of a receive address answered: the address is handed out.
    void onChainCompleted(backend::RequestId request_id);
    void onChainError(backend::RequestId request_id, const QString &message);
    // The service accepted a transaction: the account records it as broadcast.
    void onChainBroadcastFinished(backend::RequestId request_id,
                                  const backend::BroadcastResult &result);

protected:
    // Hands out receive index `index` on a worker, answering `token`.
    void takeReceive(backend::RequestId token, quint32 index);

private:
    struct Reveal {
        backend::RequestId token = 0;
        quint32 index = 0;
    };

    QPointer<backend::IBlockchain> m_chain;
    quint32 m_gap_limit = 0;
    // Receive addresses waiting for their reveal, by the reveal's request id.
    QHash<backend::RequestId, Reveal> m_reveals;
};

} // namespace musig
