#include "RegisterDescriptor.h"

#include "catalog/Button.h"
#include "catalog/ClearRows.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"

#include <QEvent>
#include <QPushButton>
#include <Qontrol>
#include <common.h>

namespace musig::modal {

namespace {

const char *const K_FINGERPRINT = "fingerprint";

auto keyEntries(Network network) -> QHash<QString, KeyEntry> {
    QHash<QString, KeyEntry> entries;
    if (auto *keys = Host::get()->keys(); keys != nullptr) {
        for (const auto &entry : keys->keys(network)) {
            entries.insert(entry.fingerprint.toLower(), entry);
        }
    }
    return entries;
}

} // namespace

RegisterDescriptor::RegisterDescriptor(MusigAccountController *controller)
    : m_controller(controller),
      m_descriptor(QString::fromStdString(std::string(controller->getDescriptor()))),
      m_fingerprints(controller->cosignerFingerprints()) {
    init();
    doConnect();
    view();
    for (const auto &fingerprint : m_fingerprints) {
        if (needsRegister(fingerprint)) {
            check(fingerprint);
        } else {
            m_states.insert(fingerprint, State::NotNeeded);
        }
    }
    refresh();
}

void RegisterDescriptor::init() {
    m_lead = new catalog::Label(catalog::LabelRole::Body);
    m_lead->setWordWrap(true);
    m_rows = new qontrol::Column;
    m_rows->spacing(resolve(Spacing::XS));
    m_done_btn = catalog::btnDone();
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::None);
    m_actions->addAction(m_done_btn);
}

void RegisterDescriptor::doConnect() {
    connect(m_done_btn, &QPushButton::clicked, this, &RegisterDescriptor::onDoneClicked,
            qontrol::UNIQUE);
    if (auto *keys = Host::get()->keys(); keys != nullptr) {
        connect(keys, &IKeys::registrationChecked, this, &RegisterDescriptor::onRegistrationChecked,
                qontrol::UNIQUE);
        connect(keys, &IKeys::descriptorRegistered, this,
                &RegisterDescriptor::onDescriptorRegistered, qontrol::UNIQUE);
        connect(keys, &IKeys::error, this, &RegisterDescriptor::onKeysError, qontrol::UNIQUE);
    }
}

void RegisterDescriptor::view() {
    setWidth(ModalWidth::M);
    setWideBody((new qontrol::Column)->spacing(resolve(Spacing::S))->push(m_lead)->push(m_rows));
    setFooter(m_actions);
    retranslateUi();
}

void RegisterDescriptor::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void RegisterDescriptor::retranslateUi() {
    setWindowTitle(TR("musig-register-descriptor-title"));
    m_lead->setText(TR("musig-register-lead"));
    m_done_btn->setText(TR("btn-done"));
    refresh();
}

auto RegisterDescriptor::needsRegister(const QString &fingerprint) const -> bool {
    auto entry = keyEntries(m_controller->network()).value(fingerprint.toLower());
    switch (entry.route) {
    case KeyRouteKind::Usb:
    case KeyRouteKind::Qr:
    case KeyRouteKind::SdCard:
        return true;
    case KeyRouteKind::Hot:
    case KeyRouteKind::Unknown:
    default:
        return false;
    }
}

auto RegisterDescriptor::onDevice(const QString &fingerprint) const -> bool {
    return keyEntries(m_controller->network()).value(fingerprint.toLower()).on_device;
}

void RegisterDescriptor::check(const QString &fingerprint) {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }
    // A QR or SD card key is never asked: it reads Not Registered until registered here.
    auto entry = keyEntries(m_controller->network()).value(fingerprint.toLower());
    if (entry.route != KeyRouteKind::Usb) {
        m_states.insert(fingerprint, State::NotRegistered);
        return;
    }
    if (!entry.on_device) {
        m_states.insert(fingerprint, State::Disconnected);
        return;
    }
    m_states.insert(fingerprint, State::Checking);
    auto req = keys->newRequest();
    m_requests.insert(req, fingerprint);
    // The answer may come during the call.
    keys->checkDescriptorRegistered(req, m_controller->network(), fingerprint, m_descriptor);
}

void RegisterDescriptor::refresh() {
    // clear() also resets the layout spacing.
    catalog::clearRows(m_rows);
    m_rows->spacing(resolve(Spacing::XS));
    auto entries = keyEntries(m_controller->network());
    for (const auto &fingerprint : m_fingerprints) {
        auto entry = entries.value(fingerprint.toLower());
        auto state = m_states.value(fingerprint, State::NotNeeded);
        catalog::Badge *pill = nullptr;
        switch (state) {
        case State::NotNeeded:
            pill = catalog::pillRegNotNeeded();
            break;
        case State::Disconnected:
            pill = catalog::pillSignerDisconnected();
            break;
        case State::Checking:
            pill = catalog::pillRegChecking();
            break;
        case State::NotRegistered:
            pill = catalog::pillRegPending();
            break;
        case State::Registering:
            pill = catalog::pillRegRegistering();
            break;
        case State::Registered:
            pill = catalog::pillRegRegistered();
            break;
        case State::Failed:
            pill = catalog::pillRegFailed();
            break;
        }
        auto *row = new catalog::SelectRow;
        row->setIcon(catalog::routeIcon(entry.route));
        row->setTitle(entry.alias.isEmpty() ? fingerprint : entry.alias);
        row->setSub(fingerprint);
        row->setStatus(pill);
        row->setChevron(false);
        row->setProperty(K_FINGERPRINT, fingerprint);
        bool clickable =
            state == State::NotRegistered || state == State::Failed || state == State::Disconnected;
        row->setClickable(clickable);
        row->setEnabled(state != State::NotNeeded);
        if (state == State::Disconnected) {
            row->setNote(TR("musig-sign-why-disconnected"));
        }
        // A failed device says why on its own row; a click tries it again.
        if (state == State::Failed) {
            row->setSub(m_failures.value(fingerprint));
            row->setTone(catalog::SelectRow::Tone::Error);
        }
        connect(row, &catalog::SelectRow::clicked, this, &RegisterDescriptor::onRowClicked,
                qontrol::UNIQUE);
        m_rows->push(row);
        row->show();
    }
    m_actions->setReason(m_requests.isEmpty() ? QString() : TR("musig-register-reason-busy"));
}

// ===== Slots =====

void RegisterDescriptor::onRowClicked() {
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return;
    }
    // A device not connected when the window opened: checked now it may be.
    if (m_states.value(fingerprint) == State::Disconnected) {
        check(fingerprint);
        refresh();
        return;
    }
    m_failures.remove(fingerprint);
    m_states.insert(fingerprint, State::Registering);
    auto req = keys->newRequest();
    m_requests.insert(req, fingerprint);
    refresh();
    // No window: the key's device registers it, checked first. The answer may come
    // during the call.
    keys->registerDescriptor(req, m_controller->network(), fingerprint, m_descriptor);
}

void RegisterDescriptor::onDoneClicked() {
    accept();
}

void RegisterDescriptor::onRegistrationChecked(ReqId req_id, bool registered) {
    if (!m_requests.contains(req_id)) {
        return;
    }
    auto fingerprint = m_requests.take(req_id);
    m_states.insert(fingerprint, registered ? State::Registered : State::NotRegistered);
    refresh();
}

void RegisterDescriptor::onDescriptorRegistered(ReqId req_id,
                                                [[maybe_unused]] const QString &fingerprint) {
    if (!m_requests.contains(req_id)) {
        return;
    }
    m_states.insert(m_requests.take(req_id), State::Registered);
    refresh();
}

void RegisterDescriptor::onKeysError(std::optional<ReqId> req_id, const QString &message,
                                     [[maybe_unused]] plugin::SignError kind) {
    if (!req_id.has_value() || !m_requests.contains(req_id.value())) {
        return;
    }
    auto fingerprint = m_requests.take(req_id.value());
    // A device that went away while checked reads Not Connected; any other failure says
    // why on the row.
    if (m_states.value(fingerprint) == State::Checking && !onDevice(fingerprint)) {
        m_states.insert(fingerprint, State::Disconnected);
    } else {
        m_states.insert(fingerprint, State::Failed);
        m_failures.insert(fingerprint, message);
    }
    refresh();
}

} // namespace musig::modal
