#pragma once

#include "interfaces/types.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <Qontrol>
#include <cstdint>
#include <optional>
#include <silent.h>

class MusigAccountWidget;
class QThread;

// Owns the Rust account and the PSBT in flight. Drives MuSig2 as one
// keys()->sign() per round: Nonces, then PartialSigs, then finalize at Ready. On
// SignError::MusigSession it drops every nonce and partial sig and goes back to
// round 1 over the prepared PSBT.
class MusigAccountController : public QObject {
    Q_OBJECT

public:
    MusigAccountController(const QString &account, MusigAccountWidget *widget);
    ~MusigAccountController() override = default;
    void init(const QString &account);
    void loadPanels();
    auto screen(const QString &screen) -> std::optional<qontrol::Screen *>;
    auto getAccount() -> std::optional<rust::Box<MusigAccount>> &;
    auto getCoins() -> rust::Vec<RustCoin>;
    auto getPaymentHistory() -> rust::Vec<RustTx>;
    auto newAddress() -> rust::String;
    auto network() -> Network;
    auto getDescriptor() -> rust::String;
    auto cosignerFingerprints() -> QStringList;
    auto addressVerifyInfo(const QString &address) -> AddressVerifyInfo;
    auto electrumUrl() -> QString;
    [[nodiscard]] auto roundStatus() const -> MusigRoundStatus;
    [[nodiscard]] auto psbtInFlight() const -> QString;
    // MusigAccount::spend_leaves for the selected coins.
    auto spendLeaves(const QStringList &outpoints) -> MusigSpendLeaves;

signals:
    void updateCoins(CoinState coins);
    void updateBalance(uint64_t balance);
    void electrumConnected(QString address);
    void electrumDisconnected();
    // The Electrum URL was saved.
    void electrumUrlChanged();
    void notificationReceived(Notification notif);
    // A prepared PSBT entered round 1.
    void prepared(QString psbt_v2_hex, QString txid);
    void prepareFailed(QString message);
    void roundChanged(MusigRoundStatus status);
    void signingFailed(QString message, plugin::SignError kind);
    void signingCancelled();
    // Finalized at Ready.
    void transactionReady(QString tx_hex);
    // finalize_psbt failed at Ready.
    void finalizeFailed(QString message);
    void broadcastDone(TxResult result);
    // Background teardown done, the owner may delete the widget.
    void stopped();

public slots:
    void loadPanel(const QString &name);
    void pollCoins();
    void handleNotification(Notification notif);
    void updateCoinLabel(const QString &outpoint, const QString &label);
    void setElectrumUrl(const QString &url);
    void startElectrum();
    void stopElectrum();
    void stop();
    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation;
    // Prepares, then restricts the PSBT to `spend` before round 1.
    void prepareTransaction(TransactionTemplate tx, MusigSpendLeaf spend);
    // keys()->sign() for the current round of the PSBT in flight.
    void signRound();
    void cancelSigning();
    void broadcast();
    void onKeysSigned(ReqId req_id, const QString &psbt_v2_hex);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);
    void onKeysCancelled(ReqId req_id);
    void onBroadcastResult(const TxResult &result);
    void onBroadcastThreadFinished();
    void historyClicked();
    void coinsClicked();
    void sendClicked();
    void receiveClicked();
    void settingsClicked();

protected:
    void finalizeIfReady();
    void restartRounds();

private:
    QPointer<qontrol::Panel> m_current_panel;
    QHash<QString, qontrol::Panel *> m_panels;
    MusigAccountWidget *m_widget = nullptr;
    std::optional<rust::Box<MusigAccount>> m_account = std::nullopt;
    QThread *m_notif_thread = nullptr;
    QThread *m_broadcast_thread = nullptr;
    QString m_prepared_psbt_v2_hex;
    QString m_psbt_v2_hex;
    QString m_tx_hex;
    ReqId m_sign_req_id = 0;
    bool m_init = false;
    bool m_stopping = false;
};
