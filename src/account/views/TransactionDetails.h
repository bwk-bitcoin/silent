#pragma once

#include "account/Services.h"
#include "catalog/containers/Modal.h"
#include "catalog/panels/send/TxFlow.h"
#include "interfaces/backend/types.h"

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

namespace backend {
class IBlockchain;
class ILabels;
} // namespace backend

namespace catalog {
class ConfirmDots;
class Label;
class ModalStatus;
class Notice;
} // namespace catalog

namespace account {

// Opens the details of `txid` over the window, until closed. The caller keeps the
// widget that opened it while it runs.
void showTransactionDetails(const Services &services, const QString &txid,
                            const QStringList &notes = {});

// One transaction of the account, the same for every account kind: direction, date,
// status, size, its flow, its inputs and outputs with editable labels, fees and total.
// Shown at once from what the account already knows of it, loading otherwise, then
// fetched from the account again and whenever the account changes.
class TransactionDetails : public catalog::Modal {
    Q_OBJECT

public:
    TransactionDetails(const Services &services, const QString &txid,
                       const QStringList &notes = {});

signals:
    void interactionFinished();

public slots:
    void onBackendUpdated();
    void onInteractionFinished();
    void onDetailsFetched(backend::RequestId request_id,
                          const backend::TransactionDetails &details);
    void onBlockchainError(backend::RequestId request_id, const QString &message);
    void onExtrasResult(backend::RequestId request_id, const QJsonValue &payload);
    void onTipRefreshed(backend::RequestId request_id);
    void onLabelSaved(backend::RequestId request_id);
    void onLabelError(backend::RequestId request_id, const QString &message);
    void onIoLabelEdited(const QString &label);
    void onAmountsChanged();

protected:
    // What the details leave out, from the provider's extension when it answers it.
    struct Extras {
        qint64 version = 0;
        qint64 lockTime = 0;
        std::optional<quint64> chainTip;
        // The script of each output that pays no address, by output index.
        QHash<quint32, QString> scripts;
    };

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
    // The address an output pays, its script when it has none (OP_RETURN).
    [[nodiscard]] auto outputId(const backend::TransactionIo &io) const -> QString;
    // Sets the flow's bands from the transaction: labels and fiat as they are now.
    void refreshFlow();
    [[nodiscard]] auto ioSections() const -> QWidget *;
    [[nodiscard]] auto ioTable(const QList<backend::TransactionIo> &ios, bool inputs) const
        -> QWidget *;
    [[nodiscard]] auto summary() const -> QWidget *;
    // Takes `details` as the transaction shown, laid out again when it changed.
    void applyDetails(const backend::TransactionDetails &details);
    // Asks the provider for what the details leave out.
    void requestExtras();
    // Asks a connected server for the tip the confirmations count from.
    void requestTip();
    [[nodiscard]] auto tip() const -> uint32_t;
    void refreshDetails();
    void updateStatus();
    void setIoLabel(const QString &outpoint, const QString &label);

private:
    struct PendingLabel {
        QString outpoint;
        QString label;
    };

    Services m_services;
    QPointer<backend::IBlockchain> m_blockchain;
    QPointer<backend::ILabels> m_labels;
    QString m_txid;
    QStringList m_notes;
    backend::RequestId m_details_request = 0;
    backend::RequestId m_extras_request = 0;
    backend::RequestId m_tip_request = 0;
    backend::TransactionDetails m_details;
    Extras m_extras;
    bool m_compact = false;
    bool m_refresh_pending = false;
    bool m_stale = false;
    QHash<backend::RequestId, PendingLabel> m_label_requests;
    QString m_label_error_text;

    catalog::ModalStatus *m_loading = nullptr;
    QWidget *m_details_widget = nullptr;
    catalog::Notice *m_label_error = nullptr;
    catalog::TxFlowCard *m_flow_card = nullptr;
    catalog::ConfirmDots *m_dots = nullptr;
    catalog::Label *m_status = nullptr;
};

} // namespace account
