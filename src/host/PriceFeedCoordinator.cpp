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
                          .connection = connection});
    connect(connection, &PriceFeedConnection::currenciesReady, this,
            &PriceFeedCoordinator::onCurrencies, qontrol::UNIQUE);
    connect(connection, &PriceFeedConnection::conversionRateReady, this,
            &PriceFeedCoordinator::onConversionRate, qontrol::UNIQUE);
    connect(feed, &QObject::destroyed, this, &PriceFeedCoordinator::onFeedDestroyed,
            qontrol::UNIQUE);
    feed->currencies();
}

auto PriceFeedCoordinator::removeSource(const QString &id) -> std::unique_ptr<PriceFeedConnection> {
    if (!m_sources.contains(id)) {
        return nullptr;
    }
    auto previous = selected();
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
    for (auto it = m_sources.begin(); it != m_sources.end();) {
        if (it->feed == nullptr) {
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
    }
}
