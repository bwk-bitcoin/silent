#include "modules/musig/views/Send.h"

namespace musig::view {

Send::Send(MusigAccountController *ctrl) {
    Q_UNUSED(ctrl);
}

void Send::init() {
}

void Send::doConnect() {
}

void Send::view() {
}

// ===== Slots =====

void Send::onProcess() {
}

void Send::onAddOutput() {
}

void Send::onCoinsSelected(const QStringList &outpoints) {
    Q_UNUSED(outpoints);
}

void Send::onSpendLeafChosen(int index) {
    Q_UNUSED(index);
}

void Send::onPrepareClicked() {
}

void Send::onPrepared(const QString &psbt_v2_hex, const QString &txid) {
    Q_UNUSED(psbt_v2_hex);
    Q_UNUSED(txid);
}

void Send::onPrepareFailed(const QString &message) {
    Q_UNUSED(message);
}

void Send::onRoundChanged(MusigRoundStatus status) {
    Q_UNUSED(status);
}

void Send::onSignRoundClicked() {
}

void Send::onSigningFailed(const QString &message, plugin::SignError kind) {
    Q_UNUSED(message);
    Q_UNUSED(kind);
}

void Send::onSigningCancelled() {
}

void Send::onTransactionReady(const QString &tx_hex) {
    Q_UNUSED(tx_hex);
}

void Send::onBroadcastClicked() {
}

void Send::onBroadcastDone(TxResult result) {
    Q_UNUSED(result);
}

void Send::onCancelClicked() {
}

} // namespace musig::view
