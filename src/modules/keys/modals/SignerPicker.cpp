#include "SignerPicker.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/Separator.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "modules/keys/KeysModule.h"
#include "theme/Icon.h"
#include "views/utils.h"

#include <algorithm>

#include <common.h>

namespace keys::modal {

using catalog::Button;
using catalog::Label;
using catalog::LabelRole;

SignerPicker::SignerPicker(const QString &title, const SigningPlan &plan, QWidget *parent)
    : m_title(title), m_sp_outputs_pending(plan.sp_outputs_pending) {
    Q_UNUSED(parent);
    for (const auto &candidate : plan.candidates) {
        m_candidates.append(candidate);
    }
    setWindowTitle(title);
    resize(440, 320);
    init();
    doConnect();
    view();
}

auto SignerPicker::selectedFingerprint() const -> QString {
    return m_selected_fingerprint;
}

void SignerPicker::init() {
    m_cancel_btn = new Button(TR("common-cancel"));
}

void SignerPicker::doConnect() {
    connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject, qontrol::UNIQUE);
}

void SignerPicker::onRowClicked() {
    auto *row = qobject_cast<QObject *>(sender());
    if (row == nullptr) {
        return;
    }
    m_selected_fingerprint = row->property("fingerprint").toString();
    emit picked(m_selected_fingerprint);
    accept();
}

auto SignerPicker::routeIcon(KeyRouteKind route) -> QIcon {
    switch (route) {
    case KeyRouteKind::Usb:
        return icon::usb(IconColor::Accent);
    case KeyRouteKind::Qr:
        return icon::qr(IconColor::Accent);
    case KeyRouteKind::SdCard:
        return icon::memoryStick(IconColor::Accent);
    case KeyRouteKind::Hot:
        return icon::key(IconColor::Accent);
    case KeyRouteKind::Unknown:
    default:
        return icon::key(IconColor::Accent);
    }
}

auto SignerPicker::reasonText(KeyUnusableReason reason) -> QString {
    switch (reason) {
    case KeyUnusableReason::NotRegistered:
        return TR("keys-picker-not-registered");
    case KeyUnusableReason::NoRoute:
        return TR("keys-picker-no-route");
    case KeyUnusableReason::DeviceNotConnected:
        return TR("keys-picker-device-offline");
    case KeyUnusableReason::SpOutputsPending:
        return TR("keys-picker-waiting-for-sp");
    case KeyUnusableReason::None:
    case KeyUnusableReason::NoHotSecret:
    default:
        return TR("keys-picker-unknown-reason");
    }
}

auto SignerPicker::buildCandidateRow(const SigningCandidate &candidate) const
    -> catalog::SelectRow * {
    QString fingerprint = KeysModule::toQt(candidate.fingerprint);
    QString alias = KeysModule::toQt(candidate.alias);

    auto *row = new catalog::SelectRow;
    row->setIcon(routeIcon(candidate.route));
    row->setTitle(alias.isEmpty() ? fingerprint : alias);

    bool unusable = !candidate.can_sign || candidate.unusable != KeyUnusableReason::None;

    QString metadata = fingerprint;
    if (candidate.must_sign_first) {
        metadata += "  " + TR("keys-picker-must-sign-first");
    }
    if (unusable) {
        metadata += "  " + reasonText(candidate.unusable);
    }

    bool enabled = true;
    if (unusable) {
        enabled = false;
        metadata += "  " + TR("keys-picker-required");
    } else if (m_sp_outputs_pending && !candidate.must_sign_first) {
        enabled = false;
        metadata += "  " + TR("keys-picker-waiting-for-sp");
    }

    row->setMetadata(metadata);
    row->setEnabled(enabled);
    row->setProperty("fingerprint", fingerprint);
    if (enabled) {
        connect(row, &catalog::SelectRow::clicked, this, &SignerPicker::onRowClicked,
                qontrol::UNIQUE);
    }
    return row;
}

auto SignerPicker::buildHintRow(ISigner *signer) -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setTitle(signer->id());
    row->setMetadata(signer->fingerprint() + "  " + TR("keys-picker-unrelated"));
    row->setEnabled(false);
    return row;
}

void SignerPicker::view() {
    auto *col = (new qontrol::Column)->push(new Label(m_title, LabelRole::Heading));
    col->pushSpacer(resolve(Spacing::M));

    if (m_candidates.isEmpty()) {
        col->push(new Label(TR("send-no-signers"), LabelRole::Body))
            ->pushSpacer(resolve(Spacing::S));
    }
    for (const auto &candidate : m_candidates) {
        col->push(buildCandidateRow(candidate))->pushSpacer(resolve(Spacing::S));
    }

    QList<ISigner *> hints;
    for (const auto &entry : Host::get()->signers()) {
        auto *signer = entry.signer;
        if (signer == nullptr) {
            continue;
        }
        QString fingerprint = signer->fingerprint();
        bool matchesCandidate =
            std::ranges::any_of(m_candidates, [&fingerprint](const SigningCandidate &candidate) {
                return KeysModule::toQt(candidate.fingerprint) == fingerprint;
            });
        if (!matchesCandidate) {
            hints.append(signer);
        }
    }
    if (!hints.isEmpty()) {
        col->push(new catalog::Separator)->pushSpacer(resolve(Spacing::S));
        col->push(new Label(TR("keys-picker-other-devices"), LabelRole::Section))
            ->pushSpacer(resolve(Spacing::S));
        for (auto *signer : hints) {
            col->push(buildHintRow(signer))->pushSpacer(resolve(Spacing::S));
        }
    }

    auto *cancelRow = (new qontrol::Row)->pushSpacer()->push(m_cancel_btn)->pushSpacer();
    col->push(cancelRow)->pushSpacer();
    setMainWidget(margin(col));
}

} // namespace keys::modal
