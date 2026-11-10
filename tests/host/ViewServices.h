#pragma once

#include "account/Extensions.h"
#include "account/Services.h"
#include "interfaces/backend/account.h"
#include "interfaces/backend/blockchain.h"
#include "interfaces/backend/labels.h"
#include "interfaces/backend/settings.h"
#include "interfaces/backend/transaction.h"
#include "interfaces/backend/types.h"

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QVariant>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

// Fake account services for the shared account views: every read comes from a public
// model the test sets, every request is recorded and answered as the test scripts it.
// Assigning a model field changes what the getters read with no signal (a polling
// consumer); the set*() helpers publish and emit the matching change signal (a notified
// consumer). GUI thread only.
namespace smoke {

// When a fake answers one kind of request.
enum class Answer : std::uint8_t {
    // During the request call.
    Inline,
    // Once the test calls answer() or flush().
    Later,
    // Never: the request stays unanswered.
    Never,
};

// One request a fake received, in the order received. Its payload is what the request
// carried: an Endpoint, Settings, TransactionTemplate, LabelKey, QByteArray or QString,
// the QList<quint32> of a reveal() (descriptor, keychain, index), or the QJsonValue of a
// value() call (the operation is then in `operation`).
struct Request {
    backend::RequestId id = 0;
    // The interface method: "testEndpoint", "save", "craft", "selectCoins", ...
    QString kind;
    QString operation;
    QVariant payload;
};

// What every fake shares: the scripts, the record and the pending answers.
class RequestLog {
public:
    struct Script {
        Answer answer = Answer::Inline;
        // Set: the request answers error() with it instead of its result.
        QString error;
    };

    RequestLog() = default;
    RequestLog(const RequestLog &) = delete;
    auto operator=(const RequestLog &) -> RequestLog & = delete;
    virtual ~RequestLog() = default;

    // The script of `kind`, created Inline with no error.
    auto script(const QString &kind) -> Script &;
    [[nodiscard]] auto requestsOf(const QString &kind) const -> QList<Request>;
    [[nodiscard]] auto count(const QString &kind) const -> qsizetype;
    // The requests scripted Later that are not answered yet, oldest first.
    [[nodiscard]] auto pending() const -> QList<backend::RequestId>;
    // Answers the pending request `request_id`: false when none is pending.
    auto answer(backend::RequestId request_id) -> bool;
    // Answers the requests pending now, oldest first. Requests made by the answers stay
    // pending.
    void flush();

    // Mutable: craft() and finalize() are const in their interface, and recorded too.
    mutable QList<Request> requests;

protected:
    // Records the request, then answers it as its kind is scripted: `reply` runs when it
    // answers, `failed` instead when the script has an error by then.
    void receive(const Request &received, std::function<void()> reply,
                 std::function<void(const QString &)> failed);

private:
    struct Pending {
        backend::RequestId id = 0;
        std::function<void()> run;
    };

    QHash<QString, Script> m_scripts;
    QList<Pending> m_pending;
};

// The IService part of every fake. value() answers values[operation], error() for an
// operation without a value; raw() always answers error().
template <typename Interface> class FakeService : public Interface, public RequestLog {
public:
    void raw(backend::RequestId request_id, QString operation, QByteArray payload) override {
        request({.id = request_id,
                 .kind = QStringLiteral("raw"),
                 .operation = operation,
                 .payload = payload},
                [this, request_id] {
                    emit this->error(request_id, QStringLiteral("raw is not supported"));
                });
    }

    void value(backend::RequestId request_id, QString operation, QJsonValue payload) override {
        request({.id = request_id,
                 .kind = QStringLiteral("value"),
                 .operation = operation,
                 .payload = QVariant::fromValue(payload)},
                [this, request_id, operation] {
                    auto found = values.constFind(operation);
                    if (found == values.cend()) {
                        emit this->error(request_id,
                                         QStringLiteral("unsupported operation %1").arg(operation));
                        return;
                    }
                    emit this->valueResult(request_id, found.value());
                });
    }

    // Sets the value of `operation` and emits it as a valueEvent().
    void publishValue(const QString &operation, const QJsonValue &payload) {
        values.insert(operation, payload);
        emit this->valueEvent(operation, payload);
    }

    QHash<QString, QJsonValue> values;

protected:
    // Records a request whose result `reply` emits; a scripted error answers error().
    void request(const Request &received, std::function<void()> reply) {
        receive(received, std::move(reply), [this, id = received.id](const QString &message) {
            emit this->error(id, message);
        });
    }
};

class FakeAccount final : public FakeService<backend::IAccountBackend> {
public:
    struct Model {
        QString id = QStringLiteral("view-account");
        QString name = QStringLiteral("View account");
        backend::Network network = backend::Network::Regtest;
        QList<backend::AddressKind> kinds;
        QList<backend::Address> addresses;
        QHash<QString, backend::AddressVerification> verification;
        // What newAddress() hands out, first one first; none left answers error().
        QList<backend::Address> fresh;
        backend::BackupDescription backup;
    };

    [[nodiscard]] auto id() const -> QString override;
    [[nodiscard]] auto name() const -> QString override;
    [[nodiscard]] auto network() const -> backend::Network override;
    [[nodiscard]] auto addressKinds() const -> QList<backend::AddressKind> override;
    [[nodiscard]] auto addresses() const -> QList<backend::Address> override;
    [[nodiscard]] auto addressVerification(const QString &address) const
        -> std::optional<backend::AddressVerification> override;
    [[nodiscard]] auto backupDescription() const -> backend::BackupDescription override;

    void setAddresses(const QList<backend::Address> &addresses);

    // Publishes the next fresh address, then answers addressCreated().
    void newAddress(backend::RequestId request_id, QString kind) override;
    // Recorded, answered error(): the views do not back up.
    void backup(backend::RequestId request_id) override;
    void inspectBackup(backend::RequestId request_id, QByteArray backup, QStringList xpubs,
                       QByteArray mnemonic) override;
    void restore(backend::RequestId request_id, QByteArray backup, QStringList xpubs,
                 QByteArray mnemonic, QString name, backend::Network network) override;

    Model model;
};

class FakeBlockchain final : public FakeService<backend::IBlockchain> {
public:
    struct Model {
        backend::BlockchainState state;
        QList<backend::Coin> coins;
        backend::Balance balance;
        QList<backend::Transaction> history;
        // What fetchTransactionDetails() publishes; a txid missing answers error().
        QHash<QString, backend::TransactionDetails> details;
        // What a broadcast answers.
        backend::BroadcastResult broadcast{.outcome = backend::BroadcastOutcome::Accepted};
        QList<backend::OwnedOutput> ownedOutputs;
        QList<backend::UsedIndex> usedIndexes;
    };

    [[nodiscard]] auto state() const -> backend::BlockchainState override;
    [[nodiscard]] auto coins() const -> QList<backend::Coin> override;
    [[nodiscard]] auto balance() const -> backend::Balance override;
    [[nodiscard]] auto history() const -> QList<backend::Transaction> override;
    [[nodiscard]] auto transactionDetails(const QString &txid) const
        -> std::optional<backend::TransactionDetails> override;
    [[nodiscard]] auto ownedOutputs() const -> QList<backend::OwnedOutput> override;
    [[nodiscard]] auto usedIndexes() const -> QList<backend::UsedIndex> override;

    void setState(const backend::BlockchainState &state);
    void setCoins(const QList<backend::Coin> &coins);
    void setBalance(const backend::Balance &balance);
    void setHistory(const QList<backend::Transaction> &history);
    void setDetails(const backend::TransactionDetails &details);
    // Sets whether the connection `id` of the state is up, then publishes the state.
    void setConnection(const QString &id, bool connected);

    // Answer completed().
    void start(backend::RequestId request_id) override;
    void stop(backend::RequestId request_id) override;
    void rescan(backend::RequestId request_id, std::optional<quint64> from_height) override;
    // Sets the connection's connected flag and publishes the state, then completed(); an
    // unknown connection answers error().
    void setConnected(backend::RequestId request_id, QString connection_id,
                      bool connected) override;
    // completed() for a known connection: its tip is what the state holds.
    void refreshTip(backend::RequestId request_id, QString connection_id) override;
    // Publishes the details of `txid`, then transactionDetailsFetched().
    void fetchTransactionDetails(backend::RequestId request_id, QString txid) override;
    void broadcast(backend::RequestId request_id, QByteArray transaction) override;
    // Answers completed().
    void reveal(backend::RequestId request_id, quint32 descriptor, quint32 keychain,
                quint32 index) override;

    Model model;

protected:
    [[nodiscard]] auto hasConnection(const QString &id) const -> bool;
};

class FakeTransaction final : public FakeService<backend::ITransaction> {
public:
    using Crafting = std::function<backend::CraftResult(const backend::TransactionTemplate &)>;
    using Finalizing =
        std::function<std::optional<QByteArray>(const backend::TransactionTemplate &)>;

    FakeTransaction();

    // Recorded as "craft", answered by `crafting`.
    [[nodiscard]] auto craft(const backend::TransactionTemplate &transaction) const
        -> backend::CraftResult override;
    // Recorded as "finalize", answered by `finalizing`.
    [[nodiscard]] auto finalize(const backend::TransactionTemplate &transaction) const
        -> std::optional<QByteArray> override;
    // coinsSelected() with what `crafting` makes of the template.
    void selectCoins(backend::RequestId request_id,
                     backend::TransactionTemplate transaction) override;

    // The transactions crafted so far, by craft() or a coin selection.
    [[nodiscard]] auto crafts() const -> qsizetype;

    // How craft() and a coin selection resolve a template: unless set, the template as
    // it is, complete.
    Crafting crafting;
    // How finalize() turns a template into PSBT bytes: unless set, it fails (nullopt).
    Finalizing finalizing;
};

class FakeLabels final : public FakeService<backend::ILabels> {
public:
    struct Model {
        QList<backend::Label> labels;
    };

    [[nodiscard]] auto labels() const -> QList<backend::Label> override;
    [[nodiscard]] auto label(const backend::LabelKey &key) const -> QString override;

    void setLabels(const QList<backend::Label> &labels);

    // Sets the label (an empty text removes it) and publishes the labels, then saved().
    void setLabel(backend::RequestId request_id, backend::LabelKey key, QString text) override;

    Model model;
};

class FakeSettings final : public FakeService<backend::IBackendSettings> {
public:
    struct Model {
        backend::Settings settings{.network = backend::Network::Regtest};
    };

    [[nodiscard]] auto settings() const -> backend::Settings override;

    void setSettings(const backend::Settings &settings);
    // Answers extension::ENDPOINTS_BUSY with `busy` from now on, and emits it as its
    // valueEvent().
    void setEndpointsBusy(bool busy);

    // endpointTested().
    void testEndpoint(backend::RequestId request_id, backend::Endpoint endpoint) override;
    // Takes each saved endpoint in place of the one of the same id (appended when new)
    // and the network, publishes them, then saved().
    void save(backend::RequestId request_id, backend::Settings settings) override;

    Model model;
};

// The five fakes, and the account::Services the views take over them.
class ViewServices {
public:
    [[nodiscard]] auto services() -> account::Services;
    // Answers every request pending now, in each fake.
    void flush();

    FakeAccount account;
    FakeBlockchain blockchain;
    FakeTransaction transaction;
    FakeLabels labels;
    FakeSettings settings;
};

} // namespace smoke
