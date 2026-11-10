#pragma once

#include "account/services/ControllerServices.h"
#include "account/services/Conversions.h"

#include <QByteArray>
#include <QDebug>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <common.h>
#include <optional>
#include <silent.h>
#include <string>
#include <utility>

namespace account {

// The service context of a built-in account whose controller holds a `Rust` account (the
// SP Account or the MusigAccount, which share the service bridge methods). It reads and
// works on that account itself, notified by the controller's updateCoins(). Reads and pure
// crafting run on the GUI thread, on RAM state; every other request runs on a worker the
// controller waits for before it drops the account.
template <typename Controller, typename Rust> class AccountServices : public ControllerServices {
public:
    AccountServices(const QString &account, Controller *controller)
        : ControllerServices(account, controller),
          m_owner(controller) {
        connect(controller, &Controller::updateCoins, this, &ControllerServices::onCoinsUpdated,
                qontrol::UNIQUE);
    }

    auto craft(const backend::TransactionTemplate &transaction) -> backend::CraftResult override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            return {.transaction = transaction, .completion = {.error = busy()}};
        }
        return services::toCraftResult(
            wallet->craft_transaction(services::toTemplate(transaction)));
    }

    auto finalize(const backend::TransactionTemplate &transaction)
        -> std::optional<QByteArray> override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            return std::nullopt;
        }
        auto psbt = wallet->finalize_transaction(services::toTemplate(transaction));
        if (!psbt.is_ok) {
            auto reason = services::toQString(psbt.error);
            qWarning() << "finalize_transaction failed:" << reason;
            emit finalizeFailed(reason);
            return std::nullopt;
        }
        return services::toBytes(psbt.psbt);
    }

    void selectCoins(backend::RequestId token, backend::TransactionTemplate transaction) override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            emit failed(token, busy());
            return;
        }
        startWork(token, QThread::create([this, wallet, token,
                                          request = services::toTemplate(transaction)]() mutable {
                      emit coinsSelected(token,
                                         services::toCraftResult(
                                             wallet->select_transaction_coins(std::move(request))));
                  }));
    }

    void newAddress(backend::RequestId token, QString kind) override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            emit failed(token, busy());
            return;
        }
        startWork(token, QThread::create([this, wallet, token, kind = kind.toStdString()] {
                      auto result = wallet->new_receive_address(rust::Str(kind));
                      emit addressFinished(token, result.is_ok, services::toAddress(result.address),
                                           services::toQString(result.error));
                  }));
    }

    void setLabel(backend::RequestId token, backend::LabelKey key, QString text) override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            emit failed(token, busy());
            return;
        }
        startWork(token,
                  QThread::create([this, wallet, token, kind = services::toLabelKind(key.kind),
                                   id = key.id.toStdString(), text = text.toStdString()] {
                      auto result = wallet->set_label(kind, rust::String(id), rust::String(text));
                      emit labelEditFinished(token, result.is_ok, result.changed,
                                             services::toQString(result.error));
                  }));
    }

    // Online only while the controller's Electrum connection serves reads.
    void fetchTransactionDetails(backend::RequestId token, QString txid) override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            emit failed(token, unavailable());
            return;
        }
        startWork(token, QThread::create([this, wallet, token, id = txid.toStdString(),
                                          online = m_owner->electrumUsable()] {
                      emit detailsFinished(token, wallet->tx_details(rust::Str(id), online));
                  }));
    }

    void broadcast(backend::RequestId token, QByteArray transaction) override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            emit failed(token, busy());
            return;
        }
        startWork(token, QThread::create([this, wallet, token, transaction] {
                      emit broadcastDone(
                          token, services::toBroadcastResult(
                                     wallet->broadcast_report(services::toSlice(transaction))));
                  }));
    }

    // The module's controller says what its kind of backup holds.
    [[nodiscard]] auto backupDescription() const -> backend::BackupDescription override {
        return Controller::backupDescription();
    }

    // On a worker: it reads the account's stores.
    void backup(backend::RequestId token) override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            emit failed(token, busy());
            return;
        }
        startWork(token, QThread::create([this, wallet, token] {
                      auto result = wallet->backup();
                      if (result.is_ok) {
                          emit backupCreated(token, services::toBytes(result.bytes),
                                             services::toQStrings(result.excluded));
                      } else {
                          emit failed(token, services::toQString(result.error));
                      }
                  }));
    }

    void inspectBackup(backend::RequestId token, const QByteArray &backup, const QStringList &xpubs,
                       const QByteArray &mnemonic) override {
        startWork(token, QThread::create([this, token, backup, xpubs, mnemonic] {
                      auto contents = ::inspect_account_backup(services::toSlice(backup),
                                                               services::toRustStrings(xpubs),
                                                               services::toRustString(mnemonic));
                      if (contents.failure == BackupFailure::Locked) {
                          emit backupLocked(token, services::toQStrings(contents.derivation_paths));
                      } else if (contents.is_ok) {
                          emit backupInspected(token, services::toBackupInfo(contents));
                      } else {
                          emit failed(token,
                                      services::backupError(contents.failure, contents.error));
                      }
                  }));
    }

protected:
    // The account, null while the controller has none (loading, closing, or handed to an
    // endpoint change).
    [[nodiscard]] auto rustAccount() const -> Rust * {
        auto &account = m_owner->getAccount();
        if (!account.has_value() || !account.value()->is_ok()) {
            return nullptr;
        }
        return &*account.value();
    }

    [[nodiscard]] auto owner() const -> Controller * {
        return m_owner;
    }

    static auto busy() -> QString {
        return QStringLiteral("Account is busy or closed");
    }

    // The snapshot is taken and converted on a worker the controller waits for before it
    // drops the account.
    auto startFundsRead() -> bool override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            return false;
        }
        return startFundsWorker([wallet] { return readFunds(*wallet); });
    }

    static auto readFunds(const Rust &wallet) -> std::optional<Funds> {
        auto snapshot = wallet.funds_snapshot();
        if (!snapshot.is_ok) {
            return std::nullopt;
        }
        Funds funds{.balance = services::toBalance(snapshot.balance),
                    .historyComplete = snapshot.history_complete};
        QSet<QString> spendable;
        for (const auto &outpoint : snapshot.spendable) {
            spendable.insert(services::toQString(outpoint));
        }
        for (const auto &coin : snapshot.coins) {
            funds.coins.append(
                services::toCoin(coin, spendable.contains(services::toQString(coin.outpoint))));
        }
        for (const auto &tx : snapshot.history) {
            funds.history.append(services::toTransaction(tx));
        }
        return funds;
    }

    auto readLabels() -> std::optional<QList<backend::Label>> override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            return std::nullopt;
        }
        auto snapshot = wallet->labels();
        if (!snapshot.is_ok) {
            return std::nullopt;
        }
        QList<backend::Label> labels;
        for (const auto &entry : snapshot.labels) {
            labels.append(services::toLabel(entry));
        }
        return labels;
    }

    auto readReceiving() -> std::optional<Receiving> override {
        auto *wallet = rustAccount();
        if (wallet == nullptr) {
            return std::nullopt;
        }
        Receiving receiving;
        for (const auto &kind : wallet->receive_kinds()) {
            receiving.kinds.append(services::toAddressKind(kind));
        }
        for (const auto &address : wallet->receive_addresses()) {
            receiving.addresses.append(services::toAddress(address));
            receiving.verification.insert(services::toQString(address.address),
                                          services::toVerification(address));
        }
        return receiving;
    }

private:
    Controller *m_owner;
};

} // namespace account
