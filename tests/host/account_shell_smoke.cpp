// The generic backup and restore flows (the shell over two plugins is in
// account_services_smoke). The real SP and Multikey accounts back up through their account
// service, from their shell; the restore modal reads a backup through the module's
// restore target, asks for a key to open it: it lists a fake signing device, fetches its
// xpubs at the backup's paths and opens the backup with them, and its other options take
// a pasted xpub and refuse an invalid xpub or mnemonic. It refuses another kind of account
// and an existing account, and restores one that is gone, which the module then lists. The
// launcher's Account Type picker lists the account types only, and offers Create or Restore for SP
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

#include "SpSigningProbe.h"
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
#include "account/views/Settings.h"
#include "catalog/Button.h"
#include "catalog/SelectRow.h"
#include "catalog/containers/ModalActions.h"
#include "catalog/display/Label.h"
#include "catalog/feedback/EmptyState.h"
#include "catalog/feedback/Notice.h"
#include "catalog/form/FieldGroup.h"
#include "catalog/form/FieldNote.h"
#include "catalog/form/ValidatedInput.h"
#include "catalog/inputs/Checkbox.h"
#include "catalog/inputs/ComboBox.h"
#include "catalog/inputs/Input.h"
#include "host/Host.h"
#include "host/PluginRegistry.h"
#include "i18n/Tr.h"
#include "interfaces/backend/account.h"
#include "modules/keys/DeviceName.h"
#include "modules/multikey/MusigAccountWidget.h"
#include "modules/multikey/MusigPlugin.h"
#include "modules/multikey/creator/CreatePolicy.h"
#include "modules/multikey/creator/MusigCreatorModule.h"
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
// The mnemonic of SP_DESCRIPTOR's key: its scan secret opens an SP backup.
const QString SP_MNEMONIC = QStringLiteral("abandon abandon abandon abandon abandon abandon "
                                           "abandon abandon abandon abandon abandon about");
const QString SP_SCAN_PATH = QStringLiteral("m/352'/1'/0'/1'/0");
// Another key's sp() descriptor: its scan secret opens no backup here.
const char *const OTHER_SP_DESCRIPTOR =
    "sp([b8688df1/352'/1'/0']tspscan1qj4e6layek964chal6ynl2zx8627tthvvkpf0yenpqndklh6pu98sxvs2qf"
    "4hta4a3se7qzn83tw2wgvqtl3x67g4znw4d708jpgrcuyz7tumh8)#dwt5rltp";
// The SP key's xpub at its BIP352 spend path: its root is the spend key the SP address
// shows, which opens no SP backup.
const QString SP_SPEND_PATH = QStringLiteral("m/352'/1'/0'/0'/0");
const QString SP_XPUB = QStringLiteral("tpubDFx6p4d9GVuNiNrxXR4MRPDFB3JSnm43ptWkAghygFYu73tXBZ5wnjX"
                                       "gzgDWqYd4m9giKQcnNwitc2etQm9ppxuDKvz"
                                       "3KkiK7rZFAdqWmwu");
// The first musig() participant of the Multikey policy, its xpub at its origin path.
const QString MULTIKEY_PATH = QStringLiteral("m/87'/1'/0'/2");
const QString MULTIKEY_XPUB = QStringLiteral("tpubDFGmrfdE8Bya4CPLde4Q54gxkY4KKJzzs6y5tGWxVEcrfmzZb"
                                             "17aCamvX9k1UwKPPHtd7u5WqyS5SiRwxBZvHhbCJum"
                                             "qwECQQycuAmYtfzZ");

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

    void onBackupCreated(backend::RequestId request_id, const QByteArray &bytes,
                         [[maybe_unused]] const QStringList &excluded) {
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

// A connected signing device that answers its xpub at each path of `xpubs` a turn of the
// event loop later, as a real one does, and refuses any other path.
class FakeDevice final : public ISigner {
    Q_OBJECT

public:
    FakeDevice(QString fingerprint, QMap<QString, QString> xpubs, QObject *parent)
        : ISigner(parent),
          m_fingerprint(std::move(fingerprint)),
          m_xpubs(std::move(xpubs)) {
    }

    [[nodiscard]] auto implemented() const -> bool override {
        return true;
    }

    [[nodiscard]] auto id() const -> QString override {
        return QStringLiteral("fake-device");
    }

    [[nodiscard]] auto fingerprint() const -> QString override {
        return m_fingerprint;
    }

    [[nodiscard]] auto walletName() const -> QString override {
        return {};
    }

    [[nodiscard]] auto brand() const -> QString override {
        return QStringLiteral("jade");
    }

    [[nodiscard]] auto model() const -> QString override {
        return {};
    }

    [[nodiscard]] auto serial() const -> QString override {
        return {};
    }

    [[nodiscard]] auto state() const -> plugin::SignerState override {
        return plugin::SignerState::Ready;
    }

    [[nodiscard]] auto stateDetail() const -> QString override {
        return {};
    }

    [[nodiscard]] auto capabilities() const -> plugin::SignerCapabilities override {
        return {};
    }

    [[nodiscard]] auto asked() const -> const QStringList & {
        return m_asked;
    }

    auto init() -> ReqId override {
        return 0;
    }

    auto info() -> ReqId override {
        return 0;
    }

    auto getXpub(const QString &path) -> ReqId override {
        m_asked.append(path);
        m_pending.append({++m_req, path});
        QTimer::singleShot(0, this, &FakeDevice::onAnswer);
        return m_req;
    }

    auto isDescriptorRegistered([[maybe_unused]] const QString &descriptor) -> ReqId override {
        return 0;
    }

    auto registerDescriptor([[maybe_unused]] const QString &descriptor) -> ReqId override {
        return 0;
    }

    auto signWithDescriptor([[maybe_unused]] const QString &psbt_base64,
                            [[maybe_unused]] const QString &descriptor) -> ReqId override {
        return 0;
    }

    auto displayAddress([[maybe_unused]] const QString &descriptor,
                        [[maybe_unused]] const QString &path,
                        [[maybe_unused]] const QString &address) -> ReqId override {
        return 0;
    }

    auto raw([[maybe_unused]] const QByteArray &request) -> ReqId override {
        return 0;
    }

public slots:

    void onAnswer() {
        auto [req, path] = m_pending.takeFirst();
        if (m_xpubs.contains(path)) {
            emit xpub(req, m_xpubs.value(path));
        } else {
            emit error(req, QStringLiteral("no xpub at %1").arg(path));
        }
    }

private:
    QString m_fingerprint;
    QMap<QString, QString> m_xpubs;
    QList<QPair<ReqId, QString>> m_pending;
    QStringList m_asked;
    ReqId m_req = 0;
};

// The instance a fake device is listed under.
class FakeDeviceInstance final : public QObject, public IInstance {
    Q_OBJECT
    Q_INTERFACES(IInstance)

public:
    FakeDeviceInstance(QString id, const QString &fingerprint, QMap<QString, QString> xpubs)
        : m_id(std::move(id)),
          m_device(new FakeDevice(fingerprint, std::move(xpubs), this)) {
    }

    [[nodiscard]] auto id() const -> QString override {
        return m_id;
    }

    void stop() override {
    }

    auto account() -> IAccount * override {
        return nullptr;
    }

    auto feed() -> IFeed * override {
        return nullptr;
    }

    auto signer() -> ISigner * override {
        return m_device;
    }

    auto theme() -> IThemeProvider * override {
        return nullptr;
    }

    [[nodiscard]] auto device() const -> FakeDevice * {
        return m_device;
    }

private:
    QString m_id;
    FakeDevice *m_device = nullptr;
};

// Backup asks the account for its backup, and shows the account's refusal.
void backupModal() {
    ViewServices fakes;
    fakes.account.script(QStringLiteral("backup")).error = QStringLiteral("disk full");
    account::view::BackupAccount modal(&fakes.account);
    require(modal.m_view == account::view::BackupAccount::View::Save && modal.m_save->isEnabled(),
            "the backup is saved with no password");
    modal.onNextClicked();
    require(fakes.account.requestsOf(QStringLiteral("backup")).size() == 1,
            "Save Backup asks the account for its backup");
    waitFor([&modal] { return modal.m_view == account::view::BackupAccount::View::Save; },
            "the account answers");
    require(!modal.m_save_error->isHidden() &&
                modal.m_save_error->m_text->text() == TR("backup-failed").arg("disk full"),
            "a refused backup says why");
    require(account::view::backupFileName(QStringLiteral("my wallet")).startsWith("my_wallet-") &&
                account::view::backupFileName(QStringLiteral("x")).endsWith(".bip138"),
            "a backup file is named after its account, as a BIP138 file");
    QTemporaryDir dir;
    auto path = dir.filePath(QStringLiteral("backup.bip138"));
    require(account::view::writeBackupFile(path, QByteArray("encrypted")).isEmpty(),
            "a backup file is written");
    QFile written(path);
    require(written.open(QIODevice::ReadOnly) && written.readAll() == "encrypted",
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
    account->backup(41);
    waitFor([&backups] { return backups.created == 41 || backups.failed == 41; },
            "the account answers its backup");
    require(backups.created == 41 && !backups.backup.isEmpty(), qPrintable(backups.error));
    Backups refused;
    QObject::connect(account, &backend::IService::error, &refused, &Backups::onError,
                     qontrol::UNIQUE);
    account->restore(42, backups.backup, {}, {}, QStringLiteral("restored"),
                     backend::Network::Regtest);
    require(refused.failed == 42, "an open account is never restored over");
    return backups.backup;
}

// The Backup modal and the Settings backup section of the account `services` serve say
// its own kind's texts, `details` and `summary`.
void backupTexts(const account::Services &services, const QString &details,
                 const QString &summary) {
    account::view::BackupAccount modal(services.account);
    require(modal.m_save_text->text() == details, "the Backup modal says its kind's backup");
    account::view::Settings settings(services, nullptr);
    require(settings.m_backup_text->text() == summary,
            "the Settings backup section says its kind's backup");
}

// Closed, its page deleted.
void closeAccount(account::AccountModule *module, const QString &id) {
    Host::get()->removeInstance(id, module);
    waitFor([module, &id] { return !module->accountUnavailable(id); }, "the account closes");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// The restore modal over `plugin_id`'s restore target, `backup` chosen.
// `module`, when set, asks Keys for scan keys.
auto restoreModal(const QString &plugin_id, const QByteArray &backup, IModule *module = nullptr)
    -> std::unique_ptr<account::view::RestoreAccount> {
    auto modal = std::make_unique<account::view::RestoreAccount>(
        account::restoreTarget(plugin_id, nullptr), module,
        static_cast<account::view::RestoreAccount::DeviceName>(&keys::deviceName));
    modal->m_backup = backup;
    modal->m_backup_path = QStringLiteral("/backups/account.bip138");
    modal->showFile();
    return modal;
}

// Reads the chosen backup: an encrypted one asks for a key.
void readBackup(account::view::RestoreAccount &modal) {
    require(modal.m_read->isEnabled(), "a chosen file reads the backup");
    modal.onNextClicked();
    require(modal.m_view == account::view::RestoreAccount::View::Busy, "the backup is read");
    waitFor([&modal] { return modal.m_view != account::view::RestoreAccount::View::Busy; },
            "the backup read answers");
    require(modal.m_view == account::view::RestoreAccount::View::Decrypt,
            "an encrypted backup asks for a key to open it");
}

// Pastes `xpub` in the Decrypt view and waits for its answer.
void pasteXpub(account::view::RestoreAccount &modal, const QString &xpub) {
    modal.m_xpub_input->input()->setText(xpub);
    modal.onXpubEdited();
    waitFor(
        [&modal] {
            return modal.m_view != account::view::RestoreAccount::View::Decrypt ||
                   !modal.m_xpub_note->isHidden();
        },
        "the pasted xpub is tried");
}

// Types `mnemonic` in the Decrypt view, its risk acknowledged, and waits for its answer.
void typeMnemonic(account::view::RestoreAccount &modal, const QString &mnemonic) {
    modal.m_mnemonic_ack->setChecked(true);
    modal.m_mnemonic_input->input()->setText(mnemonic);
    modal.onMnemonicEdited();
    waitFor(
        [&modal] {
            return modal.m_view != account::view::RestoreAccount::View::Decrypt ||
                   !modal.m_mnemonic_note->isHidden();
        },
        "the mnemonic is tried");
}

// What opens a backup in the Decrypt view: a pasted xpub, or a typed mnemonic.
struct Key {
    QString xpub;
    QString mnemonic;
};

void openWith(account::view::RestoreAccount &modal, const Key &key) {
    if (key.mnemonic.isEmpty()) {
        pasteXpub(modal, key.xpub);
    } else {
        typeMnemonic(modal, key.mnemonic);
    }
}

// The key was refused under its field, the view still asking for one.
auto refused(account::view::RestoreAccount &modal, const Key &key) -> bool {
    auto *note = key.mnemonic.isEmpty() ? modal.m_xpub_note : modal.m_mnemonic_note;
    return modal.m_view == account::view::RestoreAccount::View::Decrypt && !note->isHidden();
}

auto opener(const Key &key) -> QString {
    return key.mnemonic.isEmpty() ? TR("restore-opened-xpub") : TR("restore-opened-mnemonic");
}

// The texts of the rows of `rows`: name, sub and note.
auto rowTexts(qontrol::Column *rows) -> QStringList {
    QStringList texts;
    for (auto *row : rows->findChildren<catalog::SelectRow *>()) {
        texts.append(row->m_title->text() + QStringLiteral(" ") + row->m_sub->text() +
                     QStringLiteral(" ") + row->m_note->text());
    }
    return texts;
}

// The texts of the device rows the Decrypt view lists: name, sub and note.
auto deviceRows(account::view::RestoreAccount &modal) -> QStringList {
    return rowTexts(modal.m_device_rows);
}

// The Decrypt view of `backup`: no device listed, an invalid xpub, an xpub that does not
// open it and an invalid mnemonic refused under their field. Then a fake device that holds
// no key of the account is listed and fails, and one holding `xpub` at `path` is listed,
// asked its xpub at the backup's path and opens it: the review says which device did.
void decryptWithDevice(const QString &plugin_id, const QByteArray &backup, const QString &path,
                       const QString &xpub, const QString &other_xpub, IModule *owner) {
    auto modal = restoreModal(plugin_id, backup);
    readBackup(*modal);
    require(modal->m_paths.contains(path), "the backup names the path of its key");
    require(!modal->m_option_fields->isHidden() &&
                modal->m_airgap_text->text() == TR("restore-airgap-help"),
            "an xpub may open it: its xpub field and file are offered");
    require(deviceRows(*modal).isEmpty() &&
                !modal->m_device_rows->findChildren<catalog::EmptyState *>().isEmpty(),
            "no device connected says so");

    pasteXpub(*modal, QStringLiteral("tpubnotanxpub"));
    require(modal->m_xpub_note->m_text->text() == TR("restore-invalid-xpub"),
            "an invalid xpub is refused as such");
    pasteXpub(*modal, other_xpub);
    require(modal->m_xpub_note->m_text->text() == TR("restore-xpub-cannot-decrypt"),
            "an xpub of no key of the account cannot decrypt the backup");
    modal->m_mnemonic_ack->setChecked(true);
    require(modal->m_mnemonic_input->isEnabled(), "the mnemonic is typed once its risk is acked");
    modal->m_mnemonic_input->input()->setText(QStringLiteral("abandon abandon abandon"));
    modal->onMnemonicEdited();
    waitFor([&modal] { return !modal->m_mnemonic_note->isHidden(); }, "the mnemonic is tried");
    require(modal->m_mnemonic_note->m_text->text() == TR("restore-invalid-mnemonic"),
            "an invalid mnemonic is refused as such");

    FakeDeviceInstance stranger(QStringLiteral("fake.stranger"), QStringLiteral("deadbeef"),
                                {{path, other_xpub}});
    Host::get()->registerInstance(stranger.id(), &stranger, owner);
    waitFor(
        [&modal] {
            return deviceRows(*modal).contains(QStringLiteral("Jade #deadbeef ") +
                                               TR("restore-device-failed"));
        },
        "a device holding no key of the account is listed and fails to decrypt");
    require(stranger.device()->asked().contains(path), "the device is asked the backup's paths");

    FakeDeviceInstance holder(QStringLiteral("fake.holder"), QStringLiteral("73c5da0a"),
                              {{path, xpub}});
    Host::get()->registerInstance(holder.id(), &holder, owner);
    waitFor([&modal] { return modal->m_view == account::view::RestoreAccount::View::Review; },
            "the device holding the key opens the backup");
    require(holder.device()->asked() == modal->m_paths, "it is asked each path of the backup");
    require(!modal->m_review_opened->isHidden() &&
                modal->m_review_opened->m_text->text() ==
                    TR("restore-opened-device")
                        .arg(QStringLiteral("Jade"), QStringLiteral("#73c5da0a")),
            "the review says the device opened the backup");
    require(modal->m_xpubs == QStringList{xpub}, "its xpub is held for the restore");
    Host::get()->removeInstance(stranger.id(), owner);
    Host::get()->removeInstance(holder.id(), owner);
    QCoreApplication::processEvents();
}

// Keys holding `keys`, each a fingerprint and the sp() descriptor its key answers when asked
// for its silent payment account (a hot route), a turn of the event loop later.
class ScanKeys final : public DeferredKeys {
public:
    explicit ScanKeys(QMap<QString, QString> keys) : m_keys(std::move(keys)) {
    }

    [[nodiscard]] auto readKeys([[maybe_unused]] plugin::Network network) const
        -> KeyListResult override {
        KeyListResult result{.is_ok = true};
        for (const auto &fingerprint : m_keys.keys()) {
            result.entries.append({.fingerprint = fingerprint,
                                   .alias = QStringLiteral("key ") + fingerprint,
                                   .descriptor_route = plugin::KeyRouteKind::Hot,
                                   .route = plugin::KeyRouteKind::Hot});
        }
        return result;
    }

    void spDescriptor(ReqId req_id, [[maybe_unused]] plugin::Network network,
                      const QString &fingerprint) override {
        asked.append(fingerprint);
        m_pending.append({req_id, fingerprint});
        QTimer::singleShot(0, this, &ScanKeys::answer);
    }

    void answer() {
        auto [req, fingerprint] = m_pending.takeFirst();
        emit spDescriptorReady(req, m_keys.value(fingerprint), fingerprint);
    }

    QStringList asked;

private:
    QMap<QString, QString> m_keys;
    QList<QPair<ReqId, QString>> m_pending;
};

// The Decrypt view of the SP backup `backup`, keyed by the scan secret at the BIP352 scan
// path: a connected device is listed as giving no scan key and is asked nothing, the spend
// path's xpub (the key the SP address shows) is refused as no scan key. Of the keys of
// Keys, one holding another key's scan secret fails and the SP key opens it through its
// scan secret: the review says which key did.
void decryptWithScanKey(const QByteArray &backup, IModule *owner) {
    // Kept by the host until the end: owned by `owner`.
    auto *keys =
        new ScanKeys({{QStringLiteral("deadbeef"), QString::fromLatin1(OTHER_SP_DESCRIPTOR)},
                      {QStringLiteral("73c5da0a"), QString::fromLatin1(SP_DESCRIPTOR)}});
    keys->setParent(owner);
    Host::get()->setKeys(keys, owner);
    auto modal = restoreModal(QStringLiteral("sp"), backup, owner);
    readBackup(*modal);
    require(modal->m_paths == QStringList{SP_SCAN_PATH},
            "an SP backup names the BIP352 scan path alone");
    require(modal->m_airgap_text->text() == TR("restore-scan-key-help"),
            "an SP backup's help points to the key or its mnemonic, not to an xpub");

    FakeDeviceInstance usb(QStringLiteral("fake.usb"), QStringLiteral("73c5da0a"),
                           {{SP_SPEND_PATH, SP_XPUB}});
    Host::get()->registerInstance(usb.id(), &usb, owner);
    QCoreApplication::processEvents();
    require(modal->m_decrypt_text->isHidden() && modal->m_device_rows->isHidden() &&
                !modal->m_keys_text->isHidden() &&
                modal->m_keys_text->text() == TR("restore-keys-text"),
            "no device gives a scan key: the view lists none and leads with the keys of Keys");
    require(usb.device()->asked().isEmpty(), "the device is asked no xpub for a scan key");
    require(modal->m_option_fields->isHidden() && !modal->m_mnemonic_fields->isHidden() &&
                modal->m_airgap_text->text() == TR("restore-scan-key-help"),
            "no xpub opens it: its xpub field and file are left out, the mnemonic kept");

    require(rowTexts(modal->m_key_rows) ==
                QStringList{QStringLiteral("key 73c5da0a #73c5da0a ") + TR("restore-key-ask"),
                            QStringLiteral("key deadbeef #deadbeef ") + TR("restore-key-ask")},
            "the keys of Keys that can give a scan key are offered");
    auto clickKey = [&modal](const QString &fingerprint) {
        for (auto *row : modal->m_key_rows->findChildren<catalog::SelectRow *>()) {
            if (row->property("fingerprint").toString() == fingerprint) {
                emit row->clicked();
                return;
            }
        }
        qFatal("account shell: no key row %s", qPrintable(fingerprint));
    };
    clickKey(QStringLiteral("deadbeef"));
    waitFor(
        [&modal] {
            return rowTexts(modal->m_key_rows)
                .contains(QStringLiteral("key deadbeef #deadbeef ") + TR("restore-key-failed"));
        },
        "another key's scan secret fails to decrypt");
    clickKey(QStringLiteral("73c5da0a"));
    waitFor([&modal] { return modal->m_view == account::view::RestoreAccount::View::Review; },
            "the SP key's scan secret opens the backup");
    require(keys->asked == QStringList{QStringLiteral("deadbeef"), QStringLiteral("73c5da0a")},
            "each key is asked on its click only");
    require(modal->m_review_opened->m_text->text() ==
                TR("restore-opened-device")
                    .arg(QStringLiteral("key 73c5da0a"), QStringLiteral("#73c5da0a")),
            "the review says the key opened the backup");
    require(modal->m_xpubs == QStringList{QString::fromLatin1(SP_DESCRIPTOR)},
            "its answer is held for the restore");
    QStringList review;
    for (auto *label : modal->m_review_lists->findChildren<QLabel *>()) {
        review.append(label->text());
    }
    require(
        review.filter(QStringLiteral("holds no secret")).isEmpty() &&
            (review.contains(TR("backup-requires-sp-key")) ||
             !review.filter(QStringLiteral("holds the scan key, not the spending key")).isEmpty()),
        "an SP backup's review says it holds the scan key, never that it holds no secret");
    Host::get()->removeInstance(usb.id(), owner);
    QCoreApplication::processEvents();
}

// The restore of `backup`, the account `id`, opened with `key`: refused as another kind of
// account and while the account exists; restored once it is gone.
void restoreFlow(account::AccountModule *module, QWidget &window, const QString &plugin_id,
                 const QString &other_plugin, const QString &id, const QByteArray &backup,
                 const Key &key) {
    auto other = restoreModal(other_plugin, backup);
    readBackup(*other);
    openWith(*other, key);
    require(refused(*other, key), "another module's restore target refuses the backup");

    auto existing = restoreModal(plugin_id, backup);
    readBackup(*existing);
    openWith(*existing, key);
    require(existing->m_view == account::view::RestoreAccount::View::Review &&
                existing->m_name_input->text() == id &&
                existing->m_review_opened->m_text->text() == opener(key) &&
                existing->m_network_combo->isHidden(),
            "the backup opened with its key is reviewed, named after its account, on its own "
            "network");
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
    readBackup(*restore);
    openWith(*restore, key);
    restore->onNextClicked();
    waitFor([&] { return restore->m_view != account::view::RestoreAccount::View::Busy; },
            "the restore answers");
    require(restore->m_view == account::view::RestoreAccount::View::Restored &&
                restore->m_xpubs.isEmpty(),
            "the account is restored, the key dropped");
    require(restore->m_mnemonic.isEmpty(), "no mnemonic is held after the restore");
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
// account `id`, opened with `key`, and refuses `other`, a backup of another kind opened
// with `other_key`.
void restoresKind(account::view::RestoreAccount &modal, const QByteArray &own, const Key &key,
                  const QByteArray &other, const Key &other_key, const QString &id) {
    modal.m_backup_path = QStringLiteral("/backups/account.bip138");
    modal.m_backup = other;
    modal.showFile();
    readBackup(modal);
    openWith(modal, other_key);
    require(refused(modal, other_key),
            "the restore opened from the picker refuses another kind's backup");
    modal.onBackClicked();
    modal.m_backup = own;
    readBackup(modal);
    openWith(modal, key);
    require(modal.m_view == account::view::RestoreAccount::View::Review &&
                modal.m_name_input->text() == id,
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
                restoresKind(modal, sp_backup, {.mnemonic = SP_MNEMONIC}, multikey_backup,
                             {.xpub = MULTIKEY_XPUB}, SP_ACCOUNT);
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
                restoresKind(modal, multikey_backup, {.xpub = MULTIKEY_XPUB}, sp_backup,
                             {.mnemonic = SP_MNEMONIC}, MULTIKEY_ACCOUNT);
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
    backupTexts(spShell->services(), TR("backup-text-sp"), TR("settings-backup-text-sp"));
    backupTexts(multikeyShell->services(), TR("backup-text-multikey"),
                TR("settings-backup-text-multikey"));
    waitFor(
        [registry, spModule, multikeyCreator, createOnlyModule] {
            return !registry->launcherInstancesForModule(spModule).isEmpty() &&
                   !registry->launcherInstancesForModule(multikeyCreator).isEmpty() &&
                   !registry->launcherInstancesForModule(createOnlyModule).isEmpty();
        },
        "the launcher lists every module's entries");
    accountTypePicker(*qobject_cast<MenuTab *>(window.m_menu_tab), *createOnlyModule, spBackup,
                      multikeyBackup);
    decryptWithScanKey(spBackup, createOnlyModule);
    decryptWithDevice(QStringLiteral("musig"), multikeyBackup, MULTIKEY_PATH, MULTIKEY_XPUB,
                      SP_XPUB, createOnlyModule);
    restoreFlow(spModule, window, QStringLiteral("sp"), QStringLiteral("musig"), SP_ACCOUNT,
                spBackup, {.mnemonic = SP_MNEMONIC});
    restoreFlow(multikeyModule, window, QStringLiteral("musig"), QStringLiteral("sp"),
                MULTIKEY_ACCOUNT, multikeyBackup, {.xpub = MULTIKEY_XPUB});
    controller->start(nullptr);
}

} // namespace

void accountShellSmoke() {
    backupModal();
    builtinBackups();
}

} // namespace smoke

#include "account_shell_smoke.moc"
