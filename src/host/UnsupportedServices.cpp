#include "host/UnsupportedServices.h"

#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <optional>
#include <utility>

namespace host {
namespace {

auto unsupportedMessage() -> QString {
    return QStringLiteral("Not supported by this account");
}

// Answers both extension requests with error(), for every unsupported service.
template <typename Service> class UnsupportedService : public Service {
public:
    explicit UnsupportedService(QObject *parent) : Service(parent) {
    }

    void raw(backend::RequestId request_id, [[maybe_unused]] QString operation,
             [[maybe_unused]] QByteArray payload) override {
        emit this->error(request_id, unsupportedMessage());
    }

    void value(backend::RequestId request_id, [[maybe_unused]] QString operation,
               [[maybe_unused]] QJsonValue payload) override {
        emit this->error(request_id, unsupportedMessage());
    }
};

class UnsupportedAccount final : public UnsupportedService<backend::IAccountBackend> {
    Q_OBJECT
    Q_INTERFACES(backend::IAccountBackend)

public:
    UnsupportedAccount(QString account, backend::Network network, QObject *parent)
        : UnsupportedService(parent),
          m_account(std::move(account)),
          m_network(network) {
    }

    [[nodiscard]] auto id() const -> QString override {
        return m_account;
    }

    [[nodiscard]] auto name() const -> QString override {
        return {};
    }

    [[nodiscard]] auto network() const -> backend::Network override {
        return m_network;
    }

    [[nodiscard]] auto addressKinds() const -> QList<backend::AddressKind> override {
        return {};
    }

    [[nodiscard]] auto addresses() const -> QList<backend::Address> override {
        return {};
    }

    [[nodiscard]] auto addressVerification([[maybe_unused]] const QString &address) const
        -> std::optional<backend::AddressVerification> override {
        return std::nullopt;
    }

    void newAddress(backend::RequestId request_id, [[maybe_unused]] QString kind) override {
        emit error(request_id, unsupportedMessage());
    }

    void backup(backend::RequestId request_id) override {
        emit error(request_id, unsupportedMessage());
    }

    void inspectBackup(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                       [[maybe_unused]] QStringList xpubs,
                       [[maybe_unused]] QByteArray mnemonic) override {
        emit error(request_id, unsupportedMessage());
    }

    void restore(backend::RequestId request_id, [[maybe_unused]] QByteArray backup,
                 [[maybe_unused]] QStringList xpubs, [[maybe_unused]] QByteArray mnemonic,
                 [[maybe_unused]] QString name,
                 [[maybe_unused]] backend::Network network) override {
        emit error(request_id, unsupportedMessage());
    }

private:
    QString m_account;
    backend::Network m_network;
};

class UnsupportedBlockchain final : public UnsupportedService<backend::IBlockchain> {
    Q_OBJECT
    Q_INTERFACES(backend::IBlockchain)

public:
    using UnsupportedService::UnsupportedService;

    [[nodiscard]] auto state() const -> backend::BlockchainState override {
        return {};
    }

    [[nodiscard]] auto coins() const -> QList<backend::Coin> override {
        return {};
    }

    [[nodiscard]] auto balance() const -> backend::Balance override {
        return {};
    }

    [[nodiscard]] auto history() const -> QList<backend::Transaction> override {
        return {};
    }

    [[nodiscard]] auto transactionDetails([[maybe_unused]] const QString &txid) const
        -> std::optional<backend::TransactionDetails> override {
        return std::nullopt;
    }

    void start(backend::RequestId request_id) override {
        emit error(request_id, unsupportedMessage());
    }

    void stop(backend::RequestId request_id) override {
        emit error(request_id, unsupportedMessage());
    }

    void rescan(backend::RequestId request_id,
                [[maybe_unused]] std::optional<quint64> from_height) override {
        emit error(request_id, unsupportedMessage());
    }

    void setConnected(backend::RequestId request_id, [[maybe_unused]] QString connection_id,
                      [[maybe_unused]] bool connected) override {
        emit error(request_id, unsupportedMessage());
    }

    void refreshTip(backend::RequestId request_id,
                    [[maybe_unused]] QString connection_id) override {
        emit error(request_id, unsupportedMessage());
    }

    void fetchTransactionDetails(backend::RequestId request_id,
                                 [[maybe_unused]] QString txid) override {
        emit error(request_id, unsupportedMessage());
    }

    void broadcast(backend::RequestId request_id,
                   [[maybe_unused]] QByteArray transaction) override {
        emit error(request_id, unsupportedMessage());
    }
};

class UnsupportedTransaction final : public UnsupportedService<backend::ITransaction> {
    Q_OBJECT
    Q_INTERFACES(backend::ITransaction)

public:
    using UnsupportedService::UnsupportedService;

    [[nodiscard]] auto craft(const backend::TransactionTemplate &transaction) const
        -> backend::CraftResult override {
        return {.transaction = transaction,
                .completion = {.complete = false, .error = unsupportedMessage()}};
    }

    [[nodiscard]] auto
    finalize([[maybe_unused]] const backend::TransactionTemplate &transaction) const
        -> std::optional<QByteArray> override {
        return std::nullopt;
    }

    void selectCoins(backend::RequestId request_id,
                     [[maybe_unused]] backend::TransactionTemplate transaction) override {
        emit error(request_id, unsupportedMessage());
    }
};

class UnsupportedLabels final : public UnsupportedService<backend::ILabels> {
    Q_OBJECT
    Q_INTERFACES(backend::ILabels)

public:
    using UnsupportedService::UnsupportedService;

    [[nodiscard]] auto labels() const -> QList<backend::Label> override {
        return {};
    }

    [[nodiscard]] auto label([[maybe_unused]] const backend::LabelKey &key) const
        -> QString override {
        return {};
    }

    void setLabel(backend::RequestId request_id, [[maybe_unused]] backend::LabelKey key,
                  [[maybe_unused]] QString text) override {
        emit error(request_id, unsupportedMessage());
    }
};

class UnsupportedSettings final : public UnsupportedService<backend::IBackendSettings> {
    Q_OBJECT
    Q_INTERFACES(backend::IBackendSettings)

public:
    UnsupportedSettings(backend::Network network, QObject *parent)
        : UnsupportedService(parent),
          m_network(network) {
    }

    [[nodiscard]] auto settings() const -> backend::Settings override {
        return {.network = m_network};
    }

    void testEndpoint(backend::RequestId request_id,
                      [[maybe_unused]] backend::Endpoint endpoint) override {
        emit error(request_id, unsupportedMessage());
    }

    void save(backend::RequestId request_id, [[maybe_unused]] backend::Settings settings) override {
        emit error(request_id, unsupportedMessage());
    }

private:
    backend::Network m_network;
};

} // namespace

auto unsupportedService(backend::ServiceKind kind, const QString &account, backend::Network network,
                        QObject *parent) -> backend::IService * {
    switch (kind) {
    case backend::ServiceKind::Account:
        return new UnsupportedAccount(account, network, parent);
    case backend::ServiceKind::Blockchain:
        return new UnsupportedBlockchain(parent);
    case backend::ServiceKind::Transaction:
        return new UnsupportedTransaction(parent);
    case backend::ServiceKind::Labels:
        return new UnsupportedLabels(parent);
    case backend::ServiceKind::Settings:
        return new UnsupportedSettings(network, parent);
    }
    return nullptr;
}

} // namespace host

#include "UnsupportedServices.moc"
