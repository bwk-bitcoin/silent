#include "account/views/Receive.h"

#include "account/Extensions.h"
#include "account/RequestId.h"
#include "catalog/containers/FoldSection.h"
#include "catalog/containers/Separator.h"
#include "catalog/panels/receive/AddressRow.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "theme/Palette.h"
#include "views/modals/AddressHistoryModal.h"
#include "views/utils.h"

#include <QDebug>
#include <Qontrol>
#include <common.h>

namespace account::view {

Receive::Receive(const Services &services, IModule *module)
    : m_services(services),
      m_module(module) {
    init();
    doConnect();
    view();
}

void Receive::init() {
    bool verifiable = verifyUnavailableNote().isEmpty();
    for (const auto &kind : m_services.account->addressKinds()) {
        Section section;
        section.kind = kind;
        section.row = new catalog::AddressRow;
        section.row->setVerifyVisible(verifiable);
        if (kind.reusable) {
            section.row->setGenerateVisible(false);
            section.row->setTwoLineAddress(true); // a long static address wraps to two lines
        } else {
            section.row->setLabelVisible(true);
        }
        m_sections.append(section);
    }
    showReusableAddresses();
    // A single kind of address (Multikey Wallet) shows its row without a heading.
    if (m_sections.size() > 1) {
        for (auto &section : m_sections) {
            section.fold = new catalog::FoldSection(section.kind.name);
            // The first section opens, the others start folded.
            section.fold->setExpanded(&section == &m_sections.first());
            section.fold->setContent(section.row);
        }
    }
    retranslateUi();
}

void Receive::doConnect() {
    for (const auto &section : m_sections) {
        if (section.fold != nullptr) {
            connect(section.fold, &catalog::FoldSection::toggled, this, &Receive::onSectionToggled,
                    qontrol::UNIQUE);
        }
        connect(section.row, &catalog::AddressRow::generateRequested, this,
                &Receive::onGenerateRequested, qontrol::UNIQUE);
        connect(section.row, &catalog::AddressRow::historyRequested, this,
                &Receive::onHistoryRequested, qontrol::UNIQUE);
        connect(section.row, &catalog::AddressRow::verifyRequested, this,
                &Receive::onVerifyRequested, qontrol::UNIQUE);
    }
    auto *account = m_services.account;
    connect(account, &backend::IAccountBackend::addressesChanged, this,
            &Receive::onAddressesChanged, qontrol::UNIQUE);
    connect(account, &backend::IAccountBackend::addressCreated, this, &Receive::onAddressCreated,
            qontrol::UNIQUE);
    connect(account, &backend::IService::error, this, &Receive::onAddressError, qontrol::UNIQUE);
    connect(account, &backend::IService::valueResult, this, &Receive::onAccountValue,
            qontrol::UNIQUE);
    requestTitles();
    auto *keys = Host::get()->keys(m_module);
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
        if (section.fold != nullptr) {
            col->push(section.fold);
        } else {
            col->push(section.row);
        }
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
    showKindNames();
    for (const auto &section : m_sections) {
        section.row->setVerifyNote(verifyUnavailableNote());
    }
    requestTitles();
}

// The kinds' names as the account says them now, in the language in use.
void Receive::showKindNames() {
    for (const auto &kind : m_services.account->addressKinds()) {
        auto *section = this->section(kind.id);
        if (section == nullptr) {
            continue;
        }
        section->kind.name = kind.name;
        if (section->fold != nullptr) {
            section->fold->setTitle(kind.name);
        }
    }
}

void Receive::requestTitles() {
    m_titles_req = nextRequestId();
    // The built-in providers answer during the call; another may not answer it at all.
    m_services.account->value(m_titles_req, extension::ADDRESS_KIND_TITLES, QJsonValue());
}

auto Receive::sectionOf(const QObject *widget) -> Section * {
    for (auto &section : m_sections) {
        if (section.fold == widget || section.row == widget) {
            return &section;
        }
    }
    return nullptr;
}

auto Receive::section(const QString &kind) -> Section * {
    for (auto &section : m_sections) {
        if (section.kind.id == kind) {
            return &section;
        }
    }
    return nullptr;
}

// The provider's title for the kind, else its name.
auto Receive::awaitingTitle(const backend::AddressKind &kind) const -> QString {
    auto title = m_titles.value(kind.id).toString();
    return title.isEmpty() ? kind.name : title;
}

void Receive::showReusableAddresses() {
    for (const auto &address : m_services.account->addresses()) {
        auto *section = this->section(address.kind);
        if (section != nullptr && section->kind.reusable) {
            section->row->setAddress(address.address);
        }
    }
}

auto Receive::verifyUnavailableNote() const -> QString {
    if (Host::get()->keys(m_module) == nullptr) {
        return TR("receive-verify-unavailable");
    }
    return QString();
}

void Receive::showVerifyModal(const QString &address) {
    m_verify_error = verifyUnavailableNote();
    auto *keys = Host::get()->keys(m_module);
    if (keys == nullptr) {
        return;
    }

    auto info = m_services.account->addressVerification(address);
    auto network = publicNetwork(m_services.account->network());
    if (!info.has_value() || info->descriptor.isEmpty() || !network.has_value()) {
        qWarning() << "Receive: no descriptor for address" << address;
        m_verify_error = TR("receive-verify-unavailable");
        return;
    }

    m_verify_req_id = keys->newRequest();
    // Returns once the flow has closed.
    keys->verifyAddress(m_verify_req_id, *network, info->descriptor, info->path, address,
                        QString());
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
        auto historyTitle =
            m_sections.size() == 1 ? TR("receive-addresses") : awaitingTitle(section->kind);
        auto *historyModal = new modal::AddressHistoryModal(
            historyTitle, entries, verifyUnavailableNote().isEmpty(), m_verify_error);
        connect(historyModal, &modal::AddressHistoryModal::verifyRequested, this,
                &Receive::onVerifyHistoryAddress, qontrol::UNIQUE);
        connect(historyModal, &modal::AddressHistoryModal::labelEdited, this,
                &Receive::onHistoryLabelEdited, qontrol::UNIQUE);
        qontrol::Controller::execModal(historyModal);
        if (m_history_verify_address.isEmpty()) {
            m_history_section = nullptr;
            return;
        }
        showVerifyModal(m_history_verify_address);
    }
}

// ===== Slots =====

void Receive::onAddressesChanged() {
    showKindNames();
    showReusableAddresses();
}

void Receive::onAccountValue(backend::RequestId request_id, const QJsonValue &payload) {
    if (request_id != 0 && request_id == m_titles_req) {
        m_titles_req = 0;
        m_titles = payload.toObject();
    }
}

void Receive::onAddressCreated(backend::RequestId request_id, const backend::Address &address) {
    if (!m_address_requests.contains(request_id)) {
        return;
    }
    auto *section = this->section(m_address_requests.take(request_id));
    if (section == nullptr || address.address.isEmpty()) {
        return;
    }
    auto current = section->row->address();
    if (!current.isEmpty()) {
        section->history.prepend(qMakePair(current, section->row->labelText()));
    }
    section->row->setAddress(address.address);
    section->row->setLabelText(QString()); // the new address starts unlabeled
    section->row->setHistoryCount(static_cast<int>(section->history.size()));
}

// A failed request leaves the address shown as it is.
void Receive::onAddressError(backend::RequestId request_id) {
    m_address_requests.remove(request_id);
}

void Receive::onGenerateRequested() {
    auto *section = sectionOf(sender());
    if (section == nullptr) {
        return;
    }
    auto requestId = nextRequestId();
    // Before the request: the answer may come during the call.
    m_address_requests.insert(requestId, section->kind.id);
    m_services.account->newAddress(requestId, section->kind.id);
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
