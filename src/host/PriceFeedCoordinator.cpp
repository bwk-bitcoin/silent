#include "PriceFeedCoordinator.h"

#include "interfaces/feed.h"
#include <Qontrol>
#include <algorithm>
#include <cmath>
#include <common.h>
#include <iterator>

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
    m_sources.insert(id, {.name = name,
                          .feed = feed,
                          .currencies = {},
                          .rates = {},
                          .registration_id = registrationId,
                          .connection = connection,
                          .has_default = false,
                          .has_smart = false,
                          .default_rate = {},
                          .low = {},
                          .medium = {},
                          .fast = {}});
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
    m_selected_source = source;
    m_selected_currency = currency;
    // Plugin literals can live in an unloaded library's read-only storage.
    m_selected_source.detach();
    m_selected_currency.detach();
    notifySelected(previous);
}

auto PriceFeedCoordinator::sources() const -> QList<PriceSource> {
    QList<PriceSource> result;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        if (!it->currencies.empty()) {
            result.append({.id = it.key(), .name = it->name, .currencies = it->currencies});
        }
    }
    std::ranges::sort(result, {}, &PriceSource::id);
    return result;
}

auto PriceFeedCoordinator::selected() const -> SelectedPrice {
    SelectedPrice result{
        .source = m_selected_source, .currency = m_selected_currency, .rate = std::nullopt};
    auto source = m_sources.constFind(m_selected_source);
    if (source != m_sources.cend() && source->feed != nullptr &&
        source->currencies.contains(m_selected_currency)) {
        auto rate = source->rates.constFind(m_selected_currency);
        if (rate != source->rates.cend()) {
            result.rate = rate.value();
        }
    }
    return result;
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
    QStringList offered = currencies;
    offered.removeAll(QString());
    offered.removeDuplicates();
    offered.sort();
    source->currencies = offered;
    for (auto it = source->rates.begin(); it != source->rates.end();) {
        it = offered.contains(it.key()) ? std::next(it) : source->rates.erase(it);
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
    if (source == m_sources.end() || source->feed == nullptr ||
        !source->currencies.contains(currency)) {
        return;
    }
    auto previous = selected();
    if (std::isfinite(value) && value > 0) {
        auto index = source->currencies.indexOf(currency);
        source->rates.insert(source->currencies.at(index), value);
    } else {
        source->rates.remove(currency);
    }
    notifySelected(previous);
}

void PriceFeedCoordinator::onFeedDestroyed([[maybe_unused]] QObject *feed) {
    // QPointer is already cleared when destroyed() arrives.
    bool changed = false;
    bool feesChanged = false;
    for (auto it = m_sources.begin(); it != m_sources.end();) {
        if (it->feed == nullptr) {
            feesChanged = feesChanged || it.key() == m_fee_preferences.default_source ||
                          it.key() == m_fee_preferences.smart_source;
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
    m_fee_preferences = preferences;
    m_fee_preferences.default_source.detach();
    m_fee_preferences.smart_source.detach();
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
}

auto PriceFeedCoordinator::feeSources() const -> QList<FeeSource> {
    QList<FeeSource> result;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        if (it->feed != nullptr && (it->has_default || it->has_smart)) {
            result.append({.id = it.key(),
                           .name = it->name,
                           .has_default = it->has_default,
                           .has_smart = it->has_smart});
        }
    }
    std::ranges::sort(result, {}, &FeeSource::id);
    return result;
}

auto PriceFeedCoordinator::selectedFees() const -> SelectedFees {
    SelectedFees result{
        .preferences = m_fee_preferences, .default_rate = {}, .low = {}, .medium = {}, .fast = {}};
    auto source = m_sources.constFind(m_fee_preferences.default_source);
    if (source != m_sources.cend() && source->feed != nullptr && source->has_default) {
        result.default_rate = source->default_rate;
    }
    source = m_sources.constFind(m_fee_preferences.smart_source);
    if (source != m_sources.cend() && source->feed != nullptr && source->has_smart) {
        result.low = source->low;
        result.medium = source->medium;
        result.fast = source->fast;
    }
    return result;
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
    source->has_default = has_default;
    source->has_smart = has_smart;
    if (!has_default) {
        source->default_rate.reset();
    }
    if (!has_smart) {
        source->low.reset();
        source->medium.reset();
        source->fast.reset();
    }
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
    if (source == m_sources.end() || source->feed == nullptr || !source->has_default) {
        return;
    }
    auto previous = selectedFees();
    source->default_rate = std::isfinite(rate) && rate > 0 ? std::optional(rate) : std::nullopt;
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
}

void PriceFeedCoordinator::onSmartFees(quint64 registration_id, double low, double medium,
                                       double fast) {
    auto source = std::ranges::find_if(m_sources, [registration_id](const Source &entry) {
        return entry.registration_id == registration_id;
    });
    if (source == m_sources.end() || source->feed == nullptr || !source->has_smart) {
        return;
    }
    auto previous = selectedFees();
    auto valid = [](double rate) -> std::optional<double> {
        return std::isfinite(rate) && rate > 0 ? std::optional(rate) : std::nullopt;
    };
    source->low = valid(low);
    source->medium = valid(medium);
    source->fast = valid(fast);
    if (previous != selectedFees()) {
        emit selectedFeesChanged();
    }
}
