#include "MockPlugin.h"
#include "ViewServices.h"

#include "catalog/Button.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/panels/home/RowClickTable.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/SavedPsbts.h"
#include "modules/musig/SignModal.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThread>
#include <common.h>
#include <initializer_list>

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
}

} // namespace smoke
