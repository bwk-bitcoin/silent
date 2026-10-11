#pragma once

#include <cstdint>
#include <map>
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
class ISigner;
class QThread;
class QWidget;

namespace keys::flow {
class SigningFlow;
} // namespace keys::flow

namespace qr {
struct QrDevice;
} // namespace qr

namespace keys {

// The CXX bridge exports its opaque handle as ::KeysModule, so the C++ module lives in
// this namespace and names the Rust side through this alias.
using RustKeys = ::KeysModule;

class KeysModule;
class KeysTab;

// An xpub and the path it was derived at, as a key caches it.
struct PathXpub {
    QString path;
    QString xpub;
};

// IModule and IKeys are both QObjects, so one class cannot be both. KeysModule owns
// this bus, registers it with the host, and answers every call through it.
class KeysBus final : public IKeys {
    Q_OBJECT

public:
    explicit KeysBus(KeysModule *module, QObject *parent = nullptr);
    ~KeysBus() override = default;

    [[nodiscard]] QList<KeyEntry> keys(Network network) const override;
    auto newRequest() -> ReqId override;
    void sign(ReqId req_id, Network network, const QString &psbt_v2_hex,
              const QStringList &descriptors, const QString &wallet_name) override;
    void verifyAddress(ReqId req_id, Network network, const QString &descriptor,
                       const QString &path, const QString &address) override;
    void spDescriptor(ReqId req_id, Network network, const QString &fingerprint) override;
    void xpub(ReqId req_id, Network network, const QString &fingerprint,
              const QString &path) override;
    void registerDescriptor(ReqId req_id, Network network, const QString &fingerprint,
                            const QString &descriptor) override;

public slots:
    // Called by the module and its signing flows, which own every outstanding request.
    void deliverSigned(ReqId req_id, const QString &psbt_v2_hex);
    // `matched` is the device's own answer; false is a refusal, not a failure.
    void deliverVerified(ReqId req_id, bool matched);
    void deliverError(std::optional<ReqId> req_id, const QString &message);
    void deliverCancelled(ReqId req_id);
    void deliverSpDescriptor(ReqId req_id, const QString &descriptor, const QString &fingerprint);
    void deliverXpub(ReqId req_id, const QString &xpub, const QString &fingerprint,
                     const QString &path);
    void deliverDescriptorRegistered(ReqId req_id, const QString &fingerprint);

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

    // The keys of the active network: a request's while it runs, else the tab's.
    [[nodiscard]] QList<KeyInfo> keys() const;
    // The keys of the Keys tab's network, whatever request runs.
    [[nodiscard]] QList<KeyInfo> tabKeys() const;
    [[nodiscard]] bool hasKey(const QString &fingerprint) const;
    // Shared by RouteSigner and OfflineSigner's info() answer: the number of a key's
    // descriptors the Rust side has registered, or 0 while the handle isn't built yet.
    [[nodiscard]] auto countRegisteredDescriptors(const QString &fingerprint) const -> int;
    // Picks the signer that serves `info`'s best usable route (a live device, a
    // module-supplied route signer, or the offline stand-in) and wraps it as a
    // KeyEntry. Called by KeysBus for every KeyInfo it hands out.
    [[nodiscard]] auto buildKeyEntry(const KeyInfo &info) -> KeyEntry;
    // The route a key hands its sp() descriptor over: its first QR or usable hot
    // route, Unknown when it has none.
    [[nodiscard]] static auto descriptorRoute(const KeyInfo &info) -> KeyRouteKind;
    // The route a key hands an xpub over: its connected USB device first, then its QR
    // or usable hot route, Unknown when it has none.
    [[nodiscard]] static auto xpubRoute(const KeyInfo &info) -> KeyRouteKind;
    // Key record mutators. Each forwards to the Rust handle and, on success, emits
    // keysChanged() so the tab rebuilds; modals call these instead of rust() directly.
    [[nodiscard]] auto setAlias(const QString &fingerprint, const QString &alias) -> KeyOpResult;
    [[nodiscard]] auto addRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult;
    [[nodiscard]] auto removeRoute(const QString &fingerprint, KeyRouteKind kind) -> KeyOpResult;
    [[nodiscard]] auto setRouteOrder(const QString &fingerprint, rust::Vec<KeyRouteKind> order)
        -> KeyOpResult;
    [[nodiscard]] auto removeKey(const QString &fingerprint, bool force) -> KeyOpResult;
    // Registers a key with its routes and its xpubs, or nothing at all.
    [[nodiscard]] auto addKey(const QString &fingerprint, const QString &alias,
                              const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs)
        -> KeyOpResult;
    // Registers the key of a ready device: the key with `alias`, its USB route, the
    // xpub the device gave at `path` and the device itself, or none of them.
    [[nodiscard]] auto addDeviceKey(const plugin::SignerEntry &entry, const QString &alias,
                                    const QString &path, const QString &xpub) -> KeyOpResult;
    [[nodiscard]] auto cacheXpub(const QString &fingerprint, const QString &path,
                                 const QString &xpub) -> KeyOpResult;
    [[nodiscard]] auto removeXpub(const QString &fingerprint, const QString &path) -> KeyOpResult;
    // Remembers `entry`'s device for the key holding its fingerprint.
    [[nodiscard]] auto recordDevice(const plugin::SignerEntry &entry) -> KeyOpResult;
    // Remembers the device that handed the key holding `fingerprint` over QR.
    [[nodiscard]] auto recordQrDevice(const QString &fingerprint, const qr::QrDevice &device)
        -> KeyOpResult;
    // The path a device exports an xpub at without asking: BIP84, account 0, the coin
    // type of the network.
    [[nodiscard]] static auto defaultXpubPath() -> QString;
    // The paths a key added over QR has its xpubs fetched at: account 0 of BIP84,
    // BIP86, BIP48 P2WSH and BIP87, at the coin type of the network.
    [[nodiscard]] static auto accountXpubPaths() -> QStringList;
    // 0 on Bitcoin, 1 on every test network.
    [[nodiscard]] static auto coinType() -> int;
    // The Rust handle, for the tab and the modals of this module only. Null until the
    // handle is built, so every caller checks.
    [[nodiscard]] RustKeys *rust() const;
    // The hwi module, once one has been seen on the bus. Null before that.
    [[nodiscard]] HwiModule *hwi() const;
    [[nodiscard]] KeysBus *bus() const;
    ReqId nextReqId();
    // The Keys tab widget. Built on first call and reused afterwards; a QPointer
    // leaves this null again if the host that owns the widget deletes it.
    [[nodiscard]] auto tab() -> QWidget *;
    // Each request below opens one signing flow modal and returns once it has closed;
    // the flow answers `req_id` through the bus exactly once.
    // Signs `psbt_v2_hex` with every key it needs, one after the other.
    void sign(ReqId req_id, const QString &psbt_v2_hex, const QStringList &descriptors,
              const QString &wallet_name);
    // Shows `address` on a device of a key `descriptor` is registered against.
    void verifyAddress(ReqId req_id, const QString &descriptor, const QString &path,
                       const QString &address);
    // Fetches the sp() descriptor of `fingerprint`'s key over its QR or hot route.
    void spDescriptor(ReqId req_id, const QString &fingerprint);
    // The xpub at `path` of `fingerprint`'s key: from its cache when it holds one,
    // else over its route (see xpubRoute()), then cached.
    void fetchXpub(ReqId req_id, const QString &fingerprint, const QString &path);
    // Records `descriptor` as registered with `fingerprint`'s key and answers
    // `req_id` through the bus's descriptorRegistered or error signal.
    void registerDescriptor(ReqId req_id, const QString &fingerprint, const QString &descriptor);

    // Worker jobs owned by this module, so a thread never outlives the modal that asked
    // for it. Each starts a QThread::create lambda that captures only copied data plus
    // this module's `this`, and answers through the matching result signal below,
    // matched by the caller-allocated `req_id`.
    void generateMnemonic(ReqId req_id, std::uint32_t word_count);
    // The GUI slot always calls commit_hot_key(), even if every caller (HotKey) has
    // already closed: a close during sealing still finishes the import.
    void importHotKey(ReqId req_id, const QString &mnemonic, const QString &alias,
                      const QString &password);
    void revealMnemonic(ReqId req_id, const QString &fingerprint, const QString &password);
    void signHot(ReqId req_id, const QString &fingerprint, const QString &password,
                 const QString &psbt_v2_hex);
    void fetchHotSpDescriptor(ReqId req_id, const QString &fingerprint, const QString &password);
    void fetchHotXpub(ReqId req_id, const QString &fingerprint, const QString &password,
                      const QString &path);
    void saveSdPsbt(ReqId req_id, const QString &path, const QString &psbt_v2_hex);
    void loadSdPsbt(ReqId req_id, const QString &path);

    [[nodiscard]] static auto toQt(const rust::String &value) -> QString;

signals:
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
    void hotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error,
                         bool wrong_password);
    void hotSigned(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error,
                   bool wrong_password);
    void hotSpDescriptorFinished(ReqId req_id, bool is_ok, QString descriptor, QString error,
                                 bool wrong_password);
    void hotSpDescriptorFetched(ReqId req_id, bool is_ok, QString descriptor, QString error,
                                bool wrong_password);
    void hotXpubFinished(ReqId req_id, bool is_ok, QString xpub, QString error,
                         bool wrong_password);
    void hotXpubFetched(ReqId req_id, bool is_ok, QString xpub, QString error, bool wrong_password);
    // Emitted from the worker thread; queued to the GUI thread receivers.
    void sdSaved(ReqId req_id, bool is_ok, QString error);
    void sdLoaded(ReqId req_id, bool is_ok, QString psbt_hex, QString error);

private slots:
    void onInstanceRegistered(const QString &id);
    void onInstanceRemoved(const QString &id);
    void onDevicesChanged();
    void onMnemonicGenerationFinished(ReqId req_id, const MnemonicResult &result);
    void onHotKeySealed(ReqId req_id, SealedHotKey *sealed, const QString &alias);
    void onMnemonicRevealFinished(ReqId req_id, const MnemonicResult &result);
    void onHotSignFinished(ReqId req_id, bool is_ok, QString psbt_v2_hex, QString error,
                           bool wrong_password);
    void onHotSpDescriptorFinished(ReqId req_id, bool is_ok, QString descriptor, QString error,
                                   bool wrong_password);
    void onHotXpubFinished(ReqId req_id, bool is_ok, QString xpub, QString error,
                           bool wrong_password);
    // A flow fetched an xpub: cached on its key, then handed to the caller.
    void onFlowXpubFetched(ReqId req_id, const QString &xpub, const QString &fingerprint,
                           const QString &path);

protected:
    void createModule();
    void teardownModule();
    void refreshLiveDevices();
    void bindHwiModule();
    [[nodiscard]] static auto toLiveDevice(const plugin::SignerEntry &entry) -> LiveDevice;
    // Adds a key with its routes and its xpubs, or nothing at all, without
    // keysChanged(): the caller changes the new key further first.
    [[nodiscard]] auto insertKey(const QString &fingerprint, const QString &alias,
                                 const QList<KeyRouteKind> &routes, const QList<PathXpub> &xpubs)
        -> KeyOpResult;
    // Opens `flow`, wired to answer through the bus, and returns once it has closed.
    void runFlow(flow::SigningFlow *flow);
    // buildKeyEntry() helpers: pick a live device signer for a Usb route, or build
    // and cache a module-supplied one for any other usable route.
    [[nodiscard]] static auto findLiveDeviceSigner(const QString &fingerprint) -> ISigner *;
    [[nodiscard]] auto routeSignerFor(const QString &fingerprint, const KeyInfo &info,
                                      KeyRouteKind route) -> ISigner *;
    [[nodiscard]] auto offlineSignerFor(const QString &fingerprint, const KeyInfo &info)
        -> ISigner *;
    // A cached signer's key: the fingerprint on the active network.
    [[nodiscard]] auto signerKey(const QString &fingerprint) const -> QString;

private:
    friend class NetworkScope;

    // The network rust() and keys() answer for: a request's while it runs (see
    // NetworkScope), the Keys tab's otherwise.
    [[nodiscard]] auto activeNetwork() const -> Network;
    // The handle of `network`, opened on first use and fed the live devices.
    [[nodiscard]] auto handle(Network network) const -> RustKeys *;
    static void pushLiveDevices(RustKeys &handle);

    // One handle per network, so a request for an account on any network finds its
    // keys whatever network the Keys tab shows. Box's constness gates the pointee's
    // constness through operator->(), so this is mutable to let the const rust()
    // accessor hand callers a mutable handle.
    mutable std::map<Network, rust::Box<RustKeys>> m_handles;
    // Set by NetworkScope while a request for that network runs.
    std::optional<Network> m_scope;
    std::unique_ptr<KeysBus> m_bus;
    HwiModule *m_hwi = nullptr;
    QPointer<KeysTab> m_tab;
    ReqId m_next_req_id = 0;
    // The network the Rust manager holds. network() answers from it rather than from
    // ::hwi_config_network(), which is read from disk and can drift from the manager
    // (a failed config write, another process), so a sealed key always matches.
    static Network s_network; // NOLINT(readability-identifier-naming)
    // Module-supplied signers (hot, QR, SD card), parented to this module and
    // deleted with it, by network and fingerprint (one seed has the same fingerprint
    // on every network). Reused while a key's chosen route stays the same; replaced,
    // never mutated, when it changes.
    QHash<QString, ISigner *> m_signers;
};

// Makes `network` the keys module's network while it lives: rust(), keys() and the
// flows a request opens then answer for it, not for the Keys tab's. Every request of
// the bus runs in one.
class NetworkScope {
public:
    NetworkScope(KeysModule *module, Network network);
    ~NetworkScope();
    NetworkScope(const NetworkScope &) = delete;
    NetworkScope(NetworkScope &&) = delete;
    auto operator=(const NetworkScope &) -> NetworkScope & = delete;
    auto operator=(NetworkScope &&) -> NetworkScope & = delete;

private:
    KeysModule *m_module = nullptr;
    std::optional<Network> m_previous;
};

} // namespace keys
