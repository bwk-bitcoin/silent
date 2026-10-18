#pragma once

#include "account/AccountBackend.h"
#include "interfaces/types.h"

#include <QEvent>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>
#include <Qontrol>
#include <optional>

namespace catalog {
class AddressRow;
class FoldSection;
} // namespace catalog

namespace account::view {

// One section per kind of address the account receives on. The silent payment
// address is shown as is; the others are generated, labeled and kept in a list of
// the addresses still awaiting payment. Opening a section folds the others.
class Receive : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Receive(AccountBackend *backend);

public slots:
    void onGenerateRequested();
    void onHistoryRequested();
    void onVerifyRequested();
    void onSectionToggled(bool expanded);
    void onVerifyHistoryAddress(const QString &address);
    void onHistoryLabelEdited(const QString &address, const QString &label);
    void onKeysError(std::optional<ReqId> req_id, const QString &message);

protected:
    struct Section {
        AddressKind kind = AddressKind::SilentPayment;
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
    [[nodiscard]] static auto title(AddressKind kind) -> QString;
    [[nodiscard]] static auto awaitingTitle(AddressKind kind) -> QString;
    // Runs the verify flow; a reason it could not start is left in m_verify_error.
    void showVerifyModal(const QString &address);
    // Why the addresses cannot be verified here, empty when they can.
    [[nodiscard]] static auto verifyUnavailableNote() -> QString;
    void showAddressHistory(Section *section);

private:
    AccountBackend *m_backend = nullptr;
    QWidget *m_main_widget = nullptr;
    QList<Section> m_sections;
    // The section whose address list is open.
    Section *m_history_section = nullptr;
    // The address the history modal asked to verify, set as it closes for the flow.
    QString m_history_verify_address;
    ReqId m_verify_req_id = 0;
    QString m_verify_error;
};

} // namespace account::view
