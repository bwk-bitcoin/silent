#pragma once

#include <Qontrol>
#include <interfaces/types.h>
#include <optional>
#include <qevent.h>
#include <qtmetamacros.h>

namespace catalog {
class AddressRow;
class FoldSection;
} // namespace catalog

#include <qwidget.h>
#include <silent.h>

class AccountController;

namespace view {

class Receive : public qontrol::Screen {
    Q_OBJECT
public:
    Receive(AccountController *ctrl);

public slots:
    void onNewSegwitAddr();
    void onNewTaprootAddr();
    void onShowSegwitHistory();
    void onShowTaprootHistory();
    void onVerifyAddress();
    void onVerifyHistoryAddress(const QString &address);
    void onHistoryLabelEdited(const QString &address, const QString &label);
    void onSpSectionToggled(bool expanded);
    void onSegwitSectionToggled(bool expanded);
    void onTaprootSectionToggled(bool expanded);
    void onKeysError(std::optional<ReqId> req_id, QString message);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    void showVerifyModal(const QString &address);
    void showAddressHistory(const QString &title, const QList<QPair<QString, QString>> &addresses);

private:
    AccountController *m_controller = nullptr;
    QWidget *m_main_widget = nullptr;
    rust::String m_sp_address;
    bool m_has_sub_accounts = false;
    catalog::FoldSection *m_sp_section = nullptr;
    catalog::FoldSection *m_segwit_section = nullptr;
    catalog::FoldSection *m_taproot_section = nullptr;
    catalog::AddressRow *m_sp_row = nullptr;
    catalog::AddressRow *m_segwit_row = nullptr;
    catalog::AddressRow *m_taproot_row = nullptr;
    QList<QPair<QString, QString>> m_segwit_history;
    QList<QPair<QString, QString>> m_taproot_history;
    QString m_active_history_section; // "segwit" / "taproot" for the open modal
    ReqId m_verify_req_id = 0;
};

} // namespace view
