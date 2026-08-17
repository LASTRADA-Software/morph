// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace morph::observe {

/// @brief Kinds of metric observation the framework's hot paths can emit.
enum class Metric : std::uint8_t {
    /// @brief One dispatch's wall time in milliseconds (model strand).
    executeLatencyMs,
    /// @brief Gauge: concurrent in-flight executes.
    executeInFlight,
    /// @brief Counter: executes that resolved via an error path.
    executeErrors,
    /// @brief Counter: register calls.
    registerCount,
    /// @brief Counter: deregister calls.
    deregisterCount,
    /// @brief Gauge: `IOfflineQueue` pending items at drain.
    queueDepth,
    /// @brief Counter: `ReconnectCoordinator::onOnline` `tryReconnect` attempts.
    reconnectAttempts,
    /// @brief Counter, tagged by outcome: `ReconnectCoordinator::onOnline` results.
    reconnectOutcome,
    /// @brief Counter: an `IOfflineQueue::enqueue()` call rejected because the
    ///        queue was at its configured `maxDepth()`.
    queueOverflow,
};

/// @brief One metric observation delivered to the installed `MetricSink`.
struct MetricEvent {
    /// @brief Which metric this observation is for.
    Metric metric;
    /// @brief The observed value (a duration, a count, or a gauge level).
    double value;
    /// @brief Dimensions for this observation (e.g. `modelType`, `actionType`,
    ///        `outcome`), without baking them into `Metric` itself. Empty for
    ///        metrics that carry no dimensions.
    std::span<const std::pair<std::string_view, std::string_view>> tags;
};

/// @brief Sink signature for metric observations. Installed once via `setMetricSink`.
using MetricSink = std::function<void(const MetricEvent&)>;

/// @brief Opaque identifier for one trace span, returned by `TraceSink::beginSpan`.
///
/// The value `0` is reserved as the "no span" sentinel: `detail::beginSpan`
/// returns it when no trace sink is installed, and `detail::endSpan` treats it
/// as a no-op, so call sites never need to branch on whether tracing is enabled.
using SpanId = std::uint64_t;

/// @brief Host-supplied hooks bracketing one dispatch, for distributed tracing.
///
/// Both members must be set for the sink to take effect (see `setTraceSink`) —
/// a sink with only one of the two is treated as not installed.
struct TraceSink {
    /// @brief Called when an execute begins dispatch (on the model strand).
    ///
    /// @p requestId is `Context::requestId` (`session.hpp`), the caller's
    /// correlation id (empty if unset). Returns a `SpanId` the framework passes
    /// back to `endSpan`.
    std::function<SpanId(std::string_view requestId, std::string_view modelType, std::string_view actionType)>
        beginSpan;
    /// @brief Called when the dispatch resolves. @p ok is `true` for a
    ///        successful (`ok`/resolved-value) outcome, `false` for an error.
    std::function<void(SpanId spanId, bool ok)> endSpan;
};

namespace detail {

/// @brief Process-wide observability state: the metric sink and the trace
///        sink, each with its own mutex and lock-free "enabled" atomic —
///        mirrors `morph::log::detail::LogState` (`logger.hpp`).
struct ObserveState {
    /// @brief Metric sink, guarded by `metricMtx`. Default: none (no-op).
    ///
    /// Held behind a `shared_ptr` so an emitter can take a cheap reference-count
    /// copy under the lock and then *release the lock before invoking it* — see
    /// `emitMetric` for why calling a host sink under the mutex is not safe.
    std::shared_ptr<const MetricSink> metricSink;
    /// @brief Lock-free "is a metric sink installed?" flag, read by
    ///        `emitMetric` before constructing any `MetricEvent`.
    std::atomic<bool> metricsOn{false};
    /// @brief Guards installation and retrieval of `metricSink` — not its invocation.
    std::mutex metricMtx;

    /// @brief Trace sink, guarded by `traceMtx`. Default: none (no-op).
    std::shared_ptr<const TraceSink> traceSink;
    /// @brief Lock-free "is a (complete) trace sink installed?" flag.
    std::atomic<bool> traceOn{false};
    /// @brief Guards installation and retrieval of `traceSink` — not its invocation.
    std::mutex traceMtx;
};

/// @brief Returns the process-wide `ObserveState` singleton (Meyers singleton,
///        thread-safe first-use construction — mirrors `morph::log::detail::logState`).
inline ObserveState& observeState() {
    static ObserveState state;
    return state;
}

/// @brief Emits @p metric if a sink is installed; a single relaxed atomic load
///        (no mutex, no `MetricEvent` construction) if not.
///
/// @par Why the sink runs outside the lock, inside a `catch (...)`
/// Both properties are load-bearing, because a `MetricSink` is host code the
/// framework calls from its hot paths:
///
/// - **Outside the lock.** `std::mutex` is not recursive, so a sink that emits a
///   metric of its own, or reinstalls itself via `setMetricSink`, would deadlock
///   against the very mutex its caller holds. The sink is copied out (a
///   reference-count bump, not a `std::function` copy) and the lock released
///   before the call.
/// - **Inside `catch (...)`.** A throwing sink must not escape into the
///   framework. `LocalBackend::execute` brackets each dispatch with `beginSpan`
///   / `emitMetric` / `endSpan` and settles the caller's `Completion` *after*
///   them, precisely so a completion callback cannot observe the dispatch as
///   finished before its metrics land. An exception thrown out of instrumentation
///   would therefore skip `setValue`/`setException` entirely and be swallowed by
///   `StrandExecutor`'s catch-and-log, leaving that `Completion` unsettled
///   forever — a hung caller with neither a value nor an error, caused by a bug
///   in a metrics callback.
///
/// A sink that throws is otherwise ignored: observability is not permitted to
/// change program behavior, and reporting the failure through `morph::log` would
/// invite the same re-entrancy this function exists to tolerate.
///
/// @param metric Which metric this observation is for.
/// @param value  The observed value.
/// @param tags   Dimensions for this observation; empty by default.
inline void emitMetric(Metric metric, double value,
                       std::span<const std::pair<std::string_view, std::string_view>> tags = {}) {
    auto& state = observeState();
    if (!state.metricsOn.load(std::memory_order_relaxed)) {
        return;
    }
    std::shared_ptr<const MetricSink> sink;
    {
        std::scoped_lock const lock{state.metricMtx};
        sink = state.metricSink;
    }
    if (!sink || !*sink) {
        return;
    }
    try {
        (*sink)(MetricEvent{.metric = metric, .value = value, .tags = tags});
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
}

/// @brief Begins a span if a trace sink is installed; returns the `SpanId{0}`
///        sentinel (no mutex, no sink call) if not.
///
/// @param requestId  Caller's correlation id (`Context::requestId`); may be empty.
/// @param modelType  Target model type id.
/// @param actionType Target action type id.
/// @return A `SpanId` to pass to `endSpan`, or `0` if tracing is disabled.
[[nodiscard]] inline SpanId beginSpan(std::string_view requestId, std::string_view modelType,
                                      std::string_view actionType) {
    auto& state = observeState();
    if (!state.traceOn.load(std::memory_order_relaxed)) {
        return SpanId{0};
    }
    std::shared_ptr<const TraceSink> sink;
    {
        std::scoped_lock const lock{state.traceMtx};
        sink = state.traceSink;
    }
    if (!sink || !sink->beginSpan) {
        return SpanId{0};
    }
    // Invoked outside the lock and inside catch(...) for the same two reasons
    // documented on `emitMetric`. A sink that throws yields the "no span"
    // sentinel, which every call site already handles.
    try {
        return sink->beginSpan(requestId, modelType, actionType);
    } catch (...) {
        return SpanId{0};
    }
}

/// @brief Ends the span @p id. No-op if @p id is the sentinel `0` (tracing was
///        disabled when the matching `beginSpan` ran) or no trace sink is installed.
///
/// @param id Span id returned by the matching `beginSpan` call.
/// @param ok `true` if the dispatch resolved successfully, `false` on error.
inline void endSpan(SpanId id, bool ok) {
    if (id == 0) {
        return;
    }
    auto& state = observeState();
    std::shared_ptr<const TraceSink> sink;
    {
        std::scoped_lock const lock{state.traceMtx};
        sink = state.traceSink;
    }
    if (!sink || !sink->endSpan) {
        return;
    }
    // Outside the lock, inside catch(...) — see `emitMetric`.
    try {
        sink->endSpan(id, ok);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
}

}  // namespace detail

// ── Configuration ─────────────────────────────────────────────────────────────

/// @brief Installs the process-wide metric sink.
///
/// Thread-safe (mutex-guarded swap), exactly like `morph::log::setLogger`. Pass
/// `nullptr` (or a default-constructed `MetricSink`) to disable metrics again.
/// @param sink New sink, or an empty `MetricSink` to disable.
inline void setMetricSink(MetricSink sink) {
    auto& state = detail::observeState();
    bool const enabled = static_cast<bool>(sink);
    auto held = std::make_shared<const MetricSink>(std::move(sink));
    std::scoped_lock const lock{state.metricMtx};
    state.metricSink = std::move(held);
    state.metricsOn.store(enabled, std::memory_order_relaxed);
}

/// @brief Lock-free "is a metric sink installed?" check for the hot path.
/// @return `true` if a non-null sink is currently installed.
[[nodiscard]] inline bool metricsEnabled() noexcept {
    return detail::observeState().metricsOn.load(std::memory_order_relaxed);
}

/// @brief Installs the process-wide trace sink.
///
/// Thread-safe (mutex-guarded swap). Both `TraceSink::beginSpan` and
/// `TraceSink::endSpan` must be set for tracing to take effect — a sink with
/// only one of the two behaves as if no sink were installed (`detail::beginSpan`
/// keeps returning the `0` sentinel). Pass a default-constructed `TraceSink{}`
/// to disable tracing again.
/// @param sink New trace sink, or `TraceSink{}` to disable.
inline void setTraceSink(TraceSink sink) {
    auto& state = detail::observeState();
    bool const enabled = static_cast<bool>(sink.beginSpan) && static_cast<bool>(sink.endSpan);
    auto held = std::make_shared<const TraceSink>(std::move(sink));
    std::scoped_lock const lock{state.traceMtx};
    state.traceSink = std::move(held);
    state.traceOn.store(enabled, std::memory_order_relaxed);
}

// ── Scoped override (test fixture) ────────────────────────────────────────────

/// @brief RAII helper that snapshots the global metric and trace sinks and
///        restores them in the destructor.
///
/// Mirrors `morph::log::ScopedLoggerOverride`'s snapshot-only constructor:
/// designed for tests that install their own sink(s) mid-test via
/// `setMetricSink()` / `setTraceSink()` and want automatic restoration, so a
/// custom sink never leaks into a later test case. Because both sinks are
/// *global* state, tests using this guard must run serially — the same
/// constraint `ScopedLoggerOverride` documents.
class ScopedObserveOverride {
public:
    /// @brief Snapshots the current metric and trace sinks without changing them.
    ScopedObserveOverride() {
        auto& state = detail::observeState();
        {
            std::scoped_lock const lock{state.metricMtx};
            if (state.metricSink) {
                _savedMetricSink = *state.metricSink;
            }
        }
        {
            std::scoped_lock const lock{state.traceMtx};
            if (state.traceSink) {
                _savedTraceSink = *state.traceSink;
            }
        }
    }

    /// @brief Restores the saved metric and trace sinks.
    ~ScopedObserveOverride() {
        setMetricSink(std::move(_savedMetricSink));
        setTraceSink(std::move(_savedTraceSink));
    }

    ScopedObserveOverride(const ScopedObserveOverride&) = delete;
    ScopedObserveOverride& operator=(const ScopedObserveOverride&) = delete;
    ScopedObserveOverride(ScopedObserveOverride&&) = delete;
    ScopedObserveOverride& operator=(ScopedObserveOverride&&) = delete;

private:
    MetricSink _savedMetricSink;
    TraceSink _savedTraceSink;
};

}  // namespace morph::observe
