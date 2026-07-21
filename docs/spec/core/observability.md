# The `morph::observe` observability seam — design

`morph::observe` is a lightweight, injectable metrics/trace seam modelled on
`morph::log`'s replaceable-sink pattern ([logger.md](logger.md)). A host
installs a `MetricSink` and/or a `TraceSink` once at startup; the framework's
hot paths — `RemoteServer` dispatch/register/deregister, `LocalBackend`
dispatch/register/deregister, `SyncWorker::run()`, and
`ReconnectCoordinator::onOnline()` — feed them. With no sink installed, every
call site rejects behind a single relaxed atomic load and constructs nothing:
an unconfigured build has zero observability overhead and behaves exactly as
if the seam did not exist. `RemoteServer` additionally exposes a
`health()`/`setHealthHandler()` readiness query.

## Contents

- [Metrics](#metrics)
- [Tracing](#tracing)
- [Health / readiness](#health--readiness)
- [Call sites](#call-sites)
- [Thread safety](#thread-safety)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Metrics

`Metric` is a closed `enum class` of eight observation kinds:

| Enumerator | Kind | Meaning |
|---|---|---|
| `executeLatencyMs` | value | One dispatch's wall time in milliseconds (model strand). |
| `executeInFlight` | gauge | Concurrent in-flight executes. |
| `executeErrors` | counter | Executes that resolved via an error path. |
| `registerCount` | counter | `register` calls (attempted, not just successful). |
| `deregisterCount` | counter | `deregister` calls. |
| `queueDepth` | gauge | `IOfflineQueue` pending items at the start of a `SyncWorker::run()`. |
| `reconnectAttempts` | counter | `ReconnectCoordinator::onOnline`'s `tryReconnect` attempts. |
| `reconnectOutcome` | counter | One per `onOnline()` call, tagged by outcome. |

Each observation is a `MetricEvent{metric, value, tags}`; `tags` is a
`std::span<const std::pair<std::string_view, std::string_view>>` carrying
dimensions (`modelType`, `actionType`, `outcome`, …) without baking them into
the enum. `setMetricSink(MetricSink)` installs the process-wide sink (a
`std::function<void(const MetricEvent&)>`, thread-safe mutex-guarded swap,
exactly like `morph::log::setLogger`); `metricsEnabled()` is the lock-free
`[[nodiscard]] bool` hot-path check, backed by a relaxed `std::atomic<bool>`
set whenever a non-null sink is installed. `setMetricSink(nullptr)` disables
metrics again.

## Tracing

Tracing reuses `Context::requestId` ([session.md](../session/session.md)) as
the correlation id. `TraceSink` has two members:

```cpp
struct TraceSink {
    std::function<SpanId(std::string_view requestId, std::string_view modelType,
                         std::string_view actionType)> beginSpan;
    std::function<void(SpanId, bool ok)> endSpan;
};
```

`SpanId` is a `std::uint64_t`; `0` is a reserved "no span" sentinel. Both
`beginSpan` and `endSpan` must be set for `setTraceSink` to enable tracing — a
sink with only one of the two behaves as if none were installed, and the
internal `beginSpan` helper keeps returning `0`. Call sites unconditionally
call `beginSpan(...)` at the start of a dispatch and `endSpan(id, ok)` at the
end; when tracing is disabled (or `id == 0`), both are single cheap checks
that touch neither a mutex nor the trace sink.

## Health / readiness

`RemoteServer` ([backend.md](backend.md)) exposes:

```cpp
struct HealthStatus {
    bool ready;
    std::size_t liveModels;
    std::size_t inFlight;
};

[[nodiscard]] HealthStatus health() const;
void setHealthHandler(std::function<void(const HealthStatus&)>);
```

`health()` reads `liveModels` from the model registry (under the same mutex
`register`/`deregister`/`execute` use) and `inFlight` from `_inFlightExecutes`
— the same atomic counter `LimitPolicy::maxInFlightExecutes` enforces and the
`executeInFlight` metric reports (see [Thread safety](#thread-safety)): one
counter, three consumers, never double-counted. `setHealthHandler` installs a
callback that fires immediately with the current snapshot (so a subscriber
never has to wait for a transition to see a baseline) and would fire again on
any future readiness change. **`ready` is always `true` today** — nothing in
the current codebase flips it. It is retained as a stable seam for a future
shutdown sequence (`docs/planned/graceful_shutdown.md`'s `beginShutdown()`) to
flip to `false` and re-invoke the handler, without any change to this API.
`morph` does not embed an HTTP health endpoint; a deployment's transport (e.g.
`QtWebSocketServer`) is expected to expose `health()` over whatever probe
protocol it serves.

## Call sites

| Site | Metrics emitted | Trace hook |
|---|---|---|
| `RemoteServer::dispatchMessage`'s `register` branch | `registerCount` | — |
| `RemoteServer::dispatchMessage`'s `deregister` branch | `deregisterCount` | — |
| `RemoteServer::dispatchExecute`'s in-flight admit/complete | `executeInFlight` (on admit and again when the reply is delivered) | — |
| `RemoteServer::dispatchExecute`'s strand task | `executeLatencyMs`, `executeErrors` | `beginSpan`/`endSpan` around `ActionDispatcher::dispatch` |
| `LocalBackend::registerModel` | `registerCount` | — |
| `LocalBackend::deregisterModel` | `deregisterCount` | — |
| `LocalBackend::execute`'s strand task | `executeLatencyMs`, `executeInFlight`, `executeErrors` | `beginSpan`/`endSpan` around `localOp` |
| `SyncWorker::run()` | `queueDepth` (once, at drain) | — |
| `ReconnectCoordinator::onOnline()` | `reconnectAttempts` (per attempt), `reconnectOutcome` (once, tagged `outcome`) | — |

`registerCount`/`deregisterCount` count every call, not just successful ones —
an unauthorized or malformed `register` still increments it, so the counter
reflects load on that path rather than only outcomes. `executeLatencyMs`/
`executeErrors` are scoped to the strand-posted dispatch itself; an `execute`
that fails before dispatch even starts (unknown model id, failed
authorization, `LimitPolicy` rejecting with `"server busy"`/`"too many
models"`) does not touch the latency/error metrics or the trace hooks,
mirroring the fact that no strand task ever ran. On `RemoteServer`, a
`LimitPolicy::executeTimeout` firing before the strand task finishes still
lets that task run to completion later (the model action itself is never
interrupted, per `backend.md`'s `LimitPolicy` docs); when it does, its
`executeLatencyMs`/`executeErrors`/trace-span instrumentation still fires,
even though the client may have already received the timeout reply — the
in-flight gauge's paired decrement, by contrast, is centralized so it fires
exactly once regardless of which path (timeout or dispatch) resolves the call
first.

## Thread safety

Metrics and tracing each have their own mutex (`metricMtx`, `traceMtx`) guarding
their sink and their own lock-free `std::atomic<bool>` enabled flag
(`metricsOn`, `traceOn`), independent of each other and of `morph::log`'s
state — installing a metric sink never contends with logging or tracing.
`RemoteServer`'s `_inFlightExecutes` counter (introduced alongside
`LimitPolicy::maxInFlightExecutes`, see [backend.md](backend.md)) is a plain
`std::atomic<std::size_t>`, read/written with relaxed ordering (advisory, like
`morph::log`'s level check — a gauge sample racing a concurrent
increment/decrement may be observed slightly stale, never torn). This seam
reuses that counter for the `executeInFlight` metric and `HealthStatus::inFlight`
rather than introducing a second, separately-maintained counter for the same
concept. `LocalBackend`'s in-flight counter is a
`std::shared_ptr<std::atomic<std::size_t>>` rather than a plain member: its
strand-posted tasks capture a copy of the `shared_ptr`, never `this`, so the
counter stays valid even if the backend is destroyed while a task is still
queued or running (see [backend.md](backend.md)'s Lifetime & ownership and
the `~StrandExecutor` note below).

## API reference

### `Metric`

`enum class Metric : std::uint8_t` — see [Metrics](#metrics) for the eight enumerators.

### `MetricEvent`

| Member | Type | Notes |
|---|---|---|
| `metric` | `Metric` | Which metric this observation is for. |
| `value` | `double` | The observed value. |
| `tags` | `std::span<const std::pair<std::string_view, std::string_view>>` | Dimensions; empty for untagged metrics. |

### Metric configuration

| Symbol | Signature | Notes |
|---|---|---|
| `MetricSink` | `using MetricSink = std::function<void(const MetricEvent&)>` | Sink type. |
| `setMetricSink` | `void setMetricSink(MetricSink)` | Thread-safe (mutex-guarded swap). `nullptr` disables metrics. |
| `metricsEnabled` | `[[nodiscard]] bool metricsEnabled() noexcept` | Lock-free hot-path check (relaxed atomic load). |

### `SpanId` / `TraceSink`

| Symbol | Signature | Notes |
|---|---|---|
| `SpanId` | `using SpanId = std::uint64_t` | `0` is the "no span" sentinel. |
| `TraceSink::beginSpan` | `std::function<SpanId(std::string_view requestId, std::string_view modelType, std::string_view actionType)>` | Called at dispatch start (on the strand). |
| `TraceSink::endSpan` | `std::function<void(SpanId, bool ok)>` | Called at dispatch end. |
| `setTraceSink` | `void setTraceSink(TraceSink)` | Both callbacks must be set to enable tracing. Thread-safe. |

### `ScopedObserveOverride`

RAII guard, snapshot-only constructor, restores the previous metric and trace
sinks on destruction. Mirrors `morph::log::ScopedLoggerOverride`'s
snapshot-only constructor; used by tests to avoid leaking a sink across test
cases. Not copyable or movable.

### `RemoteServer::HealthStatus` / `health()` / `setHealthHandler()`

See [backend.md](backend.md)'s `RemoteServer` API reference table.

### Internal detail (not for direct use)

| Symbol | Signature | Notes |
|---|---|---|
| `detail::ObserveState` | struct | Process-wide singleton holding both sinks, their mutexes, and their enabled atomics. |
| `detail::observeState` | `ObserveState& observeState()` | Meyers singleton accessor. |
| `detail::emitMetric` | `void emitMetric(Metric, double, std::span<const std::pair<std::string_view, std::string_view>> tags = {})` | Framework-internal emit entry point; no-op (one atomic load) if no sink installed. |
| `detail::beginSpan` | `[[nodiscard]] SpanId beginSpan(std::string_view, std::string_view, std::string_view)` | Framework-internal; returns `0` if tracing disabled. |
| `detail::endSpan` | `void endSpan(SpanId, bool)` | Framework-internal; no-op if `id == 0` or tracing disabled. |
| `offline::detail::reconnectOutcomeName` | `constexpr std::string_view reconnectOutcomeName(ReconnectOutcome) noexcept` | Maps a `ReconnectOutcome` to the `reconnectOutcome` metric's `"outcome"` tag value. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| API surface | Mirrors `morph::log` exactly: public config functions + internal `detail::` emit helpers | Consistency with the framework's one existing observability seam; engineers who know `morph::log` already know this API's shape. |
| Two independent sinks (metric, trace), each with its own mutex/atomic | No shared state between them | Installing/using one never contends with the other; matches the fact that they observe different things (numbers vs. spans) and can be wired to entirely different backends. |
| `registerCount`/`deregisterCount` count every call, not just successes | Counts attempts | Gives an accurate load/rate signal on that path (an unauthorized or malformed register still costs the server work); `executeErrors` is the separate counter for outcome-scoped failure signal on the execute path. |
| `RemoteServer`'s in-flight metric reuses `_inFlightExecutes` | No second counter added | `LimitPolicy::maxInFlightExecutes` already introduced an atomic in-flight counter with exactly this admit/complete lifecycle; adding a parallel `_inFlight` member for the metric alone would require keeping two counters in lockstep for no benefit. `health()`'s `inFlight` field reads the same counter. |
| `LocalBackend`'s in-flight counter is a `shared_ptr`, not a plain atomic member | Avoids capturing `this` in a strand-posted lambda | `LocalBackend`'s existing strand tasks already avoid `this` captures for lifetime safety (see Limitations); the counter follows the same rule rather than becoming a new dangling-pointer risk. `RemoteServer`'s equivalent counter is a plain atomic member because its strand task already captures `self = shared_from_this()`, keeping the whole object alive. |
| `setHealthHandler` fires immediately on install | Calls the handler once, synchronously, right after storing it | A subscriber gets a baseline status without waiting for the first (currently nonexistent) transition; also makes the stored handler genuinely used rather than write-only. |
| `ready` has no internal mutator yet | `RemoteServer` never sets `_ready` to `false` in this release | The seam is deliberately landed ahead of `docs/planned/graceful_shutdown.md`'s `beginShutdown()`, which is expected to be the first caller — landing the field and the handler-firing contract now means that future change needs no API change. |

## Limitations

- **No sampling/aggregation/histograms in `morph`.** The framework emits raw
  observations (a value or a count of `1`); rate-limiting, histogram bucketing,
  and sampling are the host sink's job, exactly as `morph::log` ships no log
  rotation or filtering beyond level.
- **No metrics/tracing/HTTP dependency shipped.** The seam is a
  `std::function` pair the host wires to Prometheus, StatsD, OpenTelemetry, or
  a test spy; there is no bundled backend.
- **`ready` is always `true`.** See [Health / readiness](#health--readiness)
  and the corresponding design decision — no code path in this release flips
  it.
- **No preemption or control.** Metrics/health *observe*; they never throttle
  or cancel work. Bounding behavior belongs to [backend.md](backend.md)'s
  `LimitPolicy`, which this seam only makes visible.
- **Does not change dispatch semantics or the wire.** The hooks wrap existing
  call sites; there are no new envelope fields and no behavior change when
  unconfigured.
- **`executeLatencyMs`/`executeErrors` (and, on `LocalBackend`,
  `executeInFlight`) are scoped to the strand-posted dispatch.** A fast-fail
  before the strand task is posted (unknown model id, failed authorization, a
  `LimitPolicy` rejection) emits none of them — see [Call sites](#call-sites).

## Cross-references

- [logger.md](logger.md) — the replaceable-sink pattern (global `std::function`
  sink, mutex-guarded swap, lock-free hot-path check) this seam copies, and the
  sink-signature limitation (`(LogLevel, string_view)` carries no structured
  numbers or spans) that makes a separate seam necessary.
- [backend.md](backend.md) — `RemoteServer`/`LocalBackend` dispatch call sites
  the metric/trace hooks wrap, `LimitPolicy`'s `_inFlightExecutes` counter this
  seam reuses, and where `HealthStatus`/`health()`/`setHealthHandler()` live.
- [offline.md](../offline/offline.md) — `SyncWorker`/`ReconnectCoordinator` and
  the `IOfflineQueue` depth the `queueDepth`/`reconnectAttempts`/
  `reconnectOutcome` metrics report on.
- [session.md](../session/session.md) — `Context::requestId`, reused as the
  trace correlation id.
