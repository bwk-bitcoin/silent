#include "ExternalSpendConsumer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <interfaces/account.h>
#include <interfaces/instance.h>

namespace smoke {
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("external spend consumer: %s", message);
    }
}

// Explicitly deferred mock backend, used only for lifecycle, never signing evidence.
class DeferredAccount final : public IAccount {
public:
    [[nodiscard]] auto implemented() const -> bool override {
        return true;
    }

    auto name() -> ReqId override {
        return ++m_next;
    }

    auto info() -> ReqId override {
        return ++m_next;
    }

    auto coins() -> ReqId override {
        return ++m_next;
    }

    auto balance() -> ReqId override {
        return ++m_next;
    }

    auto newReceiveAddress() -> ReqId override {
        return ++m_next;
    }

    auto newChangeAddress() -> ReqId override {
        return ++m_next;
    }

    auto descriptor() -> ReqId override {
        return ++m_next;
    }

    auto raw([[maybe_unused]] const QByteArray &request) -> ReqId override {
        return ++m_next;
    }

private:
    ReqId m_next = 0;
};

class DeferredInstance final : public IInstance {
public:
    [[nodiscard]] auto id() const -> QString override {
        return QStringLiteral("external-lifecycle-mock");
    }

    void stop() override {
    }

    auto account() -> IAccount * override {
        return &backend;
    }

    auto feed() -> IFeed * override {
        return nullptr;
    }

    auto signer() -> ISigner * override {
        return nullptr;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }

    DeferredAccount backend;
};
} // namespace

ExternalSpendConsumer::ExternalSpendConsumer(Host *host, IModule *caller)
    : m_host(host),
      m_caller(caller) {
}

void ExternalSpendConsumer::wait() const {
    QElapsedTimer deadline;
    deadline.start();
    while (!m_answered && deadline.elapsed() < 60000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(m_answered, "public request timed out");
}

void ExternalSpendConsumer::run(const QString &fixture) {
    QFile file(fixture);
    require(file.open(QIODevice::ReadOnly), "open public request fixture");
    auto data = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    m_request.network = Network::Regtest;
    m_request.psbtV2Hex = data.value(QStringLiteral("psbt")).toString();
    m_request.walletName = QStringLiteral("external-consumer");
    auto selections = data.value(QStringLiteral("inputs")).toArray();
    for (auto *instance : m_host->instances(m_caller)) {
        auto *account = instance->account();
        if (account == nullptr || !account->implemented()) {
            continue;
        }
        connect(account, qOverload<ReqId, QList<plugin::Coin>>(&IAccount::coins), this,
                &ExternalSpendConsumer::onCoins, Qt::UniqueConnection);
        account->coins();
    }
    QElapsedTimer deadline;
    deadline.start();
    while (m_request.inputs.size() != selections.size() && deadline.elapsed() < 60000) {
        m_request.inputs.clear();
        for (auto value : selections) {
            auto selection = value.toObject();
            for (const auto &coin : m_coins) {
                if (coin.outpoint != selection.value(QStringLiteral("outpoint")).toString()) {
                    continue;
                }
                for (const auto &variant : coin.variants) {
                    if (variant.pathId == selection.value(QStringLiteral("path")).toString()) {
                        require(coin.network == Network::Regtest, "coin carries explicit network");
                        require(variant.psbtV2Hex ==
                                    selection.value(QStringLiteral("envelope")).toString(),
                                "public metadata round trips across Rust/CXX/Qt");
                        m_request.inputs.append(
                            {.inputIndex = static_cast<quint32>(m_request.inputs.size()),
                             .network = coin.network,
                             .variant = variant});
                    }
                }
            }
        }
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(m_request.inputs.size() == selections.size(),
            "discover both funded accounts through public handles");
    m_keys = m_host->keys(m_caller);
    m_transactions = m_host->transactions(m_caller);
    require(m_keys != nullptr && m_transactions != nullptr,
            "public coordination and transaction services");
    connect(m_keys, &IKeys::coordinated, this, &ExternalSpendConsumer::onCoordinated,
            Qt::UniqueConnection);
    connect(m_keys, &IKeys::coordinationError, this, &ExternalSpendConsumer::onCoordinationError,
            Qt::UniqueConnection);
    connect(m_keys, &IKeys::attemptRetirement, this, &ExternalSpendConsumer::onAttemptRetirement,
            Qt::UniqueConnection);
    connect(m_transactions, &ITransactions::error, this,
            &ExternalSpendConsumer::onCoordinationError, Qt::UniqueConnection);
    connect(m_transactions, &ITransactions::finalized, this, &ExternalSpendConsumer::onFinalized,
            Qt::UniqueConnection);
    connect(m_transactions, &ITransactions::broadcasted, this,
            &ExternalSpendConsumer::onBroadcasted, Qt::UniqueConnection);
    connect(m_transactions, &ITransactions::cancelled, this, &ExternalSpendConsumer::onCancelled,
            Qt::UniqueConnection);
    if (data.value(QStringLiteral("lifecycleReview")).toBool()) {
        lifecycleReview(data, fixture);
        return;
    }
    if (data.value(QStringLiteral("nonceReview")).toBool()) {
        nonceReview(data);
        return;
    }

    auto *unregistered = m_host->transactions(nullptr);
    connect(unregistered, &ITransactions::error, this, &ExternalSpendConsumer::onCoordinationError,
            Qt::UniqueConnection);
    m_pending = unregistered->newRequest();
    m_answered = false;
    unregistered->finalize(m_pending, m_request);
    wait();
    require(m_error_kind == SpendError::Permission,
            "unregistered caller cannot finalize or submit a transaction");
    disconnect(unregistered, &ITransactions::error, this,
               &ExternalSpendConsumer::onCoordinationError);
    m_error.clear();

    auto *otherKeys = m_host->keys(nullptr);
    m_pending = otherKeys->newRequest();
    m_answered = false;
    m_keys->coordinate(m_pending, m_request);
    wait();
    require(m_error_kind == SpendError::InvalidRequest,
            "foreign keys request ID cannot be used through this caller's handle");
    otherKeys->cancelRequest(m_pending);
    m_error.clear();
    connect(otherKeys, &IKeys::coordinationError, this, &ExternalSpendConsumer::onCoordinationError,
            Qt::UniqueConnection);
    m_pending = otherKeys->newRequest();
    m_answered = false;
    otherKeys->coordinate(m_pending, m_request);
    wait();
    require(m_error_kind == SpendError::Permission, "unregistered caller cannot use signing keys");
    disconnect(otherKeys, &IKeys::coordinationError, this,
               &ExternalSpendConsumer::onCoordinationError);
    m_error.clear();
    accountLifecycle();

    m_request.stage = SigningStage::SpContributions;
    m_pending = m_keys->newRequest();
    m_answered = false;
    m_keys->coordinate(m_pending, m_request);
    wait();
    require(m_error_kind == SpendError::Unsupported,
            "unsupported backend stage has a structured result");
    m_error.clear();
    m_request.stage = SigningStage::Signatures;
    sign(data.value(QStringLiteral("firstFingerprint")).toString(), QStringLiteral("first"),
         SigningProgress::Partial);
    sign(data.value(QStringLiteral("secondFingerprint")).toString(), QStringLiteral("second"),
         SigningProgress::Complete);

    m_pending = m_transactions->newRequest();
    m_answered = false;
    auto answers = m_answers;
    m_transactions->finalize(m_pending, m_request);
    m_transactions->finalize(m_pending, m_request);
    require(!m_answered, "duplicate in-flight finalize must not emit a competing terminal error");
    wait();
    require(m_answers == answers + 1, "duplicate finalize has exactly one terminal result");
    require(m_error.isEmpty() && !m_tx.isEmpty(), "public account-independent finalization");
    m_pending = m_transactions->newRequest();
    m_answered = false;
    answers = m_answers;
    m_transactions->broadcast(m_pending, Network::Regtest,
                              data.value(QStringLiteral("endpoint")).toString(), m_tx);
    m_transactions->broadcast(m_pending, Network::Regtest,
                              data.value(QStringLiteral("endpoint")).toString(), m_tx);
    m_transactions->finalize(m_pending, m_request);
    require(
        !m_answered,
        "duplicate broadcast or conflicting operation must not compete with its original reply");
    wait();
    require(m_answers == answers + 1, "duplicate broadcast has exactly one terminal result");
    require(m_error.isEmpty(), "public network broadcast accepts mixed account inputs");

    m_pending = m_transactions->newRequest();
    m_answered = false;
    m_transactions->finalize(m_pending, m_request);
    wait();
    require(m_error.isEmpty(), "a new request succeeds after duplicate submissions");

    answers = m_answers;
    m_pending = m_transactions->newRequest();
    m_answered = false;
    m_transactions->finalize(m_pending, m_request);
    m_transactions->cancelRequest(m_pending);
    wait();
    require(m_answers == answers + 1, "cancellation answers once");
    QElapsedTimer late;
    late.start();
    while (late.elapsed() < 250) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(m_answers == answers + 1, "cancelled worker reply is suppressed");
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "save public workflow result");
    data.insert(QStringLiteral("txid"), m_txid);
    data.insert(QStringLiteral("tx"), m_tx);
    file.write(QJsonDocument(data).toJson());
}

void ExternalSpendConsumer::nonceReview(const QJsonObject &data) {
    m_request.stage = SigningStage::MusigNonces;
    m_request.fingerprint = data.value(QStringLiteral("firstFingerprint")).toString();
    m_password = QStringLiteral("first");
    m_pending = m_keys->newRequest();
    m_answered = false;
    auto answers = m_answers;
    QTimer password;
    connect(&password, &QTimer::timeout, this, &ExternalSpendConsumer::onPassword,
            Qt::UniqueConnection);
    password.start(1);
    m_keys->coordinate(m_pending, m_request);
    wait();
    require(m_error_kind == SpendError::Unsupported && m_answers == answers + 1,
            "real IKeys refuses unequal nonce progress before selecting a route");
    require(m_password_prompts == 0,
            "cached plus password route must be refused before any password prompt");
}

void ExternalSpendConsumer::lifecycleReview(QJsonObject data, const QString &fixture) {
    m_request.stage = SigningStage::MusigNonces;
    sign(data.value(QStringLiteral("firstFingerprint")).toString(), QStringLiteral("first"),
         SigningProgress::Partial);
    require(!m_request.sessionId.isEmpty(), "IKeys allocated a real retained nonce attempt");
    data.insert(QStringLiteral("lifecyclePsbt"), m_request.psbtV2Hex);
    data.insert(QStringLiteral("lifecycleSessionId"), m_request.sessionId);

    m_request.stage = SigningStage::MusigSignatures;
    m_pending = m_keys->newRequest();
    m_answered = false;
    m_keys->coordinate(m_pending, m_request);
    wait();
    require(m_error_kind == SpendError::InvalidRequest,
            "premature signature round fails before routing");
    require(m_retired_session.isEmpty() && m_retirement_failures == 0,
            "preparation failure did not retire the existing attempt");
    m_error.clear();

    if (data.value(QStringLiteral("retryRelease")).toBool()) {
        auto broken = m_request;
        broken.stage = SigningStage::Release;
        broken.psbtV2Hex = QStringLiteral("00");
        m_pending = m_keys->newRequest();
        m_answered = false;
        m_keys->coordinate(m_pending, broken);
        wait();
        require(
            m_error_kind == SpendError::InvalidRequest && m_retirement_failures == 1,
            "failed Release reports cleanup failure instead of pretending retirement succeeded");
        require(m_retired_session.isEmpty(),
                "failed Release preserves the last valid attempt snapshot");
        m_error.clear();
        m_request.stage = SigningStage::Release;
        m_pending = m_keys->newRequest();
        m_answered = false;
        m_keys->coordinate(m_pending, m_request);
        wait();
        require(m_error.isEmpty() && m_result.progress == SigningProgress::StageComplete,
                "Release retry with the valid snapshot succeeds");
        require(m_retired_session == m_request.sessionId,
                "successful retry confirms retirement of the old attempt");
    }
    QFile output(fixture);
    require(output.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "save public lifecycle result");
    output.write(QJsonDocument(data).toJson());
    m_answered = !m_retired_session.isEmpty();
}

void ExternalSpendConsumer::waitForRetirement() const {
    wait();
    require(m_retired_session == m_request.sessionId,
            "caller unload retired the retained attempt after a preparation failure");
}

void ExternalSpendConsumer::onAttemptRetirement(const QString &session_id,
                                                plugin::RetirementStatus status,
                                                [[maybe_unused]] const QString &error) {
    require(session_id == m_request.sessionId,
            "retirement result is scoped to this caller's attempt");
    switch (status) {
    case plugin::RetirementStatus::Pending:
        break;
    case plugin::RetirementStatus::Retired:
        m_retired_session = session_id;
        m_answered = true;
        break;
    case plugin::RetirementStatus::Failed:
        ++m_retirement_failures;
        break;
    }
}

void ExternalSpendConsumer::accountLifecycle() {
    DeferredInstance instance;
    m_host->registerInstance(instance.id(), &instance, m_caller);
    QPointer<IAccount> handle;
    for (auto *entry : m_host->instances(m_caller)) {
        if (entry->id() == instance.id()) {
            handle = entry->account();
        }
    }
    require(handle != nullptr, "discover deferred public account");
    connect(handle, qOverload<ReqId, QList<plugin::Coin>>(&IAccount::coins), this,
            &ExternalSpendConsumer::onAccountCoins, Qt::UniqueConnection);
    connect(handle, &IAccount::cancelled, this, &ExternalSpendConsumer::onCancelled,
            Qt::UniqueConnection);
    m_pending = handle->coins();
    m_answered = false;
    handle->cancelRequest(m_pending);
    wait();
    emit instance.backend.IAccount::coins(m_pending, {});
    require(m_account_answers == 0, "cancelled account reply is suppressed");
    auto current = handle->coins();
    emit instance.backend.IAccount::coins(current, {});
    emit instance.backend.IAccount::coins(current, {});
    require(m_account_answers == 1, "account reply is consumed once");
    m_pending = handle->coins();
    m_answered = false;
    m_host->removeInstance(instance.id(), m_caller);
    wait();
    require(handle.isNull(), "account removal invalidates its caller handle");
    emit instance.backend.IAccount::coins(m_pending, {});
    require(m_account_answers == 1, "removed account cannot deliver a late reply");
}

void ExternalSpendConsumer::onAccountCoins([[maybe_unused]] ReqId req_id,
                                           [[maybe_unused]] const QList<plugin::Coin> &coins) {
    ++m_account_answers;
}

void ExternalSpendConsumer::sign(const QString &fingerprint, const QString &password,
                                 SigningProgress expected) {
    m_password = password;
    m_request.fingerprint = fingerprint;
    m_pending = m_keys->newRequest();
    m_answered = false;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, &ExternalSpendConsumer::onPassword,
            Qt::UniqueConnection);
    timer.start(10);
    m_keys->coordinate(m_pending, m_request);
    wait();
    if (!m_error.isEmpty()) {
        qFatal("external signing failed: %s", qPrintable(m_error));
    }
    require(m_result.progress == expected, "partial signing must not be reported complete");
    m_request.psbtV2Hex = m_result.psbtV2Hex;
    m_request.sessionId = m_result.sessionId;
}

void ExternalSpendConsumer::onCoins([[maybe_unused]] ReqId req_id,
                                    const QList<plugin::Coin> &coins) {
    for (const auto &coin : coins) {
        for (qsizetype i = m_coins.size(); i > 0; --i) {
            if (m_coins[i - 1].outpoint == coin.outpoint) {
                m_coins.removeAt(i - 1);
            }
        }
        m_coins.append(coin);
    }
}

void ExternalSpendConsumer::onCoordinated(ReqId req_id, const plugin::SigningResult &result) {
    require(req_id == m_pending, "signing result correlation");
    m_result = result;
    m_answered = true;
    ++m_answers;
}

void ExternalSpendConsumer::onCoordinationError(ReqId req_id, SpendError kind,
                                                const QString &message) {
    require(req_id == m_pending, "error correlation");
    m_error_kind = kind;
    m_error = message;
    m_answered = true;
    ++m_answers;
}

void ExternalSpendConsumer::onFinalized(ReqId req_id, [[maybe_unused]] const QString &psbt,
                                        const QString &tx, const QString &txid) {
    require(req_id == m_pending, "finalization correlation");
    m_tx = tx;
    m_txid = txid;
    m_answered = true;
    ++m_answers;
}

void ExternalSpendConsumer::onBroadcasted(ReqId req_id, const QString &txid) {
    require(req_id == m_pending && txid == m_txid, "broadcast acknowledges exact finalized txid");
    m_answered = true;
    ++m_answers;
}

void ExternalSpendConsumer::onCancelled(ReqId req_id) {
    require(req_id == m_pending, "cancellation correlation");
    m_answered = true;
    ++m_answers;
}

void ExternalSpendConsumer::onPassword() {
    for (auto *widget : QApplication::topLevelWidgets()) {
        for (auto *input : widget->findChildren<QLineEdit *>()) {
            if (input->isVisible() && input->echoMode() == QLineEdit::Password &&
                input->text().isEmpty()) {
                ++m_password_prompts;
                input->setText(m_password);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(input, &enter);
                return;
            }
        }
    }
}

} // namespace smoke
