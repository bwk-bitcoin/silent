#include "PriceFeedCoordinator.h"

#include "interfaces/feed.h"
#include <QDebug>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <string>
#include <utility>

PriceFeedConnection::PriceFeedConnection(quint64 registration_id, IFeed *feed, QObject *parent)
    : QObject(parent),
      m_registration_id(registration_id),
      m_feed(feed) {
    auto directUnique = static_cast<Qt::ConnectionType>(qontrol::UNIQUE | Qt::DirectConnection);
    connect(feed, qOverload<ReqId, QStringList>(&IFeed::currencies), this,
            &PriceFeedConnection::onCurrencies, directUnique);
    connect(feed, &IFeed::conversionRate, this, &PriceFeedConnection::onConversionRate,
            directUnique);
    connect(feed, qOverload<ReqId, bool, bool>(&IFeed::feeCapabilities), this,
            &PriceFeedConnection::onFeeCapabilities, directUnique);
    connect(feed, &IFeed::defaultFeeRate, this, &PriceFeedConnection::onDefaultFee, directUnique);
    connect(feed, &IFeed::smartFeeRates, this, &PriceFeedConnection::onSmartFees, directUnique);
}

void PriceFeedConnection::disconnectFeed() {
    if (m_feed != nullptr) {
        disconnect(m_feed, nullptr, this, nullptr);
    }
}

void PriceFeedConnection::onCurrencies([[maybe_unused]] ReqId req_id,
                                       const QStringList &currencies) {
    QStringList owned;
    owned.reserve(currencies.size());
    for (const auto &currency : currencies) {
        owned.append(QString(currency.constData(), currency.size()));
    }
    emit currenciesReady(m_registration_id, owned);
}

void PriceFeedConnection::onConversionRate([[maybe_unused]] ReqId req_id, const QString &currency,
                                           double value) {
    emit conversionRateReady(m_registration_id, QString(currency.constData(), currency.size()),
                             value);
}

PriceFeedCoordinator::PriceFeedCoordinator(QObject *parent) : QObject(parent) {
}

void PriceFeedCoordinator::addSource(const QString &id, const QString &name, IFeed *feed) {
    if (feed == nullptr || !feed->implemented() || m_sources.contains(id)) {
        return;
    }
    auto registrationId = ++m_next_registration_id;
    auto *connection = new PriceFeedConnection(registrationId, feed, this);
    m_sources.insert(
        id,
        {.name = name, .feed = feed, .registration_id = registrationId, .connection = connection});
    m_state->add_feed(rust::String(id.toStdString()));
    connect(connection, &PriceFeedConnection::currenciesReady, this,
            &PriceFeedCoordinator::onCurrencies, qontrol::UNIQUE);
    connect(connection, &PriceFeedConnection::conversionRateReady, this,
            &PriceFeedCoordinator::onConversionRate, qontrol::UNIQUE);
    connect(feed, &QObject::destroyed, this, &PriceFeedCoordinator::onFeedDestroyed,
            qontrol::UNIQUE);
    connect(connection, &PriceFeedConnection::feeCapabilitiesReady, this,
            &PriceFeedCoordinator::onFeeCapabilities, qontrol::UNIQUE);
    connect(connection, &PriceFeedConnection::defaultFeeReady, this,
            &PriceFeedCoordinator::onDefaultFee, qontrol::UNIQUE);
    connect(connection, &PriceFeedConnection::smartFeesReady, this,
            &PriceFeedCoordinator::onSmartFees, qontrol::UNIQUE);
    feed->currencies();
    if (m_sources.contains(id) && m_sources.value(id).registration_id == registrationId) {
        feed->feeCapabilities();
    }
}

auto PriceFeedCoordinator::removeSource(const QString &id) -> std::unique_ptr<PriceFeedConnection> {
    if (!m_sources.contains(id)) {
        return nullptr;
    }
    auto previous = selected();
    auto previousFees = selectedFees();
    auto source = m_sources.take(id);
    m_state->remove_feed(rust::String(id.toStdString()));
    auto connection = std::unique_ptr<PriceFeedConnection>(source.connection);
    connection->disconnectFeed();
    bool shared = false;
    for (const auto &entry : m_sources) {
        if (entry.feed == source.feed) {
            shared = true;
            break;
        }
    }
    if (!shared && source.feed != nullptr) {
        disconnect(source.feed, nullptr, this, nullptr);
    }
    emit sourcesChanged();
    notifySelected(previous);
    emit feeSourcesChanged();
    if (previousFees != selectedFees()) {
        emit selectedFeesChanged();
    }
    return connection;
}

void PriceFeedCoordinator::select(const QString &source, const QString &currency) {
    auto previous = selected();
    m_state->select_price(rust::String(source.toStdString()), rust::String(currency.toStdString()));
    notifySelected(previous);
}

auto PriceFeedCoordinator::sources() const -> QList<PriceSource> {
    QList<PriceSource> result;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        if (it->feed == nullptr) {
            continue;
        }
        auto state = m_state->feed_source(rust::String(it.key().toStdString()));
        QStringList currencies;
        for (const auto &currency : state.currencies) {
            currencies.append(QString::fromStdString(std::string(currency)));
        }
        if (!currencies.empty()) {
            result.append({.id = it.key(), .name = it->name, .currencies = currencies});
        }
    }
    std::ranges::sort(result, {}, &PriceSource::id);
    return result;
}

auto PriceFeedCoordinator::selected() const -> SelectedPrice {
    syncFeedLifetimes();
    auto state = m_state->price_selection();
    return {.source = QString::fromStdString(std::string(state.source)),
            .currency = QString::fromStdString(std::string(state.currency)),
            .rate = state.quote.available ? std::optional(state.quote.value) : std::nullopt};
}

void PriceFeedCoordinator::syncFeedLifetimes() const {
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        if (it->feed == nullptr) {
            m_state->remove_feed(rust::String(it.key().toStdString()));
        }
    }
}

void PriceFeedCoordinator::notifySelected(const SelectedPrice &previous) {
    auto current = selected();
    if (previous.source != current.source || previous.currency != current.currency ||
        previous.rate != current.rate) {
        emit selectedChanged();
    }
}

void PriceFeedCoordinator::onCurrencies(quint64 registration_id, const QStringList &currencies) {
    auto source = std::ranges::find_if(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (source == m_sources.end() || source->feed == nullptr) {
        return;
    }
    auto feed = source->feed;
    auto previous = selected();
    auto id = source.key().toStdString();
    rust::Vec<rust::String> values;
    for (const auto &currency : currencies) {
        values.push_back(rust::String(currency.toStdString()));
    }
    m_state->set_currencies(id, std::move(values));
    QStringList offered;
    for (const auto &currency : m_state->feed_source(id).currencies) {
        offered.append(QString::fromStdString(std::string(currency)));
    }
    emit sourcesChanged();
    notifySelected(previous);
    bool registered = std::ranges::any_of(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (registered && !offered.empty() && feed != nullptr) {
        feed->requestRates(offered);
    }
}

void PriceFeedCoordinator::onConversionRate(quint64 registration_id, const QString &currency,
                                            double value) {
    auto source = std::ranges::find_if(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (source == m_sources.end() || source->feed == nullptr) {
        return;
    }
    auto previous = selected();
    m_state->set_price_quote(rust::String(source.key().toStdString()),
                             rust::String(currency.toStdString()), value);
    notifySelected(previous);
}

void PriceFeedCoordinator::onFeedDestroyed([[maybe_unused]] QObject *feed) {
    // QPointer is already cleared when destroyed() arrives.
    bool changed = false;
    bool feesChanged = false;
    auto preferences = selectedFees().preferences;
    for (auto it = m_sources.begin(); it != m_sources.end();) {
        if (it->feed == nullptr) {
            feesChanged = feesChanged || it.key() == preferences.source;
            m_state->remove_feed(rust::String(it.key().toStdString()));
            delete it->connection;
            it = m_sources.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        emit sourcesChanged();
        emit selectedChanged();
        emit feeSourcesChanged();
        if (feesChanged) {
            emit selectedFeesChanged();
        }
    }
}

void PriceFeedConnection::onFeeCapabilities([[maybe_unused]] ReqId req_id, bool has_default,
                                            bool has_smart) {
    emit feeCapabilitiesReady(m_registration_id, has_default, has_smart);
}

void PriceFeedConnection::onDefaultFee([[maybe_unused]] ReqId req_id, double rate) {
    emit defaultFeeReady(m_registration_id, rate);
}

void PriceFeedConnection::onSmartFees([[maybe_unused]] ReqId req_id, double low, double medium,
                                      double fast) {
    emit smartFeesReady(m_registration_id, low, medium, fast);
}

void PriceFeedCoordinator::selectFees(const FeePreferences &preferences) {
    auto previous = selectedFees();
    RustFeePreferences stored;
    stored.mode = static_cast<uint8_t>(preferences.mode);
    stored.source = rust::String(preferences.source.toStdString());
    if (!m_state->select_fees(stored)) {
        qWarning() << "Invalid fee preferences";
        return;
    }
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
}

auto PriceFeedCoordinator::feeSources() const -> QList<FeeSource> {
    QList<FeeSource> result;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        auto state = m_state->feed_source(rust::String(it.key().toStdString()));
        if (it->feed != nullptr && (state.has_default || state.has_smart)) {
            result.append({.id = it.key(),
                           .name = it->name,
                           .has_default = state.has_default,
                           .has_smart = state.has_smart});
        }
    }
    std::ranges::sort(result, {}, &FeeSource::id);
    return result;
}

auto PriceFeedCoordinator::selectedFees() const -> SelectedFees {
    syncFeedLifetimes();
    auto state = m_state->fee_selection();
    return {
        .preferences = {.mode = static_cast<FeeMode>(state.preferences.mode),
                        .source = QString::fromStdString(std::string(state.preferences.source))},
        .default_rate =
            state.default_rate.available ? std::optional(state.default_rate.value) : std::nullopt,
        .low = state.low.available ? std::optional(state.low.value) : std::nullopt,
        .medium = state.medium.available ? std::optional(state.medium.value) : std::nullopt,
        .fast = state.fast.available ? std::optional(state.fast.value) : std::nullopt};
}

void PriceFeedCoordinator::onFeeCapabilities(quint64 registration_id, bool has_default,
                                             bool has_smart) {
    auto source = std::ranges::find_if(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (source == m_sources.end() || source->feed == nullptr) {
        return;
    }
    auto previous = selectedFees();
    auto feed = source->feed;
    m_state->set_fee_capabilities(rust::String(source.key().toStdString()), has_default, has_smart);
    emit feeSourcesChanged();
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
    auto registered = [this, registration_id]() {
        return std::ranges::any_of(m_sources, [registration_id](const Source &entry) {
            return entry.registration_id == registration_id;
        });
    };
    if (registered() && feed != nullptr && has_default) {
        feed->requestDefaultFeeRate();
    }
    if (registered() && feed != nullptr && has_smart) {
        feed->requestSmartFeeRates();
    }
}

void PriceFeedCoordinator::onDefaultFee(quint64 registration_id, double rate) {
    auto source = std::ranges::find_if(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (source == m_sources.end() || source->feed == nullptr) {
        return;
    }
    auto previous = selectedFees();
    m_state->set_default_fee(rust::String(source.key().toStdString()), rate);
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
}

void PriceFeedCoordinator::onSmartFees(quint64 registration_id, double low, double medium,
                                       double fast) {
    auto source = std::ranges::find_if(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (source == m_sources.end() || source->feed == nullptr) {
        return;
    }
    auto previous = selectedFees();
    m_state->set_smart_fees(rust::String(source.key().toStdString()), low, medium, fast);
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
}
