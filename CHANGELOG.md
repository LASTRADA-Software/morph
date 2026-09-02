# Changelog

Notable changes to morph, in [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
format. morph is pre-1.0 and has no tagged releases yet: `master` is the only
supported line, this file tracks notable changes going forward, and history
predating it lives in `git log`. From 1.0 on, versioning follows the policy
published in `docs/spec/VERSIONING.md` (SemVer for the public, non-`detail`
API surface).

## [Unreleased]

### Added

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
