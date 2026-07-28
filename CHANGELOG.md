# Changelog

Notable changes to morph, in [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
format. morph is pre-1.0 and has no tagged releases yet: `master` is the only
supported line, this file tracks notable changes going forward, and history
predating it lives in `git log`. From 1.0 on, versioning follows the policy
published in `docs/spec/VERSIONING.md` (SemVer for the public, non-`detail`
API surface).

## [Unreleased]

### Added

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

- `CustomerModel::execute(ListAccounts)` dereferenced `QuerySingle`'s optional
  unchecked (inherited verbatim from the old `AccountModel`); it now throws
  `NotFound`.
- A flaky assertion in `tests/test_concurrency_invariants.cpp`: `succeeded > 0`
  during backend churn is a scheduling race, not an invariant — under a
  thread-serialising tool the switcher can cancel every in-flight call. The
  same structural property is now asserted against the quiesced bridge.
- Stale pre-JSON "5-part"/"6-part protocol" wording in `docs/ARCHITECTURE.md`
  and a test comment — the wire has been a JSON `Envelope` since it superseded
  the pipe-delimited protocol.
