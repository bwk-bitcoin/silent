# Silent Plugin System

Silent is extensible through plugins. External logic (new account kinds, data
feeds, extra tabs, settings sections, themes) can be imported as a single ZIP bundle
and enabled at runtime without rebuilding the app.

This document describes the architecture and the contract a plugin author works
against. It is the design reference; implementation lands incrementally.

## Core Model: Plugin, Module, Instance, Capabilities

There is **no taxonomy of module "types".** There are three levels and a single
invariant:

- **Plugin**: a bundle containing a native **library** (`.so`/`.dll`/`.dylib`),
  manifest metadata and optional translations. A plugin **contains one or more modules** and exposes them to the host
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
- **External (a plugin)**: an imported bundle encrypted in the application's state
  under `~/.silent/plugins/`. One plugin **contains one or more modules**. Disabled by
  default; the user enables the plugin, which brings up all of its modules.

## Distribution unit: one ZIP bundle

Distribute one ZIP file. `.silent` is a conventional filename suffix, not a format
requirement. The importer detects ZIP contents; the same bytes work with a renamed
or extensionless filename. Drop the bundle on Settings to import it. Directories,
loose native libraries and standalone JSON files are rejected even when matching
sidecars are nearby. The distributable is not encrypted with a user's installation
key.

The ZIP has a fixed root manifest and declared payload paths:

```text
manifest.json
payload/libmyplugin.so
i18n/silent_en.lang          # optional
i18n/silent_fr.lang          # optional
```

Only declared files are allowed. Do not include directory records, source files,
sidecars, links, or alternate libraries. A Windows bundle names a `.dll`; a macOS
bundle names a `.dylib`. Build one bundle for each supported native target.

The manifest is a strict typed JSON object. Example for a Linux x86-64 library
built with Qt 6.8 and baseline x86-64 requirements:

```json
{
  "format_version": 1,
  "id": "myplugin",
  "name": "My plugin",
  "version": "0.1.0",
  "description": "Example plugin",
  "author": "Plugin author",
  "capabilities": ["feed"],
  "entry": {
    "kind": "native",
    "library": "payload/libmyplugin.so",
    "compatibility": {
      "iid": "dev.silent.IPlugin/10.0",
      "target_os": "linux",
      "target_arch": "x86_64",
      "qt_major": 6,
      "qt_minor": 8,
      "arch_requirements": 1
    }
  },
  "translations": ["i18n/silent_en.lang", "i18n/silent_fr.lang"]
}
```

Use the actual build's Qt version and embedded architecture requirements, not the
example values. `format_version` versions the container contract, `version` is a
display/release version, and `iid` versions the public plugin ABI. They are separate.
All fields are required; the two arrays may be empty. Unknown fields, duplicate
JSON keys, invalid field types, unsupported entry kinds and incompatible platforms
are rejected. `entry.kind` is currently only `native`. A future process entry can
use the same outer container; no process runner or OS sandbox is implemented here.

Import reads the manifest and library without loading plugin code. Native admission
checks the exact payload bytes against the host's actual Qt/build capabilities and
requires manifest/library agreement before replacing encrypted installed data.
Duplicate or case-ambiguous names, traversal, absolute paths, backslashes, links,
missing files and undeclared extra entries fail. Archive input and decoded data are
bounded; no archive paths are extracted to the host filesystem. Supported ZIP
entries use stored or deflate compression. ZIP64, encryption, multi-disk archives,
archive comments, overlapping records, local/central header disagreement and
unaccounted leading/trailing bytes are rejected.

The importer accepts at most 272 MiB of input, 66 entries, a 64 KiB manifest,
a 256 MiB native library and 64 translation catalogs of at most 1 MiB each.
Total decoded data is limited to 272 MiB. Decompression output is bounded while
reading and must match each entry's declared size and CRC; a forged small size
does not permit an unbounded decoded stream. Manifest text fields are limited
to 4096 bytes and cannot be empty, padded or contain control characters.

The installed format remains encrypted bundle version 1 under an opaque filename
in `~/.silent/plugins/`, sealed with the existing application state key. Compatible
old installed bundles remain readable. Import replaces only the plugin bundle,
not its #60 private logical storage or per-library-hash grants. A changed library
hash gets its own grant decisions. An invalid replacement leaves the installed
bundle unchanged. If a plugin is already running, the new library is used after
disabling and enabling it; import itself never loads code.

For a manual package, `zip -X -D myplugin.silent manifest.json payload/libmyplugin.so
i18n/silent_en.lang` omits directory records. Prefer the producer's CMake bundle
target, which also derives compatibility from the actual built library.

With `SILENT_BUILD_HOST_SMOKE=ON`, the real importer/registry consumer accepts one
producer bundle per run. Give it an isolated HOME and run offscreen:

```sh
HOME=<empty-test-home> QT_QPA_PLATFORM=offscreen build/host_smoke \
  --plugin-bundle <producer-bundle>
```

It checks metadata before enable, the actual loaded library and feed capabilities,
translations, renamed/extensionless reimport, enabled/disabled replacement and
invalid replacement. The example's separate legacy-state/storage consumer runs with
`SILENT_HOST_SMOKE=<absolute-host-smoke> SILENT_EXAMPLE_PLUGIN=<absolute-example-bundle>
cargo test --manifest-path silent/Cargo.toml --test storage_example -- --ignored`.

The legacy installed-format gate has a separate driver:
`SILENT_HOST_SMOKE=<absolute-host-smoke> cargo test --manifest-path silent/Cargo.toml
--test plugin_legacy_abi -- --ignored`. It seeds encrypted version 1 bundles for
ABIs 2 through 9 and future ABI 11, then verifies registry refusal, expected/found
diagnostics, retained metadata and absence of initializer/constructor side effects.
The `--plugin-abi` host smoke uses ABI 10 as its accepted control and verifies that
refused imports preserve its installed bytes. ABI 9 uses frozen declarations from
`tests/host/legacy/ModuleV2.h`, not the current root header. The compiled-fixture Rust
tests expect `SILENT_ABI_FIXTURE_PATH` to point to `libplugin_abi10_fixture.so`.

### Out-of-tree Plugin Repo Pattern

Plugin projects live in their own repositories and build against a pinned snapshot
of Silent's public SDK. The example, fee and fiat producers vendor this small
closure rather than an application checkout or generated Rust/CXX headers.

Recommended layout:

```text
my-plugin-repo/
  CMakeLists.txt
  src/
    MyPlugin.cpp
    myplugin.json            # build-time source metadata, packaged into manifest.json
    i18n/
      silent_en.lang
  scripts/
    bundle.py
    verify-sdk.py
  sdk/
    SHA256SUMS
    include/src/interfaces/
    include/src/theme/
```

The plugin build includes headers from:

```text
sdk/include/src
```

The local reference producer is `~/silent-plugin`. Its SDK records host commit
`53a5b96` and checks every copied public file against the committed source. The
public upstream repository is:

```text
https://github.com/pythcoiner/silent-plugin
```

#### External plugin repo layout

An out-of-tree plugin repo follows this baseline:

- Put plugin source under `src/` or `cpp/`.
- Pin the complete public SDK header closure and record its source commit and file
  checksums. Copy all public `src/interfaces/*.h` and their public dependencies,
  not just `module.h` or an updated IID string. Do not vendor Rust build targets.
- Consume the headers from `sdk/include/src`. Use the producer's SDK verifier
  against the committed host checkout when refreshing the snapshot.
- Public implementation files needed for Windows import discovery are part of the
  snapshot too. Compile them only for export-symbol discovery, not into the plugin.
- i18n: author `.lang` sources; do not treat JSON as the source of truth. When JSON
  is needed it is generated at build / pre-compile time. See [I18N.md](I18N.md).
- Build: provide a reproducible Nix build target whose naming aligns with Silent's.

The host **reads the installed manifest metadata before the library is ever loaded.** This
read-before-load ordering is the load-time boundary: a disabled plugin is
*discovered* (its metadata is listed in Settings) but its library is **never
`dlopen`'d** and **no plugin code runs** until the user enables it.

The manifest remains available for metadata display while disabled or after a
load failure. Packaging and admission checks do not prove that native code is
truthful or safe, and do not replace the public ABI contract or the second
`qobject_cast` gate at enable time.

## Lifecycle: Discover, Enable, Activate

1. **Discover.** On startup the host scans the encrypted installed bundles under
   `~/.silent/plugins/`, reads their metadata and lists every plugin in global Settings. Newly imported
   plugins are **disabled by default**. Discovery is metadata-only (no `dlopen`),
   so it is cheap and side-effect free. A **Rescan** button in global Settings
   re-runs it on demand. Dropping a ZIP bundle on Settings imports it and refreshes
   the list without a restart. Rescan only updates the discovered list; it never
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
- `deleteInstance(id)`: release runtime ownership only. It must preserve saved data.
- `deleteAccount(req_id, id)`: explicit persisted deletion after UI confirmation.
  The registry supplies a nonzero request ID and records it before dispatch. The
  module answers `accountDeleted(req_id, id, error)` exactly once, including refusal;
  an empty error means success. The result may arrive inside the call. Default is
  an explicit unsupported error, not successful no-op deletion.

A module is **not limited to the host-driven path**: it may call
`host->registerInstance(id, instance, this)` and `host->removeInstance(id, this)` at any
time, for example one instance per connected device, registered on plug and dropped on
unplug. Both calls are ownership-checked, so a module only ever drops its own instances,
and an instance can retire itself with `host->retireInstance(this)`. Dropping an instance
stops it and closes its pushed UI but **never touches persisted state**. Only confirmed
`deleteAccount` removes saved data. The module frees its own `IInstance` from its
`instanceRemoved` handler.

### Account deletion and built-in logical persistence

The launcher always sends the owning module and stable account ID through the
registry. There is no built-in versus external persistence branch. The registry
validates ownership against discovery and live registration, correlates completion,
blocks reopen and plugin unload during deletion, and relists only after success.
Failures are displayed and remain in the last discovered list.

The owning module validates the stored identity, blocks new account operations,
requests shutdown and waits for every accepted worker, queued persistence operation,
notification receiver and Rust backend to finish. Host removal and a disappeared tab
are not this barrier. SP/MuSig track closing controllers separately from live instances.
Shutdown flush failures prevent removal. An account closed with a flush failure can
be reopened to recover its saved state before another deletion attempt.

SP and MuSig now store their account files through the #60 engine, beneath their
private `sp` and `musig` logical roots. Each account owns
`accounts/<sha256 of stable account ID>/`: `config.json`, `backend-config.json`,
wallet row stores and account-owned header stores; MuSig also owns `saved_psbts.json`.
SP's `sub-0`, `sub-1`, etc. keep the upstream descriptor indexes separate.
Built-in creation reserves the global instance ID through an internal SP-context
lifetime lease and refuses a config already present in either module; the module's
payload is still written only to its own tree.
The module's factory validates every upstream `BackendIdentity` and keeps opaque row
bytes unchanged in versioned whole-file documents. `flush_batch` loads, changes and
atomically replaces one complete logical store file under the storage engine's lock.
Opaque row bytes are base64 strings, not expanded JSON integer arrays.
The Rust config callbacks cannot overwrite Silent's separate config file.

Trusted built-in Rust workers have fixed caller contexts for those two roots. They
reuse #60 authorization, encryption, path checks, interprocess tree exclusion and
durable publication. Private host-local logical lifetime leases preserve single-owner
account use across processes; no physical path, native lock or encryption key is
exposed by `IStorage`. Closing a factory fences reopen/recovery requests and drains
all its backend objects before releasing ownership. Removal uses the same authorized
logical recursive-remove operation as other storage clients, not a host account-path
or deletion-specific filesystem API.
Native lock/lease sentinels are storage-engine metadata, not account payload files.

This is a whole-file adapter, not a database filesystem or completed external
SP/MuSig plugin deployment. The built-in workers' fixed caller contexts are not held
to the `IStorage` bounds on file and tree size: a wallet or header store grows with the
wallet, each file sealed in its own blob, so a flush rewrites that store's blob and the
tree index only. A module tree is still limited to 4096 entries. The native built-in lifetime
coordinator and its host-local leases are not an external IPC lease protocol. No OS
sandbox or secure-erasure guarantee is claimed.

Deleting an account removes only its logical subtree. It preserves other accounts,
shared signer keys, nonce/session history and the application unlock key. Normal tab
close, plugin disable and unload preserve account storage. The unreleased former
built-in account layout is not migrated; #60's separate legacy external-plugin blob
migration remains unchanged, including its no-overwrite and import-once behavior.

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

    // discovery: the live instances, as the calling module sees them. A consumer
    // gates each capability on its implemented() (e.g.
    // inst->account()->implemented()). Each account() is a handle that stamps the
    // caller on every IAccount call and checks its access (see "Callers and access").
    QList<IInstance*> instances(IModule *caller);

    // the live signers, sorted by instanceId: the pre-filtered form of that walk.
    // Every device a module can see is listed, usable or not; check state().
    QList<plugin::SignerEntry> signers() const;

    // Cached global price selection, GUI thread only. rate is nullopt when unavailable.
    // PriceSource has id, name and currencies; SelectedPrice has source, currency, rate.
    auto priceSources() const -> QList<PriceSource>;
    auto selectedPrice() const -> SelectedPrice;
    void selectPrice(const QString &source, const QString &currency);

    // the keys module, as the calling module sees it: a handle that stamps the caller
    // on every IKeys call and checks its access. Null until the keys module registered.
    IKeys *keys(IModule *caller);

    // what the calling module may use: asks the user for the grants in `wanted` an
    // external plugin has no answer for yet. GUI thread only.
    plugin::Access requestAccess(IModule *caller, plugin::Access wanted);

    // the calling module's storage: its plugin's private file tree, through an IStorage
    // bound to the caller (see "Plugin storage"). The same one every call; null when the
    // caller is no active module or storage is unavailable. GUI thread only.
    IStorage *storage(IModule *caller);

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

    enum class PagePlacement : quint8 { Normal = 0, Module = 1 };
    TabId     openTab(QWidget *content, IInstance *owner,
                      PagePlacement placement = PagePlacement::Normal); // at most one/instance
    void      showInstanceTab(const QString &instance_id);
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
    void priceSourcesChanged();      // available sources or offered currencies changed
    void selectedPriceChanged();     // selection, rate or availability changed
};
```

### Callers and access

A module reaches the accounts and the keys only through the handles the host gives it:
`Host::get()->instances(this)` and `Host::get()->keys(this)`, passing its own
`IModule *`. Every request through a handle is stamped with its caller, which the
account or the keys module reads with `caller()` at the start of the request:

```cpp
struct Caller {
    QString moduleId;   // the module's meta() "id"
    QString pluginId;
    bool external;      // loaded at run time from a plugin library
    QString pluginHash; // sha256 of the plugin library, empty for an internal module
};
```

The host itself calls accounts directly: a request with an empty `moduleId` comes from
the host. A handle passes on only the answers to its own requests (the ids it got
back, or allocated with `newRequest()`); updates an account pushes with `ReqId == 0`, and
errors tied to no request, go to every caller allowed to use accounts (or keys).

The host hashes an external plugin's library before `QPluginLoader::load()`, so a plugin
update has a new hash. Built-in modules may use everything. An external plugin needs
separate grants, kept in `~/.silent/app.json` per library hash: **Accounts** (`IAccount`,
on every account), **Keys** (`IKeys`) and **Saved Data** (`storage()`). On its first
call needing a grant it has no
answer for, the host asks the user (Allow / Deny) and keeps the answer; a denied grant
is not asked again. A refused request answers `error()` (IKeys) or returns `0` and
answers `error(std::nullopt, ...)` (IAccount). A plugin can ask up front, at a moment it
picks, with `requestAccess()`. The user changes or forgets the answers in global
Settings, Plugin Manager, Access, per library hash.

An account can enforce a finer whitelist of its own on top, since it knows the caller.

### Plugin storage

A plugin persists its own data through the host rather than writing files of its own:
`Host::get()->storage(this)` gives the calling module an `IStorage`
(`interfaces/storage.h`). The plugin picks its own layout of logical files and
directories and its own format for their bytes; it never sees a physical path, a file
descriptor or the encryption.

```cpp
class IStorage : public QObject {
    Q_OBJECT
public:
    virtual void request(const QByteArray &message) = 0;  // one JSON request
    virtual void cancel(const QString &id) = 0;
signals:
    void completed(QByteArray message);                   // one JSON answer per request
};
```

Each message is one JSON object, so the same contract can later run over an IPC
connection to a sandboxed plugin process (bwk-bitcoin/silent#78). A message never names
its sender: the host binds the caller to the `IStorage` it handed out, from the identity
the plugin registry recorded when it loaded the library (plugin id, library hash,
built-in or not). Over IPC the identity comes from the authenticated connection.

```json
{"id": "1", "op": "open", "storage": "private"}
{"id": "1", "error": "", "storage": "<opaque handle>"}
{"id": "2", "op": "write", "storage": "<handle>", "path": "settings/example.json",
 "data": "<base64>", "mode": "upsert"}
{"id": "2", "error": ""}
```

Operations: `discover` (the storages the caller may open, today only
`{"id": "private", "name": "Private plugin storage"}`), `open`, `inspect`, `list`
(paged by `after` / `limit`), `mkdir`, `read`, `write` (`create`, `replace` or
`upsert`), `rename` (never replaces), `remove` (`recursive` for a non-empty directory)
and `migrate`. `interfaces/storage.h` lists every field, answer and error code. A field
the op does not take, an unknown op or a malformed message answers `invalid_request`.

- **Paths.** Logical paths are names joined by `/`, relative to the storage root. A name
  is 1 to 255 UTF-8 bytes, not `.` or `..`, without `/`, `\`, `:` or control
  characters. Names are case-sensitive and never Unicode-normalized; a path is at
  most 4096 bytes. The empty path is the root, taken only by `inspect` and `list`. An
  entry is created in an existing directory: `mkdir` does not create parents.

- **Whole files.** A read answers the whole file and a write replaces it whole: a write
  leaves the previous or the new complete file, never a part of one. No offsets, no
  random access, no chunks.
- **Backend.** The logical request/result contract does not depend on the physical
  filesystem or its layout. The backend keeps one encrypted index of the tree and one
  encrypted blob per file: a change rewrites the index and the blobs of the files it
  writes, never the other files. An embedded filesystem backend is deferred, not
  implemented here; changing the backend must not expose its paths or handles to
  plugins.
- **Limits.** A file holds at most 1 MiB before base64, a message at most 2 MiB of JSON
  and an id at most 256 bytes. A list page holds at most 128 entries. A storage holds at
  most 4096 entries, and its whole tree, counted as one JSON document with every file
  in base64, is at most 16 MiB before encryption. At most 8 requests are outstanding
  per `IStorage`, a cancelled one counted until it stopped running: one more answers
  `busy`. Every `IStorage` keeps these bounds, a built-in module's included; only the
  SP and MuSig wallet stores, written by the host's own workers outside `IStorage`,
  keep the entry bound only.
- **Scope.** The modules of one plugin share its storage, each through its own
  `IStorage`; a handle opened by one does not work on another (`invalid_handle`).
  Active caller identity is stamped from the installed registry record and loaded
  library hash at activation, not re-read from plugin or module metadata. A library
  claiming a different installed ID is refused before module activation; changing
  metadata later cannot change its identity, grants or storage root. The same
  caller identity is used for Account and Keys access.
- **On disk.** The storage lives under `.silent/<plugin_id>/` in the configured data
  directory, sealed with BIP138 to the app state's unlock key like the rest of
  `~/.silent/` (ws-plaude/silent#37): the index `tree.bip138` (paths, kinds, file sizes
  and blob ids) and one `<random id>.blob` per file holding its data. The plugin id is
  visible; file names, directories and contents are not, and a blob name tells nothing
  of the file it holds. A plugin whose id names a directory the host keeps
  there (`keys`, `plugins`, `plugin-state`, `musig-sessions`, `locked`), a reserved
  application file extension or an opaque account directory gets no storage
  (`storage()` answers null). Each document is bound to its exact plugin ID, so
  different IDs cannot silently share a root on case-insensitive filesystems.
  On Linux and macOS the files are opened relative to directory handles without
  following links. On Windows, handle-relative opens and replacement reject reparse
  points. Other platforms answer `unavailable`. The configured data directory itself
  is trusted host configuration and may be a link. A change first writes each new file
  to a new private blob and syncs it and the directory. The index replacement then
  writes and syncs a private temp, rechecks authorization, renames it and syncs the
  containing directory. Last, the blobs the index no longer names are removed. An error
  after rename can leave the complete new version published. Blobs an interrupted
  change left are removed by the next change, or when the storage is next opened. An
  index from before blobs, version 1 with every file's data in it, is read as it is
  and moves to blobs on its first change.
  A persistent, no-follow `tree.lock` is held from loading the tree through mutation,
  replacement, blob removal and migration readback. It uses an exclusive native lock through fs2
  on Unix and Windows; contention answers `busy` without waiting, and other lock
  failures are explicit errors. The lock is never replaced or unlinked. The same
  root handle is used for loading and publication, so a second process cannot
  overwrite a newer tree or publish another process's partial temp.
  The storage stays across plugin updates, while the
  Saved Data grant stays per library hash: an update is asked again, and a rollback to
  a version allowed before finds its data again.
- **Grant.** `storage()` asks for the Saved Data grant when the library has no answer
  yet; every request checks it again and answers `denied` without it, or `locked` while
  the app state is locked. An answer computed before the grant was withdrawn or the
  state locked is delivered as that error, never with its data.
  Final write authorization and rename hold the state publication barrier. Key
  changes and grant updates take its exclusive side before any key/session locks.
  Grant persistence and generation invalidation finish together after any already
  authorized rename; no older, not-yet-authorized write can publish afterwards.
  A failed grant save is logged and leaves the stored grant and generation unchanged.
  Other app-state updates use the same barrier before loading their snapshot, so
  settings persistence cannot restore a stale grant. A key/grant transition that
  completes before authorization prevents publication.
- **Asynchronous.** An accepted request runs off the GUI thread, one at a time per
  `IStorage`, and its `completed()` comes after `request()` returned, on the GUI thread.
  A request refused before it is accepted (`too_large`, `invalid_request`, `busy`,
  `revoked`) answers from inside `request()`, so a caller flooding past the bound
  piles up nothing; connect `completed()` before sending. `cancel()` answers the
  request `cancelled` from inside the call and drops its result: a request that had not
  started changes nothing, one already running may still complete.
- **Revocation.** When the plugin is disabled or unloaded the host revokes its
  `IStorage`: outstanding requests get no answer, later ones answer `revoked`.
  Session revocation and cancellation do not drain workers or wait for disk I/O.
  They prevent publication if completed before the final lifecycle check, including
  for requests queued behind another operation. A commit that passed this check may
  still finish; its response is suppressed. They are not backend-shutdown fences.
- **Migration.** `migrate` copies the plugin's single saved blob of the former
  `loadState()` / `saveState()` API to the new logical file `path`, without reading its
  format, and reads it back. It answers `migrated: false` when there is no such blob
  (`reason: "absent"`), or when migration already completed (`reason: "completed"`).
  Before completion, an existing destination answers `already_exists` and is never
  overwritten. The completion marker and file are committed together; the legacy
  envelope is retained unchanged. Removing the new file afterwards never imports the
  old blob again.

Plugins run in Silent's process: a hostile plugin can bypass the handles. The access
check gates well-behaved plugins, it is not a security boundary.

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
    virtual void  deleteInstance(const QString &id) {}   // runtime release only
    virtual void  deleteAccount(ReqId req_id, const QString &id); // explicit deletion
    // the host's side of the polling switch; default no-op for modules that poll nothing
    virtual void  setSignerPolling(bool enabled) {}
signals:
    void accountDeleted(ReqId req_id, QString id, QString error);
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
// external signing goes through the caller-scoped IKeys service.
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
    virtual ReqId descriptor() = 0;    // the full descriptor
    virtual ReqId raw(const QByteArray &request) = 0;   // custom, on demand
signals:
    void name(ReqId, QString);
    void info(ReqId, QMap<QString,QString>);
    void coins(ReqId, QList<plugin::Coin>);
    void balance(ReqId, plugin::Balance);
    void receiveAddress(ReqId, QString address);
    void changeAddress(ReqId, QString address);
    void descriptor(ReqId, QString descriptor);
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
    virtual auto currencies() -> ReqId;                // default 0: no price capability
    virtual auto requestRates(const QStringList &currencies) -> ReqId; // default 0
signals:
    virtual auto feeCapabilities() -> ReqId; // default 0
    virtual auto requestDefaultFeeRate() -> ReqId; // default 0
    virtual auto requestSmartFeeRates() -> ReqId; // default 0
    void feeCapabilities(ReqId, bool has_default, bool has_smart);
    void defaultFeeRate(ReqId, double default_satvb);
    void smartFeeRates(ReqId, double low_satvb, double medium_satvb, double fast_satvb);
    void currencies(ReqId, QStringList currencies);
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
    virtual auto capabilities() const -> plugin::SignerCapabilities = 0;
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

`ISigner::capabilities()` returns immutable `plugin::SignerCapabilities` metadata:
`ordinary`, `taproot`, `musig2` and `silentPayments`, each explicitly
`plugin::Support::Supported` or `Unsupported`. Omitted fields mean Unsupported.
Serialize each field separately as an unsigned byte, 0 for Unsupported and 1 for
Supported, not the C++ struct layout. This describes the operations exposed by this
signer, not a guarantee that a particular descriptor or request will succeed.
Metadata-only route adapters and offline stand-ins explicitly advertise no signing
operations; IKeys drives those routes separately. A provider replaces its signer
instance when metadata changes. HWI network configuration remains provider-owned.
All four fields reach Rust's live-device list as bwk-qr bits 0 through 3. HWI derives
ordinary support from its supported record and Taproot support from its provider's
kind/version compatibility metadata. The legacy SP/MuSig2 bit meanings are unchanged.
Signing candidates select a USB device supporting the operations requested of that
fingerprint by the PSBT, not merely a connected device. Mixed ordinary/Taproot inputs
require both capabilities. Unsupported operations are refused again before device
dispatch. Hot routes use their own signing implementation; cached offline MuSig2
nonce publication does not require a live device.

`Host::openTab()` defaults to `PagePlacement::Normal`, the closable account strip.
`Module` publishes a non-closable page in the right strip before the Settings gear,
in publication order, without changing the current page. Wire values are unsigned
bytes 0 and 1; other values are refused with TabId 0, leaving ownership with the
caller. `showInstanceTab(id)` activates a live instance's page and ignores removed
IDs. Both placements use the same instance teardown and title update contract.
Keys auto-starts its `keys` instance and publishes its page through this contract;
the shell does not create or obtain a concrete Keys widget. Removing the instance
destroys its page; starting it again creates a fresh one. These are in-process UI
contracts, not proof of sandbox isolation.

With `SILENT_BUILD_HOST_SMOKE=ON`, run `build/host_smoke --keys-contract` under
`QT_QPA_PLATFORM=offscreen` and a fresh HOME. An optional second argument names an
existing screenshot directory. It checks public non-HWI capability combinations,
page publication failures, placement, Settings visibility and instance recreation.

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
enum class SignError : std::uint32_t { Other = 0, MusigSession = 1, InvalidRequest = 2 };
}
```

### Accounts are uniform

`IAccount` is one interface for **every** account kind; the kind ("sp", "miniscript",
"liana", "bark") is just `info()` metadata, not a subtype. Code that drives sibling
accounts treats them all the same. The account capability covers coins, addresses,
and balance; it **does not include signing methods** (signing is the separate
`ISigner` capability, see below). Fine-grained, kind-specific extension capabilities
can be added later if a real cross-account kind-specific operation appears.

`descriptor()` answers the account's **full** descriptor on `descriptor(ReqId,
QString)`: the sp() descriptor of an sp account (its scan key and spend public key),
the tr() / musig() descriptor of a Multikey account. Whoever holds an sp descriptor
can scan the account, so an external plugin calls it only with the Accounts grant (see
"Callers and access").

### Feeds

A feed is a single capability (`IFeed`), no subtypes. It pushes up to four kinds of
update on its own signals (typically with `ReqId == 0`, since these are unsolicited
pushes a consumer subscribes to):

- `defaultFeeRate(ReqId, double default_satvb)`: default recommended sat/vB.
- `smartFeeRates(ReqId, double low_satvb, double medium_satvb, double fast_satvb)`:
  Low, Medium and Fast recommended sat/vB. Each invalid value withdraws that tier.
- `conversionRate(ReqId, QString currency, double value)`: BTC/FIAT price.
- `raw(ReqId, QByteArray data)`: feed-defined raw bytes for anything not covered by
  the typed signals.

Prices go through the host's internal coordinator, not direct consumer subscriptions.
On instance registration, including startup and late plugin loading, it connects to
the feed before calling `currencies()`. Reply with the offered currency codes through
`currencies(req_id, codes)`. This operation only discovers currencies, not rates.
Fee discovery is separate from prices: implement `feeCapabilities()` and emit
`feeCapabilities(req_id, has_default, has_smart)`. The same internal coordinator calls
`requestDefaultFeeRate()` and/or `requestSmartFeeRates()` only for offered capabilities.
Connect before requesting, since replies may be synchronous. Later recommendations
are pushed by the provider; there is no host polling or automatic expiry.

`Host::feeSources()` lists each source's Default/Smart capabilities.
`Host::selectedFees()` returns cached optional recommendations and `FeePreferences`:
`mode` Manual/Default/Smart and one `source` ID. `selectFees(preferences)` persists only
mode and source globally for all networks. Settings offers one provider picker,
including no selection and configured-but-unavailable IDs. Default, Low, Medium and
Fast recommendations all come from that one source, regardless of mode. Missing
capabilities or rates remain unavailable; another provider never supplies a substitute.
`HostEvents::feeSourcesChanged` and `selectedFeesChanged` notify consumers. Missing
sources retain their configured IDs; the host never automatically selects another.
Invalid or withdrawn rates are unavailable, not zero-fee recommendations.

Global Settings displays all four cached recommendations as read-only values in
sat/vB, including in Manual mode. Each missing value is the literal `-`, also when
the selected source is None or unavailable. Both fee notifications refresh this
display from `selectedFees()` without requesting provider data or saving preferences.

`selectFees(preferences)` applies the runtime coordinator selection immediately and
queues persistence on a Host-owned worker in FIFO order. Only fee-preference writes
become asynchronous; price persistence is unchanged. Write failures emit `qWarning`
and keep the current runtime choice, never rolling back a newer selection. The old
saved preference may remain until a later selection is saved successfully. Host
drains accepted writes on application quit and in its destructor.

Legacy preferences migrate only when deserializing persisted state. An explicit new
`source` wins, including an empty string. Without it, Default keeps `default_source`
and Smart keeps `smart_source`. Manual keeps the sole nonempty source, or the common
source when both are equal; different nonempty sources leave no selection. Provider
availability never determines migration. Old stored tiers are ignored, and subsequent
writes contain only mode and source.

New Send transactions follow their selected recommendation while editable. A manual
rate or absolute-fee override stops following. Each new Smart Send starts at Medium.
Low/Medium/Fast is a per-Send choice, not a global preference; recommendation updates
do not reset that local tier. Missing recommendations fall back to
Manual at 1 sat/vB, even after recovery; a new transaction can use the configured
preference again. Prepared/signing/restored transactions keep their actual fees.
Positive fractional rates are supported at milli-sat/vB precision by both builders.

The coordinator separately calls `requestRates(codes)` for the initial quotes. Reply
with `conversionRate(req_id, currency, value)`, in fiat units per BTC. Both synchronous
and asynchronous replies work. Subsequent quotes are pushed by the feed; the host
does not poll or expire them. Non-positive or non-finite quotes withdraw that
currency's rate. An empty currency list withdraws the price capability. Null,
unimplemented and default inert price interfaces are ignored.

For example, a provider backed by its own non-blocking transport can implement:

```cpp
auto Prices::currencies() -> ReqId {
    auto reqId = nextRequest();
    emit IFeed::currencies(reqId, m_offered_currencies);
    return reqId;
}

auto Prices::requestRates(const QStringList &currencies) -> ReqId {
    auto reqId = nextRequest();
    m_transport->requestPrices(reqId, currencies);
    return reqId;
}

void Prices::onQuote(ReqId req_id, const QString &currency, double value) {
    emit conversionRate(req_id, currency, value);
}
```

Caches are separate per source, even for the same currency and colliding request
ids. Source IDs are compact JSON arrays of plugin ID, module ID and registered
instance ID. These IDs must remain stable across plugin reloads. Library paths,
hashes, versions and pointers are not identities. Removing an instance disconnects
its feed and clears its cache before stopping it. A reload starts with fresh quotes.
Cached currency codes and the retained selection own their string storage, so plugin
literals do not leave references into an unloaded library.
Price signals first enter a per-registration host receiver through direct named
slots. It copies the list and every string while the emitter is still running;
only owned payloads and that registration's ID cross the queued connection to the
coordinator. Stale registrations are rejected before their payloads are inspected,
including when the same feed QObject is registered again. Removal disconnects the
receiver and retains it until `IInstance::stop()` returns. Stop must join all feed
emitters before returning, so no plugin code is running when its library unloads.

Global Settings offers explicit source and currency choices only while a provider
advertises currencies. Neither is selected automatically. The encrypted global app
state retains the selection even if the provider disappears or stops offering that
currency. There is no fallback to another source or a fixed price.

Consumers use cached `Host::selectedPrice()` and listen for
`HostEvents::selectedPriceChanged()`:

```cpp
connect(Host::get()->events(), &HostEvents::selectedPriceChanged,
        this, &MyWidget::onPriceChanged, qontrol::UNIQUE);
onPriceChanged();

void MyWidget::onPriceChanged() {
    auto price = Host::get()->selectedPrice();
    m_fiat->setVisible(price.rate.has_value());
    if (price.rate.has_value()) {
        m_fiat->setText(QStringLiteral("%1 %2")
                           .arg(QString::number(m_btc * price.rate.value(), 'f', 2),
                                price.currency));
    }
}
```

Price changes are display-only. In Send, only an explicit fiat input edit converts
fiat to BTC. Refreshing a rate never changes a BTC payment, MAX, a fee, or a locked
or restored PSBT. Unavailable prices hide fiat displays and fiat entry.

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

An account never picks a signer. It calls `Host::get()->keys(module)->sign(...)` with the PSBT
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

## Account UI Services

The generic account UI reads an account through the headers under `interfaces/backend/`,
apart from the public `IAccount` other accounts use, which they leave unchanged:

| Header          | Interface                                                           | IID                                       |
|-----------------|---------------------------------------------------------------------|-------------------------------------------|
| `types.h`       | Payload structs, `backend::Network`, `backend::RequestId`           |                                           |
| `service.h`     | `IService`: byte (`raw`) and Qt JSON (`value`) extensions, `error()` | none, base class only                     |
| `account.h`     | `IAccountBackend`: identity, receiving, backup and restore          | `dev.silent.backend.IAccountBackend/1.0`  |
| `blockchain.h`  | `IBlockchain`: scanning, coins, history, broadcast                  | `dev.silent.backend.IBlockchain/1.0`      |
| `transaction.h` | `ITransaction`: crafting, unsigned PSBTs, coin selection            | `dev.silent.backend.ITransaction/1.0`     |
| `labels.h`      | `ILabels`                                                           | `dev.silent.backend.ILabels/1.0`          |
| `settings.h`    | `IBackendSettings`: network and endpoints                           | `dev.silent.backend.IBackendSettings/1.0` |
| `provider.h`    | `IServiceProvider`: optional `IModule` facet supplying the others   | `dev.silent.backend.IServiceProvider/1.0` |

Getters read local state under short locks: no network I/O, persistence or worker join.
Commands are slots taking a `RequestId` scoped to the receiving object. They return at once
and answer with their result signal or `error()`, possibly inline, so connect and record the
request before calling. An unsupported request answers `error()`, never nothing.
Notifications (`historyChanged()`, ...) only say state changed: polling the same getters works
without subscribing.

The moc code of these classes is compiled into the host and exported with the other
interfaces (`--export-dynamic` on Linux, the curated export table on Windows). A plugin
compiles the headers without running moc on them. The host registers every payload type for
queued delivery before any module activates.

### Supplying services

A module supplies services by also implementing `backend::IServiceProvider`, declared with
`Q_INTERFACES`:

```cpp
class ChainModule final : public IModule, public backend::IServiceProvider {
    Q_OBJECT
    Q_INTERFACES(backend::IServiceProvider)

public:
    auto service(const QString &account, backend::Network network, backend::ServiceKind kind)
        -> backend::IService * override {
        if (kind != backend::ServiceKind::Blockchain) {
            return nullptr; // this module supplies the blockchain only
        }
        return new ChainBlockchain(account, network, this);
    }

    void releaseService(backend::IService *service) override {
        service->deleteLater();
    }
    // meta(), list(), autoStart(), startInstance(), shutdown() as for any module.
};

class ChainBlockchain final : public backend::IBlockchain {
    Q_OBJECT
    Q_INTERFACES(backend::IBlockchain)
    // getters, slots, raw() and value()
};
```

- `service()` answers this provider's service of one kind for an account (its instance id) on
  a network, or null. GUI thread, no I/O. Each non-null answer is handed back exactly once
  through `releaseService()`, which destroys it, at once or later (`deleteLater()`, a timer):
  the plugin is not unloaded while a service it was handed back still lives.
- Each service class declares `Q_INTERFACES` for its interface. The host checks the kind and
  its IID version with `qobject_cast`, so a service built against another version is refused.
- The facet is optional. `IPlugin`, `IModule`, `IInstance` and the root plugin IID are
  unchanged: a module without the facet supplies nothing.
- Values, results and events own their data. Never hand out a `QStringLiteral` or other data
  borrowed from the library: a consumer may keep it.

### Assembly

The host assembles the services of one account UI with `HostImpl::assembleServices()`. The
request names the account, its network and, for each of the five services, its source: a
provider module, or `host::Unsupported`, the host's explicit implementation whose getters
answer empty data (the account id and network for identity) and whose requests answer
`error()`. Sources are independent: the account from its own module, the blockchain from
another plugin, the settings from a built-in module.

Assembly is all or nothing. It is refused, handing back every service already obtained, when a
source is not an active module or is stopping, does not implement the facet, supplies no
service of that kind or one of another interface version, or when the account service reports
another account id or network. A missing provider is never replaced, neither by another one
nor by the unsupported implementation.

The result attributes each service to its supplier: `supplier(kind)` gives the plugin id,
module id, external flag and library hash the registry holds, empty for the unsupported
implementation. Every getter answers a live object. Providers own their services; the
assembly owns only the unsupported ones and never stops a provider.

### Retention

A supplying plugin stays loaded while an assembly uses its services. When it starts stopping
(disable or remove), every assembly using it emits `retired()`. The consumer then drops every
use of the services and deletes the assembly (`deleteLater()` is fine), which hands each
service back to its provider. The registry asks the plugin's modules to shut down only once no
assembly using them is left, a deferred deletion included, so a provider's `shutdown()` never
runs with services still out. Its library is unloaded only once every service it was handed
back is destroyed too: a provider may delete one later than its release. The other providers
of a retired assembly keep running.

`host_smoke --account-services` loads two provider plugins built against the SDK only
(`tests/host/ServiceProviderPlugin.cpp`, with the guard headers that fail on generated
Rust/CXX includes). It checks one-provider and split-provider assembly, attribution, the
unsupported implementation, refusals and retention through a plugin stop, a service its
provider deletes a while after its release included.

### Built-in providers

The SP and Multikey modules supply all five services of their open accounts (the shared
`account::AccountModule` implements the facet). `service()` answers null for an account that
is not open, still loading, being deleted, or on another network: a Rust network has no
Testnet4, so no built-in account is supplied on it. Each service is its own QObject over the
account's `account::ServiceContext`, which the account controller owns:

- The context publishes the coins, balance, history, labels, receive addresses, connection
  state and endpoint settings as the controller changes them, whether or not anyone listens.
  Getters copy them under a short lock and never reach the Rust account. The funds are read
  and converted on a worker the controller waits for before it drops the account, one read
  at a time (a change during a read is read again after it), and published on the GUI
  thread. Every getter and request of a built-in service is GUI thread only.
- A coin is spendable when a spend path can spend it now: a Multikey coin whose only paths
  are timelocked is not, until the validated tip meets the lock.
- A coin may be published before the transaction funding it is written (the pinned bwk SP
  scanner notifies coins first). Such a read is not taken as complete: the context reads the
  funds again every second, for up to two minutes or until the history covers every coin,
  and publishes the history before `historyChanged()`.
- `craft()` and `finalize()` run on RAM state. Without inputs a template stays incomplete and
  `finalize()` builds nothing: only `selectCoins()` picks coins. A complete crafted template
  states the whole transaction: its inputs, the MAX amount resolved and the absolute fee it
  pays (`fee`, which overrides `feeRate`). A Multikey PSBT pays change to the change index
  after the registered tip and leaves it unregistered; the signing owner
  (`MusigAccount::adopt_transaction`, which the Multikey Send step calls with the bytes of
  `finalize()`) registers it durably when it takes that exact PSBT, and refuses it with a
  craft-again error when another adoption took the index first, or when the PSBT does not pay
  its template: every recipient its amount (a MAX one what is left) and nothing else but the
  account's change, which must carry the derivation fields of its index (internal key, key
  origins, tap tree, MuSig participants) so signers can verify it.
- `setLabel()` with an empty text removes the label. A save failing after the RAM edit
  publishes the edit (`labelsChanged()`) and answers `error()`, not `saved()`.
- `broadcast()` takes consensus-encoded transaction bytes. It answers `Rejected` when the
  transaction never reached a server or the server refused it, `Unknown` when it was sent with
  no usable answer, and `Accepted` with warnings when recording it locally then failed. A
  server answering that it already holds the transaction accepts it. A transaction never sent
  says so in its error, with the `InvalidInput` or `Network` kind, never `Broadcast`. A
  Multikey broadcast runs through the account's signing owner, behind its saved progress
  writes: it is refused while a reset, a restart, an endpoint change or another broadcast
  runs, and the finalized transaction in flight, once accepted, ends its signing session.
- `testEndpoint()` saves nothing and leaves the live connection alone; `save()` writes the
  endpoints to the account config untested, an endpoint just tested on the transport its
  test found. Both count as an endpoint operation while they run, and both answer `error()`
  while another runs or while the account would refuse an endpoint change.
  `IBlockchain::setConnected()`, `start()` and `stop()` drive the live connections.
- `backup()` seals the account's backup under the password and `inspectBackup()` reads one,
  both on a worker (see Backup and restore below). `restore()` on an open account answers
  `error()`: a restore makes a new account, through the module's restore target.
- `rescan()` and every `raw()` operation answer `error()` for now. The
  `value()` operations below are the only ones the built-in services answer; each is listed
  in `src/account/Extensions.h`:

| Service            | Operation                        | Payload        | Answer                                                         |
|--------------------|----------------------------------|----------------|----------------------------------------------------------------|
| `IBlockchain`      | `silent.transactionExtras`       | txid           | `{version, lockTime, chainTip?, scripts}` of a fetched transaction, `scripts` by output index for outputs paying no address |
| `IBackendSettings` | `silent.endpointCapabilities`    | endpoint id    | the BlindBit server's `{tweaksOnly, tweaksFullBasic, tweaksFullWithDustFilter, tweaksCutThroughWithDustFilter}` |
| `IBackendSettings` | `silent.endpointsBusy`           | none           | whether an endpoint operation runs; also sent as a `valueEvent()` of that name when it changes |
| `IBackendSettings` | `silent.endpointPresentation`    | none           | per endpoint id: `{label, placeholder, connect, disconnect}` texts in the current language, `transport` (SSL and certificate toggles), `capabilities` (`silent.endpointCapabilities` under its row) |
| `IAccountBackend`  | `silent.addressKindTitles`       | none           | per address kind id: the title of its list of addresses awaiting payment, in the current language |

`finalize()` returns no reason: a built-in provider whose `finalize()` builds nothing logs why
and reports it as `error()` with request id 0, during the call. The kinds' names are
published again when the language changes.

`host_smoke --backend-services` assembles both providers over Regtest fixture accounts and
checks a history published after its coin through the same getters for a polling and a
notified consumer.

### The shared account views

The account panels (`src/account/views/`: History, Coins, transaction details, Receive, Send
and Settings) take an `account::Services`, one pointer per interface, and use nothing else of
the account: no Rust account, controller or worker, nor any header that reaches the generated
`silent.h`. They show their modals with `qontrol::Controller::execModal()`. The views name no
backend: what differs between providers comes from the provider (address kinds and their
titles, endpoints and their presentation), and the amount and fee rules from
`account/Money.h`.

- History, Coins and the transaction details read `IBlockchain` and refresh on its change
  signals. The details show at once what the account already fetched of the transaction,
  loading otherwise, then fetch it again; the version and lock time come from
  `silent.transactionExtras` when the provider answers it.
- Receive lists `addressKinds()`, titled by their names: a reusable kind shows its address
  from `addresses()`, the others are generated with `newAddress()`.
- Settings lists the endpoints of `IBackendSettings::settings()`, shown as the provider
  presents them. Test runs `testEndpoint()`, saves the tested endpoint alone with `save()`,
  then disconnects it with `IBlockchain::setConnected()`, so the account reopens on it. A
  built-in account refuses the test and the save up front while it would refuse that
  disconnect (a Multikey signing step running), so nothing is saved it cannot reopen on.
  Connect and Disconnect and the heights use `IBlockchain`, the capabilities and the busy
  state the extensions above. Heights and capabilities are asked again when a connection or a
  saved endpoint changes, not on scan or tip updates.
- Send crafts the template its payments, fee and coins form: `craft()` over coins picked by
  hand, `selectCoins()` otherwise (an older answer is dropped). The account's signing step
  (`account::SendSigning`, the module's own) takes the crafted template, finalizes it with
  `finalize()` and signs through the keys module: an SP account at once, a Multikey account
  through its signing owner, which adopts the PSBT and keeps the saved PSBTs, Reset and the
  nonce allocation. The signed transaction goes out with `IBlockchain::broadcast()`; only
  `Accepted` is shown sent. The step also exports the PSBT it would sign as base64 text
  (a `.psbt` file or the clipboard, `account::ExportPsbt`) and imports one
  (`account::ImportPsbt`) through the account itself, not a service: an SP account checks
  that it spends only its unspent coins and shows it in Send as if crafted there
  (`restoreTransaction()`), Sign signing it; a Multikey account saves it among its saved
  PSBTs once it spends the account's coins on one spend path, or merges it into the saved
  PSBT of the same transaction.

- Settings ends with Backup, which opens `account::view::BackupAccount` over
  `IAccountBackend` (see Backup and restore).

### The account shell

`account::AccountShell` (`src/account/AccountShell.h`) is the account page every account kind
shares: the menu, the status bar and the panels. It is UI composition, not a plugin
interface, and depends on no controller nor on `account::AccountBackend`.

- `assemble()` takes a `host::AssemblyRequest`: each service from its own source, which the
  shell neither owns nor parents. It keeps the assembly, and only that. The built-in
  controllers name their own module for all five (`account::moduleServices()`); another
  account kind may name several providers.
- `openSettings()` builds Settings over the services, with the module's own widget under the
  endpoints (a Multikey account's policy and descriptor); `openFunds()` builds History,
  Coins, Receive and Send, Send signing with the module's `account::SendSigning`.
  `insertPanel()` adds a panel of the module's own, named as its menu entry (the Multikey
  PSBT tab). The module decides when each is built: the built-in ones build Settings once
  the account has loaded, the funds panels once its first funds read has answered, so none
  opens on an empty read.
- The menu entries are `account::accountMenuEntries()`, plus the module's own. The status bar
  is the shell's (a top separator, its height and padding), the details in it the module's:
  the SP status bar (BlindBit scanning, Electrum) and the Multikey one (Electrum) read their
  controller, which no domain interface carries.
- `showPanel()` shows a panel at once or as soon as it is built.
- When a supplier retires the assembly, the shell deletes every panel, then the assembly
  (`deleteLater()`), and assembles nothing again. Deleting the shell deletes the panels first
  too; the built-in pages delete it before their controller, which the signing steps use.

`account::AccountBackend` remains the built-in controllers' own base, inside the SP and
Multikey modules: the account lifecycle `account::AccountModule` watches, the endpoint
connections their status details drive and read, and the requests behind
`account::ControllerServices`. Neither the shell nor the views use it.

### Backup and restore

A backup is opaque bytes: the generic flows only hand them over. `IAccountBackend::backup()`
seals one under a password, `inspectBackup()` answers what it holds (`BackupInfo`: the kind of
account, its id and network, what it includes, what a restore needs, warnings) and
`restore()` makes the account it holds. A password lives in the generic UI only from its entry
to the request that takes it, never logged nor saved.

- `account::view::BackupAccount` (from Settings): a password and its confirmation, then
  `backup()`, then the file the user picks (`.silent-backup`).
- `account::view::RestoreAccount`: the backup file and its password, then `inspectBackup()`,
  reviewed, then `restore()`. It runs over a module's restore target: an `IAccountBackend`
  with no account behind it, so no account is opened nor scanned to restore one. A restore
  made through an open account's own service answers `error()`.
- The restore is the module's: each built-in module lists `<id>.restore` among its creators
  (`sp.restore` in the SP module, `musig.restore` in the Multikey creator), which the account
  type picker shows with the create entries. `PluginRegistry::isCreatorInstanceId()` takes
  `.restore` ids as creators. Its restore target is `account::restoreTarget()`; once restored,
  the module refreshes the launcher, which lists the account.

The built-in backups (`silent/src/account_backup.rs`) are `SILENT-ACCOUNT-BACKUP` then the
JSON of a password envelope: Argon2id and XChaCha20-Poly1305, as hot keys seal their secret
(`keys::crypto`), bound to this use. They hold:

| Covered                  | In the backup                                                     |
|--------------------------|-------------------------------------------------------------------|
| Config                   | the descriptors (an SP one with its scan key), servers, dust limit |
| Derivation state         | the receive and change indexes each descriptor handed out          |
| SP scan start            | the block the account scans from                                   |
| Labels                   | every label, RAM edits included                                    |

and leave out, as `BackupInfo` says:

- the signing keys: they stay in Keys, which the restored account needs (`requirements`
  names their fingerprints);
- coins and history: the restored account finds them again by scanning;
- a Multikey account's saved PSBTs and signing sessions (`warnings`): restoring them could
  bring back a signing round whose nonces a signer already used.

A wrong password and a damaged ciphertext are refused alike (`BackupFailure::WrongPassword`:
the authentication cannot tell them apart), a file that is no backup as `Malformed`, a newer
backup version or another kind of account as `Unsupported`. A restore never replaces an
account: one of the same name refuses it (`Exists`). It creates the account as account
creation does, then puts the indexes and labels in it; when that fails, the account it created
is removed, so nothing partial is listed. A restored SP account scans from the block every new
one does: a backup recording another start is refused rather than scanned from the wrong
block. bwk does not expose the scan start it persisted, so a backup records the one the
account's config opened with.

`host_smoke --account-shell` checks the shell over services of two providers and its
retirement, the Backup modal's password rules and refusal, and over the real SP and Multikey
accounts a backup through the account service, a wrong password, another kind's restore
target, an existing account, and a restore once the account is gone, which then opens.
`silent/tests/account_backup.rs` checks both kinds' roundtrips, malformed backups, wrong
passwords, a newer backup version, existing accounts, another kind, and a restore failing
once the account is created, which leaves no account.

## Inter-Account Operation: Send To / Spend From Siblings

A key use case is moving funds between sibling accounts (e.g. a consolidation or
swap plugin). Accounts export spending data; external coordination signs through
the caller-scoped `IKeys` from `Host::keys(this)`, not through account controllers.

- **Send TO a sibling**: pick a recipient instance from `Host::get()->instances(this)`
  (gated on `account()->implemented()`), call `account()->newReceiveAddress()`, and
  read the address off its `receiveAddress(ReqId, ...)` signal.
- **Spend FROM siblings**: request their coins, choose one variant per input,
  assemble a PSBTv2, and call `IKeys::coordinate`. Finalize and submit through
  `Host::transactions(this)`. These operations take no account owner pointer.
  Coordination PSBTs are hex-encoded PSBTv2, not base64 PSBTv0.

### Public spending exports

`IAccount::coins()` returns typed network and status, optional confirmation height,
and independent `SpendVariant` values per coin. Persist the instance ID, outpoint
and `pathId`, never an owning account pointer. Descriptor path IDs come from bwk's
underived policy and satisfaction alternative, not a derived tapleaf hash.
`keychain` and `derivationIndex` identify the concrete derivation separately.

Each `psbtV2Hex` is a construction envelope: one populated input and no outputs.
It carries the prevout, origins, scripts and required SP/MuSig metadata, but no
signature, nonce, signing-attempt ID or transaction-dependent SP contribution.
Decode each envelope, append its input to the new transaction, and merge global
maps by key. Equal key/value pairs may be shared; a key with different values is
an error. Do not combine the envelopes as complete PSBTs. Counts, transaction
version, nLockTime, per-input nSequence and outputs belong to the assembled PSBT.

`descriptor` contains the full public Miniscript policy for registration. For SP,
it is empty and `spSpendKey` identifies the public spend policy instead. This is
not an `sp()` descriptor and must not be passed to a descriptor parser. The scan
secret is never exported by `coins()`. Required keys retain origin/derivation
expressions. Preimage requirements use `sha256:`, `hash256:`, `ripemd160:` or
`hash160:` followed by the digest, not the secret preimage.
Before coordination, the builder may supply matching preimages, a full previous
transaction matching the exported prevout, and a sighash type where the envelope
does not prescribe one. Hashes and prevouts are checked. These additions become
immutable request metadata; a signer response cannot replace them.

Absolute locks distinguish heights from Unix times. Relative locks distinguish
blocks from 512-second units; convert these units to the BIP68 sequence encoding.
CSV requires transaction version at least 2. CLTV requires a non-final input
sequence and a compatible transaction nLockTime. Incompatible absolute units
cannot share a transaction. Relative units are independent between inputs.

`satisfactionWu` is a conservative added weight in weight units over an input
with empty scriptSig/witness, including witness count, scripts and control block.
It excludes the base input, transaction marker/flag and outputs. It allows a
non-default sighash byte, not an annex. Alternative satisfactions within one
script use that script's maximum, not a guessed minimum.

`maturity` currently describes timelocks only. Without a coherent scanner
tip/hash/MTP snapshot, locked variants report `Unknown`; unlocked variants report
`Mature`. This does not assert coinbase maturity, unspent status or signer
availability. `availability` is separate: spent/being-spent inputs are `Spent`,
known non-coinbase inputs with mature locks are `Available`, immature locks are
`Immature`, and otherwise it is `Unknown`. Coinbase maturity is not guessed from
wall-clock time or a stale height. Spent and offline-signable variants remain in the export. The
account's normal coin-update signal refreshes exports, including after reorgs.
An export failure answers `error`, never a successful empty list.
Legacy non-Segwit coins are unsupported when the full previous transaction is
unavailable; a witness-UTXO-only template is not exported as a complete legacy
construction envelope.

### Signing and contribution stages

Allocate `newRequest()` before calling `IKeys::coordinate(reqId, request)`; connect
named slots first because some answers are synchronous. `SigningRequest` carries
the explicit network, PSBTv2 and one `SelectedInput` per input. Each selection has
its input index, network and exported variant. Multiple descriptors and independent
paths are allowed. The optional fingerprint selects one key instead of opening
the picker. Policy/path IDs, derivations, input metadata and typed locks are checked
before signing. A variant within a script with multiple satisfactions remains
exported, but current signing routes report `Unsupported` rather than pretending
to enforce that alternative.

| Stage | Permitted result changes |
|---|---|
| `Signatures` | Add ordinary signatures only. MuSig inputs require their explicit rounds. |
| `MusigNonces` | Add public nonces and ordinary signatures; never MuSig partial signatures. |
| `MusigSignatures` | Add signatures/partials; nonce maps and attempt identity cannot change. |
| `SpContributions` | Independent share/proof production is explicitly unsupported by current routes. |
| `VerifySpContributions` | Read-only verification of present DLEQ proofs; reports partial coverage or complete coverage. |
| `CompleteSpOutputs` | Verify shares/proofs, fill missing SP scripts, freeze transaction modification flags. |
| `SpCompleteAndSign` | Supported combined single-key SP route: add shares/proofs, fill missing SP scripts and add signatures. |
| `Release` | Retire local allocations, clear MuSig round data and return a fresh attempt identity. |

Transaction version, locks, input/output order, prevouts, amounts, existing scripts,
policy metadata and existing signatures cannot change in a signer response. Only
the combined SP stage may fill previously absent SP scripts, verified against its
shares/proofs. Existing merge/integrity checks reject substitutions and conflicting
nonce data. Every received public nonce is parsed as curve points using bwk's
NonceAgg backend, even before other nonces arrive. Every present MuSig partial is
verified with bwk's partial-verification helper against the complete nonce
transcript, policy tweaks and slot sighash. Missing or invalid transcript data is
an integrity error, not successful partial progress. Complete outputs before ordinary/MuSig signing. Changing a transaction
requires releasing its old attempt and starting again, not editing nonce-bearing
PSBTs. A nonce stage can also collect ordinary signatures for mixed inputs.

`coordinated` is a successful operation, not necessarily a completed transaction:
`Partial` means more contributions are needed; `StageComplete` completes only the
nonce/proof/output/release stage; `Complete` means aggregation and finalization
checks succeeded. Finalization errors other than missing satisfactions are errors,
not partial success. `coordinationError` distinguishes unsupported requests, invalid
requests, integrity failures, lost sessions and permission failures. Multiple SP
owners and combined SP/MuSig signing are currently unsupported. The combined SP
route requires one identifiable signing fingerprint; that is a backend limitation,
not a restriction on the request types. A nonce request with one MuSig policy
already ready for signatures and another still missing nonces is also
`Unsupported`: current routes cannot honor that stage without potentially
consuming the ready policy's nonce. This refusal happens before signer selection,
cache allocation or a password/device interaction, not after a signer response.

Runtime `sessionId` identifies a MuSig attempt and is also carried in its PSBT.
Persist both if retaining progress. A supplied session ID must match the PSBT.
It never appears in reusable coin templates. Session loss requires `Release` and
fresh nonces, not retrying a partial signature with changed transaction data.

`coordinationStarted` is nonterminal: the request passed preparation and a route
may allocate nonces. The caller handle retains that initial snapshot, then updates
it only with successfully validated progress. Preparation refusals and failed
Release requests never erase the last valid snapshot. A request error alone does
not confirm cleanup: `attemptRetirement` reports `Pending`, `Retired` or `Failed`
separately. Post-route failures start cleanup; only its confirmed `Retired` result
or completed signing drops the retained attempt. Failed cleanup is reported and
keeps the valid snapshot for retry. Caller teardown keeps the handle host-owned
until cleanup succeeds; a cleanup failure is logged and retained, not deleted
along with the unloaded caller.

### Finalization, broadcast and lifetime

`ITransactions::finalize` checks the selected paths, verifies SP proofs, aggregates
MuSig partial signatures, finalizes all inputs and returns PSBTv2, transaction hex
and txid. It does not require the inputs to belong to one account.
`broadcast(reqId, network, endpoint, txHex)` checks the endpoint's genesis header
and submits on that same connection, checking the returned txid. It uses no account
bookkeeping; scanners discover the spend independently. Both run off the GUI thread
and use the caller's account grant. Signing uses the keys grant.

Handles allocate and correlate their own requests. A caller cannot submit another
caller's keys request. An in-flight keys request cannot be submitted twice.
Transaction handles ignore duplicate in-flight submissions, including a different
operation using the same ID, without emitting a competing terminal error.
Subsequent newly allocated IDs work normally. Account
replies are consumed once, including synchronous replies held during forwarding.
Permission is checked at submission and before delivering private results.

`cancelRequest` immediately suppresses late replies. Already-running I/O may finish;
a broadcast already sent cannot be recalled. Cancelling signing starts durable
nonce retirement in the background. Use a `Release` request when the coordinator
needs acknowledgement of retirement before restarting. Caller removal retires
outstanding requests and retained MuSig attempts. Removing the keys service reports
lost sessions. Account removal cancels pending account replies and emits the host's
`instanceRemoved`; discard that instance handle and rediscover by its stable ID.
Exported value snapshots remain usable without the account. Account removal does
not cancel an independent transaction service operation.

### Conformance checks

`silent/tests/external_spend.rs` composes independently exported templates, rejects
global-map conflicts and incompatible typed locks, signs real funded BIP32/Multikey
inputs with hot keys, and exercises MuSig rounds, SP proofs, SP input/output spending
and a reorg. It checks malformed nonce points and an invalid first partial before
the other cosigner contributes. It uses public bridge operations, not private wallet stores.
`tests/host/ExternalSpendConsumer.*` imports only public interfaces and Qt. It
discovers those funded accounts through Host, signs through the real IKeys hot
route, finalizes/submits through ITransactions, and checks denied access, duplicate
in-flight requests and late reply suppression. A second real-IKeys scenario checks
pre-route refusal of unequal nonce progress with cached A nonces and a
password-needed B policy, followed by successful use of the unconsumed nonces in
the Rust signing test. This does not claim that the public IKeys route supports
that unequal-progress combination. The host smoke executable supplies normal module startup.

The real-IKeys lifecycle checks allocate nonce progress through the hot route,
request a premature signature round, then run the same caller teardown path used
by plugin unloading. The Rust driver verifies that the old allocation can no
longer publish nonces. A second case sends an invalid Release, checks its explicit
cleanup-failure status, retries with the retained valid snapshot and verifies
retirement. The consumer uses public interfaces; host/registry startup and caller
teardown belong to the test driver.

Run the combined test against an already-built host smoke executable:

```sh
SILENT_HOST_SMOKE="$PWD/build/host_smoke" cargo test --manifest-path silent/Cargo.toml \
  --test external_spend -- --include-ignored
```

Bitcoin Core, electrs and the existing BlindBit test harness are required. No device
is used. Without `SILENT_HOST_SMOKE`, the Rust public-interface tests still run but
the Qt consumer is not launched. Deferred device mocks explicitly reject coordination;
they are not evidence of a supported funded signing combination.

## ABI & Linking Rules (Plugin Authors Must Read)

The current plugin IID is `dev.silent.IPlugin/10.0`, the module IID is
`dev.silent.IModule/3.0`, the signer IID is
`dev.silent.ISigner/3.0`, the account IID is
`dev.silent.IAccount/3.0`, the keys IID is `dev.silent.IKeys/3.0`, and the transaction
service IID is `dev.silent.ITransactions/2.0`. The feed IID remains
`dev.silent.IFeed/3.0`. The account UI services carry their own IIDs (see Account UI
Services) and change no existing one. Public Keys and spending enums are SDK-owned, not generated
Rust types. Root ABI 10 changes `FeePreferences` to mode and one source, removing the
global tier. The `IFeed` methods, signals and IID are unchanged. Rebuild plugins
against this checkout and use `SILENT_IPlugin_IID` in plugin metadata. Include both
`src` only for public interfaces; generated `lib/include` and CXX headers are not needed.
Only the exact root IID is accepted. Old 1.0 to 9.0 plugins and future 11.0 plugins
are refused rather than called through incompatible vtables.

`interfaces/enums.h` defines the shared `plugin::Network`, `plugin::KeyRouteKind`
and spending/coordination enums. Network wire integers are Bitcoin=0, Testnet=1,
Signet=2, Regtest=3. Route integers are Unknown=0, Hot=1, Usb=2, Qr=3, SdCard=4.
Unknown means no configured route, not an unrecognized protocol value. The other
wire integers are explicit in that header. Encode with `plugin::enumToWire()` and
decode unsigned 32-bit integers with `plugin::enumFromWire<Enum>()`. Both return
`std::nullopt` on invalid values; a transport must reject these as protocol errors,
not substitute a default or narrow an integer before decoding. Serialize integers
with the transport's integer encoding, never enum object bytes. Keys rejects invalid
networks with `SignError::InvalidRequest`; synchronous lookups return that typed
failure in `KeyListResult`. Coordination and transaction services use `SpendError::InvalidRequest`.
Bridge conversions are internal. This contract is transport-ready, not OS sandboxing.

`IKeys::keys()` returns `KeyListResult`, not an ambiguous empty list. Its non-virtual
public entry point validates both route fields with the pure public
`plugin::validateKeyEntry()` before returning any records. Providers implement the
protected `readKeys()` hook. A malformed record rejects the entire list with
`SignError::InvalidRequest`; `entries` is empty on failure. Valid `Unknown` still
means no configured route. Consumers must check `is_ok` and present `error` rather
than rendering failure as ordinary no-route metadata. Caller handles recheck
metadata before signing, coordination or registration dispatch.

The independent check needs Qt Core and a C++20 compiler, no Rust build:
`cmake -S tests/interfaces -B <build-dir>`, then build and run CTest in that directory.
Its include guards deliberately fail if public headers include generated Rust/CXX headers.

Silent statically links Qt, and the host binary acts as the **single Qt provider**:
plugins link **no Qt of their own** and resolve Qt symbols against the host at load
time. This keeps one Qt in the process so signals, meta-types, and singletons stay
unified. Consequences:

- **Toolchain lock.** A plugin must be built against the **same Qt version and a
  compatible compiler ABI** as the host. Before loading code, Silent inspects native
  ELF, PE or Mach-O metadata sections from the same bytes it hashes and loads.
  Admission requires the current root plugin IID, Qt metadata format 1, the host's
  Qt major and no newer Qt minor, supported CPU requirements, and on Windows a
  matching debug/release build. This cannot verify the compiler ABI or prevent
  crashes from a plugin that lies about compatibility. `qobject_cast<IPlugin *>`
  remains a second gate before interface calls. In-process admission is not a sandbox.
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

### Non-executing import admission

Rust importers use `plugin_admission::inspect(library_bytes, &host, Some(&manifest))`
before replacing an installed bundle. `Compatibility` describes the root `iid`,
`target_os` (`linux`, `windows`, `macos`), `target_arch` (`x86_64`, `aarch64`, `x86`),
Qt major/minor and Qt `arch_requirements` byte (CPU level plus debug bit).
The host supplies its supported build requirements; the manifest describes the
plugin's actual requirements. The returned metadata must agree with every manifest
field, not just be compatible with the host. Release/display and bundle-format
versions are not ABI declarations. This API does no I/O, loading or construction,
and returns no Qt plugin objects or host-bound user identifiers. The loader's CXX
entry point delegates to the same reader. ZIP import validates the manifest and
passes the declared compatibility to this reader before encrypted replacement.
The encrypted installed-bundle reader is unchanged.

Inspection bounds library size to 256 MiB, metadata sections to 1 MiB and native
sections to 4096. Mach-O universal libraries select exactly one matching host slice.
Missing, malformed and ambiguous metadata fail before loading. Marker text and
complete records nested inside a CBOR value remain data, not independent records.
Metadata must agree with the image's physical and virtual mappings, flags and
alignment. An ELF Qt note must start at an actual `PT_NOTE` record boundary, not
inside another note's descriptor. PE section mappings must fit the image/header
bounds without physical or virtual overlaps. Mach-O sections must agree with their
declaring load-command segments. Appending a current metadata record cannot rescue
an inconsistent section pointer or replace the mapped metadata.
Conflicting page mappings, PE sub-page section alignment and unaligned Mach-O
segment starts are refused. A final Mach-O segment may have an unrounded size.
ELF mappings must be consistent at their declared alignment, at least 4 KiB
(64 KiB for AArch64); Mach-O uses 4 KiB on x86 and 16 KiB on AArch64.

Both `qt_plugin_instance` and `qt_plugin_query_metadata_v2` must be defined exports
into mapped executable function sections. ELF uses the `PT_DYNAMIC` symbol/string
bindings and runtime GNU/SysV hash lookup; an imported name is not a definition.
PE uses its export address/name tables; Mach-O uses its mapped dyld export trie.
Required PE forwarders, Mach-O reexports/resolvers, ELF IFUNCs and versioned ELF Qt
entry exports are unsupported and refused before loading. Mach-O files without a
dyld export trie are also refused. Export inspection allows 65536 symbols, with
1 MiB budgets for section/export names, ELF dynamic strings/hash windows and Mach-O tries.

GNU hash bloom counts must be nonzero powers of two. Bloom shifts are limited to
0 through 31 because GNU hashes are 32-bit on both ELF classes. The complete
bloom/bucket/chain layout is bounded by the actual dynamic-symbol count. Every
bucket start, chain terminator, symbol-name hash, bucket assignment and bloom
membership is checked, including buckets not used by the Qt entries. A match on
an early Qt symbol does not skip validation of the rest of the table. This is a
GNU-table check, not a complete ELF loader validator.

CBOR maps reject duplicate keys at every depth, including non-IID root fields,
nested maps and maps inside arrays or tags. Decoding is bounded to nesting depth 32,
16384 items (keys, values and tags included) and a 1 MiB input/string-byte budget.
Declared collection lengths do not cause unbounded preallocation.

The reported incompatible-plugin crash has not been reproduced: its binary and
backtrace were not supplied. Out-of-process isolation remains separate work.
These checks validate declarations and inspected native image ranges, not that
arbitrary native entry code will truthfully return that metadata or execute safely.

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
4. Ship one ZIP bundle with `manifest.json`, the declared `payload/` library and
   optional declared `i18n/*.lang` catalogs. Import this file through Settings;
   do not copy loose files into the installation directory.
5. The plugin installs disabled; enable it from global Settings.
