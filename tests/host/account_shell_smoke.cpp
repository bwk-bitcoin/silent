// The generic backup and restore flows (the shell over two plugins is in
// account_services_smoke). The real SP and Multikey accounts back up through their account
// service, from their shell; the restore modal reads a backup through the module's
// restore target, refuses a wrong password, another kind of account and an existing
// account, and restores one that is gone, which the module then lists. The launcher's
// Account Type picker lists the account types only, and offers Create or Restore for SP
// and Multikey, each opening that type's flow; a type with a create entry only starts it.

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <Qontrol>
#include <common.h>
#include <functional>
#include <memory>
#include <optional>
#include <silent.h>
#include <tuple>

#include "ViewServices.h"

#define private public
#define protected public
#include "AppController.h"
#include "MainWindow.h"
#include "account/AccountModule.h"
#include "account/AccountShell.h"
#include "account/services/ServiceAdapters.h"
#include "account/views/BackupAccount.h"
#include "account/views/RestoreAccount.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/Notice.h"
#include "catalog/inputs/Input.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "modules/musig/MusigAccountWidget.h"
#include "modules/musig/MusigPlugin.h"
#include "modules/musig/creator/CreatePolicy.h"
#include "modules/musig/creator/MusigCreatorModule.h"
#include "modules/shared/AccountLifecycle.h"
#include "modules/sp/AccountWidget.h"
#include "modules/sp/SpModule.h"
#include "modules/sp/SpPlugin.h"
#include "modules/sp/modals/CreateAccount.h"
#include "views/MenuTab.h"
#include "views/modals/AccountType.h"
#undef private
#undef protected

namespace smoke {
namespace {

constexpr int TIMEOUT_MS = 30000;
const QString PASSWORD = QStringLiteral("correct horse battery");
const QString SP_ACCOUNT = QStringLiteral("shell-sp");
const QString MULTIKEY_ACCOUNT = QStringLiteral("shell-multikey");
const QString PLUGIN_TYPE = QStringLiteral("Plugin Wallet");
const char *const SP_DESCRIPTOR =
    "sp([73c5da0a/352'/1'/0']tspscan1q8pjcdy7qzlzxl44chw2tsanvzg7dtwhkqf3nsvzmdavls2ekl8qq9qe"
    "sshy6w9knddr825kqp442302zuwddh6vtqk7zqvgszace9aczgnuqn3)#ke33wu3h";
const char *const MULTIKEY_DESCRIPTOR =
    "tr(musig([4ba43603/87'/1'/0'/2]tpubDFGmrfdE8Bya4CPLde4Q54gxkY4KKJzzs6y5tGWxVEcrfmzZb17aCamv"
    "X9k1UwKPPHtd7u5WqyS5SiRwxBZvHhbCJumqwECQQycuAmYtfzZ,[8dfc9b34/87'/1'/0'/2]tpubDFWeBGy8cbQXd"
    "CA37ePK189L12ML8fZCuuG9EZtwybxBx2jrVRLrWwsXvRHvXuCS5Q6sRyLB92TsidhBVbwWoeTAxDsDC2w5vQmPVnL2v"
    "KB)/<0;1>/*,pk([56c4fac3/87'/1'/0'/2]tpubDE1s5mmJDMZQ9uwi2KHe99ioqqEpwwZbAmAWP1UK6Y6zHLTKQw"
    "hkGLeSsQi38VaBwzfWtUrSL7v4tD1DPmHKjSfXBRbXVYUVboM7idebk7T/<2;3>/*))";

void require(bool condition, const char *message) {
    if (!condition) {
        qFatal("account shell: %s", message);
    }
}

template <typename Predicate> void waitFor(Predicate done, const char *message) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < TIMEOUT_MS) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(done(), message);
}

// The answers of an account service.
class Backups final : public QObject {
    Q_OBJECT

public:
    QByteArray backup;
    backend::RequestId created = 0;
    backend::RequestId failed = 0;
    QString error;

public slots:

    void onBackupCreated(backend::RequestId request_id, const QByteArray &bytes) {
        created = request_id;
        backup = bytes;
    }

    void onError(backend::RequestId request_id, const QString &message) {
        failed = request_id;
        error = message;
    }
};

// A plugin's creator that lists a create entry only, "plugin.create", and records the
// entries started.
class CreateOnlyModule final : public IModule {
    Q_OBJECT

public:
    [[nodiscard]] auto started() const -> const QStringList & {
        return m_started;
    }

    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("create-only.creator")},
                {QStringLiteral("name"), QStringLiteral("Create Only Creator")}};
    }

    auto list() -> ReqId override {
        emit IModule::instances(++m_req, {{QStringLiteral("plugin.create"), PLUGIN_TYPE}});
        return m_req;
    }

    auto autoStart() -> ReqId override {
        emit IModule::autoStart(++m_req, QStringList{});
        return m_req;
    }

    void startInstance(const QString &id) override {
        m_started.append(id);
    }

private:
    QStringList m_started;
    ReqId m_req = 0;
};

class CreateOnlyPlugin final : public IPlugin {
public:
    [[nodiscard]] auto meta() const -> QMap<QString, QString> override {
        return {{QStringLiteral("id"), QStringLiteral("create-only")},
                {QStringLiteral("name"), QStringLiteral("Create Only")}};
    }

    [[nodiscard]] auto modules() const -> QList<IModule *> override {
        return {&m_module};
    }

    mutable CreateOnlyModule m_module;
};

// Backup refuses a short or unconfirmed password, and shows the account's refusal.
void backupModal() {
    ViewServices fakes;
    fakes.account.script(QStringLiteral("backup")).error = QStringLiteral("disk full");
    account::view::BackupAccount modal(&fakes.account);
    modal.m_password_input->setText(QStringLiteral("short"));
    modal.onPasswordEdited();
    require(!modal.m_save->isEnabled(), "a short password saves nothing");
    modal.m_password_input->setText(PASSWORD);
    modal.m_confirm_input->setText(PASSWORD + QStringLiteral("!"));
    modal.onPasswordEdited();
    require(!modal.m_save->isEnabled(), "an unconfirmed password saves nothing");
    modal.m_confirm_input->setText(PASSWORD);
    modal.onPasswordEdited();
    require(modal.m_save->isEnabled(), "a confirmed password can seal the backup");
    modal.onNextClicked();
    auto requests = fakes.account.requestsOf(QStringLiteral("backup"));
    require(requests.size() == 1 && requests.first().payload.toByteArray() == PASSWORD.toUtf8(),
            "the backup is sealed under the password typed");
    require(modal.m_view == account::view::BackupAccount::View::Password &&
                !modal.m_password_error->isHidden() && modal.m_password_input->text().isEmpty(),
            "a refused backup says why, the password cleared");
    require(account::view::backupFileName(QStringLiteral("my wallet")).startsWith("my_wallet-") &&
                account::view::backupFileName(QStringLiteral("x")).endsWith(".silent-backup"),
            "a backup file is named after its account");
    QTemporaryDir dir;
    auto path = dir.filePath(QStringLiteral("backup.silent-backup"));
    require(account::view::writeBackupFile(path, QByteArray("sealed")).isEmpty(),
            "a backup file is written");
    QFile written(path);
    require(written.open(QIODevice::ReadOnly) && written.readAll() == "sealed",
            "the file holds the backup as it is");
}

auto openAccount(account::AccountModule *module, QWidget &window, const QString &id)
    -> account::AccountShell * {
    module->startInstance(id);
    account::AccountShell *shell = nullptr;
    waitFor(
        [&] {
            for (auto *candidate : window.findChildren<account::AccountShell *>()) {
                if (candidate->services().account != nullptr &&
                    candidate->services().account->id() == id) {
                    shell = candidate;
                }
            }
            return shell != nullptr;
        },
        "the account's shell assembles its services");
    // A request finds the account busy while its startup connection runs.
    waitFor(
        [&] {
            for (auto *lifecycle : window.findChildren<AccountLifecycle *>()) {
                if (lifecycle->property("logicalAccountId").toString() == id) {
                    return !lifecycle->endpointOperationsBusy();
                }
            }
            return false;
        },
        "the account's startup connection ends");
    return shell;
}

auto backupOf(backend::IAccountBackend *account) -> QByteArray {
    Backups backups;
    QObject::connect(account, &backend::IAccountBackend::backupCreated, &backups,
                     &Backups::onBackupCreated, qontrol::UNIQUE);
    QObject::connect(account, &backend::IService::error, &backups, &Backups::onError,
                     qontrol::UNIQUE);
    account->backup(41, PASSWORD.toUtf8());
    waitFor([&backups] { return backups.created == 41 || backups.failed == 41; },
            "the account answers its backup");
    require(backups.created == 41 && !backups.backup.isEmpty(), qPrintable(backups.error));
    Backups refused;
    QObject::connect(account, &backend::IService::error, &refused, &Backups::onError,
                     qontrol::UNIQUE);
    account->restore(42, backups.backup, PASSWORD.toUtf8());
    require(refused.failed == 42, "an open account is never restored over");
    return backups.backup;
}

// Closed, its page deleted.
void closeAccount(account::AccountModule *module, const QString &id) {
    Host::get()->removeInstance(id, module);
    waitFor([module, &id] { return !module->accountUnavailable(id); }, "the account closes");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// The restore modal over `plugin_id`'s restore target, `backup` chosen.
auto restoreModal(const QString &plugin_id, const QByteArray &backup)
    -> std::unique_ptr<account::view::RestoreAccount> {
    auto modal =
        std::make_unique<account::view::RestoreAccount>(account::restoreTarget(plugin_id, nullptr));
    modal->m_backup = backup;
    modal->m_backup_path = QStringLiteral("/backups/account.silent-backup");
    return modal;
}

void readBackup(account::view::RestoreAccount &modal, const QString &password) {
    modal.m_password_input->setText(password);
    modal.onPasswordEdited();
    require(modal.m_read->isEnabled(), "a chosen file and a password read the backup");
    modal.onNextClicked();
    require(modal.m_view == account::view::RestoreAccount::View::Busy, "the backup is read");
    waitFor([&modal] { return modal.m_view != account::view::RestoreAccount::View::Busy; },
            "the backup read answers");
}

// The restore of `backup`, the account `id`: refused with a wrong password, as another
// kind of account and while the account exists; restored once it is gone.
void restoreFlow(account::AccountModule *module, QWidget &window, const QString &plugin_id,
                 const QString &other_plugin, const QString &id, const QByteArray &backup) {
    auto wrong = restoreModal(plugin_id, backup);
    readBackup(*wrong, QStringLiteral("another password"));
    require(wrong->m_view == account::view::RestoreAccount::View::Password &&
                wrong->m_password_error->m_text->text() == TR("backup-wrong-password") &&
                wrong->m_password.isEmpty(),
            "a wrong password is refused, nothing held");

    auto other = restoreModal(other_plugin, backup);
    readBackup(*other, PASSWORD);
    require(other->m_view == account::view::RestoreAccount::View::Password &&
                !other->m_password_error->isHidden(),
            "another module's restore target refuses the backup");

    auto existing = restoreModal(plugin_id, backup);
    readBackup(*existing, PASSWORD);
    require(existing->m_view == account::view::RestoreAccount::View::Review &&
                existing->m_account == id,
            "the backup's account is reviewed before it is restored");
    existing->onNextClicked();
    waitFor([&] { return existing->m_view != account::view::RestoreAccount::View::Busy; },
            "the restore answers");
    require(existing->m_view == account::view::RestoreAccount::View::Review &&
                !existing->m_review_error->isHidden(),
            "an existing account is not restored over");

    closeAccount(module, id);
    auto removed = module->m_remove(rust::String(id.toStdString()));
    require(removed.empty(), "the account's storage is removed");
    require(!::config_from_file(rust::String(id.toStdString()))->is_ok(), "the account is gone");
    auto restore = restoreModal(plugin_id, backup);
    readBackup(*restore, PASSWORD);
    restore->onNextClicked();
    waitFor([&] { return restore->m_view != account::view::RestoreAccount::View::Busy; },
            "the restore answers");
    require(restore->m_view == account::view::RestoreAccount::View::Restored &&
                restore->m_password.isEmpty(),
            "the account is restored, the password dropped");
    require(::config_from_file(rust::String(id.toStdString()))->is_ok(),
            "the restored account is saved");
    auto *shell = openAccount(module, window, id);
    require(shell->services().account->id() == id, "the restored account opens");
    closeAccount(module, id);
}

// Runs `step` on the modal exec() shows next, a T, then closes it.
template <typename T>
void onNextModal(const std::function<void(T &)> &step, bool *ran, const char *message) {
    QTimer::singleShot(0, [step, ran, message] {
        auto *modal = qobject_cast<T *>(QApplication::activeModalWidget());
        require(modal != nullptr, message);
        step(*modal);
        *ran = true;
        if (modal->isVisible()) {
            modal->reject();
        }
    });
}

// The titles of the rows `modal` shows, in their order.
auto rowTitles(QWidget &modal) -> QStringList {
    QStringList titles;
    for (auto *row : modal.findChildren<catalog::SelectRow *>()) {
        if (row->isVisibleTo(&modal)) {
            titles.append(row->m_title->text());
        }
    }
    return titles;
}

// Clicks the row of `modal` titled `title`.
void pick(QWidget &modal, const QString &title) {
    for (auto *row : modal.findChildren<catalog::SelectRow *>()) {
        if (row->isVisibleTo(&modal) && row->m_title->text() == title) {
            emit row->clicked();
            return;
        }
    }
    qFatal("account shell: no row %s", qPrintable(title));
}

// Opens the launcher's Account Type picker and runs `step` on it.
void onPicker(MenuTab &menu, const std::function<void(modal::AccountType &)> &step) {
    bool ran = false;
    onNextModal<modal::AccountType>(step, &ran, "the launcher opens the Account Type picker");
    menu.onCreateClicked();
    require(ran, "the Account Type picker ran");
}

// Picks `type` in `picker`, which offers Create and Restore for it, then `entry` of the two,
// which opens the T modal: `step` runs on it.
template <typename T>
void pickEntry(modal::AccountType &picker, const QString &type, const QString &entry,
               const std::function<void(T &)> &step, const char *message) {
    pick(picker, type);
    require(rowTitles(picker) == QStringList{TR("account-type-create"), TR("account-type-restore")},
            "picking SP or Multikey offers Create and Restore");
    require(picker.windowTitle() == TR("account-type-choice-title").arg(type),
            "the choice names the type picked");
    bool ran = false;
    onNextModal<T>(step, &ran, message);
    pick(picker, entry);
    require(ran, message);
    require(!picker.isVisible(), "a started entry closes the picker");
}

// The restore `modal` opened from the picker reads a backup of its own kind, `own` of the
// account `id`, and refuses `other`, a backup of another kind.
void restoresKind(account::view::RestoreAccount &modal, const QByteArray &own,
                  const QByteArray &other, const QString &id) {
    modal.m_backup_path = QStringLiteral("/backups/account.silent-backup");
    modal.m_backup = other;
    readBackup(modal, PASSWORD);
    require(modal.m_view == account::view::RestoreAccount::View::Password &&
                !modal.m_password_error->isHidden(),
            "the restore opened from the picker refuses another kind's backup");
    modal.m_backup = own;
    readBackup(modal, PASSWORD);
    require(modal.m_view == account::view::RestoreAccount::View::Review && modal.m_account == id,
            "the restore opened from the picker reads its own kind's backup");
}

// The picker lists the account types only. SP and Multikey each offer Create, opening their
// create wizard, and Restore, opening the restore over their own restore target; Back
// returns to the types. The plugin's type, a create entry only, starts it at once.
void accountTypePicker(MenuTab &menu, CreateOnlyModule &plugin, const QByteArray &sp_backup,
                       const QByteArray &multikey_backup) {
    auto types = QStringList{TR("sp-account-type"), TR("musig-account-type"), PLUGIN_TYPE};
    onPicker(menu, [&types](modal::AccountType &picker) {
        require(rowTitles(picker) == types,
                "the Account Type picker lists the account types only, no restore entry");
        require(picker.windowTitle() == TR("account-type-title"), "the picker is Account Type");
        pick(picker, TR("sp-account-type"));
        require(rowTitles(picker) ==
                    QStringList{TR("account-type-create"), TR("account-type-restore")},
                "picking SP offers Create and Restore");
        auto *back = picker.findChild<catalog::ModalActions *>();
        require(back != nullptr && back->isVisibleTo(&picker), "the choice has a Back");
        emit back->backClicked();
        require(rowTitles(picker) == types && picker.windowTitle() == TR("account-type-title"),
                "Back returns to the account types");
    });
    onPicker(menu, [](modal::AccountType &picker) {
        pickEntry<modal::CreateAccount>(
            picker, TR("sp-account-type"), TR("account-type-create"), [](modal::CreateAccount &) {},
            "SP's Create opens the SP create wizard");
    });
    onPicker(menu, [&sp_backup, &multikey_backup](modal::AccountType &picker) {
        pickEntry<account::view::RestoreAccount>(
            picker, TR("sp-account-type"), TR("account-type-restore"),
            [&sp_backup, &multikey_backup](account::view::RestoreAccount &modal) {
                restoresKind(modal, sp_backup, multikey_backup, SP_ACCOUNT);
            },
            "SP's Restore opens the restore");
    });
    onPicker(menu, [](modal::AccountType &picker) {
        pickEntry<musig::modal::CreatePolicy>(
            picker, TR("musig-account-type"), TR("account-type-create"),
            [](musig::modal::CreatePolicy &) {}, "Multikey's Create opens the Multikey wizard");
    });
    onPicker(menu, [&sp_backup, &multikey_backup](modal::AccountType &picker) {
        pickEntry<account::view::RestoreAccount>(
            picker, TR("musig-account-type"), TR("account-type-restore"),
            [&sp_backup, &multikey_backup](account::view::RestoreAccount &modal) {
                restoresKind(modal, multikey_backup, sp_backup, MULTIKEY_ACCOUNT);
            },
            "Multikey's Restore opens the restore");
    });
    onPicker(menu, [&plugin](modal::AccountType &picker) {
        pick(picker, PLUGIN_TYPE);
        require(!picker.isVisible() &&
                    plugin.started() == QStringList{QStringLiteral("plugin.create")},
                "a type with a create entry only starts it at once");
    });
}

void builtinBackups() {
    for (const auto &[name, descriptor, plugin] :
         {std::tuple{SP_ACCOUNT, SP_DESCRIPTOR, "sp"},
          std::tuple{MULTIKEY_ACCOUNT, MULTIKEY_DESCRIPTOR, "musig"}}) {
        auto config = ::new_config(rust::String(name.toStdString()), Network::Regtest,
                                   rust::String(descriptor), rust::String("http://127.0.0.1:1"),
                                   rust::String(""), 0, rust::String(plugin));
        require(config->save().empty(), "the Regtest fixture account saves");
    }
    auto sp = std::make_unique<SpPlugin>();
    auto multikey = std::make_unique<MusigPlugin>();
    auto createOnly = std::make_unique<CreateOnlyPlugin>();
    auto *spModule = qobject_cast<account::AccountModule *>(sp->modules().first());
    auto *multikeyModule = qobject_cast<account::AccountModule *>(multikey->modules().first());
    auto *multikeyCreator = multikey->modules().last();
    auto *createOnlyModule = &createOnly->m_module;
    auto *controller = AppController::get();
    controller->registerBuiltinPlugin(std::move(sp));
    controller->registerBuiltinPlugin(std::move(multikey));
    controller->registerBuiltinPlugin(std::move(createOnly));
    auto *registry = controller->pluginRegistry();
    waitFor([registry] { return !registry->pluginStateLoading(); }, "the plugin state loads");
    MainWindow window;
    controller->start(&window);

    auto entries = qobject_cast<SpModule *>(spModule)->instanceEntries({});
    require(entries.contains(
                QPair<QString, QString>(QStringLiteral("sp.restore"), TR("sp-account-restore"))) &&
                PluginRegistry::isCreatorInstanceId(QStringLiteral("sp.restore")) &&
                PluginRegistry::isCreatorInstanceId(QStringLiteral("musig.restore")),
            "each built-in account kind lists its restore among the creators");

    auto *spShell = openAccount(spModule, window, SP_ACCOUNT);
    auto spBackup = backupOf(spShell->services().account);
    auto *multikeyShell = openAccount(multikeyModule, window, MULTIKEY_ACCOUNT);
    auto multikeyBackup = backupOf(multikeyShell->services().account);
    waitFor(
        [registry, spModule, multikeyCreator, createOnlyModule] {
            return !registry->launcherInstancesForModule(spModule).isEmpty() &&
                   !registry->launcherInstancesForModule(multikeyCreator).isEmpty() &&
                   !registry->launcherInstancesForModule(createOnlyModule).isEmpty();
        },
        "the launcher lists every module's entries");
    accountTypePicker(*qobject_cast<MenuTab *>(window.m_menu_tab), *createOnlyModule, spBackup,
                      multikeyBackup);
    restoreFlow(spModule, window, QStringLiteral("sp"), QStringLiteral("musig"), SP_ACCOUNT,
                spBackup);
    restoreFlow(multikeyModule, window, QStringLiteral("musig"), QStringLiteral("sp"),
                MULTIKEY_ACCOUNT, multikeyBackup);
    controller->start(nullptr);
}

} // namespace

void accountShellSmoke() {
    backupModal();
    builtinBackups();
}

} // namespace smoke

#include "account_shell_smoke.moc"
