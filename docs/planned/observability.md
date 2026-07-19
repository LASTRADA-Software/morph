# Observability: metrics, tracing, health/readiness (planned)

> **Status: planned — not yet implemented.** This spec introduces an injectable
> observability seam modelled on the replaceable-sink pattern in
> [logger.md](../spec/core/logger.md), and adds a health/readiness signal to
> [backend.md](../spec/core/backend.md)'s `RemoteServer`. See [todo.md](../todo.md).

## The gap

Logging is a replaceable sink ([logger.md](../spec/core/logger.md)): a global
`std::function<void(LogLevel, std::string_view)>` swapped once at startup via
`setLogger`, with a lock-free level check on the hot path. That is the only
observability seam. There is:

- **No metrics.** Nothing exposes dispatch latency, in-flight execute count,
  offline-queue depth, reconnect counts, or register/deregister rates. An operator
  cannot see whether a `RemoteServer` is saturated, whether
  `SyncWorker`/`ReconnectCoordinator` is churning, or how long `Model::execute`
  takes — the numbers the planned `LimitPolicy`
  ([transport_limits.md](transport_limits.md)) is meant to bound are not even
  observable.
- **No tracing hooks.** A single `execute` crosses the calling thread, the model
  strand, and the callback executor ([backend.md](../spec/core/backend.md)'s "Thread
  context"), but there is no span/correlation seam to follow one call across those
  threads. `Context::requestId` exists on the wire ([wire.md](../spec/core/wire.md))
  but nothing consumes it for tracing.
- **No health/readiness signal.** `RemoteServer` has no "am I up and able to
  serve?" callback or endpoint, so a load balancer or orchestrator has nothing to
  probe.

`morph` deliberately keeps the logger minimal — its sink signature is only
`(LogLevel, std::string_view)`, with "no source location, no category, no
timestamp, no thread id" ([logger.md](../spec/core/logger.md)'s Limitations). Metrics
and traces cannot be smuggled through that signature; they need their own seam,
built the same injectable way.

## Goal

A lightweight, **injectable** observability seam — a metrics/trace sink the host
installs once at startup, exactly like `setLogger` — that the framework's hot
paths feed, plus a health/readiness callback on `RemoteServer`. All default to a
no-op, so an unconfigured build has zero observability overhead and behaves
exactly as today.

## Design

### 1. A metrics sink (NEW), mirroring the logger

A single global sink, installed once, receiving typed metric events. The design
copies `morph::log`'s discipline: a `std::function` sink guarded for swap, and a
cheap "is anyone listening?" check on the hot path so an unconfigured build pays
nothing.

```cpp
// namespace morph::observe — NEW.

enum class Metric : std::uint8_t {
    executeLatencyMs,     // one dispatch's wall time (model strand)
    executeInFlight,      // gauge: concurrent in-flight executes
    executeErrors,        // counter: executes that resolved via err/onError
    registerCount,        // counter: register calls
    deregisterCount,      // counter: deregister calls
    queueDepth,           // gauge: IOfflineQueue pending items at drain
    reconnectAttempts,    // counter: ReconnectCoordinator tryReconnect calls
    reconnectOutcome,     // counter, tagged by ReconnectOutcome
};

/// A metric observation. `tags` carries dimensions (modelType, actionType,
/// outcome) without baking them into the enum.
struct MetricEvent {
    Metric metric;
    double value;
    std::span<const std::pair<std::string_view, std::string_view>> tags;
};

using MetricSink = std::function<void(const MetricEvent&)>;

/// Install the process-wide metrics sink. Default: none (no-op).
/// Thread-safe (mutex-guarded swap), exactly like morph::log::setLogger.
void setMetricSink(MetricSink);

/// Lock-free "is a sink installed?" check for the hot path — an atomic bool,
/// like morph::log's atomic minLevel. When false, framework call sites skip
/// building the MetricEvent entirely.
[[nodiscard]] bool metricsEnabled() noexcept;
```

- **Hot-path discipline** matches [logger.md](../spec/core/logger.md): the framework
  checks `metricsEnabled()` (a relaxed atomic load, no mutex) before constructing
  any `MetricEvent`, so a suppressed metric pays only the atomic load — the same
  contract that lets a filtered `logDebug` skip `std::format`.
- **The sink adapts to any backend.** A host wires `MetricSink` to Prometheus,
  StatsD, OpenTelemetry, or a test spy — `morph` ships no metrics dependency, just
  as it ships no crypto or logging dependency. The default is no sink installed.

### 2. A trace seam (NEW)

Tracing reuses the existing `Context::requestId` ([wire.md](../spec/core/wire.md),
[session.md](../spec/session/session.md)) as the correlation id and adds span
begin/end hooks the framework calls around a dispatch:

```cpp
// namespace morph::observe — NEW.
using SpanId = std::uint64_t;

struct TraceSink {
    /// Called when an execute begins dispatch (on the strand). Returns a span
    /// id the framework passes back to endSpan. requestId ties the span to the
    /// caller's Context.
    std::function<SpanId(std::string_view requestId,
                         std::string_view modelType,
                         std::string_view actionType)> beginSpan;
    /// Called when the dispatch resolves (ok or err).
    std::function<void(SpanId, bool ok)> endSpan;
};

void setTraceSink(TraceSink);   // default: none (no-op), thread-safe swap
```

- The framework calls `beginSpan`/`endSpan` around the `ActionDispatcher::dispatch`
  call inside `RemoteServer::dispatchExecute` and around `localOp` in
  `LocalBackend::execute` ([backend.md](../spec/core/backend.md)) — the same two call
  sites the journal records at ([ARCHITECTURE.md](../ARCHITECTURE.md)), so a trace
  covers exactly one `Model::execute`.
- With no trace sink, both hooks are skipped behind the same enabled-check as
  metrics — no overhead.

### 3. Health / readiness on `RemoteServer` (NEW)

Add a readiness query and an optional state-change callback to `RemoteServer`
([backend.md](../spec/core/backend.md)):

```cpp
// namespace morph::backend — NEW on RemoteServer.
struct HealthStatus {
    bool ready;                 // able to accept and dispatch
    std::size_t liveModels;     // current registry size
    std::size_t inFlight;       // current in-flight executes
};

/// Snapshot the server's current health. Cheap; safe from any thread.
[[nodiscard]] HealthStatus health() const;

/// Optional callback fired when readiness changes (e.g. shutdown begins).
/// Default: none. The transport (QtWebSocketServer) can expose health() over
/// an HTTP/probe endpoint; morph does not embed an HTTP server.
void setHealthHandler(std::function<void(const HealthStatus&)>);
```

- `health()` reads the counts the metrics seam already tracks (`liveModels` from
  the registry under `_regMtx`, `inFlight` from the in-flight counter that
  [transport_limits.md](transport_limits.md) also uses), so the two features share
  state rather than double-counting.
- `morph` does **not** ship an HTTP health endpoint — that is the transport's
  job, exactly as confidentiality is ([security.md](../spec/security.md)). A Qt
  deployment exposes `health()` from a small handler; a
  [non_qt_transport.md](non_qt_transport.md) exposes it however it serves.

## Non-goals

- **No metrics/tracing/HTTP dependency in the core.** Like the logger, the seam is
  a `std::function` the host wires to its stack of choice; `morph` ships the
  hooks and no backend. Default is no-op with zero overhead.
- **Not a replacement for the logger.** `morph::log` still carries free-text
  diagnostics; the observability seam carries *structured numbers and spans* the
  logger's `(LogLevel, string_view)` signature cannot express
  ([logger.md](../spec/core/logger.md)). They are complementary sinks.
- **No sampling/aggregation logic in `morph`.** The framework emits raw
  observations; rate-limiting, histograms, and sampling live in the host's sink.
- **No preemption or control.** Metrics/health *observe*; they do not throttle or
  cancel. Bounding behavior is [transport_limits.md](transport_limits.md)'s
  `LimitPolicy`, which this seam only makes visible.
- **Does not change dispatch semantics or the wire.** Hooks wrap existing call
  sites; no new envelope fields, no behavior change when unconfigured.

## Testing (planned)

- With a `MetricSink` installed, an `execute` emits `executeLatencyMs` and toggles
  `executeInFlight`; an erroring action emits `executeErrors`; register/deregister
  emit their counters. With **no** sink installed, none are constructed and
  `metricsEnabled()` short-circuits (overhead + regression guard).
- With a `TraceSink`, one `execute` produces exactly one `beginSpan`/`endSpan`
  pair carrying the caller's `requestId`, with `ok` reflecting success vs. `err`.
- `RemoteServer::health()` reports `liveModels`/`inFlight` matching the registry
  and in-flight state under register/execute churn; `setHealthHandler` fires on a
  readiness transition (e.g. shutdown).
- Sink swap is thread-safe under concurrent logging-style contention (mirrors the
  `morph::log` thread-safety test).

## Cross-references

- [logger.md](../spec/core/logger.md) — the replaceable-sink pattern (global
  `std::function` sink, mutex-guarded swap, lock-free hot-path check) this seam
  copies, and the sink-signature limitation that makes a separate metrics/trace
  seam necessary.
- [backend.md](../spec/core/backend.md) — `RemoteServer`/`LocalBackend` dispatch call
  sites the metric/trace hooks wrap, the "Thread context" a trace follows, and
  where `health()`/`setHealthHandler` live.
- [transport_limits.md](transport_limits.md) — `LimitPolicy`'s in-flight/live-model
  counters this seam surfaces (shared state), and the values metrics make
  observable so limits can be tuned.
- [offline.md](../spec/offline/offline.md) — `SyncWorker`/`ReconnectCoordinator` and the
  `IOfflineQueue` depth the `queueDepth`/`reconnect*` metrics report on.
- [wire.md](../spec/core/wire.md) / [session.md](../spec/session/session.md) —
  `Context::requestId`, reused as the trace correlation id.
