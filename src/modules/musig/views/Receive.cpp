#include "modules/musig/views/Receive.h"

#include "catalog/containers/FoldSection.h"
#include "catalog/panels/receive/AddressRow.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"
#include "views/utils.h"

#include <QDebug>
#include <QEvent>
#include <Qontrol>
#include <common.h>
#include <silent.h>
#include <string>

namespace musig::view {

Receive::Receive(MusigAccountController *ctrl) : m_controller(ctrl) {
    if (m_controller == nullptr) {
        return;
    }
    init();
    doConnect();
    view();
}

void Receive::init() {
    m_address_row = new catalog::AddressRow;
    m_address_row->setGenerateVisible(true);
    m_address_row->setLabelVisible(false);
    m_address_row->setVerifyVisible(Host::get()->keys() != nullptr);
    m_address_row->setPlaceholderText(TR("receive-not-generated"));

    m_section = new catalog::FoldSection(TR("musig-receive-address"));
    m_section->setExpanded(true);
    m_section->setContent(m_address_row);
}

void Receive::doConnect() {
    connect(m_address_row, &catalog::AddressRow::generateRequested, this,
            &Receive::onNewAddressClicked, qontrol::UNIQUE);
    connect(m_address_row, &catalog::AddressRow::verifyRequested, this,
            &Receive::onVerifyAddressClicked, qontrol::UNIQUE);
    auto *keys = Host::get()->keys();
    if (keys != nullptr) {
        connect(keys, &IKeys::verified, this, &Receive::onAddressVerified, qontrol::UNIQUE);
        connect(keys, &IKeys::error, this, &Receive::onKeysError, qontrol::UNIQUE);
    }
}

void Receive::view() {
    auto *col = (new qontrol::Column)
                    ->pushSpacer(resolve(Spacing::XXL))
                    ->push(m_section)
                    ->pushSpacer();
    setScreenContent(this, m_main_widget, dashboard(TR("receive-title"), col));
}

void Receive::changeEvent(QEvent *event) {
    // Without a controller the constructor built no widget.
    if (event->type() == QEvent::LanguageChange && m_controller != nullptr) {
        retranslateUi();
        view();
    }
    qontrol::Screen::changeEvent(event);
}

void Receive::retranslateUi() {
    m_address_row->setPlaceholderText(TR("receive-not-generated"));
    m_section->setTitle(TR("musig-receive-address"));
}

// ===== Slots =====

void Receive::onNewAddressClicked() {
    auto address = QString::fromStdString(std::string(m_controller->newAddress().c_str()));
    if (address.isEmpty()) {
        return;
    }
    m_address_row->setAddress(address);
    m_address_row->setVerifyNote(QString());
}

void Receive::onVerifyAddressClicked() {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        m_address_row->setVerifyNote(TR("receive-verify-unavailable"));
        return;
    }
    auto address = m_address_row->address();
    auto info = m_controller->addressVerifyInfo(address);
    auto descriptor = QString::fromStdString(std::string(info.descriptor.c_str()));
    auto path = QString::fromStdString(std::string(info.path.c_str()));
    if (descriptor.isEmpty()) {
        qWarning() << "musig Receive: no descriptor for address" << address;
        m_address_row->setVerifyNote(TR("receive-verify-unavailable"));
        return;
    }
    m_address_row->setVerifyNote(QString());
    m_verify_req_id = keys->newRequest();
    // Returns once the flow has closed; the answer may arrive during the call.
    keys->verifyAddress(m_verify_req_id, m_controller->network(), descriptor, path, address);
}

void Receive::onAddressVerified(ReqId req_id, bool verified) {
    if (req_id != m_verify_req_id) {
        return;
    }
    m_verify_req_id = 0;
    m_address_row->setVerifyNote(verified ? QString() : TR("musig-receive-verify-mismatch"));
}

void Receive::onKeysError(std::optional<ReqId> req_id, const QString &message,
                          [[maybe_unused]] plugin::SignError kind) {
    if (!req_id.has_value() || req_id.value() != m_verify_req_id) {
        return;
    }
    m_verify_req_id = 0;
    m_address_row->setVerifyNote(message);
}

} // namespace musig::view
