#pragma once

#include "interfaces/types.h"

#include <QString>
#include <QWidget>
#include <Qontrol>
#include <optional>

class MusigAccountController;

namespace catalog {
class AddressRow;
class FoldSection;
} // namespace catalog

namespace musig::view {

class Receive : public qontrol::Screen {
    Q_OBJECT

public:
    explicit Receive(MusigAccountController *ctrl);

public slots:
    void onNewAddressClicked();
    void onVerifyAddressClicked();
    void onAddressVerified(ReqId req_id, bool verified);
    void onKeysError(std::optional<ReqId> req_id, const QString &message, plugin::SignError kind);

protected:
    void init() override;
    void doConnect() override;
    void view() override;
    void changeEvent(QEvent *event) override;
    void retranslateUi();

private:
    MusigAccountController *m_controller = nullptr;
    QWidget *m_main_widget = nullptr;
    catalog::FoldSection *m_section = nullptr;
    catalog::AddressRow *m_address_row = nullptr;
    ReqId m_verify_req_id = 0;
};

} // namespace musig::view
