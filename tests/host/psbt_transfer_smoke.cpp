// PSBT export and import through the real views: the export modal's clipboard copy and
// file write, then a Multikey account importing a cosigner's PSBT from the clipboard on
// its PSBT tab, merging it again, refusing garbage, and exporting the saved row.

#include "AppController.h"
#include "MainWindow.h"
#include "account/AccountModule.h"
#include "account/PsbtTransfer.h"
#include "catalog/Button.h"
#include "catalog/feedback/Notice.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/host.h"
#include "modules/musig/MusigAccountController.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/musig/SavedPsbts.h"
#include "modules/musig/SigningSection.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <common.h>
#include <functional>
#include <memory>
#include <silent.h>

namespace smoke {
namespace {

constexpr int TIMEOUT_MS = 20000;

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("PSBT transfer smoke: %s", message);
    }
}

void waitFor(const std::function<bool()> &done, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(done(), message);
}

auto button(QWidget &root, const QString &text) -> catalog::Button * {
    for (auto *candidate : root.findChildren<catalog::Button *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    qFatal("PSBT transfer smoke: button missing");
}

// The text of the notice `modal` shows, empty while it is hidden.
auto noticeText(QWidget &modal) -> QString {
    auto *notice = modal.findChild<catalog::Notice *>();
    require(notice != nullptr, "the modal has a notice");
    if (!notice->isVisibleTo(&modal)) {
        return {};
    }
    QStringList texts;
    for (auto *label : notice->findChildren<QLabel *>()) {
        texts.append(label->text());
    }
    return texts.join(QStringLiteral("\n"));
}

// Runs `step` on the modal of type T once exec() shows it, then closes it.
template <typename T> void onModal(const std::function<void(T &)> &step, bool *ran) {
    QTimer::singleShot(0, [step, ran] {
        auto *modal = qobject_cast<T *>(QApplication::activeModalWidget());
        require(modal != nullptr, "the modal runs");
        step(*modal);
        *ran = true;
        modal->reject();
    });
}

void exportModal() {
    auto base64 = QStringLiteral("cHNidP8BAAoCAAAAAAAAAAAAAA==");
    account::ExportPsbt modal(base64, QStringLiteral("a label/with spaces"));
    modal.show();
    require(noticeText(modal).isEmpty(), "nothing is said before an action");
    QApplication::clipboard()->clear();
    button(modal, TR("btn-copy-to-clipboard"))->click();
    require(QApplication::clipboard()->text() == base64,
            "Copy to Clipboard puts the base64 on the clipboard");
    require(noticeText(modal).contains(TR("psbt-export-copied")), "and says it is copied");
    modal.reject();

    auto path = QDir::home().filePath(QStringLiteral("export-smoke.psbt"));
    require(account::writePsbtFile(path, base64).isEmpty(), "the file is written");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.readAll() == base64.toLatin1(),
            "the file holds the base64 text");
    require(account::psbtFileName(QStringLiteral("a label/with spaces")) ==
                QStringLiteral("a_label_with_spaces.psbt"),
            "the default file name keeps safe characters only");
    require(account::psbtFileName(QString()).startsWith(QStringLiteral("psbt-")),
            "an unnamed PSBT is named after the time");
}

void importOnTab(musig::SavedPsbts &tab, const QString &clipboard, const QString &expected) {
    bool ran = false;
    onModal<account::ImportPsbt>(
        [&clipboard, &expected](account::ImportPsbt &modal) {
            QApplication::clipboard()->setText(clipboard);
            button(modal, TR("btn-paste-from-clipboard"))->click();
            waitFor([&modal] { return !noticeText(modal).isEmpty(); }, "the import answers");
            require(noticeText(modal).contains(expected), "the import says how it went");
        },
        &ran);
    button(tab, TR("btn-import-psbt"))->click();
    require(ran, "the import modal ran");
}

} // namespace

void psbtTransferSmoke(const QString &account, const QString &psbt_file, const QString &txid,
                       const QString &outpoint) {
    exportModal();

    QFile file(psbt_file);
    require(file.open(QIODevice::ReadOnly), "the cosigner's PSBT file opens");
    auto base64 = QString::fromLatin1(file.readAll()).trimmed();
    auto multikey = std::make_unique<MusigPlugin>();
    auto *module = qobject_cast<account::AccountModule *>(multikey->modules().first());
    require(module != nullptr, "the Multikey module is an account module");
    auto *app = AppController::get();
    app->registerBuiltinPlugin(std::move(multikey));
    auto *registry = app->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    MainWindow window;
    app->start(&window);
    window.show();
    module->startInstance(account);
    auto *widget = window.findChild<MusigAccountWidget *>();
    require(widget != nullptr, "the Multikey account opens in a tab");
    auto *controller = widget->controller();
    waitFor([controller] { return controller->screen(QStringLiteral("psbt")).has_value(); },
            "the PSBT tab is built");
    controller->loadPanel(QStringLiteral("psbt"));
    auto *tab = qobject_cast<musig::SavedPsbts *>(
        controller->screen(QStringLiteral("psbt")).value_or(nullptr));
    require(tab != nullptr, "the PSBT tab is the saved PSBTs view");

    SavedPsbtList listed;
    auto list = [controller, &listed] {
        bool done = false;
        auto connection = QObject::connect(controller, &MusigAccountController::savedPsbtsListed,
                                           [&listed, &done](const SavedPsbtList &result) {
                                               listed = result;
                                               done = true;
                                           });
        controller->listSavedPsbts(true);
        waitFor([&done] { return done; }, "the saved PSBTs are listed");
        QObject::disconnect(connection);
        require(listed.is_ok, "the listing succeeds");
    };
    list();
    require(listed.entries.empty(), "nothing is saved before the import");

    // Send: a built transaction exports before any signing session starts (#83).
    auto *signing = window.findChild<musig::SigningSection *>();
    require(signing != nullptr, "the Send panel has its signing step");
    signing->setTransaction(
        backend::TransactionTemplate{
            .outputs = {{.address = QStringLiteral("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"),
                         .amount = 50000}},
            .inputs = {outpoint},
            .feeRate = 1},
        {outpoint});
    QPushButton *sendExport = nullptr;
    // The step's buttons sit in the Send view, which holds them.
    for (auto *candidate : window.findChildren<QPushButton *>()) {
        if (candidate->text() == TR("btn-export-psbt")) {
            sendExport = candidate;
        }
    }
    require(sendExport != nullptr && !sendExport->isHidden() && sendExport->isEnabled(),
            "Send shows Export as soon as a transaction is built");
    bool sendRan = false;
    QApplication::clipboard()->clear();
    onModal<account::ExportPsbt>(
        [](account::ExportPsbt &modal) { button(modal, TR("btn-copy-to-clipboard"))->click(); },
        &sendRan);
    sendExport->click();
    require(sendRan, "the export modal ran from Send");
    require(QApplication::clipboard()->text().startsWith(QStringLiteral("cHNidP8")),
            "Send exports the built transaction as a base64 PSBT");
    require(controller->psbtInFlight().isEmpty(), "exporting starts no signing session");

    importOnTab(*tab, base64, TR("psbt-import-saved"));
    list();
    require(listed.entries.size() == 1, "the import is saved");
    require(QString::fromStdString(std::string(listed.entries[0].txid)) == txid,
            "as the cosigner's transaction");

    importOnTab(*tab, QStringLiteral("  ") + base64 + QStringLiteral("\n"),
                TR("psbt-import-merged"));
    importOnTab(*tab, QStringLiteral("not a psbt"), TR("psbt-import-failed").arg(QString()));
    list();
    require(listed.entries.size() == 1, "a merge or a refusal adds no entry");

    // The saved row's Export copies the PSBT as imported.
    waitFor(
        [tab] {
            for (auto *candidate : tab->findChildren<catalog::Button *>()) {
                if (candidate->toolTip() == TR("btn-export-psbt") && candidate->isVisible()) {
                    return true;
                }
            }
            return false;
        },
        "the saved row shows Export");
    catalog::Button *exportButton = nullptr;
    for (auto *candidate : tab->findChildren<catalog::Button *>()) {
        if (candidate->toolTip() == TR("btn-export-psbt") && candidate->isVisible()) {
            exportButton = candidate;
        }
    }
    bool ran = false;
    QApplication::clipboard()->clear();
    onModal<account::ExportPsbt>(
        [](account::ExportPsbt &modal) { button(modal, TR("btn-copy-to-clipboard"))->click(); },
        &ran);
    exportButton->click();
    require(ran, "the export modal ran");
    require(QApplication::clipboard()->text() == base64,
            "the row exports the saved PSBT as base64");

    Host::get()->removeInstance(account, module);
    waitFor([module, &account] { return !module->accountUnavailable(account); },
            "the account closes");
}

} // namespace smoke
