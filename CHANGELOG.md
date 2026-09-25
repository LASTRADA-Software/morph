# Changelog

Notable changes to morph, in [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
format. morph is pre-1.0 and has no tagged releases yet: `master` is the only
supported line, this file tracks notable changes going forward, and history
predating it lives in `git log`. From 1.0 on, versioning follows the policy
published in `docs/spec/VERSIONING.md` (SemVer for the public, non-`detail`
API surface).

## [Unreleased]

### Changed

- **morph's per-model strands are core-cpp's `KeyedStrands`.**
  `morph::exec::detail::StrandExecutor` is gone: a backend's strands are
  `morph::exec::detail::ModelStrands`, over core-cpp 0.4.0's
  `core::async::KeyedStrands<ModelId>`, and morph keeps only the adapter to its
  `IExecutor`, the log of a throwing task and a Task handler's session. What a
  consumer can see:
  - A Task handler that awaits `core::async::AsyncQueue::pop` comes back to its
    model's strand, with its session, a stop included.
  - An ordinary execute allocates less: `bench.alloc_budget` measures the local
    round trip.
  - Under single-threaded WebAssembly, `~LocalBackend` and
    `~SynchronousBackendAdapter` no longer wait for their strands, which only
    that thread could run. `~LocalBackend` seals its strands before it stops
    its Task handlers, so each one unwinds inline; other work still queued is
    dropped.
  - A strand queues itself on the backend's `IExecutor` once per turn and runs
    up to 32 tasks there, where it used to post each task.

- **`LocalBackend` no longer runs an action whose call was already failed.**
  An action still waiting for its model when `Bridge::switchBackend` or
  `~Bridge` fails its call (`BackendChangedError`, `BridgeDestroyedError`) is
  now skipped when its turn comes: its handler does not run, and it used to.
  A skipped action still counts as a failed execute in the
  `executeErrors` metric and ends its span as failed.
  `~LocalBackend` also stops the Task handlers it started, then waits for its
  strand to drain before it returns. See `docs/spec/core/coroutines.md`,
  "Teardown".

- **morph depends on core-cpp v0.4.1.** It is fetched through CPM, and
  `morph::morph` links its `core::base`, `core::async`, `core::net` and, natively,
  `core::platform`. morph stays header-only, but those are static libraries, so
  a project that links morph now builds them. `TimeoutScheduler` runs on
  core-cpp's event-loop timers: natively on a thread of its own, and under
  single-threaded WebAssembly on a loop the browser's timer pumps, where
  `cancel()` now also retires the timer. An install of morph installs
  core-cpp's package next to it, and `find_package(morph)` finds it through
  `find_dependency(core-cpp 0.4)`.

- **Dependencies are fetched through CPM, and cached by CPM.** glaze, Catch2,
  doxygen-awesome-css and Lightweight come through `CPMAddPackage` when no
  installed copy is found, and CPM keeps their sources in `CPM_SOURCE_CACHE`,
  which defaults to `.cache/cpm` inside the checkout. A second configure of the
  checkout clones nothing. `cmake/DepCache.cmake` and its `MORPH_DEP_CACHE`
  environment variable are gone: set `CPM_SOURCE_CACHE` instead.

- **`LocalBackend::execute` no longer rescans the pending-completion list on
  every dispatch.** `trackPending` used to `std::erase_if` the whole `_pending`
  vector before each append, so admitting one call with *n* already in flight
  cost *n* atomic `weak_ptr::expired()` loads under `_pendingMtx`, and a burst of
  *n* cost O(n²) — before any model work started. Measured against one parked
  model (clang 22.1.8, `-O2`, 8-core Linux), timing only the `execute()` calls:
  32,000 queued executes spent **362 ms** in admission alone, at **24.1 µs** per
  admission over the last tenth; 16,000 spent 77.8 ms. The sweep is now
  amortised — it runs only when the list reaches a threshold re-armed at twice
  the surviving entry count after each sweep — which brings the same case to
  **7.6 ms** total and **0.31 µs** per admission, flat in *n*, and within noise of
  the 7.6 ms measured with the sweep deleted outright.

  The list is now bounded at twice the live count rather than exactly it, which
  is the whole price; a new `LocalBackend::trackedPendingCount()` makes that
  observable. `cancelPending` is unaffected and still fails every live
  completion on a backend swap or `~Bridge`: it never saw dead entries in the
  first place, because `weak.lock()` has always skipped them. See
  `docs/spec/core/backend.md`, "The pending list and its amortised compaction",
  and morph#528.

- **`Quantity::equation()` writes out at most 100 derivation steps by default,
  and takes the limit as an argument.** A derivation has no bound — a
  data-driven `total = total + row` loop records one step per iteration — and
  `equation()` used to render every one of them: 100,000 steps produced a
  **500,001-character** first line in **58.8 s** (clang 22.1.8, `-O1` under
  ASan+UBSan). It now produces **502 characters in 0.057 s**, with the deep end
  of the derivation collapsed to an `e1` whose value carries a legend line:
  `e1 = 99900 (elided at the 100-step limit)`. The result line is unaffected —
  eliding changes the account of how a value was reached, never the value.

  **Public signature change**:
  `equation(std::size_t maxSteps = kDefaultEquationSteps)`. Source-compatible
  (every existing call site compiles and keeps working), but the *output* of an
  existing call changes once a derivation passes 100 steps. Callers that want
  the old rendering pass `kEquationStepsUnlimited`; `0` returns the formatted
  value alone. The limit is a parameter rather than a constant because how much
  of a derivation is worth reading is the reading layer's decision, not the
  value type's — an audit log and a tooltip do not want the same answer. The
  default sits at the human end of the range because the two errors are not
  symmetric: too low costs a caller one argument, too high costs everyone a
  half-megabyte line they never knew was unbounded. Pre-1.0, per
  `docs/spec/VERSIONING.md`. See `docs/spec/util/quantity_type.md`, "The
  rendering is bounded; the derivation is not", and morph#582.

  Rendering a derivation **in full** also got cheap, which is a separate half of
  the same issue: `combine` appends to its left operand instead of copying it
  into a fresh string, so the left-leaning chain an accumulate loop records is
  linear rather than quadratic in its depth — 70,000 steps rendered whole went
  from 27.7 s to 0.11 s under ASan+UBSan, and from over ctest's 120 s timeout to
  1.3 s at `-O0` under TSan. The `[slow]` tag and 600 s timeout exception added
  for that test in morph#590 are gone again; every test is back under one 120 s
  cap. A right-leaning chain and a chain of unary negations still copy the big
  operand per level and are still quadratic — the step limit is what bounds
  those.

- **`Completion<T>` has a stated value-handling contract, and `T` no longer has
  to be copyable.** `std::move_constructible<T>` is now the whole type
  requirement; copyability became a *per-handler* obligation, diagnosed where
  the handler is written. A handler taking `const T&` costs **zero** copies, one
  taking `T` by value costs **exactly one**, and neither number depends on how
  many handlers are attached or on which side of the settle they attached.
  Measured with a copy-counting `T`, the budget was **N + 2M** (N handlers
  attached before settling, M after) and is now **one per by-value handler**:
  three `const T&` handlers before a settle plus three after went from 9 copies
  to 0. Mechanically, `onOk` is erased as
  `std::vector<std::function<void(const T&)>>` and `CompletionState<T>` derives
  from `std::enable_shared_from_this`, so both dispatch closures read the stored
  value in place instead of carrying a copy. `Completion<std::unique_ptr<int>>`
  now instantiates and fans out; `IExecutor::post` is unchanged, because the
  closure captures a `shared_ptr` rather than the value.

  **Public signature change**: `then()`, `thenDetached()` and their gated
  overloads take `std::function<void(const T&)>` rather than
  `std::function<void(T)>`. Source-compatible for lambdas taking `const T&`,
  `T` by value or `auto` by value, and for existing `std::function<void(T)>`
  objects — all 312 `.then(` call sites in the tree compile unchanged. A handler
  taking `T&` or `T&&` would not; none exists. Pre-1.0, per
  `docs/spec/VERSIONING.md`. See `docs/spec/core/completion.md`,
  "Value-handling contract", and morph#553.

- **`morph::log` no longer takes over a consumer's `stderr` by default.**
  `LogState::minLevel` defaults to `LogLevel::warn` instead of
  `LogLevel::debug`, so linking morph no longer emits its dispatch tracing
  unasked — a plain `morph_tests` run wrote 2,949 such lines against 23 lines of
  actual test output. `setLogLevel(LogLevel::debug)` restores the previous
  behaviour in one line. The new default is deliberately the same in every build
  configuration rather than keyed on `NDEBUG`: `minLevel` is a default member
  initializer in a header, so a configuration-dependent one would give
  translation units compiled with different settings two definitions of
  `LogState` and of the inline `logState()`, an ODR violation the linker
  resolves silently. Pre-1.0, so no deprecation window; see
  `docs/spec/core/logger.md`, "The default level".
- **Every Catch2 test binary takes `--log-level` and is silent by default.**
  The suites exercise error paths on purpose, so even `warn` left 54
  `[ERROR]`/`[WARN ]` records that read as failures but are not; test binaries
  now default to `LogLevel::off` and accept
  `--log-level=debug|info|warn|error|off`, falling back to
  `$MORPH_TEST_LOG_LEVEL` when the switch is absent (CI sets it to `debug`,
  because `ctest` gives no place to thread an argument through). A default
  `morph_tests` run is down from 2,972 lines to 24. Implemented by replacing
  `Catch2::Catch2WithMain` with `morph_test_main` — a Catch2 event listener
  cannot add a command-line option, since listeners are constructed after the
  parser is built. The four Qt-owning mains keep their own `main` and call the
  same `morph::testkit::configureSession()`.

### Added

- **Coroutines on `core::async`.**
  - `Completion<T>` is awaitable: `co_await std::move(completion)` yields `T`
    or rethrows, resumes on the executor the coroutine suspended on (core-cpp's
    current-executor context), and honours a stop request on the awaiting
    coroutine's token.
  - `morph::async::spawn(executor, task)` starts a coroutine from ordinary
    code, with every step on that executor.
  - `morph::async::delay(scheduler, duration)` is a stop-aware timer.
  - A model's `execute` may return `core::async::Task<R>`. The bridge drives it
    on the model's strand, and the model's next action waits until the Task has
    completed. `ActionTraits<A>::Result` is `R`.
  - `ActionDispatcher::dispatchAsync` runs Task handlers remotely;
    `ActionDispatcher::dispatch` throws `std::logic_error` for one, and
    `ActionDispatcher::dispatchesAsync` says which of the two an action needs.
  - `RemoteServer` now replies `err "unknown exception"` to a handler that
    throws something other than a `std::exception`, where it used to send no
    reply.
  - `LimitPolicy::executeTimeout` also stops a suspended Task handler, so the
    model's next action is not held behind it.
  - A Task handler follows `ActionRecordingError` as an ordinary handler does:
    once its Task has completed, a result that will not serialise or a journal
    append that throws reaches the caller as `ActionRecordingError`, and is
    never journalled as `Outcome::Failed`.

  See `docs/spec/core/coroutines.md`.

- **`SlotRegistry.byKind(kind, component)` — one host control per kind of
  control.** The JSON type `byType` keys on does not identify a control: a
  `Quantity` and a nested object are both `"object"`, a `Choice` is
  `"integer"`, an enum and a `Timestamp` are `"string"`. Field descriptors now
  carry `kind` (`quantity`, `choice`, `enum`, `datetime`, `date`, `boolean`,
  `integer`, `number`, `string`, `array`, `objectArray`, `object`), and
  `resolve()` takes it as an optional sixth argument, consulted after unit and
  before type. See `docs/spec/forms/forms.md`, "Theming / component-override
  registry" (fixes #812).
- **CollectionView and WizardView chrome goes through `SlotRegistry.byChrome`,
  and both hand their registry to the forms they embed.** Their title, column
  headers, row cells and buttons, confirm and editor dialogs, and Back/Next were
  hard-coded, and the editor/step `DynamicForm`s got no `slotRegistry` at all, so
  a host's field slots and form chrome stopped at the view's edge. New
  `slotRegistry` properties and roles `collectionHeader`, `collectionRow`,
  `confirmDialog`, `editorDialog` (a `contentItem` the editor is reparented
  into), `wizardHeader` and `wizardNav`; the chrome's `fire()`/`accept()`/
  `next()` make exactly the built-in buttons' calls, gates included. See
  `docs/spec/forms/views.md` and `workflows_navigation.md`, "Chrome slots"
  (fixes #813).

- **Chrome slots: a host replaces DynamicForm's labels, containers and buttons,
  not only its controls.** `SlotRegistry.byChrome(role, component)` /
  `resolveChrome(role)` register one Component per role — `fieldLabel`,
  `fieldHelp`, `section`, `accordion` (falls back to `section`), `tabset`,
  `header`, `status`, `submitButton`, `preview`, `result` — each loaded in place
  of the built-in, which is hidden. Container chrome declares a `contentItem`
  the form creates its field grid inside, so every field still exists once and
  keeps its `field_<name>` objectName. A label chrome gets live `required` and
  `invalid` states (`DynamicForm.fieldInvalid(name)`); the status line's text is
  now `DynamicForm.statusText`. With nothing registered, every form renders as
  before. See `docs/spec/forms/forms.md`, "Chrome slots".
- **`DynamicForm.prefill(values)` / `prefillFromJson(text)` — load a stored
  record for editing.** Every prefill path wrote *control text*, so an editing
  flow had to turn each wire value into the text its control holds by hand —
  a `{num,den,dp}` into locale digits, an ISO instant into the display zone, a
  row of a `std::vector<Row>` into cell texts. `decodeFieldValue(field, value)`
  is now the inverse of the encoders, and `prefill` replaces the draft from a
  payload, re-seeds every control and slot (`fieldText`, `rows`), re-selects
  fetched `Choice`s and never submits. Prefilling and editing nothing
  assembles the same payload. See `docs/spec/forms/forms.md`, "Prefill"
  (fixes #814).

- **A host slot can draw a `std::vector<Row>` member, and the form encodes it.**
  A collection of objects had no built-in control and was reported
  unrepresentable, so a form carrying one could not be submitted whatever the
  host drew. When a `SlotRegistry` slot claims such a (top-level) member,
  `DynamicForm` now describes the row type as `field.itemFields` (the grid's
  columns: label, unit, decimals, read-only, required, kind flags), accepts the
  rows as `{member: cellText}` objects, and encodes every cell with the encoder
  the same member gets at the top level (`encodeFieldText`) — a `Quantity` cell
  is exact, an over-precise one is refused, a blank required cell keeps the form
  unready. Slots gain four optional members, assigned only when declared:
  `fieldText` (the retained value, kept current through prefill, reset and tab
  rebuilds), `rows`, `setRows(rows)` and `form`. Without a slot nothing changes.
  See `docs/spec/forms/forms.md`, "Collections of objects — a host slot draws
  them".

- **`FieldMeta::unit` / `FieldMeta::decimals` — a display unit and precision
  for a plain `double`/`float`/integral member.** A DTO holding lab readings as
  plain `double`s had no way to tell a renderer "kg/m³, three decimals": that
  knowledge lived only in `Quantity`'s type. `unit` is emitted as the
  property's `ExtUnits` (the key a `Quantity` already carries, so the shipped
  renderer's unit suffix, `SlotRegistry.byUnit` and view columns all see it
  unchanged); `decimals` is emitted as the new `x-displayDecimals`, **not**
  `x-decimalPlaces`, because the latter switches a property to the exact
  `{num,den,dp}` encoding a `double` cannot decode. `DynamicForm` keeps the
  JSON-number encoding, refuses an entry with more fraction digits than
  declared (as it does for a `Quantity`), spells the placeholder from it, and
  hands slots `field.decimals` / `field.decimalsDeclared`. Both keys are
  ignored on a `Quantity` member and apply at any nesting depth. See
  `docs/spec/forms/forms.md`, "Display unit and decimals for a plain member".

- **The MorphForms QML module installs, as the `forms_qml` component.**
  `cmake --install` of a `MORPH_BUILD_FORMS_QML=ON` build installed the
  controller-core header and nothing of the QML module, so a packaged morph
  (e.g. a vcpkg port) could not be used to render a form. It now installs and
  exports `morph::forms_qml` / `morph::forms_qmlplugin` together with the
  object libraries a static QML module needs (compiled resources, the plugin's
  static initialiser), and `qmldir`/`.qmltypes`/sources under
  `MORPH_INSTALL_QMLDIR` (exported as `morph_QML_IMPORT_PATH`). A consumer
  links `morph::forms_qmlplugin` and imports `MorphForms`;
  `scripts/check_forms_qml_install.sh` (CI `install-export-forms-qml`) builds
  and runs one against an install.

- **A locale numeric entry accepts an explicit `+`.**
  `morph::render::normalizeLocaleNumber` had no notion of a positive sign: a
  leading `+` fell through to the "any other character is malformed" arm, so
  `"+5"` was rejected in every locale, `"C"` included. It now takes a fifth
  `std::string_view positiveSign = "+"`, matched exactly as `negativeSign` is —
  the locale's own spelling as a whole string, plus a bare ASCII `'+'`
  everywhere. Measured over `QLocale::positiveSign` for the 711 locales Qt
  6.11.2 knows, 54 spell it with a bidi control mark before the `+` (U+061C,
  U+200E or U+200F; e.g. `ar_EG`, `ar_DZ`, `az_IR`, `ckb_IQ`) and the other 657
  use the bare `+`. Unlike the negative side there is no U+2212 analogue, so
  *every* non-ASCII spelling here is multi-code-point and whole-string matching
  is the only thing that can match any of them. **The sign is dropped, not
  carried**: canonical text is `-?[0-9]+(\.[0-9]+)?` and has no `+` in it, so
  `"+5"` yields `"5"`. **`formatCanonicalNumber` deliberately takes no
  `positiveSign` and never emits one** — `+` is the positive sign in 657 of 711
  locales, so emitting it would turn every positive number in every form from
  `5` into `+5`. The pair is therefore not a strict inverse across a positive
  sign, which is stated in `docs/spec/forms/forms.md`, "Locale data formatting",
  rather than left to be inferred from a missing parameter. The JavaScript
  mirror in `src/qt/forms/qml/DynamicForm.qml` carries the identical change and
  the renderer forwards `qtLocale.positiveSign` at the two entry call sites. The
  parameter is defaulted, so no existing call site changes behaviour. See
  morph#596.

- **Every form in the ladder's showcase GUI now renders through
  `morph::forms`.** `examples/kanban` gains schema-driven `CreateProject`,
  `CreateColumn`, `CreateSwimlane`, `CreateTask` and `AddComment` forms
  alongside the `Login` form it already had — six in total, none with a
  hand-written field or submit button. `BoardBridge` grows the same
  `schemasJson`/`submitIfValid`/`replyReceived` controller contract
  `ProjectAdminBridge` has, both serving one schema document
  (`gui_lib/kanban_schemas.hpp`); the five demoted `Q_INVOKABLE`s stay as plain
  C++ methods. `CreateTask`'s column/swimlane ids and `AddComment`'s task id are
  declared `hidden` in their DTOs' `fieldMetadata` and supplied by the view that
  owns the form, so "type a title in the column you want" survives the
  conversion with no row id on screen, and `CreateColumn::wipLimit` joins
  `optionalFields` so a blank WIP limit still means unlimited.
  `tests/test_gui_forms_render.cpp` drives each form in a real QML engine over a
  real backend and asserts the controls drawn, the body assembled and the row
  written.

- **An out-of-process scenario corpus covering every ladder rung.**
  `scripts/scenario/scenarios/` grows from 4 files to 72, and
  `scripts/scenario/scenario_coverage.py` now exits `0` on both of its axes:
  all 71 registered actions across the five server rungs are dispatched, every
  rung meets its workflow floor (pastebin 8, polls 10, bookmarks 12, ledger 15,
  kanban 20), all 8 envelope kinds are sent, and every refusal is either
  asserted or exempt with a written reason. Each file is a named journey
  sourced from its rung's own README, verified against a real
  `ladder_<rung>_server`, re-run against the database the first pass left
  behind, and mutation-tested.

- **`scripts/scenario/run_scenarios.py`**, the server lifecycle
  `morph_scenario.py` deliberately does not own: it starts one server per rung
  on a fresh SQLite database in its own working directory with the port bound
  to `0`, runs that rung's whole directory against it, and tears the process
  group down. `--twice` proves every file is re-runnable, `--mutate` proves its
  assertions are load-bearing, `--rung` and `--build-dir` narrow the run.

- **A replay outcome that distinguishes undelivered from rejected.**
  `morph::offline::ReplayOutcome` (`Succeeded`/`Rejected`/`Undelivered`) and a
  `SyncWorker::DetailedReplayFunction` overload taking it. Only `Rejected`
  spends an attempt against the 5-attempt retry budget, so a run of reconnect
  flaps no longer dead-letters queued work the server never saw — previously a
  transport failure and a server-side rejection were charged identically, and
  the budget is durable, so five flaps dropped every queued item through the
  same `DeadLetterSink` call and the same user-facing "could not be synced"
  state a genuine rejection produces. `SyncResult` gains `undelivered`. The
  boolean `ReplayFunction` is unchanged and keeps its exact previous meaning
  (`false` → `Rejected`); a throw is still charged, from either form.

- Root project docs: `CHANGELOG.md`, `SECURITY.md`, `CONTRIBUTING.md`.
- Planned specs: connection-scoped model cleanup
  (`docs/planned/connection_scoped_cleanup.md`), graceful shutdown & drain
  (`docs/planned/graceful_shutdown.md`), journal format versioning & retention
  (`docs/planned/journal_evolution.md`), GUI localisation
  (`docs/planned/gui_i18n.md`), with matching roadmap entries in
  `docs/todo.md` (A7, C5, B4, E-G10).
- GUI program: presentation rules (`visibleWhen`/`readonlyWhen`) in
  `docs/planned/gui_cross_field_rules.md`; accessibility assertions in
  `docs/planned/gui_renderer_toolkit.md`'s conformance kit; a
  banned-terminology check in `docs/planned/drift_guard.md`'s prose lint.
- API stability & versioning policy (`docs/spec/VERSIONING.md`, folded in
  from `docs/planned/api_stability.md`): the semantic-versioning rules, the
  stable (non-`detail`) surface definition, and the deprecation-window
  discipline morph commits to starting at 1.0. Backed by two new mechanical
  checks: the `include/morph/version.hpp` version constants (cross-checked
  against `CMakeLists.txt`'s `project(VERSION ...)` by
  `tests/test_version.cpp`), and the `deprecation-lint` CI job
  (`scripts/check_deprecated_markers.sh`) enforcing that every
  `[[deprecated("...")]]` marker names a replacement and a target removal
  version.
- **Keyed, shareable model instances.** One `BRIDGE_MODEL_KEY(Model, Action,
  &Action::field)` beside the existing registrations designates the action that
  defines a model's key and deduces the key type from that field, so the model's
  own class body says nothing about keys; further actions carrying the same key
  use `BRIDGE_KEY_FROM`, and creating actions use the `…_FROM_RESULT` variants.
  `BridgeHandler<M, AllowShared>` joins a server-side directory keyed on
  `(typeId, primary)`, so handlers in one process — or in two clients over one
  `RemoteServer` — reach the same instance. Adds `attach()`, `primary()` and
  `instances()` to the handler, the `primary`/`shared` envelope fields and the
  `attach`/`assign`/`instances` wire kinds, all additive. See
  `docs/spec/core/shared_instances.md`.
- **Instance subscriptions.** `BridgeHandler::subscribe<R>(cb)`, keyed on the
  result/state type, fires whenever an `R` is produced on the instance the
  handler is attached to — by any handler attached to it. Fan-out is per
  `Bridge`; there is no server-initiated push. See
  `docs/spec/core/bridge.md#subscription-semantics`.

### Changed

- **`RemoteServer::closeConnection` now releases one reference per attachment
  rather than erasing every model in the scope.** Required by cross-client
  instance sharing: otherwise one client's disconnect destroys an instance
  another client is still attached to. A connection scope records an
  attachment *count* per instance for the same reason. Unshared instances have
  exactly one attacher, so their lifetime is unchanged.
- `examples/bank` is reshaped onto stateful, keyed models: `AccountModel` holds
  one account in memory keyed by account id, and the new `CustomerModel` takes
  the per-owner half (`ListAccounts`/`OpenAccount`).
- The ladder rungs that hand-wrote `ModelKeyTraits`/`ActionKeyTraits` because
  `ModelKey` rejected their strong ids — kanban, ledger, lims — now use
  `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM`/`BRIDGE_KEY_FROM_RESULT` instead.
  `BoardModel`, `SampleModel` and `RuleModel` key on their strong id itself
  (`ProjectId`, `SampleId`, `LedgerId`), so `primary()`/`instances()` return
  it; `LedgerModel` and `BudgetModel` keep `std::int64_t` as a nested
  `PrimaryKey` alias, because each is named by actions carrying two different
  strong id types.

### Removed

- **`morph/net/detail/base64.hpp`.** The WebSocket handshake uses core-cpp's
  `core::base64::encode` (`<core/Base64.hpp>`), and `SocketServer`'s accept loop
  waits on `core::platform::Wakeup` instead of a self-pipe of its own.

- **The reactive-draft mechanism** — `BridgeHandler::set<&A::field>`,
  `reset<A>`, the action-keyed `subscribe<A>`, and their in-flight coalescing.
  Its job is done better by a stateful model holding the draft itself, and
  `subscribe` now means instance subscriptions (above). `morph::flows::FlowSession`
  already owned its own draft tuple and now gates on `ActionValidator` and
  dispatches directly; its public API, the `w-*`/`app-*` schema and
  `WizardView.qml` are unchanged. `ActionValidator` keeps its server-side
  validation role and loses only its draft-readiness one. Pre-1.0, per
  `docs/spec/VERSIONING.md`.

### Fixed

- **An installed `qt_forms` component compiles.** `forms_controller_core.hpp`
  includes `morph/qt/qt_executor.hpp`, which was installed only by the `qt`
  component (`MORPH_BUILD_QT`, which needs Qt WebSockets), so an install built
  with `MORPH_BUILD_FORMS_QML` alone shipped a header that could not be
  included: `fatal error C1083: Cannot open include file:
  'morph/qt/qt_executor.hpp'`. In-tree builds and the header-set verification
  both read the source tree and could not see it. `qt_forms` now ships the
  header too; it needs only QtCore.

- **The QML renderer's numeric-entry mirror matched the decimal and group
  separators as one UTF-16 code unit while the C++ edge matched whole
  strings.** `src/qt/forms/qml/DynamicForm.qml`'s `normalizeLocaleNumber` tested
  `ch === groupSeparator` and `ch === decimalSeparator`, where `ch` is
  `text[i]` — a single code unit — while
  `include/morph/render/locale_format.hpp` has always used
  `rest.starts_with(...)`. Both now use `text.startsWith(sep, i)`.

  **No locale reaches this, and no user is affected.** Measured on `91515ae5`
  with `QLocale::matchingLocales` under Qt 6.11.2, over all 711 locales:
  `decimalPoint` and `groupSeparator` are **one UTF-16 code unit in every
  single one** (0 multi-unit each), against 54 multi-unit for `negativeSign`
  and 54 for `positiveSign` — the control that shows the measurement is not
  trivially zero for any locale field. What is live is the *mixed idiom*:
  morph#583 and morph#596 converted the signs in that same function to
  whole-string matching and left the separators on a code-unit comparison a few
  lines away, with nothing saying why. `docs/spec/forms/forms.md`, "Both edges,
  or neither": a divergence between the mirror and the C++ edge is a divergence
  in what the product accepts, whether or not a locale can currently express
  it. Behaviour for every locale Qt knows is byte-identical before and after.

  Because no real locale can drive the difference, the tests pass **synthetic**
  multi-unit separators straight to the function; a test built on
  `Qt.locale(...)` would pass against the unfixed code and be evidence of
  nothing. The same corpus is pinned on both edges — `[morph599]` in
  `tests/test_render_locale_format.cpp` and three `test_aMultiUnitSeparator*`
  functions in `src/qt/forms/tests/tst_i18n.qml` — and each half of the fix was
  confirmed by mutation: restoring `ch === groupSeparator`, restoring
  `ch === decimalSeparator`, or dropping the index advance each takes the QML
  suite to `28 passed, 2 failed`. morph#599.

- **`bank_gui_qml_tests` did not compile.** The target compiles
  `examples/common/testkit/testkit_main.cpp`, whose `#include
  <testkit/log_level.hpp>` resolves only against the repository's `tests/`
  directory, and it reached that directory through nothing: it links
  `Catch2::Catch2` directly, where its sibling `bank_gui_tests` links
  `morph_test_main` (which carries the include path publicly, via
  `morph_test_log_level`). Reproduced on `26bfdb8f` with the `linux-everything`
  preset, the only configuration that switches `MORPH_BUILD_BANK_GUI` on:
  `fatal error: 'testkit/log_level.hpp' file not found`. The target now also
  links `morph_test_log_level` — the same split, for the same reason, that
  `morph_qt_tests` already uses for a suite that owns its own `main`. It
  builds, its 32 assertions pass offscreen, and `--log-level` (the helper that
  header declares) is live on it. No CI job configures
  `MORPH_BUILD_BANK_GUI`, so this fix is not yet gated by anything; morph#605
  covers that separately. morph#604.

- **The compiler-cache module told you to throw away your build directory, and
  for a preset it did not even work.** When a launcher is already pinned in the
  CMake cache, `cmake/CompileCache.cmake` advised "reconfigure with `--fresh`
  to let this module choose instead". Reconfiguring the *same* directory with
  `-UCMAKE_C_COMPILER_LAUNCHER -UCMAKE_CXX_COMPILER_LAUNCHER` removes exactly
  the two entries the module's guard reads, reaches the same selection, and
  leaves the build directory standing; `--fresh` deletes `CMakeFiles/` and,
  unless the generator is re-passed, drops `CMAKE_GENERATOR` back to the
  platform default (measured: a `-G Ninja` tree came back `Unix Makefiles`
  with a stale `build.ninja` beside the new `Makefile`). And where the pin
  comes from a **preset** — one of the three sources the message itself
  names — `--fresh` clears nothing at all, because the preset re-applies its
  `cacheVariables` on the very reconfigure `--fresh` triggers: the tree is
  gone and the launcher is still pinned. The message now names `-U` first and
  keeps `--fresh` with its actual cost and its actual limits. Behaviour is
  unchanged; only the advice is. Measured on an isolated harness that does
  nothing but `include()` the module. morph#592.

- **`equation()` no longer walks a shared derivation once per path.**
  `EquationRenderer::assignLabels` was the one traversal without a visited set,
  so a node reachable by *k* displayed paths was walked *k* times. Since the
  derivation is a DAG, that is exponential in the node count: 31 nodes built by
  repeated `q = q + q` have 2³⁰ root-to-leaf paths and took **10.3 s** to render
  33 short lines. With the set, the same call is instant and its output is
  byte-identical, and 61 nodes (2⁶⁰ paths) render instantly too. morph#602.

- **A model registered privately over `morph::net` was not journalled at all.**
  `morph::net::SocketBackend` left `IBackend::registerModelWithContext`
  unoverridden, so its default dropped the `contextKey`, and the native
  `bindModel` path added in morph#586 dropped it again by omission at the
  `wire::makeRegister` call site. Because `RemoteServer::attachLogIfConfigured`
  returns *without consulting its `LogProvider`* when the envelope's
  `contextKey` is empty, the effect was not a log entry missing its entity
  key — no log was attached, so the instance produced no audit record at all,
  while the same registration over `SimulatedRemoteBackend` produced one. It
  failed open. `SocketBackend` now overrides `registerModelWithContext` and
  passes `request.contextKey` to `makeRegister` on the `bindModel` private
  branch, so both edges carry the key; the empty-`primary` degrades of
  `registerModelShared`/`attachModel`, which route through
  `registerModelWithContext`, are fixed with them. The shared and attach shapes
  already carried it and are unchanged. Reproduced end-to-end over a real
  socket in `tests/net/test_socket_backend.cpp`, which asserts the provider was
  consulted with the key and that the attached log records the action under it.
  See morph#587.

- **`formatCanonicalNumber`'s doc comment described the behaviour morph#574
  removed.** It told the reader that grouping is "never accepted back on entry"
  and that `normalizeLocaleNumber` "strips it unconditionally" — both false, and
  in opposite directions. Measured on `be64026a`:
  `normalizeLocaleNumber("1.050,25", ",", ".")` is `"1050.25"` (grouping *is*
  accepted back) and `normalizeLocaleNumber("1.5", ",", ".")` is `std::nullopt`
  (it is *not* stripped unconditionally — stripping is what used to turn that
  entry into `15`). No behaviour changed: the spec and the code already agreed,
  and only the comment was stale, dating to `d2cb3ae1` and never updated when
  morph#574 deliberately reversed what it describes. It matters because it is
  the comment a reader consults when deciding whether a grouped display can be
  fed back through the entry edge, and it told them the round trip does not
  hold when it does. See morph#597.
- **77 of the 711 locales Qt knows could not enter a negative number.**
  `morph::render::normalizeLocaleNumber` matched the minus sign as the literal
  byte `'-'`, and `formatCanonicalNumber` emitted one whatever the locale, so
  the pair was not inverse wherever the locale spells the sign differently.
  Measured over `QLocale::matchingLocales` under Qt 6.11.2: 23 locales use
  U+2212 (e.g. `eu_ES`) and 54 more prefix the sign with a bidi control mark,
  making it two or three code points (e.g. `fa_IR`, `az_IR`, `ar_EG`) —
  including `ar_DZ`, which failed although its sign *is* the ASCII hyphen,
  because of the U+200E in front of it. Both functions now take a fourth
  `std::string_view negativeSign = "-"`, matched and emitted as a whole string
  the way the separators already were, and the JavaScript mirror in
  `src/qt/forms/qml/DynamicForm.qml` does the same with
  `text.startsWith(sign, i)` — a one-code-unit comparison could not match the
  multi-unit forms. The renderer forwards `qtLocale.negativeSign` alongside the
  decimal point and group separator it already forwarded. A bare `'-'` stays
  accepted in every locale, since U+2212 and the bidi marks are on no keyboard,
  and an empty `negativeSign` reads as `"-"` rather than as "no sign" — a sign
  formatted to nothing would turn `-5` into `5`. The parameter is defaulted, so
  no existing call site changes behaviour. See `docs/spec/forms/forms.md`,
  "Locale data formatting"; morph#583.
- **A locale-formatted entry could submit ten times what the user typed.**
  `morph::render::normalizeLocaleNumber` dropped every occurrence of the group
  separator unconditionally, with no check on placement, so a de-DE user typing
  the US form `"1.5"` into a price field submitted `15` — a perfectly valid
  number that nothing downstream could recognise as wrong. `"1.50"` gave `150`,
  `"1.2.3.4"` gave `1234`, the en-US mirror image `"1,5"` gave `15`, and two
  equal separators silently ate the decimal. A group separator is now dropped
  only where one can legally be — preceded by one to three digits, followed by
  exactly three, never after the decimal separator — and everything else is
  reported as malformed; equal separators are rejected outright. Every
  well-formed entry (`"1.050,25"`, `"1.000.000,25"`, an ungrouped `"1050,25"`,
  fr-FR's U+202F grouping) normalises exactly as before. The JavaScript mirror
  in `src/qt/forms/qml/DynamicForm.qml` produced byte-identical wrong answers
  and carries the identical validation now. See `docs/spec/forms/forms.md`,
  "Locale data formatting"; morph#574.
- **A deep `Quantity` derivation overflowed the stack.** A running total
  (`total = total + one` in a loop) records one `ASTNode` per iteration chained
  through `left`, and every walk over that chain was recursive: destroying a
  21,000-node chain segfaulted at `-O0` while surviving 200,000 at `-O2`, where
  clang rewrites the release into a loop, and `equation()` segfaulted at 25,000
  nodes at every optimisation level. `~ASTNode` now releases the chain through
  a local worklist and all four `equation()` traversals run over an explicit
  stack, with the symbolic and substituted renderings unified into one stack
  machine; `equation()` output is unchanged. `MORPH_QUANTITY_PROVENANCE` keeps
  its default of `1` — the measured cost is real (54,056 KB and 0.034 s against
  12,236 KB and 0.006 s for a 200,000-iteration total) but the toggle changes
  observable behaviour, not just cost, so a bulk path that never calls
  `equation()` should set it to `0` rather than have it flipped underneath every
  build that did not. See `docs/spec/util/quantity_type.md`, *Provenance* and
  *Limitations*; morph#574.
- **The `RemoteServer` throughput benchmark wrote into a destroyed stack
  frame.** `tests/test_server_limits.cpp`'s reply counter was a stack local of
  the `BENCHMARK` body captured by reference into callbacks that run on pool
  workers; the body's 1 s deadline let it return with replies still in flight,
  and Catch2's next sample then constructed a fresh counter over the same stack
  slot. ThreadSanitizer reported two data races, both frames in this test — a
  false signal that reads as a race inside `RemoteServer`. The counter is now a
  `shared_ptr<std::atomic<int>>` the callbacks co-own; the deadline stays as a
  benchmark timeout rather than a correctness device. Test-only. See morph#565.
- **Both of the cross-field rule vocabulary's safety checks were bypassed by
  wrapping a rule in one combinator.** Unsatisfiability detection stopped at
  the first compound node, because it skipped any node without a `fields` key
  and `and`/`or`/`not` emit `conditions`/`condition` instead — so
  `ruleList(exactlyOneOf(&A::a, &A::b))` was a hard build failure while the
  identical contradiction inside `andOf(…)` shipped as a form nobody can
  submit. It now descends through `and` at any depth, and deliberately not
  through `or` or `not`, neither of which preserves a contradiction (a capping
  rule under `not` is satisfied by engaging every field, which is exactly what
  `required` demands). Separately, `andOf`/`orOf`/`notOf` and the `when` clause
  of `requiredWhen`/`visibleWhen`/`readonlyWhen` admitted any node exposing
  `test()`, which every rule node does — including `visibleWhen` and
  `readonlyWhen`, whose `test()` returns `true` unconditionally by design, so
  nesting one contributed a constant and silently vanished from the
  conjunction. Condition operands are now constrained on a new
  `morph::forms::Condition` concept that those three rules do not declare. The
  same constraints turn two deep-template failures into call-site errors:
  `andOf` over two different action types (153 lines → 24), and `equals`
  against a literal the field cannot be compared to (83 → 54). See
  `docs/spec/forms/forms.md`, "Unsatisfiable declarations" and "What may be a
  condition".
- **Two `Choice` (or `Ranged`) fields of different payload types in one action
  collapsed into a single `$defs` entry, and the renderer drew the wrong
  control.** Every `Choice<...>` instantiation was named `"Choice"` and every
  `Ranged<...>` `"Ranged"`, and glaze populates a `$defs` entry only once — so
  the second distinct instantiation was skipped and `$ref`ed the first one's
  definition. An `int64_t` picklist beside a `bool` one was described as a
  boolean, and `DynamicForm.qml` resolves the `$ref` and draws a checkbox for
  `"boolean"`; a `Ranged<0.0, 1.0, 0.1>` beside a `Ranged<0, 100>` was served
  as an integer, so every legal value of the double slider failed the type it
  was handed under. Both names are now composed per instantiation from
  arguments spelled in morph's own sources — never from `glz::name_v`, whose
  compiler-derived fallback would make the key differ between builds. **This
  changes `$defs` keys**, so it is a wire-shape change for any client that
  resolves `$ref` targets by name; the previous shape was a wrong schema rather
  than a compatible one. See `docs/spec/forms/choice.md` and
  `docs/spec/forms/widget_hints.md`, "Schema representation".
- **A shared instance whose first action failed could still be handed to a
  second attacher.** `docs/spec/core/shared_instances.md` promises it cannot be:
  a keyed instance created by an `AllowShared` handler whose hydrating first
  action fails must be evicted rather than shared. Both backends recorded that
  outcome too late to keep the promise. `LocalBackend` moved two independent
  atomics, so an attacher landing between them saw a not-poisoned instance whose
  first action had already failed; and both backends recorded it only *after*
  the trace sink's `endSpan`, the metric sink and the reply or `Completion`
  callback had run — every one of which is host code free to attach to the same
  key. Hydration now settles the instant the outcome is known, ahead of all of
  them, as a single compare-exchange on one atomic cell. Both backends also now
  keep their live instances in one `detail::InstanceDirectory` — one record per
  instance instead of the six and eight parallel `ModelId`-keyed maps they kept
  in lockstep by convention — so the register-or-attach logic, the eviction and
  the promotion exist once rather than once per backend.

- **`assignPrimary` could re-key an instance evicted from a key as poisoned.**
  Eviction unfiles the instance but does not make it anonymous: it was created
  for its original key and told so once, permanently, through
  `IModelHolder::attachIdentity`, and the action log is attached under that key
  too. Promoting it onto a second key therefore handed every later attacher a
  model that still identified itself as the first entity — and, since the
  poison came with it, the very next attach to the new key evicted it again and
  silently created a duplicate under a key the host had just assigned by hand.
  Promotion now applies only to an instance that has *never* held a key; the
  rest is a silent no-op like `assignPrimary`'s other declined cases, so the
  poisoned instance stays unshareable and the key stays free for a healthy one,
  exactly as `docs/spec/core/shared_instances.md`'s Failure modes section
  describes.

- **Sockets `morph::net::SocketServer` accepted were left non-blocking on
  macOS/BSD, failing every WebSocket handshake.** A regression from the
  accept-loop wakeup work below: since that change `listen()` puts the
  *listening* socket into `O_NONBLOCK`, and macOS/BSD propagate a listener's
  `O_NONBLOCK` onto the sockets `accept(2)` returns (POSIX permits this; Linux
  does not do it). `TcpSocket::tryAccept()` did nothing to the descriptor it
  handed back, so on those platforms every accepted connection was non-blocking
  too — and `recvSome()` treats `EAGAIN` as a fatal error, having cases only for
  `EINTR` and `ECONNRESET`. `SocketServer::clientLoop()`'s first act on a new
  connection is `performServerHandshake()`, a read issued before the client's
  `Upgrade` request has necessarily arrived, so it threw, `clientLoop()`
  swallowed the exception and closed the connection, and *every* connection
  failed its handshake. Linux-only CI could not see any of it. `TcpSocket`'s
  fd-adopting constructor now clears `O_NONBLOCK` on every descriptor it takes
  ownership of, which fixes `accept()` and `tryAccept()` by the same rule and
  leaves one place where a socket's blocking mode is decided; `setNonBlocking()`
  remains the explicit opt-out, applied after construction, as `listen()` does
  to its listener. `recvSome()` is deliberately unchanged: with the accepted
  socket blocking there is no correct answer for it to give on `EAGAIN` —
  returning `0` would be indistinguishable from a peer close and would truncate
  the handshake, looping would busy-wait — and swallowing it there would have
  hidden this bug rather than surfaced it. Because Linux never propagates the
  flag, an accept-side assertion cannot fail on CI's platform; the regression
  test asserts the constructor's reset directly, on a descriptor it makes
  non-blocking itself, and fails on Linux with the fix removed (verified both
  ways). The macOS half of the diagnosis rests on the probe in morph#478, not on
  a measurement made here. (morph#478)

- **`morph::net::SocketServer` teardown relied on Linux-only kernel behaviour
  and hung forever on macOS/BSD.** The accept thread parked in a blocking
  `accept(2)` and had no wakeup of its own: `close()` called
  `shutdownBoth()` on the *listening* socket and then joined the thread,
  which works only because Linux chooses to kick a parked `accept()` when its
  listener is shut down. That is not a POSIX guarantee, and macOS/BSD do not
  do it — the accept thread stayed parked, `join()` never returned, and
  `~SocketServer()` hung with no timeout, so no `tests/net` suite could run to
  completion on a Mac. The wakeup is now the server's own: `listen()` makes the
  listening socket non-blocking and creates a self-pipe, `acceptLoop()` waits in
  one `poll()` over the listener and that pipe and takes connections with the
  new `TcpSocket::tryAccept()` (which yields `std::nullopt` instead of parking
  on a stale readiness report), and `close()` writes one byte to the pipe before
  `join()`. Unconditionally compiled — there is no `#ifdef` on platform
  anywhere in the change — and it *replaces* the old mechanism rather than
  supplementing it: `close()` no longer touches the listening socket's
  `shutdown(2)` at all, so the pipe is the only thing that ends the loop on
  every platform, and deleting the wakeup `write()` hangs the Linux build as
  well. Two follow-on contract changes: `listen()` now returns `false` (and
  spawns no thread) if the pipe cannot be created, and `close()` releases the
  listening descriptor after the join, so `port()` reads `0` afterwards —
  the same post-close observation `QtWebSocketServer` already makes.
  `docs/spec/core/backend.md` records all of it. (morph#437)

- **`TcpSocket::accept()`'s `ECONNABORTED` comment named the wrong errno for
  the mechanism it was guarding.** It justified not retrying `ECONNABORTED` on
  the grounds that `shutdownBoth()` was "the documented way to break out of
  this call" — but `shutdown(listenfd, SHUT_RDWR)` unblocks a parked `accept()`
  with `EINVAL` (measured: `rc=-1 errno=22` on Linux 7.1.11), not
  `ECONNABORTED`, so the sentence was inaccurate from the day it was written
  and morph#437 makes it doubly so. Corrected in place, and the doc comment
  above it now states that `accept()` has no portable interruption mechanism at
  all. No `ECONNABORTED` retry was added: a reset-race repro over six shapes
  produced zero `ECONNABORTED` in 8,000 `accept()` calls on Linux (the kernel
  queues the reset connection and the abort surfaces as `ECONNRESET` on the
  next `recv`, which `recvSome` already absorbs), so a retry would be behaviour
  no test on this project's only CI platform could make fail. (morph#465)

- **A loaded machine could turn the `StrandExecutor` serialisation test into no
  serialisation check at all.** `tests/test_strand_race.cpp` drained 3200
  strand-serialised tasks against a fixed budget of 2000 x 1 ms sleeps, twenty
  times over, and asserted the drain with a `REQUIRE` placed *before* the
  `maxInFlight == 1` invariant the file exists for. When the budget expired,
  Catch2 aborted the case on the drain and the invariant was never evaluated —
  a scheduling outcome reported as a strand-serialisation failure. The drain is
  now `~StrandExecutor`'s own blocking wait rather than a deadline, which
  `docs/spec/concurrency_and_lifetimes.md` now documents as an exact
  complete-drain barrier: `_inFlight` never dips to zero across a re-arm, so
  once posting has stopped, `_inFlight == 0` means every queued task has run.
  The deficit is a `CHECK` carrying its numbers and the invariant stays a
  `REQUIRE`, so the two diagnoses are independent.

- **`scripts/scenario/mutate_scenario.py` never flipped `!=` back to `==`.**
  The pair list read `(" == ", " != "), (" !~ ", " ~ "), (" ~ ", " !~ ")`, so
  ` != ` was a *destination* of a flip and the source of none. Every `!=`
  assertion in the corpus was therefore mutated only at its `expect ok`/`expect
  err` kind — which the reply's own kind catches — while the comparison that
  carries the claim went unmeasured. Eleven assertions across four files were
  affected, five of them the `status != 2` lines whose file header explains
  they were written that way *because* `status == 0` raced the report runner's
  timer: the operator was the whole claim, and it was the one thing nothing
  checked. The mutator's own docstring already promised both directions
  (`==`↔`!=`, `~`↔`!~`). With the pair added, every one of the eleven now
  produces an `op !=->==` mutant and every one is caught.

- **`DynamicForm` rendered a C++ `enum class` as a free-text field, and its
  submit gate accepted values outside the set.** `schemaJson<A>()` describes a
  `glz::enumerate`d enum member completely — glaze emits a closed `oneOf` of
  `const` alternatives, each with its own `title` — but the renderer read none
  of it: `resolveProp` collapsed the `oneOf` to its first non-null branch (the
  nullable-`$ref` path added for morph#189, where branches differ only in
  nullability) and no field flag ever looked at `const`. A three-value set drew
  a `TextField`, and `ready` was `true` for `role = "Emperor"`. The renderer
  now recognises "every branch bar `null` carries a `const`" as a closed set,
  distinct from the nullability shape, draws it with the combo box a `Choice`
  already uses (no options action, no fetch — the values and their labels are
  already in the schema), and refuses a value outside the set so the gate means
  what it says. The bare JSON-Schema `enum` keyword is read the same way. Two
  shipped rungs were rendering free-text boxes for fully enumerated members
  today: `pastebin::CreatePaste`'s `visibility`/`editability` and
  `bookmarks::CreateBookmark`'s `visibility`. `docs/spec/forms/forms.md` gains
  a "Closed sets" section, and loses the false claim — the stated reason
  `oneOf` was treated as nullability-only — that glaze does not emit `oneOf`.

- **`docs/spec/error_handling.md`'s remote-error table was a subset presenting
  itself as an enumeration.** It listed 8 refusals where `RemoteServer` emits
  17, and told a client implementer that `unknown envelope kind` fires for
  anything that is not `register`/`deregister`/`execute` — where
  `dispatchMessage` compares `env.kind` against eight values, five of which
  (`attach`, `assign`, `instances`, `schemas`, `hello`) reach real branches. The
  section now names all eight kinds, groups every refusal by what a client
  should do about it (malformed request / refused on the merits / server
  condition), records which are reachable only under a non-default
  `LimitPolicy`, and separates the one message no transport can reach.

- **A scenario's `client` step sent `$capture` references literally.**
  `scripts/scenario/morph_scenario.py`'s `client` directive read its
  `principal=`/`token=`/`contextKey=` options raw while `session` ran the same
  values through the capture table, so `client books token=$token` — sign in,
  then work, the most natural shape there is — put the six characters `$token`
  on the wire as the bearer token. Nothing rejected the option and nothing
  warned; the run failed several steps later with a bare `unauthorized`, which
  reads as a broken authorizer rather than an unsubstituted variable. The two
  directives now agree: the three credential options expand captures, and a
  capture written in `url`, `model` or `protocol` — which are not expanded —
  is refused by name at the step that wrote it rather than sent literally.

- **`ladder_kanban_server` refused every client, including at login.**
  `kanban::auth::KanbanAuthorizer` inherited `SigningAuthorizer` without the
  anonymous carve-out `bookmarks::auth::BookmarksAuthorizer` carries, so
  `authorize()` demanded a bearer token for `AuthModel`/`Login` — the one
  action that mints one. Every action a fresh remote client could send was
  answered `err "unauthorized"`, login included, leaving the server usable only
  to a client already holding a token minted out of band (which is why
  `ladder_kanban_headless` takes one on its command line). It now admits that
  single model/action pair without a token, compared exactly, exactly as
  bookmarks does; `Login` still refuses the reserved `system:` principal
  namespace, so an anonymous caller can mint a token for a name it chooses and
  nothing more. Found by driving the real server from an out-of-process client
  — the rung's own tests call `AuthModel::execute()` directly, which never
  consults an authorizer.

- A throw out of `RemoteServer::dispatchExecute` stranded the same per-model
  execute-ordering ticket the shutdown gate did, by a different route.
  `dispatchExecute` has no `try`/`catch` of its own and its `rejectAndRelease`
  helper covers only the explicit early returns, so an exception unwound past
  all of them into `dispatchMessage`'s outer catch, which replied but released
  nothing — leaving a later same-model `execute` parked in `awaitExecuteTurn`
  forever, a pool worker blocked for the rest of the process's life, and
  `drainedWithin()` unable to succeed. Reachable through documented extension
  points: `IAuthorizer::authorize`/`authenticate`/`authorizeInstance` are
  non-`noexcept` virtuals a host implements, and `missingRequiredFields` parses
  the payload under `PayloadCompleteness::RequireDeclaredFields`. The ticket is
  now owned by an RAII holder that releases it on every exit path — return,
  throw, or a branch added later — rather than by a release written out at each
  call site, which is the convention that had now been missed twice.
- An `execute` refused by `RemoteServer`'s shutdown gate took a per-model
  execute-ordering ticket and never released it, stranding a same-model
  `execute` that had already passed the gate. Tickets are taken in send order
  on the transport thread, but the pool may run the two posted tasks in either
  order, so the later ticket can be parked in `awaitExecuteTurn` — a wait with
  no deadline — by the time the earlier one is refused. That caller then never
  received a reply (or a spurious `err "timeout"` where `executeTimeout` was
  configured), a pool worker was blocked for the rest of the process's life,
  and `drainedWithin()` could never succeed — so the defect broke the very
  graceful-shutdown sequence during which it fired. The gate now releases the
  ticket before replying.
- `Completion<T>` could be settled into a state that never resolved: rejecting
  with a **null** `std::exception_ptr` set `ready` while leaving `error` falsy,
  and neither `then()` nor `onError()` can act on that — so a handler attached
  afterwards was neither fired nor queued, and the orphan logger was suppressed
  too. A handler attached *before* such a rejection did fire, but with a null
  `exception_ptr`, which is undefined behaviour to `std::rethrow_exception`.
  `CompletionState::setException` now substitutes a real exception rather than
  storing a null, so `ready` always implies exactly one of `value`/`error`.
  `Bridge`'s registration resolver — which reached this through
  `whenBound()` when a registration reply's id was discarded — supplies a
  message naming the binding instead of relying on the generic substitute.

- `CustomerModel::execute(ListAccounts)` dereferenced `QuerySingle`'s optional
  unchecked (inherited verbatim from the old `AccountModel`); it now throws
  `NotFound`.
- Key extraction for an action carrying an *empty* strong id was undefined
  behaviour in the ledger and lims rungs: their hand-written `key()` bodies
  did `*action.id` on a disengaged `std::optional`, which handed back whatever
  the union held and attached the caller to an arbitrary shared instance.
  Routing through `morph::model::keyToString` refuses the empty id, and
  `BridgeHandler::execute` turns the refusal into a rejected `Completion`.
- A flaky assertion in `tests/test_concurrency_invariants.cpp`: `succeeded > 0`
  during backend churn is a scheduling race, not an invariant — under a
  thread-serialising tool the switcher can cancel every in-flight call. The
  same structural property is now asserted against the quiesced bridge.
- Stale pre-JSON "5-part"/"6-part protocol" wording in `docs/ARCHITECTURE.md`
  and a test comment — the wire has been a JSON `Envelope` since it superseded
  the pipe-delimited protocol.
