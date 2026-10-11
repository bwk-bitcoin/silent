#include "Receive.h"
#include "AccountController.h"
#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/CopyButton.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/inputs/Input.h"
#include "catalog/panels/receive/AddressRow.h"
#include "catalog/panels/receive/LabelRow.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/keys/KeysModule.h"
#include "theme/Palette.h"
#include "views/modals/AddressHistoryModal.h"
#include "views/utils.h"
#include <Qontrol>
#include <common.h>

namespace view {

Receive::Receive(AccountController *ctrl) : m_controller(ctrl), m_sp_address("") {
    if (m_controller == nullptr) {
        return;
    }
    this->init();
    this->doConnect();
    this->view();
}

void Receive::init() {
    m_sp_address = m_controller->getSpAddress();
    m_has_segwit = m_controller->hasSegwit();
    m_has_taproot = m_controller->hasTaproot();
    m_has_sub_accounts = m_has_segwit || m_has_taproot;
    m_sp_section = new catalog::FoldSection(TR("receive-silent-payment-address"));
    m_sp_row = new catalog::AddressRow;
    m_sp_row->setGenerateVisible(false);
    m_sp_row->setVerifyVisible(false);
    m_sp_row->setTwoLineAddress(true); // long SP address wraps to two lines
    m_sp_row->setAddress(QString(m_sp_address.c_str()));
    m_sp_section->setContent(m_sp_row);

    if (m_has_sub_accounts) {
        m_segwit_section = new catalog::FoldSection(TR("receive-segwit-address"));
        m_segwit_row = new catalog::AddressRow;
        m_segwit_row->setLabelVisible(true);
        m_segwit_row->setVerifyVisible(verifyUnavailableNote().isEmpty());
        m_segwit_section->setExpanded(false);
        m_segwit_section->setContent(m_segwit_row);
        m_taproot_section = new catalog::FoldSection(TR("receive-taproot-address"));
        m_taproot_row = new catalog::AddressRow;
        m_taproot_row->setLabelVisible(true);
        m_taproot_row->setVerifyVisible(verifyUnavailableNote().isEmpty());
        m_taproot_section->setExpanded(false);
        m_taproot_section->setContent(m_taproot_row);
    }

    retranslateUi();
}

void Receive::doConnect() {
    connect(m_sp_section, &catalog::FoldSection::toggled, this, &Receive::onSpSectionToggled,
            qontrol::UNIQUE);
    if (m_has_sub_accounts) {
        connect(m_segwit_row, &catalog::AddressRow::generateRequested, this,
                &Receive::onNewSegwitAddr, qontrol::UNIQUE);
        connect(m_taproot_row, &catalog::AddressRow::generateRequested, this,
                &Receive::onNewTaprootAddr, qontrol::UNIQUE);
        connect(m_segwit_row, &catalog::AddressRow::historyRequested, this,
                &Receive::onShowSegwitHistory, qontrol::UNIQUE);
        connect(m_taproot_row, &catalog::AddressRow::historyRequested, this,
                &Receive::onShowTaprootHistory, qontrol::UNIQUE);
        connect(m_segwit_row, &catalog::AddressRow::verifyRequested, this,
                &Receive::onVerifyAddress, qontrol::UNIQUE);
        connect(m_taproot_row, &catalog::AddressRow::verifyRequested, this,
                &Receive::onVerifyAddress, qontrol::UNIQUE);
        connect(m_segwit_section, &catalog::FoldSection::toggled, this,
                &Receive::onSegwitSectionToggled, qontrol::UNIQUE);
        connect(m_taproot_section, &catalog::FoldSection::toggled, this,
                &Receive::onTaprootSectionToggled, qontrol::UNIQUE);
    }
    auto *keys = Host::get()->keys();
    if (keys != nullptr) {
        connect(keys, &IKeys::error, this, &Receive::onKeysError, qontrol::UNIQUE);
    }
}

void Receive::onNewSegwitAddr() {
    if (m_controller == nullptr) {
        return;
    }
    auto addr = QString::fromStdString(std::string(m_controller->newSegwitAddr().c_str()));
    if (addr.isEmpty()) {
        return;
    }
    auto current = m_segwit_row->address();
    if (!current.isEmpty()) {
        m_segwit_history.prepend(qMakePair(current, m_segwit_row->labelText()));
    }
    m_segwit_row->setAddress(addr);
    m_segwit_row->setLabelText(QString()); // the new address starts unlabeled
    m_segwit_row->setHistoryCount(static_cast<int>(m_segwit_history.size()));
}

void Receive::onNewTaprootAddr() {
    if (m_controller == nullptr) {
        return;
    }
    auto addr = QString::fromStdString(std::string(m_controller->newTaprootAddr().c_str()));
    if (addr.isEmpty()) {
        return;
    }
    auto current = m_taproot_row->address();
    if (!current.isEmpty()) {
        m_taproot_history.prepend(qMakePair(current, m_taproot_row->labelText()));
    }
    m_taproot_row->setAddress(addr);
    m_taproot_row->setLabelText(QString()); // the new address starts unlabeled
    m_taproot_row->setHistoryCount(static_cast<int>(m_taproot_history.size()));
}

void Receive::onShowSegwitHistory() {
    showAddressHistory(TR("receive-awaiting-segwit"), m_segwit_history);
}

void Receive::onShowTaprootHistory() {
    showAddressHistory(TR("receive-awaiting-taproot"), m_taproot_history);
}

void Receive::onVerifyAddress() {
    auto *row = qobject_cast<catalog::AddressRow *>(sender());
    if (row == nullptr) {
        return;
    }
    showVerifyModal(row->address());
    row->setVerifyNote(m_verify_error);
}

auto Receive::verifyUnavailableNote() -> QString {
    if (Host::get()->keys() == nullptr) {
        return TR("receive-verify-unavailable");
    }
    return QString();
}

void Receive::showVerifyModal(const QString &address) {
    m_verify_error = verifyUnavailableNote();
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }

    auto info = m_controller->addressVerifyInfo(address);
    auto descriptor = keys::KeysModule::toQt(info.descriptor);
    auto path = keys::KeysModule::toQt(info.path);
    if (descriptor.isEmpty()) {
        qWarning() << "Receive: no descriptor for address" << address;
        m_verify_error = TR("receive-verify-unavailable");
        return;
    }

    m_verify_req_id = keys->newRequest();
    // Returns once the flow has closed.
    keys->verifyAddress(m_verify_req_id, descriptor, path, address);
}

void Receive::onKeysError(std::optional<ReqId> req_id, const QString &message) {
    if (!req_id.has_value() || req_id.value() != m_verify_req_id) {
        return;
    }
    m_verify_req_id = 0;
    m_verify_error = message;
}

void Receive::onSpSectionToggled(bool expanded) {
    if (!expanded || !m_has_sub_accounts) {
        return;
    }
    m_segwit_section->setExpanded(false);
    m_taproot_section->setExpanded(false);
}

void Receive::onSegwitSectionToggled(bool expanded) {
    if (!expanded) {
        return;
    }
    m_sp_section->setExpanded(false);
    if (m_taproot_section != nullptr) {
        m_taproot_section->setExpanded(false);
    }
}

void Receive::onTaprootSectionToggled(bool expanded) {
    if (!expanded) {
        return;
    }
    m_sp_section->setExpanded(false);
    if (m_segwit_section != nullptr) {
        m_segwit_section->setExpanded(false);
    }
}

void Receive::onVerifyHistoryAddress(const QString &address) {
    m_history_verify_address = address;
}

void Receive::onHistoryLabelEdited(const QString &address, const QString &label) {
    if (m_active_history_section == "segwit") {
        if (m_segwit_row != nullptr && m_segwit_row->address() == address) {
            m_segwit_row->setLabelText(label);
        }
        for (auto &item : m_segwit_history) {
            if (item.first == address) {
                item.second = label;
            }
        }
    }
    if (m_active_history_section == "taproot") {
        if (m_taproot_row != nullptr && m_taproot_row->address() == address) {
            m_taproot_row->setLabelText(label);
        }
        for (auto &item : m_taproot_history) {
            if (item.first == address) {
                item.second = label;
            }
        }
    }
}

void Receive::showAddressHistory(const QString &title,
                                 const QList<QPair<QString, QString>> &addresses) {
    m_active_history_section = title == TR("receive-awaiting-segwit") ? "segwit" : "taproot";
    m_verify_error = verifyUnavailableNote();
    // The verify flow takes the list's place while it runs, then the list comes back.
    while (true) {
        // Prepend the current (newest) address so the modal lists it alongside the
        // ones already awaiting payment.
        QList<QPair<QString, QString>> fullList = addresses;
        if (m_active_history_section == "segwit" && m_segwit_row != nullptr &&
            !m_segwit_row->address().isEmpty()) {
            fullList.prepend(qMakePair(m_segwit_row->address(), m_segwit_row->labelText()));
        }
        if (m_active_history_section == "taproot" && m_taproot_row != nullptr &&
            !m_taproot_row->address().isEmpty()) {
            fullList.prepend(qMakePair(m_taproot_row->address(), m_taproot_row->labelText()));
        }

        m_history_verify_address.clear();
        auto *historyModal = new modal::AddressHistoryModal(
            title, fullList, verifyUnavailableNote().isEmpty(), m_verify_error);
        connect(historyModal, &modal::AddressHistoryModal::verifyRequested, this,
                &Receive::onVerifyHistoryAddress, qontrol::UNIQUE);
        connect(historyModal, &modal::AddressHistoryModal::labelEdited, this,
                &Receive::onHistoryLabelEdited, qontrol::UNIQUE);
        AppController::execModal(historyModal);
        if (m_history_verify_address.isEmpty()) {
            return;
        }
        showVerifyModal(m_history_verify_address);
    }
}

void Receive::view() {
    auto *col = (new qontrol::Column)->pushSpacer(resolve(Spacing::XXL))->push(m_sp_section);

    auto vSpace = Spacing::S;
    if (m_has_segwit) {
        col->pushSpacer(resolve(vSpace))
            ->push(new catalog::Separator)
            ->pushSpacer(resolve(vSpace))
            ->push(m_segwit_section);
    }
    if (m_has_taproot) {
        col->pushSpacer(resolve(vSpace))
            ->push(new catalog::Separator)
            ->pushSpacer(resolve(vSpace))
            ->push(m_taproot_section);
    }

    col->pushSpacer();

    setScreenContent(this, m_main_widget, dashboard(TR("receive-title"), col));
}

void Receive::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        view();
    }
    qontrol::Screen::changeEvent(event);
}

void Receive::retranslateUi() {
    m_sp_section->setTitle(TR("receive-silent-payment-address"));
    m_sp_row->setVerifyNote(TR("receive-sp-verify-note"));
    if (m_segwit_section != nullptr) {
        m_segwit_section->setTitle(TR("receive-segwit-address"));
    }
    if (m_segwit_row != nullptr) {
        m_segwit_row->setPlaceholderText(TR("receive-not-generated"));
        m_segwit_row->setVerifyNote(verifyUnavailableNote());
    }
    if (m_taproot_section != nullptr) {
        m_taproot_section->setTitle(TR("receive-taproot-address"));
    }
    if (m_taproot_row != nullptr) {
        m_taproot_row->setPlaceholderText(TR("receive-not-generated"));
        m_taproot_row->setVerifyNote(verifyUnavailableNote());
    }
}

} // namespace view
