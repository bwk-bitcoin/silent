#pragma once

#include <QEvent>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <silent.h>

class AccountBackend;
class QResizeEvent;

namespace catalog {
class CoinsTable;
class Notice;
} // namespace catalog

namespace account::view {

class Coins : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Coins(AccountBackend *backend);

signals:
    void interactionFinished();

public slots:
    void onInteractionFinished();
    void onCoinsUpdated(const CoinState &state);
    void onLabelsChanged();
    void onLabelEdited(uint64_t req_id, bool accepted, const QString &error);
    void onRowClicked(const QString &outpoint);
    void onCoinLabelEdited(const QString &outpoint, const QString &label);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // The coins, unconfirmed ones first.
    [[nodiscard]] auto readCoins() const -> QList<RustCoin>;
    // Lays the rows out again, or once the open details or the label being edited
    // are done: either still holds a widget of the rows.
    void refresh();
    [[nodiscard]] auto isBusy() const -> bool;

private:
    AccountBackend *m_backend = nullptr;
    CoinState m_state{};
    QList<RustCoin> m_coins;
    bool m_compact = false;
    bool m_stale = false;
    bool m_details_open = false;
    QSet<uint64_t> m_label_requests;
    QString m_label_error;
    QWidget *m_main_widget = nullptr;
    QPointer<catalog::CoinsTable> m_table;
    QPointer<catalog::Notice> m_notice;
};

} // namespace account::view
