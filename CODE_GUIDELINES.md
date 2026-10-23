# CODE_GUIDELINES.md

Project-specific coding conventions and patterns for the Silent wallet.

## Rust: CXX Bridge Conventions (`silent/src/lib.rs`)

All FFI surface area lives in `lib.rs` inside the `#[cxx::bridge]` module.
Implementation goes in `account.rs` or `config.rs`.

**Shared types** use `#[derive(Debug, Clone)]` (structs) or `#[derive(Debug, Clone,
Copy)]` (enums). Prefix wrapper types with `Rust` when they mirror an internal type
(`RustCoin`, `RustTx`). Intermediate serde types use the `Internal` suffix
(`NetworkInternal`).

**Opaque types** (`Config`, `Account`, `Poll`, `PsbtResult`) are declared without
fields in the `extern "Rust"` block. Methods use explicit receiver syntax:
```rust
fn method(self: &Account) -> RetType;
fn method_mut(self: &mut Account) -> RetType;
```

**Error handling across the bridge:** never expose internal error types to C++.
Convert all errors to `String` at the FFI boundary:
```rust
pub fn method(&self) -> Result<String, String> {
    operation.map_err(|e| format!("context: {e}"))
}
```

**Option types** cannot cross the bridge. Use sentinel values (0 for `Option<u64>`)
and convert on the Rust side:
```rust
pub fn get_dust_limit(&self) -> u64 {
    self.dust_limit.unwrap_or(0)
}
pub fn set_dust_limit(&mut self, limit: u64) {
    self.dust_limit = if limit > 0 { Some(limit) } else { None };
}
```

**Sections** in `lib.rs` are delimited by comment headers for organization:
```rust
// ===== Shared Enums =====
// ===== Shared Structs =====
// ===== Opaque Rust Types =====
// ===== Utility Functions =====
// ===== Config Methods =====
// ===== Account Methods =====
```

## Rust: Error Handling

**Never return `Result` from Rust to C++ across the CXX FFI boundary.** CXX converts
`Result::Err` into a C++ exception, which we do not want. Instead:

- **Void operations:** return `bool` (`true` = success, `false` = failure). Log the
  error on the Rust side with `log::error!()`.
  ```rust
  // FFI function
  pub fn persist_config(config: &Config) -> bool {
      match config.write_file() {
          Ok(()) => true,
          Err(e) => { log::error!("failed to save: {e}"); false }
      }
  }
  ```

- **Operations with response data:** return a specific shared struct with `is_ok: bool`,
  `error: String`, and response fields. Do **not** create a generic `FfiResult` — each
  return type should be purpose-built.
  ```rust
  // In #[cxx::bridge]
  pub struct TxResult {
      pub is_ok: bool,
      pub error: String,
      pub value: String,
  }
  ```

- **Opaque types that can fail construction:** wrap an `Option<Inner>` and expose
  `is_ok()` / `get_error()` methods. Always return the type (never `Result`).
  ```rust
  pub struct Account {
      inner: Option<AccountInner>,
      error: String,
  }
  // FFI: fn new_account(name: String) -> Box<Account>;
  // C++: check account->is_ok() before use
  ```

- `Result` is fine for **internal Rust code** and tests — the restriction only applies
  to functions declared in the `extern "Rust"` block of the CXX bridge.
- File I/O never panics; log errors with `log::error!()` and return gracefully.
- `unreachable!()` branches on CXX enum conversions include a safety comment:
  ```rust
  _ => unreachable!("CXX enum type safety guarantees no other variants exist")
  ```

## Rust: Config File I/O (`silent/src/config.rs`)

Built-in configs are pretty-printed JSON files inside their module-owned logical
account subtree. See `docs/MODULES.md`, Account deletion and built-in logical persistence.

- Rust workers use #60's encrypted plugin tree, not native account directories.
- SP/MuSig wallet/header stores use the caller-owned bwk `BackendFactory`.
- No physical paths or application keys cross the public storage contract.
- `#[serde(skip)]` on runtime-only fields (`data_dir`).
- Custom serde module (`network_serde`) for CXX enums that don't derive Serde.
- Logical parent directories are created during account creation/opening.

## Rust: Notification Polling

The scanner runs in a background thread and sends `SpNotification` values through an
`mpsc::channel`. The C++ side polls via `try_recv()` which returns a `Box<Poll>`
wrapper — never blocks. The `Poll` type encapsulates Ok/None/Err states with
getter methods callable from C++.

## C++: Class Member Organization

**Section order in class declarations:**
1. `public:` — constructors, public methods
2. `signals:` — Qt signals (if applicable)
3. `public slots:` — Qt slot methods
4. `protected:` — overridden methods, then non-virtual methods (no private methods)
5. `private:` — member variables only (no methods)

**Private section contains only members, no methods.** Non-public helper methods go in
`protected`. This keeps the private section focused on data.

**Member variables are declared last within private.** Order logically by functionality,
not alphabetically.

## C++: Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Classes | PascalCase | `AccountController`, `MainWindow` |
| Methods | camelCase, trailing return type | `auto get() -> AppController *` |
| Local variables | camelBack | `coinState`, `urlRow`, `btnFont` |
| Parameters | lower_case | `tip_height`, `blind_bit_url` |
| Member variables | `m_` prefix + lower_case | `m_account`, `m_notif_timer` |
| Local constants | `c_` prefix + lower_case | `c_threshold` |
| Constants | UPPER_CASE (in `utils.h`) | `MARGIN`, `SATS`, `V_SPACER` |
| Signals | camelCase, descriptive noun/verb | `accountCreated`, `scanProgress` |
| Slot handlers | `on` prefix for signal handlers | `onBackendInfoReady`,
`onLabelEdited` |
| Action slots | imperative verb | `poll()`, `loadPanel()`, `sendTransaction()` |
| Screen classes | `screen::` namespace | `screen::Send`, `screen::Coins` |
| Modal classes | `modal::` namespace | `modal::SelectCoins`, `modal::CoinWidget` |

**Widget member suffixes:**
`m_*_btn` (QPushButton), `m_*_input` (QLineEdit/QTextEdit), `m_*_label` (QLabel),
`m_*_column` (qontrol::Column), `m_*_row` (qontrol::Row), `m_*_table`
(QTableWidget), `m_*_timer` (QTimer), `m_*_frame` (framed widgets).

**Signal/slot naming convention:**
- **Signals:** `camelCase`, no `on` prefix. Prefer event/state names (`coinsUpdated`,
  `feeRate`, `enabledPluginsChanged`).
- **Slots used as signal handlers:** prefix with `on` + source/event (`onThemeNames`,
  `onModuleInstances`, `onHostInstanceChanged`).
- **Slots that are user/actions (not signal handlers):** imperative verb
  (`refreshLauncherItems`, `sendTransaction`, `openAccount`).

**Local const usage policy:**
- **Do not declare routine local variables as `const`.**
- This includes scalar and value locals in function bodies (`bool`, `int`, `QString`, containers, ids, etc.).
- Keep function-local variables mutable and use `camelBack` naming.
- `const` is allowed only where required by API shape/read-only traversal semantics (for example: `for (const auto &item : items)`).
- Use named constants (`c_*` or shared constants) only when the value is truly a stable, intentional constant, not just a one-off local.

## C++: Clangd Lint Rules

The `.clangd` config enforces clang-tidy checks and `UnusedIncludes: Strict`. All
new code must satisfy these:

**Include hygiene:**
- Headers use `#pragma once` (not traditional include guards).
- Sort `#include` directives alphabetically within each block (enforced by
  `llvm-include-order`). Case-sensitive sort (uppercase before lowercase).
- Only include headers whose symbols are directly used in the file. clangd's
  `UnusedIncludes: Strict` flags unused includes.
- When a forward declaration (`class Foo;`) is sufficient (pointer/reference members
  or parameters), prefer it over `#include "Foo.h"` in headers. Include the full
  header in the `.cpp` file instead.

**`[[nodiscard]]`:** Mark `const` methods that return a value (especially `bool`) with
`[[nodiscard]]`:
```cpp
[[nodiscard]] auto isAccountOpen(const QString &name) const -> bool;
```

**QObject interface destructors:** For classes deriving from `QObject`, declare the
destructor as `~Type() override = default;` (not `virtual ~Type()`), so override intent
is explicit and clang-tidy-clean.

**Parameter naming in declarations/signatures:** Parameters must be `lower_case`,
including signal/slot signatures and interface declarations (`req_id`, `plugin_id`,
`instance_id`), not camelCase (`reqId`, `pluginId`, `instanceId`).

**Trailing return types:** Use trailing return type syntax for all functions with
non-void return types (enforced by `modernize-use-trailing-return-type`):
```cpp
auto main(int argc, char *argv[]) -> int;
auto getCoins() -> rust::Vec<RustCoin>;
```

**Void functions use traditional syntax.** Do not use trailing return type for void
functions — write `void fn()`, not `auto fn() -> void`:
```cpp
void applyRole();                          // good
auto applyRole() -> void;                  // bad
void Settings::actionSave() { ... }        // good
auto Settings::actionSave() -> void { ... } // bad
```

**Explicit nullptr comparisons:** Never rely on implicit pointer-to-bool conversion.
Use explicit comparisons (enforced by `readability-implicit-bool-conversion`):
```cpp
if (m_controller != nullptr) { ... }  // good
if (m_controller) { ... }             // bad
```

**Container emptiness:** Use `empty()` instead of `size() == 0` (enforced by
`readability-container-size-empty`):
```cpp
if (m_outputs.empty()) { ... }  // good
if (m_outputs.size() == 0) { ... }  // bad
```

**Prefer algorithms over manual loops:** Use `std::ranges::any_of`,
`std::ranges::sort`, etc. instead of hand-written loops when possible (enforced by
`readability-use-anyofallof`):
```cpp
return std::ranges::any_of(m_tabs, [&name](const auto &tab) { return tab.first == name; });
```

**Switch over enum if-else chains:** When handling `NotificationFlag` or other CXX
enums, prefer `switch` statements over long if-else chains. This reduces cognitive
complexity (enforced by `readability-function-cognitive-complexity`, threshold 25):
```cpp
switch (flag) {
case NotificationFlag::ScanProgress: { ... break; }
case NotificationFlag::NewOutput:
case NotificationFlag::OutputSpent:
    pollCoins();
    break;
default: break;
}
```

**One declaration per statement:** Don't combine multiple variable declarations
(enforced by `readability-isolate-declaration`):
```cpp
bool ok1 = false;  // good
bool ok2 = false;
bool ok1 = false, ok2 = false;  // bad
```

**No redundant base class initializers:** Don't explicitly call the default
constructor of a base class in the initializer list (enforced by
`readability-redundant-member-init`):
```cpp
ConfirmDelete(...) : m_name(name) { ... }           // good
ConfirmDelete(...) : qontrol::Modal(), m_name(name) { ... }  // bad
```

**Static member access:** Access static members through the class name, not through an
instance (enforced by `readability-static-accessed-through-instance`):
```cpp
QApplication::exec();  // good
app.exec();            // bad
```

**Unused parameters:** For parameters required by a signal-slot signature but unused
in the implementation, annotate with `[[maybe_unused]]`:
```cpp
void onScanProgress(uint32_t height, [[maybe_unused]] uint32_t tip) { ... }
```

**Qt slots and `readability-convert-member-functions-to-static`:** Qt slots must
remain non-static member functions. Suppress with `// NOLINTNEXTLINE`:
```cpp
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void sendTransaction() { ... }
```

**Qt container + ranges checks:** Prefer ranges algorithms when they compile cleanly.
If clang-tidy requests ranges but Qt container/value category mismatches make it invalid,
keep the explicit loop and add a targeted `// NOLINTNEXTLINE(readability-use-anyofallof)`.

## C++: Qontrol Framework Usage

**Screen subclasses** (`qontrol::Screen`) implement a three-phase lifecycle:
1. `init()` — create all widgets
2. `doConnect()` — connect signals/slots
3. `view()` — build layout (called again to rebuild on data changes)

Constructor calls all three phases in order, plus any initial state setup.

**Layout building** uses the fluent API:
```cpp
auto *col = (new qontrol::Column)
    ->push(titleRow)
    ->pushSpacer(20)
    ->push(m_outputs_column)
    ->pushSpacer();
```

**Layout rebuilding:** delete the old layout and main widget, build new ones, then
call `setLayout()`:
```cpp
auto *oldWidget = m_main_widget;
m_main_widget = margin(newRow, 10);
delete oldWidget;
delete this->layout();
this->setLayout(m_main_widget->layout());
```

**Modal subclasses** (`qontrol::Modal`) follow the same three-phase lifecycle as
screens:
1. `init()` — create all widgets
2. `doConnect()` — connect signals/slots
3. `view()` — build layout with `setMainWidget(widget)`

Always use `qontrol::Modal`, never `QDialog`. Modals are displayed via
`AppController::execModal(modal)`.

**Static widgets with conditional visibility.** When content is not dynamic (just
conditional visibility or state), create the widget in `init()`, store it in a member,
and hide it by default. Control visibility in slots rather than conditionally creating
in `view()`:
```cpp
// good — create once in init(), toggle visibility
void init() {
    m_total_row = (new qontrol::Row)->push(m_total_label)->push(m_total);
    m_total_row->setVisible(false);
}
void updateTotal() {
    m_total_row->setVisible(hasSelection);
}

// bad — conditionally create in view()
void view() {
    std::optional<QWidget *> totalRow = std::nullopt;
    if (hasSelection) {
        totalRow = (new qontrol::Row)->push(...);
    }
}
```

**Signal connections** use `qontrol::UNIQUE` flag to avoid duplicate connections:
```cpp
connect(source, &Source::signal, dest, &Dest::slot, qontrol::UNIQUE);
```

**Prefer `clicked` over `toggled` for toggle widgets with side effects.** `toggled`
fires on both user clicks and programmatic `setChecked()` calls, which causes
feedback loops when a slot updates the toggle state. `clicked` only fires on user
interaction, eliminating the problem:
```cpp
// good — clicked only fires on user interaction
connect(m_toggle, &QCheckBox::clicked, this, &MyClass::onToggled, qontrol::UNIQUE);

// bad — toggled fires on setChecked() too, requires blockSignals()
connect(m_toggle, &QCheckBox::toggled, this, &MyClass::onToggled, qontrol::UNIQUE);
```

`blockSignals()` is acceptable only when the widget genuinely lacks a user-only
signal and programmatic state changes would trigger handlers with side effects
(network calls, state mutations). Always add a comment explaining why
`blockSignals()` is needed. Never use it as a lazy substitute for connecting to the
right signal.

**Lambdas in `connect()` are FORBIDDEN.** Always create named slots. Lambdas make
signal-slot connections harder to read, test, and debug. They also prevent using
`qontrol::UNIQUE` for duplicate connection prevention. The only exception is when
there is absolutely no other choice (extremely rare). Extract the callback logic
into a named slot method instead:
```cpp
// good — named slot
connect(modal, &ConfirmDelete::confirmed, this, &AppController::onDeleteConfirmed);

// bad — lambda
connect(modal, &ConfirmDelete::confirmed, this, [this](const QString &account) { ... });
```

When a signal needs to trigger actions on multiple objects (e.g., validation + processing),
connect to a single slot that handles both:
```cpp
// good — single slot handles all actions
connect(m_amount, &QLineEdit::textChanged, screen, &Send::process);
// process() calls updateOutputValidations() + updateInputsTotal() internally

// bad — lambda to call multiple methods
connect(m_amount, &QLineEdit::textChanged, [this, screen]() {
    updateValidation();
    screen->process();
});
```

**Chain signals to slots for state updates.** When an action (e.g. account deletion)
needs to trigger a UI refresh, call the refresh from the slot that performs the work —
never from the caller that showed the modal. This keeps the update in the signal-slot
chain where Qt manages lifetime and ordering correctly:
```cpp
// good — slot does work then refreshes
void PluginRegistry::onAccountDeleted(ReqId req_id, const QString &id, const QString &error) {
    auto *module = qobject_cast<IModule *>(sender());
    if (!m_pending_deletions.contains(module) ||
        !m_pending_deletions.value(module).contains(req_id) ||
        m_pending_deletions.value(module).value(req_id) != id) {
        return;
    }
    m_pending_deletions[module].remove(req_id);
    if (m_pending_deletions.value(module).isEmpty()) {
        m_pending_deletions.remove(module);
    }
    if (error.isEmpty()) {
        requestModuleInstances(module); // refresh only on successful completion
    }
}

// bad — caller refreshes after modal returns
void MenuTab::deleteAccount(const QString &name) {
    auto *modal = new ConfirmDelete(name);
    connect(modal, &ConfirmDelete::confirmed, this, &MenuTab::onDeleteConfirmed);
    AppController::execModal(modal);
    onRefreshLauncherItems(); // wrong: dispatch is not deletion completion
}
```
Never use `QTimer::singleShot(0, ...)` or `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`
to work around signal-slot ordering issues — restructure the chain instead.

**Don't call `view()` from widget signal handlers.** When a signal handler would trigger
`view()` which reparents the signaling widget, update state directly instead:
```cpp
// bad — reparents checkbox during its own signal handler
auto SelectCoins::checked() -> void {
    m_ok->setEnabled(hasChecked());
    view(); // view() reparents checkboxes → undefined behavior
}

// good — update display directly without layout rebuild
auto SelectCoins::checked() -> void {
    m_ok->setEnabled(hasChecked());
    updateTotal(); // separate slot for the action
}
```

**Extract signal-triggered actions into separate slots.** When a signal handler performs
an action (update display, calculate state), move that logic into a dedicated slot.
This improves readability and allows the action to be called from multiple places:
```cpp
// good — action logic in dedicated slot
auto SelectCoins::checked() -> void {
    m_ok->setEnabled(hasChecked());
    updateTotal();
}

auto SelectCoins::updateTotal() -> void {
    uint64_t total = calculateSelectedAmount();
    m_total->setText(formatBtc(total));
    m_total->setVisible(total > 0);
}
```

**Early exit with direct action over counting or algorithms.** When checking if any item
matches a condition, prefer a simple loop with early break and direct action:
```cpp
// bad — wasteful counting when you only need to know if any exist
int count = 0;
for (auto *cw : m_coins) {
    if (cw->isChecked()) {
        count++;
    }
}
m_ok->setEnabled(count > 0);

// ok — but less readable and no performance benefit here
bool hasChecked = std::ranges::any_of(m_coins, [](auto *cw) { return cw->isChecked(); });
m_ok->setEnabled(hasChecked);

// preferred — simple loop, early break, direct action
m_ok->setEnabled(false);
for (auto *cw : m_coins) {
    if (cw->isChecked()) {
        m_ok->setEnabled(true);
        break;
    }
}
```

**Clang-tidy NOLINT:** slots that clang-tidy suggests could be made static must be
suppressed with `// NOLINT` — Qt slots must remain non-static member functions.

## C++: Singleton Pattern

`AppController` inherits from `qontrol::Controller`:
```cpp
AppController::init();           // create singleton (once, fatal on duplicate)
AppController::get();            // retrieve via dynamic_cast
```

## C++: Error Handling

Never throw or catch exceptions. Rust FFI functions never return `Result` (see
"Rust: Error Handling" above), so no CXX exceptions will be thrown. Check return
values instead:
- `bool` returns: check directly (`if (!config->is_ok()) { ... }`)
- Struct returns: check `is_ok` field (`if (!result.is_ok) { ... }`)
- Opaque types: check `is_ok()` method (`if (!account->is_ok()) { ... }`)

Display errors via `qontrol::Modal` or log with `qWarning()` / `qCritical()`.

## C++: Rust FFI Integration

**Type conversions:**
```cpp
// rust::String -> QString
QString::fromStdString(std::string(rust_str.c_str()))

// QString -> rust::String
rust::String(qstring.toStdString())
```

**Owned Rust objects** are held as `std::optional<rust::Box<Account>>`. Always check
`has_value()` before access.

**Never block the GUI thread.** Any Rust FFI call that performs I/O or network
requests (e.g. `get_backend_info()`) must run on a background `QThread`. Return the
result to the main thread by emitting a signal from the background thread and
connecting it to a slot. Example:
```cpp
// In header — declare signal and slot:
// signals:
//     void backendInfoReady(BackendInfo info);
// public slots:
//     auto onBackendInfoReady(BackendInfo info) -> void;

auto *thread = QThread::create([this, url = url.toStdString()]() {
    auto info = ::get_backend_info(rust::String(url));
    emit backendInfoReady(info);
});
connect(this, &MyClass::backendInfoReady, this, &MyClass::onBackendInfoReady,
        qontrol::UNIQUE);
connect(thread, &QThread::finished, thread, &QThread::deleteLater);
thread->start();
```
This pattern applies to any new blocking FFI call — always `QThread::create` +
lambda + emit signal back to the GUI thread.

**`QMetaObject::invokeMethod` is FORBIDDEN** unless there is absolutely no other
solution. Always prefer the signal+slot pattern above.

## C++: Timer-Based Polling

`AccountController` runs two polling timers:
- **100ms** (`m_notif_timer`) — polls `try_recv()` for scanner notifications
- **1000ms** (`m_coins_timer`) — polls coin state and balance

Both timers are stopped in `stop()` before the scanner is shut down. The
`pollNotifications()` loop drains all pending notifications in a single tick.

## C++: Composite Widget Pattern

Reusable widget groups (e.g. `OutputW`, `InputW`, `CoinWidget`, `RadioElement`) are
plain classes (not QWidget subclasses) that own a widget tree and expose it via
`widget()`. Stored in `QHash<int, WidgetType*>` keyed by ID. Manually deleted on
removal:
```cpp
auto *output = m_outputs.take(id);
delete output->widget();
delete output;
```

## C++: Utility Functions (`src/screens/utils.h`)

```cpp
auto margin(QWidget *widget) -> QWidget *;             // default margin wrapper
auto margin(QWidget *widget, int margin) -> QWidget *;  // custom margin
auto frame(QWidget *widget) -> QWidget *;               // rounded-rect border
auto toBitcoin(uint64_t sats, bool with_unit = true) -> QString;  // sat -> BTC
string
auto coinsCount(uint64_t count) -> QString;             // pluralized coin count
```

`Frame` is a custom `QFrame` subclass with a `paintEvent` that draws a rounded
rectangle border.

## C++: Pointer Conventions

- **Raw pointers** (`QWidget *m_btn = nullptr`): parent-owned Qt objects (most
widgets).
- **QPointer** (`QPointer<qontrol::Panel>`): observing pointers that auto-null on
deletion.
- **std::optional<rust::Box<T>>**: owned Rust objects that may not be initialized
yet.
- All member pointers initialized to `nullptr` in the header.

## C++: ComboBox Enum Binding

Network enums are stored as `QVariant` item data:
```cpp
m_network_combo->addItem("Regtest", static_cast<int>(Network::Regtest));
// Retrieve:
auto network = static_cast<Network>(m_network_combo->currentData().toInt());
```

## Testing Conventions

**Account naming:** `format!("test_account_{}", std::process::id())` for unique
per-run names.

**Standard test mnemonic:** `"abandon abandon abandon abandon abandon abandon
abandon abandon abandon abandon abandon about"` (BIP39 test vector, Regtest network).

**Lifecycle pattern:**
```rust
let account_name = test_account_name();
let config = create_test_config(account_name.clone());
let account = Account::new(config).expect("...");
// ... assertions ...
cleanup_test_account(&account_name);
```

**Cleanup:** drop every backend first, then remove only the account's logical subtree.
The Rust test cleanup helper uses the same storage-backed deletion; it cannot remove
an account while a live backend still owns it. Use public logical storage operations
to inject missing/corrupt files in fixtures, not the former native account layout.

**`#[ignore]` tests** require external infrastructure (BlindBit server, Bitcoin
node). Run with `cargo test -- --ignored`.

**Assertions always include descriptive messages** with context values:
```rust
assert!(sp_addr.len() >= 100, "SP address should be at least 100 characters, got
{}", sp_addr.len());
```

**Temporary directories:** Use the `temp-dir` crate (`TempDir::new()`) for temporary
directories in tests. Never use `std::env::temp_dir()` directly. The caller must hold
the `TempDir` handle for the lifetime it's needed — do not leak or forget it.

**Test sections** are grouped with comment headers:
```rust
// ===== Wallet Creation and Config Tests =====
// ===== Error Handling Tests =====
```

## C++: Input Validation Pattern

All input fields must provide visual validation feedback to users:

- **Valid input** → green ✓ check mark inside the field's trailing edge
- **Invalid input** → red ✗ cross inside the field's trailing edge
- **Empty input** → no mark, with the same reserved text gutter

Use `catalog::ValidatedInput` for integrated validation. Its child `ValidationMark`
overlays the actual `Input`, follows that input's resize/show events, and ignores
mouse events. The right text margin stays reserved for every state, in addition to
the base Input stylesheet's horizontal padding. State changes must not shift the
field or let long text overlap the mark. Do not add a sibling indicator to the row.

**Implementation:**

1. Create a `catalog::ValidatedInput` and put the wrapper in the layout.
2. Use `input()` to set placeholders, validators, object names and focus, or to
   connect the underlying `Input` signals to named validation slots.
3. Update the wrapper's state without changing its geometry or visibility:

```cpp
if (m_address_field->input()->text().isEmpty()) {
    m_address_field->setState(catalog::ValidationMark::State::None);
} else {
    m_address_field->setValid(valid);
}
```

An `EndpointRow` test-result mark is intentionally outside the field, after its
Test button. It is not integrated input validation.

**Use Qt validators for input restriction.** When restricting input format (numeric ranges,
decimal places, etc.), use Qt's built-in validators rather than manual validation logic:

```cpp
// sats/vb: 3 decimal places max (milli-sats precision)
auto *validator = new QDoubleValidator(0.001, 1000000.0, 3, m_fee_value);
validator->setNotation(QDoubleValidator::StandardNotation);
m_fee_value->setValidator(validator);

// sats: integers only
m_fee_value->setValidator(new QIntValidator(1, 100000000, m_fee_value));
```

Available validators: `QIntValidator`, `QDoubleValidator`, `QRegularExpressionValidator`.

**Validation examples:**
- Address fields: use `::validate_address()` FFI function (returns empty string if valid)
- Amount fields: `QDoubleValidator` with appropriate range and decimals
- Fee rate fields: `QDoubleValidator` with 3 decimal places (milli-sats precision)
- Absolute fee fields: `QIntValidator` (satoshis are integers)
