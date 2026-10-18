# Silent Plugin System

Silent is extensible through plugins. External logic (new account kinds, data
feeds, extra tabs, settings sections, themes) can be loaded at runtime from a
plugins folder without rebuilding the app.

This document describes the architecture and the contract a plugin author works
against. It is the design reference; implementation lands incrementally.

## Core Model: Plugin, Module, Instance, Capabilities

There is **no taxonomy of module "types".** There are three levels and a single
invariant:

- **Plugin**: the loaded **library** (`.so`/`.dll`/`.dylib`) plus its sidecar
  metadata. A plugin **contains one or more modules** and exposes them to the host
  (`modules() -> [IModule*]`). It is the unit of distribution, discovery, and
  enable/disable; it is not itself a module.
- **Module** (`IModule`): a unit of functionality inside a plugin. It owns its
  instances and their IDs: `list()` asks for the instances it **can start** as
  `(id, name)` pairs (the launcher renders one button per entry, labelled by name),
  and `startInstance(id)` brings one up. The module manages any persistence; the host
  does not own instance state.
- **Instance** (`IInstance`): a configured, running entity created from a module.
  **The invariant: every instance is an `IInstance`.** An `IInstance` exposes the
  **data** capabilities (`IAccount`, `IFeed`, `ISigner`, `IThemeProvider`) as
  sub-objects; which ones a given instance actually offers is decided **per
  capability** by `implemented()`.

`implemented()` is a **hardcoded, compile-time constant** (`const`): a capability is
either built into this instance or it is not. If `implemented()` is `false` the
capability is **inert**: its methods do nothing and it **never emits a result
signal**. Discovery is therefore just "check `implemented()`": no registrar, no
`qobject_cast` probing.

**UI is not a capability; it is pushed.** An instance does not expose tab/settings/
wizard interfaces for the host to pull. Instead the instance **calls the host** to
register a widget and gets back an id it later uses to update or remove it:
`openTab(widget, this) -> TabId`, `addSettingSection(widget, this) -> SectionId`,
`openWizard(widget, this) -> WizardId` (with `close`/`del` counterparts). A headless
instance (e.g. a feed) simply never calls them.

Cardinality follows from the invariant: each data capability appears **once per
instance**. More of something means more instances. A wallet module with three
accounts is **three instances**, each an `IInstance` whose account capability is
`implemented()`. A price-feed instance has its feed `implemented()` and pushes no UI.

| Data capability  | What it provides                                       |
|------------------|-------------------------------------------------------|
| `IAccount`       | An account: coins, addresses, balance                 |
| `IFeed`          | Pushed data: fee rate, conversion rate, raw bytes     |
| `ISigner`        | Full interface to a signing device                    |
| `IThemeProvider` | Named color palettes selectable in Settings           |

An "account type" (SP, miniscript, Liana, bark) is a module whose instances have the
account capability implemented and that push a tab (and usually a create wizard);
signing is a **separate** capability (`ISigner`), not part of the account. A
price-feed module's instances have the feed capability implemented and push no UI. An
aggregated-graph instance has no data capability implemented, pushes a tab, and
*consumes* other instances' accounts + feeds. Nothing is special-cased.

```
   app start: the launcher asks every enabled module what it can start:
             module->list()  -> instances(req, [(id, name)])  // startable, not live
             launcher renders one button per (id, name)

   user clicks a button:
             module->startInstance(id)
             module constructs the IInstance and calls
                 host->registerInstance(id, instance, module)
             the instance then PUSHES any UI it wants:
                 host->openTab(widget, instance)            -> TabId
                 host->addSettingSection(widget, instance)  -> SectionId
                 host->openWizard(widget, instance)         -> WizardId
             and exposes data via its capabilities, gated by implemented():
                 instance->account()->implemented()  -> true / false
                 (false => inert: no-op, never emits a result signal)
```

> A **plugin** is one delivery vehicle for modules; the modules it contains
> implement `IModule` / `IInstance` exactly like built-in ones.

## Built-in vs External Modules

Modules reach the app two ways; the module/instance/capability contract is identical
either way.

- **Built-in**: shipped in mainline Silent, **statically linked** into the binary
  (e.g. the Silent Payments module). Trusted, always enabled. Registered via
  `Q_IMPORT_PLUGIN`.
- **External (a plugin)**: a **library + metadata** loaded at runtime from
  `~/.silent/plugins/`. One plugin **contains one or more modules**. Disabled by
  default; the user enables the plugin, which brings up all of its modules.

## Distribution Unit: Library + Metadata

Every external plugin ships as **two files**:

```
~/.silent/plugins/
  myplugin/
    libmyplugin.so          # the library
    myplugin.json           # sidecar metadata (id, name, version, capabilities...)
    i18n/
      silent_en.lang        # optional translations (see below)
      silent_fr.lang
```

### Out-of-tree Plugin Repo Pattern

Plugin projects are expected to live in their own repositories and consume Silent as
a git submodule.

Recommended layout:

```text
my-plugin-repo/
  CMakeLists.txt
  plugins/
    myplugin/
      CMakeLists.txt
      MyPlugin.cpp
      myplugin.json
      i18n/
        silent_en.lang
  third_party/
    silent/                 # git submodule
```

The plugin build includes headers from:

```text
third_party/silent/src
```

Reference implementation (pin to a tag/commit when stabilized):

```text
https://github.com/pythcoiner/silent-plugin
```

#### External plugin repo layout

An out-of-tree plugin repo follows this baseline:

- Put plugin source under `src/` (or `plugins/<id>/` as shown above).
- Vendor Silent as a git **submodule** (not a symlink); the layout above uses
  `third_party/silent`, a plain `lib/silent` submodule works the same way.
- Consume the interface headers from the submodule, e.g.
  `third_party/silent/src/interfaces/*`.
- i18n: author `.lang` sources; do not treat JSON as the source of truth. When JSON
  is needed it is generated at build / pre-compile time. See [I18N.md](I18N.md).
- Build: provide a reproducible Nix build target whose naming aligns with Silent's.

The host **reads the sidecar metadata before the library is ever loaded.** This
read-before-load ordering is the load-time boundary: a disabled plugin is
*discovered* (its metadata is listed in Settings) but its library is **never
`dlopen`'d** and **no plugin code runs** until the user enables it.

> The metadata format is defined now and is forward-compatible. How metadata may
> later be *used for trust* (hashing the library, verifying a signature against a
> trusted key set) is intentionally out of scope for the initial implementation.
> Only the `library + metadata` structure and the read-first ordering are fixed.

## Lifecycle: Discover, Enable, Activate

1. **Discover.** On startup the host scans `~/.silent/plugins/`, reads each sidecar
   metadata file, and lists every plugin in global Settings. Newly discovered
   plugins are **disabled by default**. Discovery is metadata-only (no `dlopen`),
   so it is cheap and side-effect free. A **Rescan** button in global Settings
   re-runs it on demand: a plugin dropped into the folder while the app is running
   shows up without a restart. Rescan only updates the discovered list; it never
   loads or unloads code or changes the enabled state of plugins already present.
2. **Enable** (user action in Settings). The host `dlopen`s the library
   (`QPluginLoader::load()`) and asks the plugin for its modules
   (`modules() -> [IModule*]`). For each module the host calls `autoStart()`
   (starting those instances at once) and `list()` (the launcher renders a button per
   entry for the rest); see "Instance lifecycle". This happens **live, in-session,
   with no restart**: launcher buttons, tabs, settings sections, and themes appear
   immediately.
3. **Disable.** The host stops every instance of every module in the plugin (each
   removes its pushed UI, stops its thread, withdraws its capabilities from
   discovery) then unloads the library.

### Instance lifecycle

The **module owns its instances and their IDs.** It is free to persist them however
it likes (the SP module, for example, keeps `~/.silent/<id>/config.json`); the host
keeps no instance state. The module exposes:

- `list()`: asks for the instances this module **can start**, delivered as
  `(id, name)` pairs on the `instances(ReqId, ...)` signal. These are not live
  instances; the launcher renders one button per entry, labelled by name.
- `autoStart()`: asks for the ids the host must start **immediately after the module
  loads**, delivered on the `autoStart(ReqId, ...)` signal (e.g. feeds, signing-device
  modules).
- `startInstance(id)`: bring an instance up. The module constructs the `IInstance`
  (on its own thread), calls `host->registerInstance(id, instance, this)`, and the instance
  then pushes any UI it wants and exposes its `implemented()` capabilities. The host
  starts launcher/auto-start instances through the registry so each is **attributed to
  its owning plugin**; that ownership is what lets the host tear them down cleanly on
  disable.
- `deleteInstance(id)`: drop an instance's **persisted state**. The host removes the
  live instance and its pushed UI itself, then calls this so the module deletes its
  own on-disk state (the module owns persistence). Default is a no-op for modules that
  persist nothing. The launcher offers a delete (trash) affordance for a module's
  startable instances; confirming it routes here.

A module is **not limited to the host-driven path**: it may call
`host->registerInstance(id, instance, this)` and `host->removeInstance(id, this)` at any
time, for example one instance per connected device, registered on plug and dropped on
unplug. Both calls are ownership-checked, so a module only ever drops its own instances,
and an instance can retire itself with `host->retireInstance(this)`. Dropping an instance
stops it and closes its pushed UI but **never touches persisted state**; `deleteInstance`
stays the only thing that does. The module frees its own `IInstance` from its
`instanceRemoved` handler.

(`meta()` is `IModule`'s only synchronous method: a hardcoded `const`, like a
capability's `implemented()`. Everything else on a module is a request, `ReqId` +
signal. On `IPlugin`, `meta()` and `modules()` are likewise synchronous `const`
accessors.)

An instance is brought up by one of:

- **Auto-start on load**: right after a module loads, the host calls `autoStart()`
  and starts every id it returns. Used for headless modules (feeds, signing devices)
  that should come up without a user click.
- **The launcher**: the user clicks a button from the `instances` list, which calls
  `startInstance(id)`. Creating a *new* instance is the same path: the module offers a
  "create" entry in that list; clicking it starts an instance that pushes a create
  wizard (`host->openWizard(...)`), persisting itself on finish, discarding on cancel.
  Once the new instance is on disk the wizard calls `Host::get()->refreshLauncher()`:
  the host asks every enabled module to `list()` again and the launcher shows the new
  entry. The new instance is not started; the user opens it from the launcher.
  The launcher lists every "create" entry in its Account Type picker, one row each with
  the icon its module names in `meta()` under `icon` (`silent`, `users`), a puzzle
  piece when it names none.
- **Another instance**: an instance may ask a module to start further instances.

Because instances appear and disappear at runtime, **discovery is dynamic**:
consumers must not cache the account/feed/signer set at startup. Subscribe to the
host's events (below) and re-query when they fire.

## The Host

The host is a **singleton**, reached via `Host::get()`. It is the single
read/discovery point into the rest of the app: a module registers its instances,
anyone enumerates the live instances, an instance pushes UI, and code reaches
translations and events. Nothing is passed an `IHost*`; code calls `Host::get()`
directly.

```cpp
class Host {
public:
    static Host *get();                       // singleton accessor

    // a module calls this when it instantiates an instance, so the host can
    // track it. The module owns the id; the host records the owning module.
    void registerInstance(const QString &id, IInstance *instance, IModule *owner);

    // teardown, both ownership-checked: a module drops one of its own instances, or
    // an instance retires itself. Either way the instance is stopped, its pushed UI
    // closed, and instanceRemoved fired. Persisted state is untouched.
    void removeInstance(const QString &id, IModule *owner);
    void retireInstance(IInstance *self);

    // discovery: the live instances. A consumer gates each capability on its
    // implemented() (e.g. inst->account()->implemented()).
    QList<IInstance*> instances() const;

    // the live signers, sorted by instanceId: the pre-filtered form of that walk.
    // Every device a module can see is listed, usable or not; check state().
    QList<plugin::SignerEntry> signers() const;

    // a module states whether it needs signing devices polled. One flag per module,
    // ORed: polling runs while any module wants it. Asking twice is asking once.
    void requestSignerPolling(IModule *requester, bool enabled);

    // asks every enabled module to list() again, so the launcher and account
    // discovery see an instance a module just persisted. Starts nothing.
    void refreshLauncher();

    // UI is PUSHED by the instance: it supplies a widget + itself, gets back an id,
    // and later updates or removes the widget by that id. The host owns the
    // id -> widget map. All of an instance's UI is removed on its teardown.
    using TabId = quint64;  using SectionId = quint64;  using WizardId = quint64;

    // a registered settings section, as returned by settingSections()
    struct SettingSectionView {
        SectionId id = 0;          // the id returned by addSettingSection
        QWidget  *content = nullptr;
        QString   title;           // empty when none was given
    };

    TabId     openTab(QWidget *content, IInstance *owner);           // at most one/instance
    void      setTabTitle(TabId tab, const QString &title);          // dynamic, any time
    void      closeTab(TabId tab);

    // global Settings lists the title with a button; the widget is shown only while
    // the modal that button opens is up, so a section can gate work on being visible
    SectionId addSettingSection(QWidget *content, IInstance *owner,
                                const QString &title = QString());
    void      delSettingSection(SectionId section);
    QList<SettingSectionView> settingSections() const;              // live sections

    WizardId  openWizard(QWidget *content, IInstance *owner);        // modal-ish flow
    void      closeWizard(WizardId wizard);

    HostI18n   *i18n();                       // translations (see i18n section)
    HostEvents *events();                     // event bus: NAMED slots (never lambdas)
};

class HostEvents : public QObject {
    Q_OBJECT
signals:
    void instanceRegistered(QString id);
    void instanceRemoved(QString id);
    void settingSectionsChanged();   // a section was added or removed
    void signerPollingChanged(bool enabled);  // the global flag, only when it flips
    void launcherRefreshRequested();  // refreshLauncher() was called
};
```

## Threading Model

**Each instance lives in its own `QThread`.** Consequently **no interface method
blocks and no method returns mutable instance state synchronously** (reading that
state from another thread would be a data race).

- `registerInstance`, `removeInstance`, `retireInstance` and `refreshLauncher` are
  callable **from any thread**. The host marshals to the GUI thread itself, so a
  module reacting to a device thread (a hotplug event) just calls them. Off the GUI thread the call is asynchronous:
  it returns before the work happens, so read the result on `instanceRegistered` /
  `instanceRemoved`, never on return. The instance must outlive that hop: do not delete
  an instance inside `retireInstance`, and do not free one you have just asked the host
  to drop. Free it from `instanceRemoved`.
- The **only** synchronous methods are the ones that never touch mutable state:
  `implemented()` on each capability, `meta()` on a plugin/module, `modules()` on a
  plugin, and `id()` on an instance (set once at instantiation, immutable thereafter).
  These are safe to call directly across the thread boundary.
- **Every other method is a request**: it returns a `ReqId` immediately, the work
  runs on the instance's (or module's) thread, and the result is delivered later by a
  **signal** carrying that same `ReqId`. Because *every* such method is a request,
  method names carry **no `request` prefix**: `coins()`, `signWithDescriptor(...)`,
  `list()` are all requests by definition.
- Plugins should emit request results asynchronously (for example, `QTimer::singleShot(0, ...)`)
  so callers can register pending `ReqId` state first. The host is defensive and also
  tolerates accidental synchronous emissions.

```cpp
using ReqId = quint64;   // correlates a request with its result signal
```

**Signals live on the capability** that owns the request, not on the host. A request
method and its result signal **share a name**, distinguished by signature: the method
takes the inputs and returns a `ReqId`; the signal carries the `ReqId` plus the
result (method `coins()` -> signal `coins(ReqId, QList<...>)`; connect with
`qOverload`). The consumer **connects its own named slot directly to the capability
object** and matches emissions by `ReqId` (`if (req == mine)`). **The host does not
route responses**: there is no `ReqId -> slot` table; correlation is the caller's,
via the id carried in the signal. Consumers may **unregister** when done (named slots
only, never lambdas; `qontrol::UNIQUE`). Streaming updates (e.g. coins, balance, fee
rate) use the same signals: connect once and every update arrives; a `ReqId` of 0
marks an unsolicited push not tied to a specific request. If a capability's
`implemented()` is `false`, calling its methods is a no-op and **no result signal is
ever emitted**. Results are delivered to the GUI thread via queued signals.

## Capability Interfaces

These are the **data** capabilities only; **UI is pushed to the host** (see The
Host), not exposed as an interface. Each capability is its own `QObject` (so it can
carry its own methods *and* signals), pure-virtual, declared with
`Q_DECLARE_INTERFACE`, using only Qt/std types across the boundary (see ABI rules).
**Each carries its own `implemented()`** (a hardcoded, compile-time `const`, the only
synchronous method). Every other method is a request (returns a `ReqId`; result on
that capability's matching signal).

> Because a class cannot multiply-inherit `QObject`, `IInstance` does **not** inherit
> the capabilities; it **composes** them, exposing each as a sub-object accessor
> (`account()`, `feed()`, `signer()`, `theme()`). Each sub-object lives in the
> instance's thread and emits its own signals.

The host is a **singleton** (`Host::get()`), so methods never take an `IHost*`.
Sketches:

```cpp
// The plugin: the loaded library. Contains one or more modules. This is the type
// QPluginLoader resolves (Q_PLUGIN_METADATA); the host asks it for its modules.
class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual QMap<QString,QString> meta() const = 0;     // plugin id, name, version...
    virtual QList<IModule*> modules() const = 0;        // one or more
};

// A module: a unit of functionality inside a plugin. Owns its instances and their
// IDs (+ persistence). A QObject so it can deliver results by signal.
class IModule : public QObject {
    Q_OBJECT
public:
    virtual QMap<QString,QString> meta() const = 0;     // hardcoded const: sync
    virtual ReqId list()      = 0;          // -> instances(req, [(id, name)])
    virtual ReqId autoStart() = 0;          // -> autoStart(req, [id]) to start on load
    virtual void  startInstance(const QString &id) = 0;  // construct + register it
    virtual void  deleteInstance(const QString &id) {}   // drop persisted state (default no-op)
    // the host's side of the polling switch; default no-op for modules that poll nothing
    virtual void  setSignerPolling(bool enabled) {}
signals:
    void instances(ReqId, QList<QPair<QString,QString>>);  // (id, name) it can start
    void autoStart(ReqId, QStringList);                    // ids to start immediately
    // error carries an OPTIONAL ReqId: set when the error answers a specific
    // request, empty for a spontaneous/connection error not tied to one.
    void error(std::optional<ReqId>, QString message);
};

// Each capability is a QObject: implemented() (sync const) + request methods + the
// result signals for those requests. implemented()==false => inert (no-op, never
// emits). Method and result signal share a name (distinguished by signature).
// EVERY capability also exposes:
//   - raw(QByteArray) -> raw(ReqId, QByteArray): a generic escape hatch for
//     custom, capability-defined calls not covered by the typed methods.
//   - error(std::optional<ReqId>, QString): failures; the ReqId is set when the
//     error answers a specific request, empty for a spontaneous error.

// An account: coins, addresses, balance. Signing is NOT part of this capability;
// it is the separate ISigner capability.
class IAccount : public QObject {
    Q_OBJECT
public:
    virtual bool  implemented() const = 0;
    virtual ReqId name()    = 0;
    virtual ReqId info()    = 0;       // kind, displayName...
    virtual ReqId coins()   = 0;
    virtual ReqId balance() = 0;
    virtual ReqId newReceiveAddress() = 0;
    virtual ReqId newChangeAddress()  = 0;
    virtual ReqId raw(const QByteArray &request) = 0;   // custom, on demand
signals:
    void name(ReqId, QString);
    void info(ReqId, QMap<QString,QString>);
    void coins(ReqId, QList<plugin::Coin>);
    void balance(ReqId, plugin::Balance);
    void receiveAddress(ReqId, QString address);
    void changeAddress(ReqId, QString address);
    void raw(ReqId, QByteArray response);
    void error(std::optional<ReqId>, QString message);
};

// A data feed. No subtypes: one capability that pushes typed updates (whichever a
// given feed produces), typically with ReqId 0 (unsolicited push).
class IFeed : public QObject {
    Q_OBJECT
public:
    virtual bool  implemented() const = 0;
    virtual ReqId raw(const QByteArray &request) = 0;   // custom, on demand
signals:
    void feeRate(ReqId, double satvb);
    void conversionRate(ReqId, QString currency, double value);
    void raw(ReqId, QByteArray response);
    void error(std::optional<ReqId>, QString message);
};

// A signer, the Qt/std projection of bwk's `Signer` trait as implemented by
// `HwSigner` (over the lower-level `HWI` device trait). Descriptor-based, not
// policy-based. id/fingerprint/wallet_name are set at construction and immutable
// (sync-safe); everything else talks to the device and is a request. None of the
// three identifies the entry: several live signers can share a fingerprint, and
// id() is module-defined. Host::signers() hands back the identity.
class ISigner : public QObject {
    Q_OBJECT
public:
    virtual bool    implemented() const = 0;
    // immutable accessors (set at construction; sync-safe)
    virtual QString id()          const = 0;
    virtual QString fingerprint() const = 0;   // master fingerprint; NOT unique
    virtual QString walletName()  const = 0;
    virtual plugin::SignerState state() const = 0;  // Ready / Locked / Unsupported
    virtual QString stateDetail() const = 0;   // why it is not Ready, in one line
    // requests (touch the device): result on the matching signal
    virtual ReqId init() = 0;                                       // open/handshake
    virtual ReqId info() = 0;                                       // device kind, version...
    virtual ReqId getXpub(const QString &derivationPath) = 0;
    virtual ReqId isDescriptorRegistered(const QString &descriptor) = 0;
    virtual ReqId registerDescriptor(const QString &descriptor) = 0;
    // One round per call: a PSBT returned with only MuSig2 nonces added is success;
    // the caller calls again for the next round. A device with no matching session
    // answers error() with SignError::MusigSession.
    virtual ReqId signWithDescriptor(const QString &psbtBase64,
                                     const QString &descriptor) = 0;  // -> updated PSBT
    virtual ReqId raw(const QByteArray &request) = 0;              // custom, on demand
signals:
    void initialized(ReqId);
    void info(ReqId, QMap<QString,QString>);     // device kind, version...
    void xpub(ReqId, QString xpub);
    void descriptorRegistered(ReqId, bool ok);
    void descriptorIsRegistered(ReqId, bool isRegistered);
    void signed_(ReqId, QString psbtBase64);     // PSBT signed with the descriptor
    void raw(ReqId, QByteArray response);
    void error(std::optional<ReqId>, QString message,
               plugin::SignError kind = plugin::SignError::Other);
};

class IThemeProvider : public QObject {      // named palettes
    Q_OBJECT
public:
    virtual bool  implemented() const = 0;
    virtual ReqId themes() = 0;
    virtual ReqId palette(const QString &name) = 0;
    virtual ReqId raw(const QByteArray &request) = 0;   // custom, on demand
signals:
    void themes(ReqId, QStringList);
    void palette(ReqId, theme::Palette);
    void raw(ReqId, QByteArray response);
    void error(std::optional<ReqId>, QString message);
};

// THE INVARIANT: every instance is an IInstance. It composes the capabilities
// (each a QObject living in the instance's own QThread) and exposes them by
// accessor. A capability is active iff its implemented() is true; otherwise it is
// inert and never emits. (UI is pushed to the host, not exposed here.)
class IInstance {
public:
    virtual ~IInstance() = default;
    virtual QString id() const = 0;       // set at instantiation; immutable (sync-safe)
    virtual void    stop() = 0;           // teardown (pushed UI removed, thread stopped)

    virtual IAccount       *account() = 0;
    virtual IFeed          *feed()    = 0;
    virtual ISigner        *signer()  = 0;
    virtual IThemeProvider *theme()   = 0;
};
```

A consumer reaches a capability via its accessor and gates on `implemented()`, e.g.
`instance->account()->implemented()`; if true it issues requests and connects that
capability's signals to named slots. The host needed by any of this is reached
through the singleton `Host::get()`, not passed in.

Neutral data types crossing the boundary live in the SDK (no Rust types, see ABI
rules):

```cpp
namespace plugin {
struct Coin    { QString outpoint; quint64 value; quint32 height;
                 QString label; bool spent; QString accountType; };
struct Balance { quint64 confirmed; quint64 unconfirmed; };
enum class SignerState { Ready, Locked, Unsupported };
// one live signer: which entry it is, where it came from, how to reach it
struct SignerEntry { QString instanceId; IModule *module; ISigner *signer; };
// why a sign request failed, beyond its message
enum class SignError { Other, MusigSession };
}
```

### Accounts are uniform

`IAccount` is one interface for **every** account kind; the kind ("sp", "miniscript",
"liana", "bark") is just `info()` metadata, not a subtype. Code that drives sibling
accounts treats them all the same. The account capability covers coins, addresses,
and balance; it **does not include signing methods** (signing is the separate
`ISigner` capability, see below). Fine-grained, kind-specific extension capabilities
can be added later if a real cross-account kind-specific operation appears.

### Feeds

A feed is a single capability (`IFeed`), no subtypes. It pushes up to three kinds of
update on its own signals (typically with `ReqId == 0`, since these are unsolicited
pushes a consumer subscribes to):

- `feeRate(ReqId, double satvb)`: current fee-rate estimate.
- `conversionRate(ReqId, QString currency, double value)`: BTC/FIAT price.
- `raw(ReqId, QByteArray data)`: feed-defined raw bytes for anything not covered by
  the typed signals.

A consumer walks `Host::get()->instances()`, gates on `feed()->implemented()`, and
connects the signals it cares about:

```cpp
for (auto *inst : Host::get()->instances()) {
    auto *f = inst->feed();
    if (!f->implemented()) continue;
    connect(f, &IFeed::conversionRate, this, &MyWidget::onPrice, qontrol::UNIQUE);
}
```

Mainline Silent ships **no feeds**. Fee rate and conversion rate arrive as plugins;
UI that consumes them must degrade gracefully when none is present.

### Signing devices are a separate capability

`ISigner` is the full interface to a signer, a distinct capability separate from
`IAccount` (the account capability has no signing methods). `Host::get()->signers()` is the
pre-filtered form of the walk used for the other capabilities: one `plugin::SignerEntry` per
live instance whose `signer()->implemented()` is true, sorted by `instanceId`. It is not
cached; re-query it on `instanceRegistered` / `instanceRemoved`.

**A fingerprint is not an identity.** It identifies a seed, not a device, so several live
entries can carry the same one:

- the same seed loaded on two devices, or a device and its backup;
- two modules exposing the same physical device (a USB module and an airgap module);
- one module holding several devices that share a seed.

**Every device a module can see is listed, usable or not.** A locked device, one whose
Bitcoin app is closed, one on the wrong network: leaving those out of `signers()` tells the
user nothing, so they are listed and `state()` says why they cannot be used. A consumer that
wants a signature keeps the `Ready` ones; a picker shows the rest disabled with
`stateDetail()` next to them, which is how the user learns there is something to unlock.
Requests on a signer that is not `Ready` answer with `error()` rather than going quiet.

`state()` is immutable like the other accessors: a module whose device changes state drops
the instance and registers a fresh one, so a reader never sees it change underneath.

So a consumer that needs a signature filters `signers()` by `fingerprint()` to get
**candidates**, expects more than one, picks one (asking the user when the choice is
theirs, which is what a signer picker is for), and routes the PSBT plus the relevant
descriptor through that entry's `signer`. The three parts of an entry have three jobs:

- `instanceId` is the **identity**: unique across live instances, enforced by the host,
  and the string to persist when a chosen device must be remembered across restarts;
- `module` is the **discriminator**: it says which module produced the entry, which is what
  separates two entries carrying the same key. `IModule::meta()` is synchronous, so a
  picker can label the row with the module name without issuing a request;
- `signer` is the **routing handle** for this session.

A signing-device module typically runs **one instance per connected device**: it registers
an instance when a device shows up and drops it when the device goes away, so `signers()`
tracks what is actually plugged in.

### Signing through the keys module (IKeys)

An account never picks a signer. It calls `Host::get()->keys()->sign(...)` with the PSBT
and its descriptors, and the keys module owns the picker and every route step (USB, QR,
hot key, SD card). Each request answers exactly once: `signed_`, `error` or `cancelled`.

A request is **one round per call**. A MuSig2 PSBT takes two calls: the first gathers
every cosigner's nonce and is answered `signed_` with only nonces added, the second
gathers the partial signatures. The caller calls `sign()` again for the next round.

`error` carries a kind next to its message:

```cpp
void error(std::optional<ReqId> req_id, QString message,
           plugin::SignError kind = plugin::SignError::Other);
```

`kind` is `MusigSession` when a device lost its MuSig2 session (QR error `0x0D`, a hot
key, or a USB device reporting it): the caller drops every nonce and partial signature
and starts again from round 1. It is `Other` for everything else.

`keys(network)` lists the registered keys as `KeyEntry` rows. Before asking
`verifyAddress()`, a caller checks `can_show_address`: true when the key has a USB or QR
route, a device with a screen that can show the address. A hot key, an SD card only key
or a key with no route cannot.

`registerDescriptor()` records the descriptor on the key and also registers it on the
key's connected USB device (checked first, registered when the device does not have it);
it answers once the device has, or at once when no device is connected. The proof a
device returns for a registration (the Ledger's wallet HMAC) lives on the key record, per
key and descriptor: `storeRegistrationHmac()` writes it, called by the device module when
a registration answers with one, and `registrationHmac()` reads it back, called by the
device module for every request scoped to that wallet (sign, show an address, and the
registration check, which a stored proof answers without asking a Ledger).

### Polling is off until a module asks for it

Watching a USB bus is expensive, so it does not run just because the app is open. Any module
calls `Host::get()->requestSignerPolling(this, true)` while it needs devices and `false` when
it stops. The host keeps **one flag per module** and ORs them: polling runs while any module
wants it. Because the requester is named rather than counted, asking twice is the same as
asking once and there is nothing to balance; a module that is disabled has its request
dropped for it.

The keys module asks while the Keys tab is visible **or** while any key of the current network
has a USB route: signing, address checks, xpub fetches and registrations with such a key need
its device already listed when their flow opens, from whatever tab they start. So polling stays
on for as long as a USB key exists, by design.

When the global flag flips, **every** module is told through `IModule::setSignerPolling`,
whether or not it asked for anything, and a module enabled later is told the current value as
it activates. A module that owns a device service starts or stops it there; one that polls
nothing leaves the default no-op alone. A module told to stop drops its device instances
along with its service, so `signers()` never reports a device nobody is watching.

`ISigner` mirrors bwk's **`Signer`** trait as implemented by `HwSigner` (which itself
sits over the lower-level `HWI` device trait). It is **descriptor-based**:
- `id()`, `fingerprint()` and `walletName()` are immutable, set at construction. None of
  them is the entry's identity: `id()` is module-defined and carries no uniqueness
  guarantee, `fingerprint()` is shared by every device holding the seed, and `walletName()`
  is a label. Address an entry by its `instanceId`;
- requests `init()`, `info()` (device kind / version), `getXpub(path)`,
  `isDescriptorRegistered(descriptor)`, `registerDescriptor(descriptor)`,
  `signWithDescriptor(psbt, descriptor)` and `displayAddress(descriptor, path, address)`.
  The last two are scoped to the registered wallet of `descriptor`.

Every request talks to a physical device, so (per the Threading Model) it returns a
`ReqId` and the result arrives on the matching signal. The C++ interface is the
Qt/std projection of the trait across the module boundary; a signer module typically
wraps a `bwk` `HwSigner` internally via CXX.

## Inter-Account Operation: Send To / Spend From Siblings

A key use case is moving funds between sibling accounts (e.g. a consolidation or
swap plugin). The account capability has no signing methods, so signing always routes
through one or more `ISigner` instances.

- **Send TO a sibling**: pick a recipient instance from `Host::get()->instances()`
  (gated on `account()->implemented()`), call `account()->newReceiveAddress()`, and
  read the address off its `receiveAddress(ReqId, ...)` signal.
- **Spend FROM a sibling**: call the source `account()->coins()`, assemble a PSBT
  spanning the chosen UTXOs, then route the PSBT + descriptor to the `Host::get()->signers()`
  entries holding the keys via `signWithDescriptor` (the signed PSBT arrives on
  the `signed_` signal); combine partial signatures and broadcast. PSBTs cross the
  boundary as base64 strings.

> Cross-account PSBT construction over multiple accounts' UTXOs may need wallet-core
> support in `bwk-sp`, coordinated separately. The signing path (`ISigner`) exists
> from the start; the full multi-account build flow lands once core support is in
> place.

## ABI & Linking Rules (Plugin Authors Must Read)

Silent statically links Qt, and the host binary acts as the **single Qt provider**:
plugins link **no Qt of their own** and resolve Qt symbols against the host at load
time. This keeps one Qt in the process so signals, meta-types, and singletons stay
unified. Consequences:

- **Toolchain lock.** A plugin must be built against the **same Qt version and a
  compatible compiler ABI** as the host. `QPluginLoader` enforces a build-key check
  and refuses mismatched plugins (logged, not crashed).
- **Boundary types are Qt/std only.** Everything crossing the host/plugin boundary
  uses C++ virtual interfaces and `QString`/`QList`/PODs/base64 strings, **never
  `rust::*` types.** A plugin may use Rust internally and bridge it with CXX
  *inside* its own library; that Rust never crosses the boundary.
- **Supported Qt surface is fixed at host build.** A plugin may use **no more Qt
  than the host embeds.** The default supported surface is **Qt Core, Gui, Widgets,
  Svg + the qontrol framework.** Using Qt machinery the host did not link will fail
  to resolve at load time. Extending the surface requires a host change (an anchor
  upstreamed into the host build) and ships with the next host release. Plugins are
  runtime-loadable, but the Qt surface is build-time-fixed. This is intentional for
  a security-sensitive wallet.

## Plugin Translations (i18n)

Modules reuse Silent's existing `.lang` translation system (see
[I18N.md](I18N.md)): same catalog format, same author scripts.

- **Delivery.** Ship `.lang` catalogs alongside the library at
  `~/.silent/plugins/<id>/i18n/silent_<locale>.lang`. On enable, the host loads the
  current locale's catalog; on language change it reloads, so plugin text switches
  **live** like host text.
- **Namespacing.** Plugin keys are automatically prefixed with the plugin id, so a
  plugin's `TR("send-title")` becomes `"<id>.send-title"` internally and cannot
  collide with host or sibling-plugin keys. Authors write **unprefixed** keys and
  `.lang` files exactly as in mainline.
- **Macro.** The SDK provides a plugin-side `TR(...)` that injects the id prefix;
  call sites read identically to host code.

## Authoring Checklist

1. Implement `IPlugin` and its `IModule`(s); each module's `IInstance` composes the
   capability objects you need (leave the rest `implemented() == false`). Push UI to
   the host (`openTab` / `addSettingSection` / `openWizard`).
2. Follow the project [code guidelines](../CODE_GUIDELINES.md): named slots (no
   lambdas in `connect`), three-phase widget lifecycle (`init`/`doConnect`/`view`),
   `screen::`/`modal::` namespaces, `m_` members, composite widgets exposing
   `widget()`, toggles via `&QCheckBox::toggled`.
3. Build against the plugin SDK headers; link no Qt of your own; match the host Qt
   version/toolchain.
4. Ship `library + <id>.json` metadata (plus optional `i18n/*.lang`) under
   `~/.silent/plugins/<id>/`.
5. The plugin installs disabled; enable it from global Settings.
