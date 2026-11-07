#pragma once

#include "account/Services.h"
#include "interfaces/backend/types.h"

#include <QEvent>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <Qontrol>

class QResizeEvent;

namespace catalog {
class CoinsTable;
class Notice;
} // namespace catalog

namespace account::view {

// The unspent coins and the balance, read from IBlockchain whenever it reports a change.
class Coins : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Coins(const Services &services);

signals:
    void interactionFinished();

public slots:
    void onInteractionFinished();
    void onFundsChanged();
    void onLabelSaved(backend::RequestId request_id);
    void onLabelError(backend::RequestId request_id, const QString &message);
    void onRowClicked(const QString &outpoint);
    void onCoinLabelEdited(const QString &outpoint, const QString &label);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // The unspent coins, unconfirmed ones first.
    [[nodiscard]] auto readCoins() const -> QList<backend::Coin>;
    // The date of each funding transaction the history knows, by txid.
    [[nodiscard]] auto readDates() const -> QHash<QString, quint64>;
    // Lays the rows out again, or once the open details or the label being edited
    // are done: either still holds a widget of the rows.
    void refresh();
    [[nodiscard]] auto isBusy() const -> bool;
    void showLabelResult(const QString &error);

private:
    Services m_services;
    backend::Balance m_balance;
    QList<backend::Coin> m_coins;
    QHash<QString, quint64> m_dates;
    bool m_compact = false;
    bool m_stale = false;
    bool m_details_open = false;
    QSet<backend::RequestId> m_label_requests;
    QString m_label_error;
    QWidget *m_main_widget = nullptr;
    QPointer<catalog::CoinsTable> m_table;
    QPointer<catalog::Notice> m_notice;
};

} // namespace account::view
