#include "DeviceModal.h"

#include "catalog/Button.h"
#include "catalog/display/Label.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/signer.h"
#include "views/utils.h"

#include <common.h>

namespace keys::modal {

using catalog::Button;
using catalog::Input;
using catalog::Label;
using catalog::LabelRole;

namespace {

// A device only exports an xpub without asking the user to approve it when the path is a
// standard one for the network it runs on, so the coin type has to follow the network.
auto defaultXpubPath(Network network) -> QString {
    int coinType = network == Network::Bitcoin ? 0 : 1;
    return QStringLiteral("m/84'/%1'/0'").arg(coinType);
}

auto stateText(plugin::SignerState state) -> QString {
    switch (state) {
    case plugin::SignerState::Ready:
        return TR("hwi-state-ready");
    case plugin::SignerState::Locked:
        return TR("hwi-state-locked");
    default:
        return TR("hwi-state-unsupported");
    }
}

} // namespace

DeviceModal::DeviceModal(ISigner *signer, Network network, QWidget *parent)
    : m_signer(signer)
    , m_network(network) {
    Q_UNUSED(parent);
    setWindowTitle(TR("keys-device-title"));
    init();
    doConnect();
    view();
}

void DeviceModal::init() {
    m_path_input = new Input(defaultXpubPath(m_network));
    m_path_input->setWidth(Size::M);
    m_result_label = new Label(QString(), LabelRole::Mono);
    m_result_label->setWordWrap(true);
    m_info_btn = new Button(TR("hwi-info"));
    m_xpub_btn = new Button(TR("hwi-get-xpub"));
    m_close_btn = new Button(TR("common-close"));

    bool ready = m_signer != nullptr && m_signer->state() == plugin::SignerState::Ready;
    m_info_btn->setEnabled(ready);
    m_xpub_btn->setEnabled(ready);
    m_path_input->setEnabled(ready);
}

void DeviceModal::doConnect() {
    connect(m_info_btn, &QPushButton::clicked, this, &DeviceModal::onInfoClicked, qontrol::UNIQUE);
    connect(m_xpub_btn, &QPushButton::clicked, this, &DeviceModal::onXpubClicked, qontrol::UNIQUE);
    connect(m_close_btn, &QPushButton::clicked, this, &QDialog::accept, qontrol::UNIQUE);

    if (m_signer != nullptr) {
        connect(m_signer, qOverload<ReqId, QMap<QString, QString>>(&ISigner::info), this,
                &DeviceModal::onSignerInfo, qontrol::UNIQUE);
        connect(m_signer, &ISigner::xpub, this, &DeviceModal::onSignerXpub, qontrol::UNIQUE);
        connect(m_signer, &ISigner::error, this, &DeviceModal::onSignerError, qontrol::UNIQUE);
    }
}

void DeviceModal::view() {
    QString summary;
    QString stateLine;
    if (m_signer != nullptr) {
        if (m_signer->brand().isEmpty() && m_signer->model().isEmpty() &&
            m_signer->serial().isEmpty()) {
            summary = m_signer->id();
        } else {
            summary = QStringLiteral("%1  %2  %3  %4")
                          .arg(m_signer->brand(), m_signer->model(), m_signer->serial(),
                               m_signer->fingerprint())
                          .simplified();
        }

        QString detail = m_signer->stateDetail();
        stateLine = detail.isEmpty() ? stateText(m_signer->state())
                                     : stateText(m_signer->state()) + " - " + detail;
    }

    auto *actions = (new qontrol::Row)
                        ->push(m_info_btn)
                        ->pushSpacer(resolve(Spacing::XS))
                        ->push(m_path_input)
                        ->pushSpacer(resolve(Spacing::XS))
                        ->push(m_xpub_btn)
                        ->pushSpacer();

    auto *col = (new qontrol::Column)
                    ->push(new Label(summary, LabelRole::Heading))
                    ->push(new Label(stateLine, LabelRole::Caption))
                    ->pushSpacer(resolve(Spacing::S))
                    ->push(actions)
                    ->push(m_result_label)
                    ->pushSpacer(resolve(Spacing::M))
                    ->push((new qontrol::Row)->pushSpacer()->push(m_close_btn));

    setMainWidget(margin(col));
}

void DeviceModal::onInfoClicked() {
    if (m_signer == nullptr) {
        return;
    }
    m_pending_info = m_signer->info();
}

void DeviceModal::onXpubClicked() {
    if (m_signer == nullptr) {
        return;
    }
    m_pending_xpub = m_signer->getXpub(m_path_input->text().trimmed());
    m_result_label->setText(TR("hwi-waiting"));
}

void DeviceModal::onSignerInfo(ReqId req_id, const QMap<QString, QString> &info) {
    if (req_id != m_pending_info) {
        return;
    }
    m_pending_info = 0;
    m_result_label->setText(TR("hwi-info-template")
                                .arg(info.value(QStringLiteral("kind")),
                                     info.value(QStringLiteral("fingerprint")),
                                     info.value(QStringLiteral("version"))));
}

void DeviceModal::onSignerXpub(ReqId req_id, const QString &xpub) {
    if (req_id != m_pending_xpub) {
        return;
    }
    m_pending_xpub = 0;
    m_result_label->setText(xpub);
}

void DeviceModal::onSignerError(std::optional<ReqId> req_id, const QString &message) {
    bool wasXpub = req_id.has_value() && req_id.value() == m_pending_xpub;
    if (req_id.has_value() && req_id.value() != m_pending_info && !wasXpub) {
        return;
    }
    m_pending_info = 0;
    m_pending_xpub = 0;

    // A device reports a raw status word for a refused xpub, which says nothing about
    // what was asked, so pair it with the path through hwi-xpub-error.
    m_result_label->setText(
        wasXpub ? TR("hwi-xpub-error").arg(m_path_input->text().trimmed(), message) : message);
}

} // namespace keys::modal
