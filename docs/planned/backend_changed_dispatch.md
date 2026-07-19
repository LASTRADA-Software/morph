# Compile-time `onBackendChanged` dispatch (planned)

> **Status: planned — not yet implemented.** This spec extends
> [backend.md](../spec/core/backend.md) and [bridge.md](../spec/core/bridge.md). It removes the RTTI
> `dynamic_cast` sweep in `LocalBackend::notifyBackendChanged` documented as a
> limitation in `backend.md`. See [todo.md](../todo.md).

## The gap

`LocalBackend::notifyBackendChanged` (`backend.hpp`) iterates **every** live
model under the registry lock and `dynamic_cast`s each holder to
`IBackendChangedSink`:

```cpp
void notifyBackendChanged() override {
    std::scoped_lock lock{_regMtx};                 // held across the whole sweep
    for (auto& [mid, holder] : _models) {
        if (dynamic_cast<IBackendChangedSink*>(holder.get()) != nullptr) {
            auto* sink = dynamic_cast<IBackendChangedSink*>(holder.get());
            // post sink->onBackendChanged() to the model's strand
        }
    }
}
```

Three costs:

1. **RTTI dependency.** `dynamic_cast` requires RTTI enabled; the rest of the
   framework does not otherwise force it on this path.
2. **O(models) under the lock.** The sweep runs `_regMtx`-held and its cost
   scales with the number of live models — even though, in a typical app, *few or
   none* implement `IBackendChangedSink`.
3. **Two casts per holder** in the shipped code (test-once, cast-again).

The information the sweep recovers at runtime — "does this model type implement
the sink?" — is known at **compile time**, at the point the model is registered.

## Goal

Record, at registration time, whether each model type is backend-change-aware,
and drive `notifyBackendChanged` from that record — so the sweep visits only the
models that actually care, needs no RTTI, and does no per-holder dynamic cast.

## Design

### Detect the capability at registration

`ModelHolder<M>` already knows `M` statically. The `BackendChangedNotifiable`
concept (`model.hpp`) already detects a `void onBackendChanged()` member at
compile time (it is how the mixin is selected today). Capture that bit when the
holder is built:

```cpp
// In IModelHolder (morph::model::detail):
[[nodiscard]] virtual bool isBackendChangeAware() const noexcept = 0;

// In ModelHolder<M>: answer from the concept, evaluated at compile time.
[[nodiscard]] bool isBackendChangeAware() const noexcept override {
    return BackendChangedNotifiable<M>;
}

// onBackendChanged() also becomes a (no-op-by-default) virtual on IModelHolder,
// overridden by ModelHolder<M> to call M::onBackendChanged() only when the
// concept holds — so the backend calls it through the base, no cast:
void onBackendChanged() override { /* ModelHolder<M> forwards iff aware */ }
```

This replaces the runtime `dynamic_cast` with a virtual call whose result is a
compile-time constant per model type. No RTTI is required for either the
capability query or the invocation.

### Narrow the sweep

`LocalBackend` maintains a **set of change-aware model ids** alongside `_models`,
updated at register/deregister:

- `registerModel` — if `holder->isBackendChangeAware()`, insert `mid` into
  `_changeAware`.
- `deregisterModel` — erase `mid` from `_changeAware` (no-op if absent).

`notifyBackendChanged` then iterates only `_changeAware`, posting
`holder->onBackendChanged()` (the base virtual) to each such model's strand:

```cpp
void notifyBackendChanged() override {
    std::scoped_lock lock{_regMtx};
    for (auto mid : _changeAware) {
        auto it = _models.find(mid);
        if (it != _models.end()) {
            // post it->second->onBackendChanged() to mid's strand (unchanged)
        }
    }
}
```

When no model is change-aware (the common case) the loop body runs zero times;
cost is O(aware models), not O(all models).

### The threading contract is unchanged

Everything `bridge.md` and `concurrency_and_lifetimes.md` guarantee about
`onBackendChanged` still holds verbatim: it is **posted** onto the model's own
strand (not run inline under `_mtx`), fires exactly once per `switchBackend`, on
the new backend's fresh instance, after all handlers are re-registered. This spec
changes only *how the backend finds which models to notify* (a maintained set vs.
an RTTI sweep), not *when or where the callback runs*. The `_regMtx`/strand
interaction is identical.

## Backward compatibility

- **No public API change.** `IBackendChangedSink` remains the model-facing
  contract; a model still just defines `void onBackendChanged()`. The
  `isBackendChangeAware`/`onBackendChanged` additions are on the internal
  `IModelHolder` (`morph::model::detail`), which application code never names.
- **Same observable behavior.** The exact same set of models get notified, in the
  same posted-to-strand manner; only the discovery mechanism changes. A model
  that does not implement the callback is skipped, as before.
- **RTTI no longer required on this path.** Builds that disabled RTTI (or want
  to) are no longer blocked by `notifyBackendChanged`.

## Non-goals

- **Not a change to `SimulatedRemoteBackend`.** Its `notifyBackendChanged` is
  already a no-op (models live in the `RemoteServer`); this spec is about
  `LocalBackend` only.
- **Not a new notification ordering.** The set is iterated in an unspecified
  order, same as the map sweep today; models must not assume an order across each
  other (each runs serialised on its own strand regardless).

## Testing (planned)

- A model implementing `onBackendChanged` is notified on `switchBackend`
  (posted to its strand, fires once) — unchanged from today.
- A model **not** implementing it is never notified and incurs no per-switch
  work; a build with RTTI disabled still compiles and passes.
- Register/deregister correctly maintain `_changeAware`: a deregistered
  change-aware model is not notified on a subsequent switch; a re-registered one
  is.
- Mixed population (some aware, some not): only the aware subset is notified.

## Cross-references

- [backend.md](../spec/core/backend.md) — `LocalBackend::notifyBackendChanged`, the
  `IBackendChangedSink` limitation ("uses RTTI over every model under the
  registry lock") this removes, and register/deregister where `_changeAware` is
  maintained.
- [bridge.md](../spec/core/bridge.md) — `switchBackend` → `notifyBackendChanged`, and the
  posted-to-strand `onBackendChanged` contract this preserves.
- [concurrency_and_lifetimes.md](../spec/concurrency_and_lifetimes.md) — the
  strand-serialised, fires-once, on-the-new-instance guarantees that are
  unchanged by this refactor.
- [registry.md](../spec/core/registry.md) — `ModelHolder<M>`, `BackendChangedNotifiable`, the
  `BackendChangedMixin` compile-time detection reused here.
