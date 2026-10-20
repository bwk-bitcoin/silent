#include "FetchNonces.h"

#include "catalog/Button.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/feedback/ModalStatus.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/Field.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/form/ValidationMark.h"
#include "catalog/inputs/Input.h"
#include "i18n/Tr.h"
#include "interfaces/signer.h"
#include "modules/keys/flow/signingflow/LedgerNonces.h"
#include "modules/keys/flow/signingflow/LiveDevice.h"

#include <QIntValidator>
#include <QLocale>
#include <Qontrol>

#include <utility>

#include <common.h>
#include <silent.h>

namespace keys::modal {

FetchNonces::FetchNonces(KeysModule *module, QString fingerprint, QString descriptor)
    : m_module(module), m_fingerprint(std::move(fingerprint)), m_descriptor(std::move(descriptor)) {
    init();
    doConnect();
    view();
    onQuantityChanged();
}

void FetchNonces::init() {
    m_quantity = new catalog::ValidatedInput;
    m_quantity->setWidth(Size::S);
    auto limit = static_cast<int>(::ledger_nonce_limit());
    m_quantity->input()->setValidator(new QIntValidator(1, limit, m_quantity));
    m_quantity->input()->setText(QStringLiteral("1"));
    m_fetch = new catalog::Button(TR("keys-fetch-nonces"), catalog::ButtonRole::Primary);
    m_actions = new catalog::ModalActions;
    m_actions->addAction(m_fetch);
    m_status = new catalog::ModalStatus;
    m_status->setSpinner();
    m_status->setTitle(TR("keys-nonces-fetching"));
    m_status->setSubtitle(TR("keys-nonces-wait"));
    m_status->setVisible(false);
    m_error = new catalog::Notice(catalog::Notice::Tone::Error);
    m_error->setVisible(false);
}

void FetchNonces::doConnect() {
    connect(m_fetch, &QPushButton::clicked, this, &FetchNonces::onNextClicked, qontrol::UNIQUE);
    connect(m_quantity, &catalog::ValidatedInput::textChanged, this,
            &FetchNonces::onQuantityChanged, qontrol::UNIQUE);
    connect(m_quantity->input(), &QLineEdit::returnPressed, this,
            &FetchNonces::onNextClicked, qontrol::UNIQUE);
}

void FetchNonces::view() {
    setWindowTitle(TR("keys-fetch-nonces"));
    setHeader(nullptr);
    setBody((new qontrol::Column)->spacing(resolve(Spacing::M))
                ->push(new catalog::Field(TR("keys-nonces-quantity"), m_quantity))
                ->push(m_status)->push(m_error));
    setFooter(m_actions);
    setWidth(ModalWidth::M);
}

void FetchNonces::onNextClicked() {
    if (m_fetching || !m_quantity->input()->hasAcceptableInput()) {
        return;
    }
    auto device = flow::liveDevice(m_fingerprint);
    if (!device.has_value() ||
        !device->signer->brand().startsWith(QStringLiteral("ledger"), Qt::CaseInsensitive)) {
        onFetched(TR("keys-nonces-connect"));
        return;
    }
    m_quantity->setEnabled(false);
    m_fetching = true;
    m_fetch->setEnabled(false);
    m_error->setVisible(false);
    m_status->setVisible(true);
    auto quantity = m_quantity->input()->validator()->locale().toUInt(m_quantity->input()->text());
    auto *fetch =
        new flow::LedgerNonces(m_module, device->signer, device->instanceId, m_fingerprint,
                               m_descriptor, quantity, this);
    connect(fetch, &flow::LedgerNonces::finished, this, &FetchNonces::onFetched, qontrol::UNIQUE);
    fetch->start();
}

void FetchNonces::onFetched(const QString &error) {
    m_fetching = false;
    if (error.isEmpty()) {
        accept();
        return;
    }
    m_quantity->setEnabled(true);
    onQuantityChanged();
    m_status->setVisible(false);
    m_error->setText(error);
    m_error->setVisible(true);
}

void FetchNonces::onQuantityChanged() {
    bool valid = m_quantity->input()->hasAcceptableInput();
    m_quantity->setState(m_quantity->input()->text().isEmpty()
                            ? catalog::ValidationMark::State::None
                            : valid ? catalog::ValidationMark::State::Valid
                                    : catalog::ValidationMark::State::Invalid);
    m_fetch->setEnabled(!m_fetching && valid);
}

} // namespace keys::modal
