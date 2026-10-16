#include "modules/musig/MusigAccountController.h"

MusigAccountController::MusigAccountController(const QString &account, MusigAccountWidget *widget) {
    Q_UNUSED(account);
    Q_UNUSED(widget);
}

void MusigAccountController::init(const QString &account) {
    Q_UNUSED(account);
}

void MusigAccountController::loadPanels() {
}

auto MusigAccountController::screen(const QString &screen) -> std::optional<qontrol::Screen *> {
    Q_UNUSED(screen);
    return std::nullopt;
}

auto MusigAccountController::getAccount() -> std::optional<rust::Box<MusigAccount>> & {
    return m_account;
}

auto MusigAccountController::getCoins() -> rust::Vec<RustCoin> {
    return {};
}

auto MusigAccountController::getPaymentHistory() -> rust::Vec<RustTx> {
    return {};
}

auto MusigAccountController::newAddress() -> rust::String {
    return {};
}

auto MusigAccountController::network() -> Network {
    return Network::Regtest;
}

auto MusigAccountController::getDescriptor() -> rust::String {
    return {};
}

auto MusigAccountController::cosignerFingerprints() -> QStringList {
    return {};
}

auto MusigAccountController::addressVerifyInfo(const QString &address) -> AddressVerifyInfo {
    Q_UNUSED(address);
    return {};
}

auto MusigAccountController::electrumUrl() -> QString {
    return {};
}

auto MusigAccountController::roundStatus() const -> MusigRoundStatus {
    return {};
}

auto MusigAccountController::psbtInFlight() const -> QString {
    return {};
}

auto MusigAccountController::spendLeaves(const QStringList &outpoints) -> MusigSpendLeaves {
    Q_UNUSED(outpoints);
    return {};
}

void MusigAccountController::finalizeIfReady() {
}

void MusigAccountController::restartRounds() {
}

// ===== Slots =====

void MusigAccountController::loadPanel(const QString &name) {
    Q_UNUSED(name);
}

void MusigAccountController::pollCoins() {
}

void MusigAccountController::handleNotification(Notification notif) {
    Q_UNUSED(notif);
}

void MusigAccountController::updateCoinLabel(const QString &outpoint, const QString &label) {
    Q_UNUSED(outpoint);
    Q_UNUSED(label);
}

void MusigAccountController::setElectrumUrl(const QString &url) {
    Q_UNUSED(url);
}

void MusigAccountController::startElectrum() {
}

void MusigAccountController::stop() {
}

auto MusigAccountController::simulateTx(TransactionTemplate tx) -> TransactionSimulation {
    Q_UNUSED(tx);
    return {};
}

void MusigAccountController::prepareTransaction(TransactionTemplate tx, MusigSpendLeaf spend) {
    Q_UNUSED(tx);
    Q_UNUSED(spend);
}

void MusigAccountController::signRound() {
}

void MusigAccountController::cancelSigning() {
}

void MusigAccountController::broadcast() {
}

void MusigAccountController::onKeysSigned(ReqId req_id, const QString &psbt_v2_hex) {
    Q_UNUSED(req_id);
    Q_UNUSED(psbt_v2_hex);
}

void MusigAccountController::onKeysError(std::optional<ReqId> req_id, const QString &message,
                                         plugin::SignError kind) {
    Q_UNUSED(req_id);
    Q_UNUSED(message);
    Q_UNUSED(kind);
}

void MusigAccountController::onKeysCancelled(ReqId req_id) {
    Q_UNUSED(req_id);
}

void MusigAccountController::onBroadcastResult(TxResult result) {
    Q_UNUSED(result);
}

void MusigAccountController::historyClicked() {
}

void MusigAccountController::coinsClicked() {
}

void MusigAccountController::sendClicked() {
}

void MusigAccountController::receiveClicked() {
}

void MusigAccountController::settingsClicked() {
}
