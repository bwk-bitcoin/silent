#pragma once

#include "interfaces/host.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <memory>

class IFeed;

// Direct ingress owns plugin strings before any cross-thread delivery is queued.
class PriceFeedConnection final : public QObject {
    Q_OBJECT

public:
    PriceFeedConnection(quint64 registration_id, IFeed *feed, QObject *parent);
    void disconnectFeed();

signals:
    void currenciesReady(quint64 registration_id, QStringList currencies);
    void conversionRateReady(quint64 registration_id, QString currency, double value);
    void feeCapabilitiesReady(quint64 registration_id, bool has_default, bool has_smart);
    void defaultFeeReady(quint64 registration_id, double rate);
    void smartFeesReady(quint64 registration_id, double low, double medium, double fast);

public slots:
    void onCurrencies(ReqId req_id, const QStringList &currencies);
    void onConversionRate(ReqId req_id, const QString &currency, double value);
    void onFeeCapabilities(ReqId req_id, bool has_default, bool has_smart);
    void onDefaultFee(ReqId req_id, double rate);
    void onSmartFees(ReqId req_id, double low, double medium, double fast);

private:
    quint64 m_registration_id = 0;
    QPointer<IFeed> m_feed;
};

class PriceFeedCoordinator : public QObject {
    Q_OBJECT

public:
    explicit PriceFeedCoordinator(QObject *parent = nullptr);
    void addSource(const QString &id, const QString &name, IFeed *feed);
    // Keep the disconnected receiver alive until the instance has stopped its emitters.
    auto removeSource(const QString &id) -> std::unique_ptr<PriceFeedConnection>;
    void select(const QString &source, const QString &currency);
    [[nodiscard]] auto sources() const -> QList<PriceSource>;
    [[nodiscard]] auto selected() const -> SelectedPrice;
    void selectFees(const FeePreferences &preferences);
    [[nodiscard]] auto feeSources() const -> QList<FeeSource>;
    [[nodiscard]] auto selectedFees() const -> SelectedFees;

signals:
    void sourcesChanged();
    void selectedChanged();
    void feeSourcesChanged();
    void selectedFeesChanged();

public slots:
    void onCurrencies(quint64 registration_id, const QStringList &currencies);
    void onConversionRate(quint64 registration_id, const QString &currency, double value);
    void onFeedDestroyed(QObject *feed);
    void onFeeCapabilities(quint64 registration_id, bool has_default, bool has_smart);
    void onDefaultFee(quint64 registration_id, double rate);
    void onSmartFees(quint64 registration_id, double low, double medium, double fast);

protected:
    void notifySelected(const SelectedPrice &previous);

private:
    struct Source {
        QString name;
        QPointer<IFeed> feed;
        QStringList currencies;
        QHash<QString, double> rates;
        quint64 registration_id = 0;
        PriceFeedConnection *connection = nullptr;
        bool has_default = false;
        bool has_smart = false;
        std::optional<double> default_rate;
        std::optional<double> low;
        std::optional<double> medium;
        std::optional<double> fast;
    };

    QHash<QString, Source> m_sources;
    QString m_selected_source;
    QString m_selected_currency;
    quint64 m_next_registration_id = 0;
    FeePreferences m_fee_preferences;
};
