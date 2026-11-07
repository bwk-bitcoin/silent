#pragma once

#include "account/Services.h"
#include "interfaces/backend/types.h"
#include "interfaces/types.h"

#include <QEvent>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <optional>

class IModule;

namespace catalog {
class AddressRow;
class FoldSection;
} // namespace catalog

namespace account::view {

// One section per kind of address the account receives on, as IAccountBackend lists
// them. A reusable address is shown as is; the others are generated, labeled and kept
// in a list of the addresses still awaiting payment. Opening a section folds the others.
// `module` is the caller the verify flow names to the keys module.
class Receive : public qontrol::Screen {
    Q_OBJECT

public:
    Receive(const Services &services, IModule *module);

public slots:
    void onAddressesChanged();
    void onAccountValue(backend::RequestId request_id, const QJsonValue &payload);
    void onAddressCreated(backend::RequestId request_id, const backend::Address &address);
    void onAddressError(backend::RequestId request_id);
    void onGenerateRequested();
    void onHistoryRequested();
    void onVerifyRequested();
    void onSectionToggled(bool expanded);
    void onVerifyHistoryAddress(const QString &address);
    void onHistoryLabelEdited(const QString &address, const QString &label);
    void onKeysError(std::optional<ReqId> req_id, const QString &message);

protected:
    struct Section {
        backend::AddressKind kind;
        // nullptr when the account receives on a single kind: its row has no heading.
        catalog::FoldSection *fold = nullptr;
        catalog::AddressRow *row = nullptr;
        // The addresses generated before the current one, with their labels.
        QList<QPair<QString, QString>> history;
    };

    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    // The section `widget` (its fold or its row) belongs to, nullptr when none.
    [[nodiscard]] auto sectionOf(const QObject *widget) -> Section *;
    [[nodiscard]] auto section(const QString &kind) -> Section *;
    [[nodiscard]] auto awaitingTitle(const backend::AddressKind &kind) const -> QString;
    void showKindNames();
    // Asks the provider for the kinds' awaiting titles (extension::ADDRESS_KIND_TITLES).
    void requestTitles();
    // Shows the static address of each reusable kind, as the account publishes it.
    void showReusableAddresses();
    // Runs the verify flow; a reason it could not start is left in m_verify_error.
    void showVerifyModal(const QString &address);
    // Why the addresses cannot be verified here, empty when they can.
    [[nodiscard]] auto verifyUnavailableNote() const -> QString;
    void showAddressHistory(Section *section);

private:
    Services m_services;
    IModule *m_module = nullptr;
    QWidget *m_main_widget = nullptr;
    QList<Section> m_sections;
    // The section whose address list is open.
    Section *m_history_section = nullptr;
    // The address the history modal asked to verify, set as it closes for the flow.
    QString m_history_verify_address;
    // The kind of each address request in flight, by request id.
    QHash<backend::RequestId, QString> m_address_requests;
    // The provider's awaiting title of each kind, by kind id.
    QJsonObject m_titles;
    backend::RequestId m_titles_req = 0;
    ReqId m_verify_req_id = 0;
    QString m_verify_error;
};

} // namespace account::view
