#include "MockPlugin.h"
#include "SpSigningProbe.h"
#include "ViewServices.h"

#include "AppController.h"
#include "catalog/Button.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/panels/home/RowClickTable.h"
#include "host/Host.h"
#include "i18n/Tr.h"
#include "modules/multikey/MusigAccountController.h"
#include "modules/multikey/MusigPlugin.h"
#include "modules/multikey/SavedPsbts.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThread>
#include <common.h>
#include <initializer_list>
#include <memory>
#include <silent.h>

#define private public
#define protected public
#include "modules/multikey/SignModal.h"
#undef protected
#undef private

namespace smoke {
namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("MuSig reset smoke: %s", message);
    }
}

auto button(QWidget &widget, const QString &text) -> catalog::Button * {
    for (auto *candidate : widget.findChildren<catalog::Button *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    qFatal("MuSig reset smoke: button missing");
}

// Runs the posted table rebuilds and the deletes of the rows they replace.
void settle() {
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

// A saved PSBT of 2 signers, its attempts listed active first.
auto savedEntry(const char *id, std::initializer_list<SavedPsbtAttemptState> attempts)
    -> SavedPsbtEntry {
    SavedPsbtEntry entry{};
    entry.record.id = rust::String(id);
    entry.record.updated_at = 1760000000;
    entry.signatures_known = true;
    entry.signatures_required = 2;
    for (const auto &attempt : attempts) {
        entry.attempts.push_back(attempt);
    }
    entry.signatures_complete = entry.attempts[0].signatures_complete;
    return entry;
}

// The fold buttons of the rows shown, by saved PSBT id.
auto foldButtons(QWidget &tab) -> QHash<QString, catalog::Button *> {
    QHash<QString, catalog::Button *> buttons;
    for (auto *candidate : tab.findChildren<catalog::Button *>()) {
        if (candidate->isVisibleTo(&tab) && (candidate->toolTip() == TR("btn-show-attempts") ||
                                             candidate->toolTip() == TR("btn-hide-attempts"))) {
            buttons.insert(candidate->property("savedId").toString(), candidate);
        }
    }
    return buttons;
}

// The attempt sub-rows shown: each attempt's name, then the signatures in on its row.
auto attemptRows(QWidget &tab) -> QStringList {
    QStringList rows;
    for (auto *name : tab.findChildren<catalog::Label *>()) {
        if (!name->isVisibleTo(&tab) || !name->text().startsWith(QStringLiteral("Attempt"))) {
            continue;
        }
        auto *row = name->parentWidget();
        while (row->property("class").toString() != QStringLiteral("table-row")) {
            row = row->parentWidget();
        }
        for (auto *signatures : row->findChildren<catalog::Label *>()) {
            if (signatures->property("tblmono").isValid()) {
                rows.append(name->text() + QStringLiteral(" ") + signatures->text());
            }
        }
    }
    return rows;
}

void click(QWidget *widget) {
    auto center = QPointF(widget->rect().center());
    auto global = QPointF(widget->mapToGlobal(widget->rect().center()));
    QMouseEvent press(QEvent::MouseButtonPress, center, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

// One attempt: no fold control. Several: one fold, folded at first, one sub-row per
// attempt once unfolded, read from the attempt; neither the fold nor a sub-row opens
// (activates) the saved PSBT.
void savedAttempts(MusigAccountController &controller, ViewServices &services) {
    musig::SavedPsbts tab(&controller, services.services());
    tab.resize(1200, 600);
    tab.show();
    SavedPsbtList listed{};
    listed.is_ok = true;
    listed.entries.push_back(savedEntry("single", {{.attempt_id = 0, .active = true}}));
    listed.entries.push_back(
        savedEntry("multi", {{.attempt_id = 2, .active = true, .signatures_complete = 1},
                             {.attempt_id = 1, .active = false, .signatures_complete = 0},
                             {.attempt_id = 0, .active = false, .signatures_complete = 2}}));
    tab.onListed(listed);
    settle();
    SignalSpy opened;
    auto *table = tab.findChild<catalog::RowClickTable *>();
    QObject::connect(table, &catalog::RowClickTable::rowClicked, &opened, &SignalSpy::onSignal,
                     qontrol::UNIQUE);

    auto folds = foldButtons(tab);
    require(folds.size() == 1 && folds.contains(QStringLiteral("multi")),
            "only the PSBT of several attempts has a fold control");
    require(folds.value(QStringLiteral("multi"))->toolTip() == TR("btn-show-attempts"),
            "it is folded by default");
    require(attemptRows(tab).isEmpty(), "folded: no attempt sub-row");

    folds.value(QStringLiteral("multi"))->click();
    settle();
    require(foldButtons(tab).value(QStringLiteral("multi"))->toolTip() == TR("btn-hide-attempts"),
            "the fold control unfolds");
    auto rows = attemptRows(tab);
    rows.sort();
    require(rows == QStringList{TR("psbt-attempt-archived").arg(1) + QStringLiteral(" 2/2"),
                                TR("psbt-attempt-archived").arg(2) + QStringLiteral(" 0/2"),
                                TR("psbt-attempt-active").arg(3) + QStringLiteral(" 1/2")},
            "one sub-row per attempt with its own signatures in");
    for (auto *name : tab.findChildren<catalog::Label *>()) {
        if (name->isVisibleTo(&tab) && name->text().startsWith(QStringLiteral("Attempt"))) {
            click(name);
        }
    }
    settle();
    require(opened.count() == 0 && controller.psbtInFlight().isEmpty(),
            "unfolding and clicking an attempt leave the active attempt unchanged");

    foldButtons(tab).value(QStringLiteral("multi"))->click();
    settle();
    require(attemptRows(tab).isEmpty(), "the fold control folds again");
    require(opened.count() == 0, "folding opens nothing");
}

// The keys a signing modal lists, without any device or key store behind them.
class ListedKeys final : public DeferredKeys {
public:
    void setEntries(const QList<KeyEntry> &entries) {
        m_entries = entries;
    }

protected:
    [[nodiscard]] auto readKeys([[maybe_unused]] plugin::Network network) const
        -> KeyListResult override {
        return {.is_ok = true, .entries = m_entries};
    }

private:
    QList<KeyEntry> m_entries;
};

const char *const CACHED_FINGERPRINT = "aaaaaaaa";
const char *const OTHER_FINGERPRINT = "bbbbbbbb";

auto candidate(const char *fingerprint, bool nonces_done, bool cache_ready) -> SigningCandidate {
    SigningCandidate entry{};
    entry.fingerprint = rust::String(fingerprint);
    entry.route = KeyRouteKind::Hot;
    entry.can_sign = true;
    entry.unusable = KeyUnusableReason::None;
    entry.registered = true;
    entry.musig_done = nonces_done;
    entry.nonces_done = nonces_done;
    entry.cache_ready = cache_ready;
    entry.cache_supported = true;
    return entry;
}

// The nonce step over two cosigners, each as `first` and `second` say.
void renderNonceStep(musig::SignModal &signing, const SigningCandidate &first,
                     const SigningCandidate &second) {
    SigningPlan plan{};
    plan.is_ok = true;
    plan.candidates.push_back(first);
    plan.candidates.push_back(second);
    *signing.m_plan = plan;
    signing.m_plan_ready = true;
    signing.m_partial = false;
    MusigRoundStatus status{};
    status.is_ok = true;
    status.round = MusigRound::Nonces;
    for (const auto *fingerprint : {CACHED_FINGERPRINT, OTHER_FINGERPRINT}) {
        status.cosigners.push_back(MusigCosignerStatus{.fingerprint = rust::String(fingerprint),
                                                       .nonces = 0,
                                                       .partial_sigs = 0,
                                                       .expected = 1});
    }
    signing.render(status);
}

// The reason the footer shows, empty when it shows none.
auto footerReason(musig::SignModal &signing) -> QString {
    for (auto *label : signing.m_actions->findChildren<catalog::Label *>()) {
        if (label->isVisibleTo(signing.m_actions)) {
            return label->text();
        }
    }
    return {};
}

// A cosigner whose session is cached counts as done on the nonce step: the counter
// leaves it out and Continue opens once every cosigner is cached or in.
void cachedNonceStep() {
    AppController::get()->registerBuiltinPlugin(std::make_unique<MusigPlugin>());
    auto *host = dynamic_cast<HostImpl *>(Host::get());
    require(host != nullptr, "real host exists");
    ListedKeys keys;
    keys.setEntries({KeyEntry{.fingerprint = QString::fromLatin1(CACHED_FINGERPRINT)},
                     KeyEntry{.fingerprint = QString::fromLatin1(OTHER_FINGERPRINT)}});
    host->setKeys(&keys, MusigModule::get());
    auto descriptor = QStringLiteral("tr(musig("
                                     "tpubDEGU7K8ab62XbG1bKoCnHDtUx44DxVKyAQVS96EN1EUVTQRW2KnFEvTtm"
                                     "nUJSonudSK8HcXTexf56Kir1JzoQAzFRHfvNLEcqV7KxUHZVYa,"
                                     "tpubDDzMtUDKwjvTehAmAfwYiBGzUFDcF8AyEaJf78JhQam5knvsGTgqF7Vvh"
                                     "uuDZLYityND66FG88sS3q28Yi5Ysumvx5DfMgdm6nc6zntvpHM"
                                     ")/<0;1>/*)");
    auto name = QStringLiteral("smoke-musig-cached-nonces");
    auto config = ::new_config(rust::String(name.toStdString()), Network::Testnet,
                               rust::String(descriptor.toStdString()), rust::String(""),
                               rust::String(""), 546, rust::String("musig"));
    require(config->save().empty(), "save an offline MuSig account");
    {
        MusigAccountController controller(name, nullptr);
        QElapsedTimer opening;
        opening.start();
        while (controller.accountLoading() && opening.elapsed() < 30000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        require(controller.hasAccountMetadata(), "the offline MuSig account opens");
        ViewServices services;
        musig::SignModal signing(&controller, &services.blockchain);
        signing.show();

        renderNonceStep(signing, candidate(CACHED_FINGERPRINT, false, true),
                        candidate(OTHER_FINGERPRINT, false, false));
        require(footerReason(signing) == TR("musig-sign-reason-one"),
                "a cached cosigner is not counted as still to go");
        require(signing.m_continue_btn->isVisible() && !signing.m_continue_btn->isEnabled(),
                "Continue waits for the cosigner whose session is missing");

        renderNonceStep(signing, candidate(CACHED_FINGERPRINT, false, true),
                        candidate(OTHER_FINGERPRINT, false, true));
        require(footerReason(signing).isEmpty(), "no cosigner is left once every one is cached");
        require(signing.m_continue_btn->isEnabled(),
                "Continue opens once every cosigner is cached");

        renderNonceStep(signing, candidate(CACHED_FINGERPRINT, false, true),
                        candidate(OTHER_FINGERPRINT, true, true));
        require(footerReason(signing).isEmpty() && signing.m_continue_btn->isEnabled(),
                "a cached cosigner and one whose nonces are in leave none to go");
        signing.reject();

        SignalSpy stopped;
        QObject::connect(&controller, &MusigAccountController::stopped, &stopped,
                         &SignalSpy::onSignal, qontrol::UNIQUE);
        controller.stop();
        QElapsedTimer stopping;
        stopping.start();
        while (stopped.count() == 0 && stopping.elapsed() < 30000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        require(stopped.count() == 1, "the MuSig account shuts down");
    }
    require(::delete_musig_account(rust::String(name.toStdString())).empty(),
            "remove the MuSig test account");
}

} // namespace

void musigResetSmoke() {
    SignalSpy confirmed;
    musig::ConfirmSigningReset confirmation;
    QObject::connect(&confirmation, &musig::ConfirmSigningReset::confirmed, &confirmed,
                     &SignalSpy::onSignal, qontrol::UNIQUE);
    confirmation.show();
    button(confirmation, TR("common-cancel"))->click();
    require(confirmed.count() == 0 && confirmation.result() == QDialog::Rejected,
            "Cancel rejects without a reset request");
    confirmation.show();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&confirmation, &escape);
    require(confirmed.count() == 0 && confirmation.result() == QDialog::Rejected,
            "Escape dismisses without a reset request");
    confirmation.show();
    confirmation.close();
    require(confirmed.count() == 0, "window dismissal emits no reset request");
    confirmation.accept();
    require(confirmed.count() == 0, "generic dialog acceptance is not affirmative reset consent");
    confirmation.show();
    auto *yes = button(confirmation, TR("psbt-reset-confirm"));
    require(yes->role() == catalog::ButtonRole::Destructive, "affirmative reset is destructive");
    yes->click();
    require(confirmed.count() == 1 && confirmation.result() == QDialog::Accepted,
            "only explicit Yes emits one reset request");

    // An unloaded controller needs no account fixture or device to check the real footer.
    MusigAccountController controller(QStringLiteral("missing-reset-smoke-account"), nullptr);
    QElapsedTimer opening;
    opening.start();
    while (controller.accountLoading() && opening.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(!controller.accountLoading(), "missing account opening finishes safely");
    ViewServices services;
    musig::SignModal signing(&controller, &services.blockchain);
    signing.show();
    QApplication::processEvents();
    auto *reset = button(signing, TR("psbt-reset"));
    auto *actions = reset->parentWidget();
    require(reset->role() == catalog::ButtonRole::Destructive, "Reset uses destructive role");
    require(reset->geometry().left() < actions->width() / 2,
            "Reset is left aligned before the footer spacer");
    require(controller.psbtInFlight().isEmpty() && !controller.restartPending() &&
                services.blockchain.requests.isEmpty(),
            "rendering leaves controller state unchanged and broadcasts nothing");
    signing.reject();
    savedAttempts(controller, services);
    cachedNonceStep();
}

} // namespace smoke
