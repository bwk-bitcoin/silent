#pragma once

#include "account/AccountBackend.h"
#include "catalog/containers/Modal.h"
#include "catalog/panels/send/TxFlow.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <silent.h>

namespace catalog {
class ConfirmDots;
class Label;
class ModalStatus;
class Notice;
} // namespace catalog

namespace account {

// Opens the details of `txid` over the window, until closed. The caller keeps the
// widget that opened it while it runs.
void showTransactionDetails(AccountBackend *backend, const QString &txid,
                            const QStringList &notes = {});

// One transaction of the account, the same for every account kind: direction, date,
// status, size, its flow, its inputs and outputs with editable labels, fees and total.
// Fetched from the account on open; shown loading until then.
class TransactionDetails : public catalog::Modal {
    Q_OBJECT

public:
    TransactionDetails(AccountBackend *backend, const QString &txid, const QStringList &notes = {});

signals:
    void interactionFinished();

public slots:
    void onBackendUpdated();
    void onInteractionFinished();
    void onTxDetailsReady(uint64_t req_id, const RustTxDetails &details);
    void onEndpointTipReady(uint64_t req_id, Endpoint endpoint, const ElectrumTipResult &result);
    void onLabelEdited(uint64_t req_id, bool accepted, const QString &error);
    void onIoLabelEdited(const QString &label);
    void onAmountsChanged();

protected:
    void init();
    void doConnect();
    void view();

    // Loading view
    void initLoading();
    void showLoading();
    void showFailed(const QString &error);

    // Details view
    auto viewDetails() -> QWidget *;
    void showDetails();
    [[nodiscard]] auto headerRow() const -> QWidget *;
    auto metadataRow() -> QWidget *;
    auto flowCard() -> QWidget *;
    [[nodiscard]] auto flowInputs() const -> QList<catalog::TxFlow::InputItem>;
    [[nodiscard]] auto flowOutputs() const -> QList<catalog::TxFlow::OutputItem>;
    // Sets the flow's bands from the transaction: labels and fiat as they are now.
    void refreshFlow();
    [[nodiscard]] auto ioSections() const -> QWidget *;
    [[nodiscard]] auto ioTable(const rust::Vec<RustTxIo> &ios, bool inputs) const -> QWidget *;
    [[nodiscard]] auto summary() const -> QWidget *;
    // Asks a connected endpoint for the tip the confirmations count from.
    void requestTip();
    void refreshDetails();
    void updateStatus();

private:
    struct PendingLabel {
        QString outpoint;
        QString label;
    };

    QPointer<AccountBackend> m_backend;
    QString m_txid;
    QStringList m_notes;
    uint64_t m_details_request = 0;
    uint64_t m_tip_request = 0;
    RustTxDetails m_details;
    uint32_t m_tip = 0;
    bool m_compact = false;
    bool m_refresh_pending = false;
    bool m_stale = false;
    QHash<uint64_t, PendingLabel> m_label_requests;
    QString m_label_error_text;

    catalog::ModalStatus *m_loading = nullptr;
    QWidget *m_details_widget = nullptr;
    catalog::Notice *m_label_error = nullptr;
    catalog::TxFlowCard *m_flow_card = nullptr;
    catalog::ConfirmDots *m_dots = nullptr;
    catalog::Label *m_status = nullptr;
};

} // namespace account
