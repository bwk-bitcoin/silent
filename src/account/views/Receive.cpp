#include "account/views/Receive.h"

#include "AppController.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/panels/receive/AddressRow.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "theme/Palette.h"
#include "views/modals/AddressHistoryModal.h"
#include "views/utils.h"

#include <QDebug>
#include <Qontrol>
#include <common.h>
#include <string>

namespace account::view {

Receive::Receive(AccountBackend *backend) : m_backend(backend) {
    init();
    doConnect();
    view();
}

void Receive::init() {
    bool verifiable = verifyUnavailableNote().isEmpty();
    for (auto kind : m_backend->addressKinds()) {
        Section section;
        section.kind = kind;
        section.row = new catalog::AddressRow;
        section.row->setVerifyVisible(verifiable);
        if (kind == AddressKind::SilentPayment) {
            section.row->setGenerateVisible(false);
            section.row->setTwoLineAddress(true); // long SP address wraps to two lines
            section.row->setAddress(m_backend->newAddress(kind));
        } else {
            section.row->setLabelVisible(true);
        }
        section.fold = new catalog::FoldSection(title(kind));
        // The first section opens, the others start folded.
        section.fold->setExpanded(m_sections.isEmpty());
        section.fold->setContent(section.row);
        m_sections.append(section);
    }
    retranslateUi();
}

void Receive::doConnect() {
    for (const auto &section : m_sections) {
        connect(section.fold, &catalog::FoldSection::toggled, this, &Receive::onSectionToggled,
                qontrol::UNIQUE);
        connect(section.row, &catalog::AddressRow::generateRequested, this,
                &Receive::onGenerateRequested, qontrol::UNIQUE);
        connect(section.row, &catalog::AddressRow::historyRequested, this,
                &Receive::onHistoryRequested, qontrol::UNIQUE);
        connect(section.row, &catalog::AddressRow::verifyRequested, this,
                &Receive::onVerifyRequested, qontrol::UNIQUE);
    }
    auto *keys = Host::get()->keys();
    if (keys != nullptr) {
        connect(keys, &IKeys::error, this, &Receive::onKeysError, qontrol::UNIQUE);
    }
}

void Receive::view() {
    auto *col = (new qontrol::Column)->pushSpacer(resolve(Spacing::XXL));
    bool first = true;
    for (const auto &section : m_sections) {
        if (!first) {
            col->pushSpacer(resolve(Spacing::S))
                ->push(new catalog::Separator)
                ->pushSpacer(resolve(Spacing::S));
        }
        first = false;
        col->push(section.fold);
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
    for (const auto &section : m_sections) {
        section.fold->setTitle(title(section.kind));
        if (section.kind != AddressKind::SilentPayment) {
            section.row->setPlaceholderText(TR("receive-not-generated"));
        }
        section.row->setVerifyNote(verifyUnavailableNote());
    }
}

auto Receive::sectionOf(const QObject *widget) -> Section * {
    for (auto &section : m_sections) {
        if (section.fold == widget || section.row == widget) {
            return &section;
        }
    }
    return nullptr;
}

auto Receive::title(AddressKind kind) -> QString {
    switch (kind) {
    case AddressKind::SilentPayment:
        return TR("receive-silent-payment-address");
    case AddressKind::Segwit:
        return TR("receive-segwit-address");
    case AddressKind::Taproot:
        return TR("receive-taproot-address");
    }
    return {};
}

auto Receive::awaitingTitle(AddressKind kind) -> QString {
    return kind == AddressKind::Segwit ? TR("receive-awaiting-segwit")
                                       : TR("receive-awaiting-taproot");
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

    auto info = m_backend->addressVerifyInfo(address);
    auto descriptor = QString::fromStdString(std::string(info.descriptor.c_str()));
    auto path = QString::fromStdString(std::string(info.path.c_str()));
    if (descriptor.isEmpty()) {
        qWarning() << "Receive: no descriptor for address" << address;
        m_verify_error = TR("receive-verify-unavailable");
        return;
    }

    m_verify_req_id = keys->newRequest();
    // Returns once the flow has closed.
    keys->verifyAddress(m_verify_req_id, m_backend->network(), descriptor, path, address);
}

void Receive::showAddressHistory(Section *section) {
    m_history_section = section;
    m_verify_error = verifyUnavailableNote();
    // The verify flow takes the list's place while it runs, then the list comes back.
    while (true) {
        // The current (newest) address is listed with the ones already awaiting payment.
        auto entries = section->history;
        if (!section->row->address().isEmpty()) {
            entries.prepend(qMakePair(section->row->address(), section->row->labelText()));
        }

        m_history_verify_address.clear();
        auto *historyModal =
            new modal::AddressHistoryModal(awaitingTitle(section->kind), entries,
                                           verifyUnavailableNote().isEmpty(), m_verify_error);
        connect(historyModal, &modal::AddressHistoryModal::verifyRequested, this,
                &Receive::onVerifyHistoryAddress, qontrol::UNIQUE);
        connect(historyModal, &modal::AddressHistoryModal::labelEdited, this,
                &Receive::onHistoryLabelEdited, qontrol::UNIQUE);
        AppController::execModal(historyModal);
        if (m_history_verify_address.isEmpty()) {
            m_history_section = nullptr;
            return;
        }
        showVerifyModal(m_history_verify_address);
    }
}

// ===== Slots =====

void Receive::onGenerateRequested() {
    auto *section = sectionOf(sender());
    if (section == nullptr) {
        return;
    }
    auto address = m_backend->newAddress(section->kind);
    if (address.isEmpty()) {
        return;
    }
    auto current = section->row->address();
    if (!current.isEmpty()) {
        section->history.prepend(qMakePair(current, section->row->labelText()));
    }
    section->row->setAddress(address);
    section->row->setLabelText(QString()); // the new address starts unlabeled
    section->row->setHistoryCount(static_cast<int>(section->history.size()));
}

void Receive::onHistoryRequested() {
    auto *section = sectionOf(sender());
    if (section != nullptr) {
        showAddressHistory(section);
    }
}

void Receive::onVerifyRequested() {
    auto *row = qobject_cast<catalog::AddressRow *>(sender());
    if (row == nullptr) {
        return;
    }
    showVerifyModal(row->address());
    row->setVerifyNote(m_verify_error);
}

void Receive::onSectionToggled(bool expanded) {
    if (!expanded) {
        return;
    }
    for (const auto &section : m_sections) {
        if (section.fold != sender()) {
            section.fold->setExpanded(false);
        }
    }
}

void Receive::onVerifyHistoryAddress(const QString &address) {
    m_history_verify_address = address;
}

void Receive::onHistoryLabelEdited(const QString &address, const QString &label) {
    if (m_history_section == nullptr) {
        return;
    }
    if (m_history_section->row->address() == address) {
        m_history_section->row->setLabelText(label);
    }
    for (auto &item : m_history_section->history) {
        if (item.first == address) {
            item.second = label;
        }
    }
}

void Receive::onKeysError(std::optional<ReqId> req_id, const QString &message) {
    if (!req_id.has_value() || req_id.value() != m_verify_req_id) {
        return;
    }
    m_verify_req_id = 0;
    m_verify_error = message;
}

} // namespace account::view
