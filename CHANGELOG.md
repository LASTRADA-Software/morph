# Changelog

Notable changes to morph, in [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
format. morph is pre-1.0 and has no tagged releases yet: `master` is the only
supported line, this file tracks notable changes going forward, and history
predating it lives in `git log`. From 1.0 on, versioning follows the policy
published in `docs/spec/VERSIONING.md` (SemVer for the public, non-`detail`
API surface).

## [Unreleased]

### Added

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
