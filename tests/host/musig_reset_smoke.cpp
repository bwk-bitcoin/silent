#include "MockPlugin.h"

#include "catalog/Button.h"
#include "catalog/containers/ModalActions.h"
#include "i18n/Tr.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/SignModal.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QThread>
#include <common.h>

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
    musig::SignModal signing(&controller);
    signing.show();
    QApplication::processEvents();
    auto *reset = button(signing, TR("psbt-reset"));
    auto *actions = reset->parentWidget();
    require(reset->role() == catalog::ButtonRole::Destructive, "Reset uses destructive role");
    require(reset->geometry().left() < actions->width() / 2,
            "Reset is left aligned before the footer spacer");
    require(controller.psbtInFlight().isEmpty() && !controller.restartPending(),
            "rendering leaves controller state unchanged");
    signing.reject();
}

} // namespace smoke
