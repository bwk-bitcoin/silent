#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <interfaces/host.h>
#include <interfaces/keys.h>
#include <interfaces/transactions.h>
#include <optional>

class IModule;

namespace smoke {

// The consumer imports no controller, module implementation or private wallet state.
class ExternalSpendConsumer final : public QObject {
    Q_OBJECT
public:
    ExternalSpendConsumer(Host *host, IModule *caller);
    void run(const QString &fixture);
    void waitForRetirement() const;

public slots:
    void onCoins(ReqId req_id, const QList<plugin::Coin> &coins,
                 const std::optional<plugin::ChainTip> &tip);
    void onCoordinated(ReqId req_id, const plugin::SigningResult &result);
    void onCoordinationError(ReqId req_id, plugin::SpendError kind, const QString &message);
    void onFinalized(ReqId req_id, const QString &psbt, const QString &tx, const QString &txid);
    void onBroadcasted(ReqId req_id, const QString &txid);
    void onCancelled(ReqId req_id);
    void onPassword();
    void onAccountCoins(ReqId req_id, const QList<plugin::Coin> &coins,
                        const std::optional<plugin::ChainTip> &tip);
    void onAttemptRetirement(const QString &session_id, plugin::RetirementStatus status,
                             const QString &error);

protected:
    void wait() const;
    [[nodiscard]] auto tipsMatch(const QJsonObject &expected, qsizetype accounts) const -> bool;
    void sign(const QString &fingerprint, const QString &password,
              plugin::SigningProgress expected);
    void accountLifecycle();
    void nonceReview(const QJsonObject &data);
    void lifecycleReview(QJsonObject data, const QString &fixture);

private:
    Host *m_host;
    IModule *m_caller;
    IKeys *m_keys = nullptr;
    ITransactions *m_transactions = nullptr;
    QList<plugin::Coin> m_coins;
    // The latest tip each public account handle answered with.
    QHash<const QObject *, std::optional<plugin::ChainTip>> m_tips;
    plugin::SigningRequest m_request;
    plugin::SigningResult m_result;
    QString m_password;
    QString m_tx;
    QString m_txid;
    QString m_error;
    plugin::SpendError m_error_kind = plugin::SpendError::Other;
    ReqId m_pending = 0;
    bool m_answered = false;
    int m_answers = 0;
    int m_account_answers = 0;
    int m_password_prompts = 0;
    QString m_retired_session;
    int m_retirement_failures = 0;
};

} // namespace smoke
