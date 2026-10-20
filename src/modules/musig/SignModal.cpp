#include "SignModal.h"

#include "catalog/Button.h"
#include "catalog/ClearRows.h"
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
#include "interfaces/signer.h"
#include "modules/keys/DeviceName.h"
#include "modules/keys/KeysModule.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigModule.h"
#include "theme/Palette.h"

#include <QDialog>
#include <QEvent>
#include <QHash>
#include <QPushButton>
#include <QThread>
#include <Qontrol>
#include <algorithm>
#include <common.h>
#include <memory>
#include <utility>

namespace musig {

namespace {

const char *const K_FINGERPRINT = "fingerprint";

auto toQString(const rust::String &value) -> QString {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// The cosigner gave its part of the step shown: its nonce, else its partial signature.
auto partIn(bool partial, const MusigCosignerStatus &cosigner) -> bool {
    return partial ? cosigner.partial_sigs >= cosigner.expected
                   : cosigner.nonces >= cosigner.expected;
}

// A key's row: its route icon, its alias (its fingerprint when it has none), the
// fingerprint under it, and `pill`.
auto keyRow(const KeyEntry &entry, const QString &fingerprint, catalog::Badge *pill)
    -> catalog::SelectRow * {
    auto *row = new catalog::SelectRow;
    row->setIcon(catalog::routeIcon(entry.route));
    row->setTitle(entry.alias.isEmpty() ? fingerprint : entry.alias);
    row->setSub(fingerprint);
    row->setStatus(pill);
    row->setChevron(false);
    row->setClickable(false);
    row->setProperty(K_FINGERPRINT, fingerprint);
    return row;
}

// A row that cannot sign, greyed out, the reason on its row just left of its pill.
void greyRow(catalog::SelectRow *row, const QString &why) {
    row->setEnabled(false);
    row->setNote(why);
}

} // namespace

SignModal::SignModal(MusigAccountController *controller) : m_controller(controller) {
    // Reopened past its nonces, the session goes on with the partial signatures.
    auto round = m_controller->roundStatus().round;
    m_partial = round == MusigRound::PartialSigs || round == MusigRound::Ready;
    init();
    doConnect();
    view();
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
    m_retry_btn = catalog::btnTryAgain();
    m_retry_btn->setVisible(false);
    m_actions = new catalog::ModalActions;
    m_actions->setBack(catalog::ModalActions::Back::None);
    m_actions->addAction(m_continue_btn);
    m_actions->addAction(m_broadcast_btn);
    m_actions->addAction(m_retry_btn);
}

void SignModal::doConnect() {
    connect(m_controller, &QObject::destroyed, this, &QDialog::reject, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRegistered, this,
            &SignModal::onDevicesChanged, qontrol::UNIQUE);
    connect(Host::get()->events(), &HostEvents::instanceRemoved, this,
            &SignModal::onDevicesChanged, qontrol::UNIQUE);
    connect(m_continue_btn, &QPushButton::clicked, this, &SignModal::onContinueClicked,
            qontrol::UNIQUE);
    connect(m_broadcast_btn, &QPushButton::clicked, this, &SignModal::onBroadcastClicked,
            qontrol::UNIQUE);
    connect(m_retry_btn, &QPushButton::clicked, this, &SignModal::onRetryClicked, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::roundChanged, this, &SignModal::onRoundChanged,
            qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::transactionReady, this,
            &SignModal::onTransactionReady, qontrol::UNIQUE);
    connect(m_controller, &MusigAccountController::signerAnswered, this,
            &SignModal::onSignerAnswered, qontrol::UNIQUE);
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

void SignModal::refresh() {
    requestPlan();
    render(m_controller->roundStatus());
}

void SignModal::requestPlan() {
    m_plan_ready = false;
    if (m_plan_loading) {
        m_plan_dirty = true;
        return;
    }
    m_plan_loading = true;
    m_plan_dirty = false;
    m_plan_psbt = m_controller->psbtInFlight();
    m_plan = std::make_shared<SigningPlan>();
    auto plan = m_plan;
    rust::Vec<LiveDevice> devices;
    for (const auto &entry : Host::get()->signers()) {
        if (entry.signer != nullptr && entry.signer->state() == plugin::SignerState::Ready) {
            devices.push_back(keys::KeysModule::toLiveDevice(entry));
        }
    }
    auto *thread = QThread::create([plan, network = m_controller->network(),
                                    psbt = m_plan_psbt.toStdString(),
                                    devices = std::move(devices)]() mutable {
        *plan = ::musig_session_plan(network, psbt, std::move(devices));
    });
    connect(thread, &QThread::finished, this, &SignModal::onPlanReady, qontrol::UNIQUE);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater, qontrol::UNIQUE);
    thread->start();
}

void SignModal::render(const MusigRoundStatus &status) {
    // clear() also resets the layout spacing.
    catalog::clearRows(m_rows);
    m_rows->spacing(resolve(Spacing::XS));
    m_retry_btn->setVisible(m_controller->restartPending());
    m_retry_btn->setEnabled(!m_controller->isRestarting());
    if (!status.is_ok) {
        showFailure(Failure::Sign, toQString(status.error));
        m_continue_btn->setVisible(false);
        m_broadcast_btn->setEnabled(false);
        return;
    }
    if (!m_plan_ready) {
        m_lead->setText(TR("pill-checking"));
        m_continue_btn->setEnabled(false);
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
    if (auto *keys = Host::get()->keys(MusigModule::get()); keys != nullptr) {
        for (const auto &entry : keys->keys(m_controller->network())) {
            entries.insert(entry.fingerprint.toLower(), entry);
        }
    }
    if (m_plan->is_ok) {
        for (const auto &candidate : m_plan->candidates) {
            auto fingerprint = toQString(candidate.fingerprint).toLower();
            auto entry = entries.value(fingerprint);
            bool cachedRoundOne = !entry.can_sign && candidate.can_sign &&
                                  candidate.route == KeyRouteKind::Usb;
            entry.can_musig = entry.can_musig || cachedRoundOne;
            entry.can_sign = candidate.can_sign;
            if (candidate.route != KeyRouteKind::Unknown) {
                entry.route = candidate.route;
            }
            entry.on_device = entry.on_device && entry.route == KeyRouteKind::Usb && entry.can_sign;
            entries.insert(fingerprint, entry);
        }
    }
    bool finalized = !m_controller->finalizedTx().isEmpty();
    // The keys asked to sign now, each on its device (or in its window).
    auto busy = m_controller->signingKeys();
    bool hotSigning = false;
    bool deviceSigning = false;
    // The spend path's signers, and whether each gave its part of the step shown.
    QList<std::pair<QString, bool>> signers;
    if (single) {
        for (const auto &fingerprint : m_controller->spendSigners()) {
            signers.append({fingerprint, finalized || m_signed.contains(fingerprint.toLower())});
        }
    } else {
        for (const auto &cosigner : status.cosigners) {
            signers.append(
                {toQString(cosigner.fingerprint), finalized || partIn(partial, cosigner)});
        }
    }
    QList<catalog::SelectRow *> ready;
    QList<catalog::SelectRow *> unusable;
    int left = 0;
    for (const auto &[fingerprint, in] : signers) {
        auto entry = entries.value(fingerprint.toLower());
        if (!in) {
            ++left;
        }
        bool signing = !in && busy.contains(fingerprint.toLower());
        if (signing && entry.route == KeyRouteKind::Hot) {
            hotSigning = true;
        } else if (signing) {
            deviceSigning = true;
        }
        // A signer still to sign that cannot: no connected device or route, or a MuSig2
        // round on a device that does not sign MuSig2. Greyed, its reason on its row.
        QString why;
        catalog::Badge *pill = nullptr;
        if (!in && !entry.can_sign && entry.route == KeyRouteKind::Usb) {
            why = TR("musig-sign-why-disconnected");
            pill = catalog::pillSignerDisconnected();
        } else if (!in && !entry.can_sign) {
            why = TR("musig-sign-why-no-route");
            pill = catalog::pillSignerNoRoute();
        } else if (!in && !single && !entry.can_musig) {
            why = TR("musig-sign-why-unsupported");
            pill = catalog::pillSignerUnsupported();
        }
        bool usable = why.isEmpty();
        if (usable) {
            if (signing) {
                pill = entry.route == KeyRouteKind::Hot ? catalog::pillMusigSigning()
                                                        : catalog::pillMusigOnDevice();
            } else if (!in) {
                pill = catalog::pillMusigNeeded();
            } else if (partial) {
                pill = catalog::pillSignerSigned();
            } else {
                pill = catalog::pillMusigNonceAdded();
            }
        }
        auto *row = keyRow(entry, fingerprint, pill);
        // Each key answers one request at a time; the others stay clickable meanwhile.
        row->setClickable(!in && usable && !signing && m_plan->is_ok &&
                          !m_controller->restartPending());
        if (!usable) {
            greyRow(row, why);
        }
        connect(row, &catalog::SelectRow::clicked, this, &SignModal::onCosignerClicked,
                qontrol::UNIQUE);
        (usable ? ready : unusable).append(row);
    }
    // The wallet's other keys: listed, greyed out, as this path does not use them.
    QList<catalog::SelectRow *> others;
    for (const auto &fingerprint : m_controller->cosignerFingerprints()) {
        bool inPath = std::ranges::any_of(signers, [&fingerprint](const auto &signer) {
            return signer.first.compare(fingerprint, Qt::CaseInsensitive) == 0;
        });
        if (inPath) {
            continue;
        }
        auto *row = keyRow(entries.value(fingerprint.toLower()), fingerprint,
                           catalog::pillSignerNotInPath());
        greyRow(row, TR("musig-sign-why-not-in-path"));
        others.append(row);
    }
    // Connected devices whose key is not one of this wallet's: listed last, greyed.
    QList<catalog::SelectRow *> strangers;
    auto wallet = m_controller->cosignerFingerprints();
    for (const auto &device : Host::get()->signers()) {
        if (device.signer == nullptr) {
            continue;
        }
        auto fingerprint = device.signer->fingerprint();
        bool known = std::ranges::any_of(wallet, [&fingerprint](const QString &fp) {
            return fp.compare(fingerprint, Qt::CaseInsensitive) == 0;
        });
        if (known || fingerprint.isEmpty()) {
            continue;
        }
        auto entry = entries.value(fingerprint.toLower());
        entry.route = KeyRouteKind::Usb;
        if (entry.alias.isEmpty()) {
            entry.alias = keys::deviceName(*device.signer);
        }
        auto *row = keyRow(entry, fingerprint, catalog::pillSignerNotInWallet());
        greyRow(row, TR("musig-sign-why-not-in-wallet"));
        strangers.append(row);
    }
    for (const auto &group : {ready, unusable, others, strangers}) {
        for (auto *row : group) {
            m_rows->push(row);
            row->show();
        }
    }

    QString reason;
    if (deviceSigning) {
        reason = TR("musig-sign-reason-on-device");
    } else if (hotSigning) {
        reason = TR("musig-sign-reason-signing");
    } else if (left == 1) {
        reason = TR("musig-sign-reason-one");
    } else if (left > 1) {
        reason = TR("musig-sign-reason").arg(left);
    }
    m_actions->setReason(reason);
    bool idle = busy.isEmpty() && m_plan->is_ok && !m_controller->restartPending();
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
    auto *keys = Host::get()->keys(MusigModule::get());
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

void SignModal::onPlanReady() {
    m_plan_loading = false;
    if (m_plan_dirty || m_plan_psbt != m_controller->psbtInFlight()) {
        refresh();
        return;
    }
    m_plan_ready = true;
    if (!m_plan->is_ok) {
        showFailure(Failure::Sign, toQString(m_plan->error));
    }
    render(m_controller->roundStatus());
}

void SignModal::onDevicesChanged() {
    refresh();
}

void SignModal::onRetryClicked() {
    m_notice->setVisible(false);
    m_controller->restartRounds();
    refresh();
}

void SignModal::onCosignerClicked() {
    if (!m_plan_ready || !m_plan->is_ok || m_plan_psbt != m_controller->psbtInFlight() ||
        m_controller->restartPending()) {
        return;
    }
    auto fingerprint = sender()->property(K_FINGERPRINT).toString();
    bool usable = false;
    for (const auto &candidate : m_plan->candidates) {
        if (toQString(candidate.fingerprint).compare(fingerprint, Qt::CaseInsensitive) == 0) {
            usable = candidate.can_sign && !candidate.musig_done;
            break;
        }
    }
    if (!usable) {
        return;
    }
    m_notice->setVisible(false);
    // A key on its connected USB device signs in place, its row On Device while every
    // other key can be asked too; another route runs in its own window, over this one.
    if (onDevice(fingerprint)) {
        m_controller->signRoundOnDevice(fingerprint);
        refresh();
        return;
    }
    // Returns once the flow's window has closed, its answer delivered.
    m_controller->signRound(fingerprint);
    refresh();
}

void SignModal::onContinueClicked() {
    m_partial = true;
    refresh();
}

void SignModal::onBroadcastClicked() {
    m_broadcast_btn->setEnabled(false);
    m_controller->broadcast();
}

void SignModal::onSignerAnswered(const QString &fingerprint) {
    // A single-key path has no rounds to say who signed: the key that answered did.
    if (m_controller->roundStatus().round == MusigRound::NotMusig) {
        m_signed.insert(fingerprint.toLower());
    }
}

void SignModal::onRoundChanged(const MusigRoundStatus &status) {
    // A single-key path has no rounds to say who signed: the key asked did.
    requestPlan();
    render(status);
}

// Finalized once every partial signature is in: Broadcast opens.
void SignModal::onTransactionReady([[maybe_unused]] const QString &tx_hex) {
    refresh();
}

void SignModal::onSigningFailed(const QString &message, plugin::SignError kind) {
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
