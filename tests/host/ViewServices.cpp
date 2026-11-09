#include "ViewServices.h"

#include <algorithm>

namespace smoke {

// ===== RequestLog =====

auto RequestLog::script(const QString &kind) -> Script & {
    return m_scripts[kind];
}

auto RequestLog::requestsOf(const QString &kind) const -> QList<Request> {
    QList<Request> found;
    for (const auto &request : requests) {
        if (request.kind == kind) {
            found.append(request);
        }
    }
    return found;
}

auto RequestLog::count(const QString &kind) const -> qsizetype {
    return std::ranges::count_if(requests,
                                 [&kind](const Request &request) { return request.kind == kind; });
}

auto RequestLog::pending() const -> QList<backend::RequestId> {
    QList<backend::RequestId> ids;
    for (const auto &waiting : m_pending) {
        ids.append(waiting.id);
    }
    return ids;
}

auto RequestLog::answer(backend::RequestId request_id) -> bool {
    for (qsizetype i = 0; i < m_pending.size(); ++i) {
        if (m_pending.at(i).id == request_id) {
            // Taken first: the answer may make another request.
            auto run = m_pending.takeAt(i).run;
            run();
            return true;
        }
    }
    return false;
}

void RequestLog::flush() {
    for (auto id : pending()) {
        answer(id);
    }
}

void RequestLog::receive(const Request &received, std::function<void()> reply,
                         std::function<void(const QString &)> failed) {
    requests.append(received);
    auto kind = received.kind;
    auto run = [this, kind, reply = std::move(reply), failed = std::move(failed)] {
        auto error = m_scripts.value(kind).error;
        if (error.isEmpty()) {
            reply();
        } else {
            failed(error);
        }
    };
    switch (m_scripts.value(kind).answer) {
    case Answer::Inline:
        run();
        break;
    case Answer::Later:
        m_pending.append({.id = received.id, .run = std::move(run)});
        break;
    case Answer::Never:
        break;
    }
}

// ===== FakeAccount =====

auto FakeAccount::id() const -> QString {
    return model.id;
}

auto FakeAccount::name() const -> QString {
    return model.name;
}

auto FakeAccount::network() const -> backend::Network {
    return model.network;
}

auto FakeAccount::addressKinds() const -> QList<backend::AddressKind> {
    return model.kinds;
}

auto FakeAccount::addresses() const -> QList<backend::Address> {
    return model.addresses;
}

auto FakeAccount::addressVerification(const QString &address) const
    -> std::optional<backend::AddressVerification> {
    auto found = model.verification.constFind(address);
    if (found == model.verification.cend()) {
        return std::nullopt;
    }
    return found.value();
}

void FakeAccount::setAddresses(const QList<backend::Address> &addresses) {
    model.addresses = addresses;
    emit addressesChanged();
}

void FakeAccount::newAddress(backend::RequestId request_id, QString kind) {
    request({.id = request_id, .kind = QStringLiteral("newAddress"), .payload = kind},
            [this, request_id] {
                if (model.fresh.isEmpty()) {
                    emit error(request_id, QStringLiteral("no fresh address"));
                    return;
                }
                auto address = model.fresh.takeFirst();
                model.addresses.append(address);
                emit addressesChanged();
                emit addressCreated(request_id, address);
            });
}

void FakeAccount::backup(backend::RequestId request_id) {
    request({.id = request_id, .kind = QStringLiteral("backup")}, [this, request_id] {
        emit error(request_id, QStringLiteral("backup is not supported"));
    });
}

void FakeAccount::inspectBackup(backend::RequestId request_id, QByteArray backup,
                                [[maybe_unused]] QStringList xpubs,
                                [[maybe_unused]] QByteArray mnemonic) {
    request(
        {.id = request_id, .kind = QStringLiteral("inspectBackup"), .payload = backup},
        [this, request_id] { emit error(request_id, QStringLiteral("backup is not supported")); });
}

void FakeAccount::restore(backend::RequestId request_id, QByteArray backup,
                          [[maybe_unused]] QStringList xpubs, [[maybe_unused]] QByteArray mnemonic,
                          [[maybe_unused]] QString name,
                          [[maybe_unused]] backend::Network network) {
    request(
        {.id = request_id, .kind = QStringLiteral("restore"), .payload = backup},
        [this, request_id] { emit error(request_id, QStringLiteral("backup is not supported")); });
}

// ===== FakeBlockchain =====

auto FakeBlockchain::state() const -> backend::BlockchainState {
    return model.state;
}

auto FakeBlockchain::coins() const -> QList<backend::Coin> {
    return model.coins;
}

auto FakeBlockchain::balance() const -> backend::Balance {
    return model.balance;
}

auto FakeBlockchain::history() const -> QList<backend::Transaction> {
    return model.history;
}

auto FakeBlockchain::transactionDetails(const QString &txid) const
    -> std::optional<backend::TransactionDetails> {
    auto found = model.details.constFind(txid);
    if (found == model.details.cend()) {
        return std::nullopt;
    }
    return found.value();
}

void FakeBlockchain::setState(const backend::BlockchainState &state) {
    model.state = state;
    emit stateChanged();
}

void FakeBlockchain::setCoins(const QList<backend::Coin> &coins) {
    model.coins = coins;
    emit coinsChanged();
}

void FakeBlockchain::setBalance(const backend::Balance &balance) {
    model.balance = balance;
    emit balanceChanged();
}

void FakeBlockchain::setHistory(const QList<backend::Transaction> &history) {
    model.history = history;
    emit historyChanged();
}

void FakeBlockchain::setDetails(const backend::TransactionDetails &details) {
    model.details.insert(details.transaction.txid, details);
    emit transactionDetailsChanged(details.transaction.txid);
}

void FakeBlockchain::setConnection(const QString &id, bool connected) {
    for (auto &connection : model.state.connections) {
        if (connection.id == id) {
            connection.connected = connected;
        }
    }
    emit stateChanged();
}

auto FakeBlockchain::hasConnection(const QString &id) const -> bool {
    return std::ranges::any_of(
        model.state.connections,
        [&id](const backend::Connection &connection) { return connection.id == id; });
}

void FakeBlockchain::start(backend::RequestId request_id) {
    request({.id = request_id, .kind = QStringLiteral("start")},
            [this, request_id] { emit completed(request_id); });
}

void FakeBlockchain::stop(backend::RequestId request_id) {
    request({.id = request_id, .kind = QStringLiteral("stop")},
            [this, request_id] { emit completed(request_id); });
}

void FakeBlockchain::rescan(backend::RequestId request_id, std::optional<quint64> from_height) {
    request({.id = request_id,
             .kind = QStringLiteral("rescan"),
             .payload = from_height.has_value() ? QVariant::fromValue(*from_height) : QVariant()},
            [this, request_id] { emit completed(request_id); });
}

void FakeBlockchain::setConnected(backend::RequestId request_id, QString connection_id,
                                  bool connected) {
    request({.id = request_id,
             .kind = QStringLiteral("setConnected"),
             .operation = connection_id,
             .payload = connected},
            [this, request_id, connection_id, connected] {
                if (!hasConnection(connection_id)) {
                    emit error(request_id,
                               QStringLiteral("unknown connection %1").arg(connection_id));
                    return;
                }
                setConnection(connection_id, connected);
                emit completed(request_id);
            });
}

void FakeBlockchain::refreshTip(backend::RequestId request_id, QString connection_id) {
    request({.id = request_id, .kind = QStringLiteral("refreshTip"), .operation = connection_id},
            [this, request_id, connection_id] {
                if (!hasConnection(connection_id)) {
                    emit error(request_id,
                               QStringLiteral("unknown connection %1").arg(connection_id));
                    return;
                }
                emit completed(request_id);
            });
}

void FakeBlockchain::fetchTransactionDetails(backend::RequestId request_id, QString txid) {
    request({.id = request_id, .kind = QStringLiteral("fetchTransactionDetails"), .payload = txid},
            [this, request_id, txid] {
                auto details = transactionDetails(txid);
                if (!details.has_value()) {
                    emit error(request_id, QStringLiteral("unknown transaction %1").arg(txid));
                    return;
                }
                emit transactionDetailsChanged(txid);
                emit transactionDetailsFetched(request_id, *details);
            });
}

void FakeBlockchain::broadcast(backend::RequestId request_id, QByteArray transaction) {
    request({.id = request_id, .kind = QStringLiteral("broadcast"), .payload = transaction},
            [this, request_id] { emit broadcastFinished(request_id, model.broadcast); });
}

// ===== FakeTransaction =====

FakeTransaction::FakeTransaction()
    : crafting([](const backend::TransactionTemplate &transaction) {
          return backend::CraftResult{.transaction = transaction, .completion = {.complete = true}};
      }),
      finalizing([]([[maybe_unused]] const backend::TransactionTemplate &transaction) {
          return std::optional<QByteArray>();
      }) {
}

auto FakeTransaction::craft(const backend::TransactionTemplate &transaction) const
    -> backend::CraftResult {
    requests.append({.kind = QStringLiteral("craft"), .payload = QVariant::fromValue(transaction)});
    return crafting(transaction);
}

auto FakeTransaction::finalize(const backend::TransactionTemplate &transaction) const
    -> std::optional<QByteArray> {
    requests.append(
        {.kind = QStringLiteral("finalize"), .payload = QVariant::fromValue(transaction)});
    return finalizing(transaction);
}

void FakeTransaction::selectCoins(backend::RequestId request_id,
                                  backend::TransactionTemplate transaction) {
    request(
        {.id = request_id,
         .kind = QStringLiteral("selectCoins"),
         .payload = QVariant::fromValue(transaction)},
        [this, request_id, transaction] { emit coinsSelected(request_id, crafting(transaction)); });
}

auto FakeTransaction::crafts() const -> qsizetype {
    return count(QStringLiteral("craft")) + count(QStringLiteral("selectCoins"));
}

// ===== FakeLabels =====

auto FakeLabels::labels() const -> QList<backend::Label> {
    return model.labels;
}

auto FakeLabels::label(const backend::LabelKey &key) const -> QString {
    for (const auto &label : model.labels) {
        if (label.key.kind == key.kind && label.key.id == key.id) {
            return label.text;
        }
    }
    return {};
}

void FakeLabels::setLabels(const QList<backend::Label> &labels) {
    model.labels = labels;
    emit labelsChanged();
}

void FakeLabels::setLabel(backend::RequestId request_id, backend::LabelKey key, QString text) {
    request({.id = request_id,
             .kind = QStringLiteral("setLabel"),
             .operation = text,
             .payload = QVariant::fromValue(key)},
            [this, request_id, key, text] {
                model.labels.removeIf([&key](const backend::Label &label) {
                    return label.key.kind == key.kind && label.key.id == key.id;
                });
                if (!text.isEmpty()) {
                    model.labels.append({.key = key, .text = text});
                }
                emit labelsChanged();
                emit saved(request_id);
            });
}

// ===== FakeSettings =====

auto FakeSettings::settings() const -> backend::Settings {
    return model.settings;
}

void FakeSettings::setSettings(const backend::Settings &settings) {
    model.settings = settings;
    emit settingsChanged();
}

void FakeSettings::setEndpointsBusy(bool busy) {
    publishValue(account::extension::ENDPOINTS_BUSY, busy);
}

void FakeSettings::testEndpoint(backend::RequestId request_id, backend::Endpoint endpoint) {
    request({.id = request_id,
             .kind = QStringLiteral("testEndpoint"),
             .payload = QVariant::fromValue(endpoint)},
            [this, request_id] { emit endpointTested(request_id); });
}

void FakeSettings::save(backend::RequestId request_id, backend::Settings settings) {
    request({.id = request_id,
             .kind = QStringLiteral("save"),
             .payload = QVariant::fromValue(settings)},
            [this, request_id, settings] {
                auto merged = model.settings;
                merged.network = settings.network;
                for (const auto &endpoint : settings.endpoints) {
                    auto known = std::ranges::find_if(
                        merged.endpoints,
                        [&endpoint](const backend::Endpoint &e) { return e.id == endpoint.id; });
                    if (known == merged.endpoints.end()) {
                        merged.endpoints.append(endpoint);
                    } else {
                        *known = endpoint;
                    }
                }
                setSettings(merged);
                emit saved(request_id);
            });
}

// ===== ViewServices =====

auto ViewServices::services() -> account::Services {
    return {.account = &account,
            .blockchain = &blockchain,
            .transaction = &transaction,
            .labels = &labels,
            .settings = &settings};
}

void ViewServices::flush() {
    account.flush();
    blockchain.flush();
    transaction.flush();
    labels.flush();
    settings.flush();
}

} // namespace smoke
