#pragma once

#include <memory>
#include <optional>

#include <QHash>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <interfaces/keys.h>
#include <interfaces/module.h>
#include <interfaces/types.h>
#include <silent.h>

class HwiModule;
class QThread;
class QWidget;

namespace keys {

// The CXX bridge exports its opaque handle as ::KeysModule, so the C++ module lives in
// this namespace and names the Rust side through this alias.
using RustKeys = ::KeysModule;

class KeysModule;
class KeysTab;
class RouteSigner;

// IModule and IKeys are both QObjects, so one class cannot be both. KeysModule owns
// this bus, registers it with the host, and answers every call through it.
class KeysBus final : public IKeys {
    Q_OBJECT

public:
    explicit KeysBus(KeysModule *module, QObject *parent = nullptr);
    ~KeysBus() override = default;

    [[nodiscard]] QList<KeyEntry> keys(Network network) const override;
    auto newRequest() -> ReqId override;
    void sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors) override;
    void verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                       const QString &address) override;
    void spDescriptor(ReqId req_id, const QString &fingerprint) override;
    void registerDescriptor(ReqId req_id, const QString &fingerprint,
                            const QString &descriptor) override;

    // Called by the module, which owns every outstanding request.
    void deliverSigned(ReqId req_id, const QString &psbt_v2_hex);
    void deliverVerified(ReqId req_id, const QString &address);
    void deliverError(std::optional<ReqId> req_id, const QString &message);

private:
    KeysModule *m_module = nullptr;
};

// Runs the keys module's blocking notification receiver on a background thread and
// feeds it the live device list every time the hwi module's device set changes.
class KeysModule final : public IModule, public IPlugin {
    Q_OBJECT
    Q_INTERFACES(IPlugin)

public:
    explicit KeysModule(QObject *parent = nullptr);
    ~KeysModule() override;

    [[nodiscard]] QMap<QString, QString> meta() const override;
    [[nodiscard]] QList<IModule *> modules() const override;
    ReqId list() override;
    ReqId autoStart() override;
    void startInstance(const QString &id) override;

    [[nodiscard]] static Network network();
    void setNetwork(Network network);

    [[nodiscard]] QList<KeyInfo> keys() const;
    [[nodiscard]] bool hasKey(const QString &fingerprint) const;
    // Shared by RouteSigner and OfflineSigner's info() answer: the number of a key's
    // descriptors the Rust side has registered, or 0 while the handle isn't built yet.
    [[nodiscard]] auto countRegisteredDescriptors(const QString &fingerprint) const -> int;
    // Picks the signer that serves `info`'s best usable route (a live device, a
    // module-supplied route signer, or the offline stand-in) and wraps it as a
    // KeyEntry. Called by KeysBus for every KeyInfo it hands out.
    [[nodiscard]] auto buildKeyEntry(const KeyInfo &info) -> KeyEntry;
    // Key record mutators. Each forwards to the Rust handle and, on success, emits
    // keysChanged() so the tab rebuilds; modals call these instead of rust() directly.
    [[nodiscard]] auto setAlias(const QString &fingerprint, const QString &alias) -> KeyOpResult;
    [[nodiscard]] auto addRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult;
    [[nodiscard]] auto removeRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult;
    [[nodiscard]] auto setRouteOrder(const QString &fingerprint, rust::Vec<KeyRouteKind> order)
        -> KeyOpResult;
    [[nodiscard]] auto removeKey(const QString &fingerprint, bool force) -> KeyOpResult;
    // The Rust handle, for the tab and the modals of this module only. Null until the
    // handle is built, so every caller checks.
    [[nodiscard]] RustKeys *rust() const;
    // The hwi module, once one has been seen on the bus. Null before that.
    [[nodiscard]] HwiModule *hwi() const;
    [[nodiscard]] KeysBus *bus() const;
    ReqId nextReqId();
    // Registers/clears an in-flight request on a module-supplied signer, keyed by
    // its own req_id, so routeSignerFor() defers rebuilding the signer until the
    // request completes. Called by RouteSigner around its interactive/background
    // flows (hot sign, QR, SD card).
    void trackPendingRequest(ReqId req_id, ISigner *signer);
    void untrackPendingRequest(ReqId req_id);
    // Owns the hot-sign password prompt modal so no signer builds one itself. Answers
    // through `requester->onPasswordResolved(req_id, password)`, with an empty password
    // meaning the user cancelled the modal.
    void promptPassword(RouteSigner *requester, ReqId req_id);
    // The Keys tab widget. Built on first call and reused afterwards; a QPointer
    // leaves this null again if the host that owns the widget deletes it.
    [[nodiscard]] auto tab() -> QWidget *;

    // Worker jobs owned by this module, so a thread never outlives the modal or
    // signer that asked for it. Each starts a QThread::create lambda that captures
    // only copied data plus this module's `this`, and answers through the matching
    // result signal below, matched by the caller-allocated `req_id`.
    void generateMnemonic(ReqId req_id, std::uint32_t word_count);
    // The GUI slot always calls commit_hot_key(), even if every caller (HotKey) has
    // already closed: a close during sealing still finishes the import.
    void importHotKey(ReqId req_id, QString mnemonic, QString alias, QString password);
    void revealMnemonic(ReqId req_id, QString fingerprint, QString password);
    void signHot(ReqId req_id, QString fingerprint, QString password, QString psbt_v2_hex);

    [[nodiscard]] static auto toQt(const rust::String &value) -> QString;

signals:
    // Emitted from the receiver thread, handled on the GUI thread.
    void keyEventReceived(KeyEvent event);
    // The key records changed; the tab rebuilds on it.
    void keysChanged();
    // The live device set changed and the Rust handle already has it.
    void liveDevicesChanged();

    // Worker job results. Each pair below is: emitted from the background thread,
    // re-owned by the matching private slot, which re-emits the second (public)
    // signal that callers connect to.
    void mnemonicGenerationFinished(ReqId req_id, MnemonicResult result);
    void mnemonicGenerated(ReqId req_id, MnemonicResult result);
    // The sealed key's raw pointer is re-owned by onHotKeySealed(), along with the
    // alias importHotKey() was called with, so the commit still has it once the
    // worker answers.
    void hotKeySealed(ReqId req_id, SealedHotKey *sealed, QString alias);
    void hotKeyImported(ReqId req_id, KeyInfoResult result);
    void mnemonicRevealFinished(ReqId req_id, MnemonicResult result);
    void mnemonicRevealed(ReqId req_id, MnemonicResult result);
    void hotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error);
    void hotSigned(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error);

public slots:
    void onRouteSignRequested(ReqId req_id, const QString &fingerprint, const QString &psbt_v2_hex,
                              const QString &descriptor);
    void onRouteDisplayRequested(ReqId req_id, const QString &fingerprint,
                                 const QString &descriptor, const QString &path,
                                 const QString &address);
    void onSignerPicked(QString fingerprint);

private slots:
    void onKeyEventReceived(const KeyEvent &event);
    void onInstanceRegistered(const QString &id);
    void onInstanceRemoved(const QString &id);
    void onDevicesChanged();
    void onPasswordPromptEntered(QString password);
    void onPasswordPromptRejected();
    void onMnemonicGenerationFinished(ReqId req_id, MnemonicResult result);
    void onHotKeySealed(ReqId req_id, SealedHotKey *sealed, QString alias);
    void onMnemonicRevealFinished(ReqId req_id, MnemonicResult result);
    void onHotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error);

protected:
    void createModule();
    void teardownModule();
    void refreshLiveDevices();
    void bindHwiModule();
    // buildKeyEntry() helpers: pick a live device signer for a Usb route, or build
    // and cache a module-supplied one for any other usable route.
    [[nodiscard]] static auto findLiveDeviceSigner(const QString &fingerprint) -> ISigner *;
    [[nodiscard]] auto routeSignerFor(const QString &fingerprint, const KeyInfo &info,
                                      KeyRouteKind route) -> ISigner *;
    [[nodiscard]] auto offlineSignerFor(const QString &fingerprint, const KeyInfo &info)
        -> ISigner *;
    // Whether `signer` has a request in flight through the module (the interactive
    // route flows), so a rebuild never deletes an instance out from under a caller.
    [[nodiscard]] auto hasPendingRequests(const ISigner *signer) const -> bool;
    // Shows the PSBT-driven signer picker and answers with the fingerprint the user
    // picked, or an empty string on cancel. Reads the answer back through
    // onSignerPicked(), never off the modal after execModal() returns.
    auto showSignerPicker(const QString &title, const SigningPlan &plan) -> QString;
    // Builds the signer picker plan for `psbt_v2_hex`. Shows an error modal and
    // still returns the (is_ok false) plan on failure, never a fabricated one.
    [[nodiscard]] auto signingPlanFor(const QString &psbt_v2_hex) const -> SigningPlan;
    // Translates a RouteSigner/device signer's own ReqId back to the caller's
    // request id via m_signer_to_request, removing the entry. Falls back to
    // `signer_req_id` unchanged when no mapping was recorded: some requests
    // (spDescriptor) never allocate a separate signer id.
    [[nodiscard]] auto requestIdFor(ReqId signer_req_id) -> ReqId;

private:
    // A hot-sign password prompt in flight, keyed by the prompt instance rather than a
    // scalar field: a second promptPassword() call may open a second prompt before the
    // first resolves.
    struct PasswordRequest {
        RouteSigner *requester = nullptr;
        ReqId req_id = 0;
    };

    // Box's constness gates the pointee's constness through operator->(), so this is
    // mutable to let the const rust() accessor hand callers a mutable handle.
    mutable std::optional<rust::Box<RustKeys>> m_rust;
    QThread *m_notif_thread = nullptr;
    std::unique_ptr<KeysBus> m_bus;
    HwiModule *m_hwi = nullptr;
    QPointer<KeysTab> m_tab;
    ReqId m_next_req_id = 0;
    // The network the Rust manager was last synced to. Guards setNetwork() against its
    // own state rather than ::hwi_config_network(), which HwiModule::setNetwork also
    // writes and may already reflect the incoming value by the time this runs.
    Network m_synced_network = KeysModule::network();
    // Module-supplied signers (hot, QR, SD card), parented to this module and
    // deleted with it. Reused while a fingerprint's chosen route stays the same;
    // replaced, never mutated, when it changes.
    QHash<QString, ISigner *> m_signers;
    // Requests currently in flight on a module-supplied signer, through the
    // interactive route flows this module drives.
    QHash<ReqId, ISigner *> m_pending_requests;
    // Maps a RouteSigner or device ISigner's own ReqId (returned by
    // signWithDescriptor()/displayAddress()) back to the caller-allocated request
    // id, so an answer arriving with the signer's id can be delivered under the
    // id the caller stored. Populated by sign()/verifyAddress() and by
    // signWithDevice()/verifyWithDevice(); consumed by requestIdFor().
    QHash<ReqId, ReqId> m_signer_to_request;
    // Password prompts currently open, keyed by the modal instance.
    QHash<QObject *, PasswordRequest> m_password_requests;
    // The fingerprint picked in the signer picker last shown, set by onSignerPicked()
    // while showSignerPicker()'s execModal() call is still on the stack.
    QString m_picked_fingerprint;
};

} // namespace keys
