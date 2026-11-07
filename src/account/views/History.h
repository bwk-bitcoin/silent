#pragma once

#include "account/Services.h"
#include "interfaces/backend/types.h"

#include <QEvent>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <cstdint>

class QResizeEvent;
class QShowEvent;

namespace catalog {
class HistoryTable;
class Notice;
} // namespace catalog

namespace account::view {

// The account's transactions and its balance, read from IBlockchain whenever it reports
// a change. Confirmations count from the tip of a connected server.
class History : public qontrol::Screen {
    Q_OBJECT

public:
    explicit History(const Services &services);

signals:
    void interactionFinished();

public slots:
    void onInteractionFinished();
    void onFundsChanged();
    void onStateChanged();
    void onTipRefreshed(backend::RequestId request_id);
    void onBlockchainError(backend::RequestId request_id);
    void onLabelSaved(backend::RequestId request_id);
    void onLabelError(backend::RequestId request_id, const QString &message);
    void onRowClicked(const QString &txid);
    void onTxLabelEdited(const QString &txid, const QString &label);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    // The history, unconfirmed transactions first.
    [[nodiscard]] auto readHistory() const -> QList<backend::Transaction>;
    // Lays the rows out again, or once the open details or the label being edited
    // are done: either still holds a widget of the rows.
    void refresh();
    [[nodiscard]] auto isBusy() const -> bool;
    // Asks a connected server for the tip the confirmations count from.
    void requestTip();
    void updateTip(uint32_t tip);
    [[nodiscard]] static auto connectedIds(const backend::BlockchainState &state) -> QSet<QString>;
    void showLabelResult(const QString &error);

private:
    Services m_services;
    backend::Balance m_balance;
    QList<backend::Transaction> m_history;
    uint32_t m_tip = 0;
    backend::RequestId m_tip_request = 0;
    // The connections up, the last time the state was read.
    QSet<QString> m_connected;
    bool m_compact = false;
    bool m_stale = false;
    bool m_details_open = false;
    QSet<backend::RequestId> m_label_requests;
    QString m_label_error;
    QWidget *m_main_widget = nullptr;
    QPointer<catalog::HistoryTable> m_table;
    QPointer<catalog::Notice> m_notice;
};

} // namespace account::view
