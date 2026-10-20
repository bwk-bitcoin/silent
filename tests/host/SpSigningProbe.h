#pragma once

#include "interfaces/keys.h"
#include "modules/sp/AccountController.h"
#include <QApplication>
#include <QDialog>
#include <QList>
#include <QString>
#include <QThread>
#include <utility>

namespace smoke {

class SpController final : public AccountController {
public:
    explicit SpController(const QString &name) : AccountController(nullptr) {
        getAccount() = ::new_account(rust::String(name.toStdString()));
        if (!getAccount().value()->is_ok()) {
            qFatal("SP fixture account must load");
        }
    }

    ~SpController() override {
        discardWorkers();
    }

    void startWorker(QThread *worker) override {
        workers.append(worker);
    }

    void discardWorkers() {
        qDeleteAll(workers);
        workers.clear();
    }

    auto simulateTx(TransactionTemplate tx) -> TransactionSimulation override {
        ++simulations;
        return AccountController::simulateTx(std::move(tx));
    }

    int simulations = 0;
    QList<QThread *> workers;
};

class DeferredKeys final : public IKeys {
public:
    [[nodiscard]] auto keys([[maybe_unused]] Network network) const -> QList<KeyEntry> override {
        return {};
    }

    auto newRequest() -> ReqId override {
        return ++next_request;
    }

    void sign(ReqId req_id, [[maybe_unused]] Network network, const QString &psbt_v2_hex,
              [[maybe_unused]] const QStringList &descriptors,
              [[maybe_unused]] const QString &wallet_name,
              [[maybe_unused]] const QString &fingerprint) override {
        request = req_id;
        psbt = psbt_v2_hex;
        ++requests;
    }

    void signOnDevice([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                      [[maybe_unused]] const QString &psbt_v2_hex,
                      [[maybe_unused]] const QString &descriptor,
                      [[maybe_unused]] const QString &fingerprint) override {
        qFatal("SP fixture must use the shared sign request");
    }

    void verifyAddress([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                       [[maybe_unused]] const QString &descriptor,
                       [[maybe_unused]] const QString &path,
                       [[maybe_unused]] const QString &address,
                       [[maybe_unused]] const QString &fingerprint) override {
    }

    void verifyOnDevice([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                        [[maybe_unused]] const QString &descriptor,
                        [[maybe_unused]] const QString &path,
                        [[maybe_unused]] const QString &address,
                        [[maybe_unused]] const QString &fingerprint) override {
    }

    void spDescriptor([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                      [[maybe_unused]] const QString &fingerprint) override {
    }

    void xpub([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
              [[maybe_unused]] const QString &fingerprint,
              [[maybe_unused]] const QString &path) override {
    }

    void xpubOnDevice([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                      [[maybe_unused]] const QString &fingerprint,
                      [[maybe_unused]] const QString &path) override {
    }

    [[nodiscard]] auto cachedXpub([[maybe_unused]] Network network,
                                  [[maybe_unused]] const QString &fingerprint,
                                  [[maybe_unused]] const QString &path) const -> QString override {
        return {};
    }

    [[nodiscard]] auto registrationHmac([[maybe_unused]] Network network,
                                        [[maybe_unused]] const QString &fingerprint,
                                        [[maybe_unused]] const QString &descriptor) const
        -> QString override {
        return {};
    }

    auto storeRegistrationHmac([[maybe_unused]] Network network,
                               [[maybe_unused]] const QString &fingerprint,
                               [[maybe_unused]] const QString &descriptor,
                               [[maybe_unused]] const QString &hmac) -> bool override {
        return false;
    }

    void registerDescriptor([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                            [[maybe_unused]] const QString &fingerprint,
                            [[maybe_unused]] const QString &descriptor) override {
    }

    void checkDescriptorRegistered([[maybe_unused]] ReqId req_id, [[maybe_unused]] Network network,
                                   [[maybe_unused]] const QString &fingerprint,
                                   [[maybe_unused]] const QString &descriptor) override {
    }

    ReqId next_request = 0;
    ReqId request = 0;
    int requests = 0;
    QString psbt;
};

class ModalAnswer final : public QObject {
    Q_OBJECT

public:
    bool accept = true;
    int answers = 0;

public slots:

    void onTimeout() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog != nullptr) {
            ++answers;
            dialog->done(accept ? QDialog::Accepted : QDialog::Rejected);
        }
    }
};

void spSigningSmoke(const QString &name);

} // namespace smoke
