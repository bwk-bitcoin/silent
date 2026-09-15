#include "SelectSigner.h"

#include "../utils.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/display/Label.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/signer.h"
#include "theme/Icon.h"

#include <QVariant>
#include <common.h>
#include <cstdint>

namespace modal {

using catalog::Button;
using catalog::ButtonRole;
using catalog::Label;
using catalog::LabelRole;

namespace {

struct SignerOption {
    QString name;
    QString detail;
    QString fingerprint;
    QString kind;
};

#ifdef SILENT_MOCK_UI
const QList<SignerOption> C_SIGNERS = {
    {"Ledger Nano S Plus", "USB signer", "#a4f9c821", "usb"},
    {"Coldcard Mk4", "USB signer", "#7e02da41", "usb"},
    {"BitBox02", "USB signer", "#b1740c3e", "usb"},
    {"SD Card Airgap Device", "Airgap signer", "", "sd"},
};
#endif

} // namespace

SelectSigner::SelectSigner(const QString &title, bool include_sd, QWidget *parent)
    : m_title(title)
    , m_include_sd(include_sd) {
    Q_UNUSED(parent);
    setWindowTitle(title);
    resize(440, 320);
    init();
    doConnect();
    view();
}

auto SelectSigner::selectedName() const -> QString {
    return m_selected_name;
}

auto SelectSigner::selectedKind() const -> QString {
    return m_selected_kind;
}

auto SelectSigner::selectedSigner() const -> ISigner * {
    return m_selected_signer;
}

void SelectSigner::init() {
    m_cancel_btn = new Button(TR("common-cancel"));
}

void SelectSigner::doConnect() {
    connect(m_cancel_btn, &QPushButton::clicked, this, &QDialog::reject, qontrol::UNIQUE);
}

void SelectSigner::onSignerClicked() {
    auto *row = qobject_cast<QObject *>(sender());
    if (row == nullptr) {
        return;
    }
    m_selected_name = row->property("signerName").toString();
    m_selected_kind = row->property("signerKind").toString();
    m_selected_signer = reinterpret_cast<ISigner *>(row->property("signerPtr").value<quintptr>());
    accept();
}

void SelectSigner::view() {
    auto *col = (new qontrol::Column)->push(new Label(m_title, LabelRole::Heading));
    col->pushSpacer(resolve(Spacing::M));
    bool hasSigner = false;
#ifdef SILENT_MOCK_UI
    for (const auto &signer : C_SIGNERS) {
        if (!m_include_sd && signer.kind == "sd") {
            continue;
        }
        hasSigner = true;
        auto *row = new catalog::SelectRow;
        // sd_card glyph is not vendored; folder is the closest storage stand-in.
        row->setIcon(signer.kind == "sd" ? icon::folder(IconColor::Accent)
                                         : icon::usb(IconColor::Accent));
        row->setTitle(signer.name);
        row->setMetadata(signer.fingerprint.isEmpty() ? signer.detail : signer.fingerprint + "  " + signer.detail);
        row->setProperty("signerName", signer.name);
        row->setProperty("signerKind", signer.kind);
        row->setProperty("signerPtr", QVariant::fromValue<quintptr>(0));
        connect(row, &catalog::SelectRow::clicked, this, &SelectSigner::onSignerClicked, qontrol::UNIQUE);
        col->push(row)->pushSpacer(resolve(Spacing::S));
    }
#else
    for (const auto &entry : Host::get()->signers()) {
        auto *signer = entry.signer;
        if (signer == nullptr) {
            continue;
        }
        hasSigner = true;
        auto *row = new catalog::SelectRow;
        row->setIcon(icon::usb(IconColor::Accent));
        row->setTitle(signer->id());
        auto detail = signer->fingerprint();
        if (!signer->stateDetail().isEmpty()) {
            detail += detail.isEmpty() ? signer->stateDetail() : "  " + signer->stateDetail();
        }
        row->setMetadata(detail);
        row->setEnabled(signer->state() == plugin::SignerState::Ready);
        row->setProperty("signerName", signer->id());
        row->setProperty("signerKind", QStringLiteral("usb"));
        row->setProperty("signerPtr", QVariant::fromValue(reinterpret_cast<quintptr>(signer)));
        connect(row, &catalog::SelectRow::clicked, this, &SelectSigner::onSignerClicked,
                qontrol::UNIQUE);
        col->push(row)->pushSpacer(resolve(Spacing::S));
    }
#endif
    if (!hasSigner) {
        col->push(new Label(TR("send-no-signers"), LabelRole::Body))->pushSpacer(resolve(Spacing::S));
    }
    auto *cancelRow = (new qontrol::Row)->pushSpacer()->push(m_cancel_btn)->pushSpacer();
    col->push(cancelRow)->pushSpacer();
    setMainWidget(margin(col));
}

} // namespace modal
