#include "SignModal.h"

#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/Variants.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Badge.h"
#include "catalog/display/Label.h"
#include "catalog/display/RouteIcons.h"
#include "catalog/display/WizardSteps.h"
#include "catalog/feedback/Notice.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "interfaces/keys.h"
#include "modules/musig/MusigAccountController.h"
#include "theme/Palette.h"

#include <QDialog>
#include <QEvent>
#include <QHash>
#include <QPushButton>
#include <Qontrol>
#include <algorithm>
#include <common.h>

namespace musig {

namespace {

// exec() result of the modal closed to run a cosigner's signing flow in its place.
constexpr int FLOW_RESULT = QDialog::Accepted + 1;

const char *const K_FINGERPRINT = "fingerprint";

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// The cosigner gave its part of the step shown: its nonce, else its partial signature.
auto partIn(bool partial, const MusigCosignerStatus &cosigner) -> bool {
    return partial ? cosigner.partial_sigs >= cosigner.expected
                   : cosigner.nonces >= cosigner.expected;
}

} // namespace

SignModal::SignModal(MusigAccountController *controller) : m_controller(controller) {
    init();
    doConnect();
    view();
    // Reopened past its nonces, the session goes on with the partial signatures.
    auto round = m_controller->roundStatus().round;
    m_partial = round == MusigRound::PartialSigs || round == MusigRound::Ready;
    refresh();
}

auto SignModal::exec() -> int {
    int result = catalog::Modal::exec();
    while (result == FLOW_RESULT) {
        runFlow();
        result = catalog::Modal::exec();
    }
    return result;
}

void SignModal::init() {
    m_steps =
        new catalog::WizardSteps({TR("musig-sign-step-nonces"), TR("musig-sign-step-partial")});
    m_notice = new catalog::Notice;
    m_notice->setVisible(false);
    m_lead = new catalog::Label(catalog::LabelRole::Body);
    m_lead->setWordWrap(true);
    m_rows = new qontrol::Column;
    m_rows->spacing(resolve(Spacing::XS));
    m_continue_btn = catalog::btnContinue();
    m_broadcast_btn = catalog::btnBroadcast();
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::None);
    m_actions->addAction(m_continue_btn);
    m_actions->addAction(m_broadcast_btn);
}

void SignModal::doConnect() {
    connect(m_continue_btn, &QPushButton::clicked, this, &SignModal::onContinueClicked,
            qontrol::UNIQUE);
    connect(m_broadcast_btn, &QPushButton::clicked, this, &SignModal::onBroadcastClicked,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::roundChanged, this, &SignModal::onRoundChanged,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::transactionReady, this,
            &SignModal::onTransactionReady, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::signingFailed, this, &SignModal::onSigningFailed,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::finalizeFailed, this,
            &SignModal::onFinalizeFailed, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::broadcastDone, this, &SignModal::onBroadcastDone,
            qontrol::UNIQUE);
}

void SignModal::view() {
    setWidth(ModalWidth::Wizard);
    setWizardHeight(true);
    setHeader(m_steps);
    setWideBody((new qontrol::Column)
                    ->spacing(resolve(Spacing::S))
                    ->push(m_notice)
                    ->push(m_lead)
                    ->push(m_rows)
                    ->pushSpacer());
    setFooter(m_actions);
    retranslateUi();
}

void SignModal::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    catalog::Modal::changeEvent(event);
}

void SignModal::retranslateUi() {
    setWindowTitle(TR("musig-sign-title"));
    m_continue_btn->setText(TR("btn-continue"));
    m_broadcast_btn->setText(TR("common-broadcast"));
    m_notice->setTitle(failureTitle());
    refresh();
}

void SignModal::runFlow() {
    // Returns once the flow's window has closed, its answer delivered.
    m_controller->signRound(m_flow_fingerprint);
    refresh();
}

void SignModal::refresh() {
    render(m_controller->roundStatus());
}

void SignModal::render(const MusigRoundStatus &status) {
    // clear() also resets the layout spacing.
    m_rows->clear();
    m_rows->spacing(resolve(Spacing::XS));
    if (!status.is_ok) {
        showFailure(Failure::Sign, toQString(status.error));
        m_continue_btn->setVisible(false);
        m_broadcast_btn->setEnabled(false);
        return;
    }
    // A single-key spend path signs once, with no MuSig2 rounds.
    bool single = status.round == MusigRound::NotMusig;
    bool partial = single || m_partial;
    m_steps->setVisible(!single);
    m_steps->setCurrent(partial ? 1 : 0);
    if (single) {
        m_lead->setText(TR("musig-sign-lead-single"));
    } else {
        m_lead->setText(partial ? TR("musig-sign-lead-partial") : TR("musig-sign-lead-nonce"));
    }

    QHash<QString, KeyEntry> entries;
    if (auto *keys = Host::get()->keys(); keys != nullptr) {
        for (const auto &entry : keys->keys(m_controller->network())) {
            entries.insert(entry.fingerprint.toLower(), entry);
        }
    }
    bool finalized = !m_controller->finalizedTx().isEmpty();
    int left = 0;
    for (const auto &cosigner : status.cosigners) {
        auto fingerprint = toQString(cosigner.fingerprint);
        auto entry = entries.value(fingerprint.toLower());
        bool in = finalized || partIn(partial, cosigner);
        if (!in) {
            ++left;
        }
        bool onDevice = !in && fingerprint.compare(m_on_device, Qt::CaseInsensitive) == 0;
        catalog::Badge *pill = nullptr;
        if (onDevice) {
            pill = catalog::pillMusigOnDevice();
        } else if (!in) {
            pill = catalog::pillMusigNeeded();
        } else if (partial) {
            pill = catalog::pillSignerSigned();
        } else {
            pill = catalog::pillMusigNonceAdded();
        }
        auto *row = new catalog::SelectRow;
        row->setIcon(catalog::routeIcon(entry.route));
        row->setTitle(entry.alias.isEmpty() ? fingerprint : entry.alias);
        row->setSub(fingerprint);
        row->setStatus(pill);
        row->setChevron(false);
        // One cosigner at a time: none is clicked while a device signs.
        row->setClickable(!in && m_on_device.isEmpty());
        row->setProperty(K_FINGERPRINT, fingerprint);
        connect(row, &catalog::SelectRow::clicked, this, &SignModal::onCosignerClicked,
                qontrol::UNIQUE);
        m_rows->push(row);
        row->show();
    }

    QString reason;
    if (!m_on_device.isEmpty()) {
        reason = TR("musig-sign-reason-on-device");
    } else if (left == 1) {
        reason = TR("musig-sign-reason-one");
    } else if (left > 1) {
        reason = TR("musig-sign-reason").arg(left);
    }
    m_actions->setReason(reason);
    bool idle = m_on_device.isEmpty();
    m_continue_btn->setVisible(!partial);
    m_continue_btn->setEnabled(idle && left == 0);
    m_broadcast_btn->setVisible(partial);
    m_broadcast_btn->setEnabled(idle && left == 0 && finalized);
}

void SignModal::showFailure(Failure failure, const QString &text) {
    m_failure = failure;
    m_notice->setTone(failure == Failure::SessionLost ? catalog::Notice::Tone::Warning
                                                      : catalog::Notice::Tone::Error);
    m_notice->setTitle(failureTitle());
    m_notice->setText(text);
    m_notice->setVisible(true);
}

auto SignModal::onDevice(const QString &fingerprint) const -> bool {
    auto *keys = Host::get()->keys();
    if (keys == nullptr) {
        return false;
    }
    auto entries = keys->keys(m_controller->network());
    return std::ranges::any_of(entries, [&fingerprint](const KeyEntry &entry) {
        return entry.on_device && entry.fingerprint.compare(fingerprint, Qt::CaseInsensitive) == 0;
    });
}

auto SignModal::failureTitle() const -> QString {
    switch (m_failure) {
    case Failure::SessionLost:
        return TR("musig-sign-session-lost-title");
    case Failure::Finalize:
        return TR("musig-send-finalize-failed");
    case Failure::None:
    case Failure::Sign:
        return {};
    }
    return {};
}

// ===== Slots =====

void SignModal::onCosignerClicked() {
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    m_notice->setVisible(false);
    if (onDevice(fingerprint)) {
        m_on_device = fingerprint;
        refresh();
        m_controller->signRoundOnDevice(fingerprint);
        return;
    }
    m_flow_fingerprint = fingerprint;
    done(FLOW_RESULT);
}

void SignModal::onContinueClicked() {
    m_partial = true;
    refresh();
}

void SignModal::onBroadcastClicked() {
    m_broadcast_btn->setEnabled(false);
    m_controller->broadcast();
}

void SignModal::onRoundChanged(const MusigRoundStatus &status) {
    m_on_device.clear();
    render(status);
}

// Finalized once every partial signature is in: Broadcast opens.
void SignModal::onTransactionReady([[maybe_unused]] const QString &tx_hex) {
    refresh();
}

void SignModal::onSigningFailed(const QString &message, plugin::SignError kind) {
    m_on_device.clear();
    if (kind == plugin::SignError::MusigSession) {
        // The controller dropped every nonce: the session starts over at round 1.
        m_partial = false;
        showFailure(Failure::SessionLost, TR("musig-send-session-lost"));
    } else {
        showFailure(Failure::Sign, message);
    }
    refresh();
}

// Finalizing is pure computation over the PSBT: retrying gives the same error.
void SignModal::onFinalizeFailed(const QString &message) {
    showFailure(Failure::Finalize, message);
    refresh();
}

void SignModal::onBroadcastDone(const TxResult &result) {
    if (result.is_ok) {
        accept();
        return;
    }
    showFailure(Failure::Sign, TR("confirm-send-failed").arg(toQString(result.error)));
    refresh();
}

} // namespace musig
