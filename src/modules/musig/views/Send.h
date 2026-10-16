#pragma once

#include "interfaces/types.h"

#include <QString>
#include <QStringList>
#include <Qontrol>
#include <silent.h>

class MusigAccountController;

namespace musig::view {

class Send : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Send(MusigAccountController *ctrl);

public slots:
    void onProcess();
    void onAddOutput();
    // Refreshes the spend path picker from controller->spendLeaves() for the inputs.
    void onCoinsSelected(const QStringList &outpoints);
    void onSpendLeafChosen(int index);
    void onPrepareClicked();
    void onPrepared(const QString &psbt_v2_hex, const QString &txid);
    void onPrepareFailed(const QString &message);
    void onRoundChanged(MusigRoundStatus status);
    void onSignRoundClicked();
    void onSigningFailed(const QString &message, plugin::SignError kind);
    void onSigningCancelled();
    void onTransactionReady(const QString &tx_hex);
    void onBroadcastClicked();
    void onBroadcastDone(TxResult result);
    void onCancelClicked();

protected:
    void init() override;
    void doConnect() override;
    void view() override;
};

} // namespace musig::view
