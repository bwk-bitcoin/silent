#pragma once

#include "account/AccountBackend.h"

#include <QEvent>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <cstdint>
#include <silent.h>

class QResizeEvent;
class QShowEvent;

namespace catalog {
class HistoryTable;
class Notice;
} // namespace catalog

namespace account::view {

class History : public qontrol::Screen {
    Q_OBJECT

public:
    explicit History(AccountBackend *backend);

signals:
    void interactionFinished();

public slots:
    void onInteractionFinished();
    void onCoinsUpdated(const CoinState &state);
    void onScanProgress(uint32_t height, uint32_t tip);
    void onEndpointStateChanged(Endpoint endpoint);
    void onEndpointTipReady(uint64_t req_id, Endpoint endpoint, const ElectrumTipResult &result);
    void onLabelsChanged();
    void onLabelEdited(uint64_t req_id, bool accepted, const QString &error);
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
    [[nodiscard]] auto readHistory() const -> QList<RustTx>;
    // Lays the rows out again, or once the open details or the label being edited
    // are done: either still holds a widget of the rows.
    void refresh();
    [[nodiscard]] auto isBusy() const -> bool;
    // Asks a connected endpoint for the tip the confirmations count from.
    void requestTip();

private:
    AccountBackend *m_backend = nullptr;
    CoinState m_state{};
    QList<RustTx> m_history;
    uint32_t m_tip = 0;
    uint64_t m_tip_request = 0;
    bool m_compact = false;
    bool m_stale = false;
    bool m_details_open = false;
    QSet<uint64_t> m_label_requests;
    QString m_label_error;
    QWidget *m_main_widget = nullptr;
    QPointer<catalog::HistoryTable> m_table;
    QPointer<catalog::Notice> m_notice;
};

} // namespace account::view
