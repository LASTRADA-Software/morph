// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "../core/logger.hpp"
#include "action_log.hpp"

namespace morph::journal {

/// @brief Outcome of one `OutboxRelay::relay()` call.
struct OutboxRelayResult {
    /// @brief Outbox rows drained, appended to `OutboxRelay::sink`, and marked
    ///        relayed in this call. Includes rows the sink silently deduped via
    ///        `LogEntry::idempotencyKey` — a row is still marked relayed exactly
    ///        once even when the sink treats its append as a no-op.
    std::size_t relayed = 0;
};

/// @brief Moves committed-but-unrelayed rows from a model's own outbox table to
///        a durable `IActionLog`, then marks them relayed in the model's own
///        store.
///
/// Closes the "two independent writes" gap between a model's own transactional
/// store and the action log: a store-backed model writes its business tables
/// *and* an outbox row (shaped like `LogEntry`, including a stable
/// `idempotencyKey`) in one local transaction — see
/// `IModelHolder::setOutboxManaged` — and `OutboxRelay` is the separate,
/// asynchronous step that moves that row into the real durable sink.
///
/// `drainOutbox`/`markRelayed` are injected callables against the model's own
/// store, exactly as `morph::offline::ReconnectCoordinator::Deps` and
/// `morph::offline::SyncWorker::ReplayFunction` inject their side effects —
/// `morph::journal` never touches the model's database. Mirroring
/// `ReconnectCoordinator::Deps`, a null member is logged (via
/// `morph::log::logError`) at the start of every `relay()` call but is not
/// rejected — invoking a null member still throws (`std::bad_function_call`) or
/// crashes as usual.
///
/// @par Crash safety
/// `relay()` is idempotent and safe to call repeatedly: it appends every row
/// `drainOutbox()` currently reports to `sink`, flushes `sink`, and only then
/// calls `markRelayed` with the whole batch. A crash between the sink append and
/// `markRelayed` committing simply means the next `relay()` call sees the same
/// row again from `drainOutbox()` — appending it again is a safe no-op as long
/// as `sink` dedups by `LogEntry::idempotencyKey` (`InMemoryActionLog` and
/// `FileActionLog` do this out of the box — see their class docs; `SessionLog`
/// deliberately does not, since its contract is full fidelity with nothing
/// dropped). This is at-least-once-plus-dedup, not a two-phase commit: `sink`
/// and the model's own store are never committed as a single distributed
/// transaction.
struct OutboxRelay {
    /// @brief Pulls committed-but-unrelayed rows from the model's own store.
    ///        Must be non-destructive: the same rows are re-drained until
    ///        `markRelayed` records them, including across process restarts.
    std::function<std::vector<LogEntry>()> drainOutbox;

    /// @brief Marks @p rows relayed in the model's own store (by identity or
    ///        `LogEntry::idempotencyKey`) so a later `drainOutbox()` call no
    ///        longer returns them.
    std::function<void(std::span<const LogEntry>)> markRelayed;

    /// @brief Durable audit sink rows are forwarded to.
    std::shared_ptr<IActionLog> sink;

    /// @brief Moves every row `drainOutbox()` currently reports to `sink`, then
    ///        marks the whole batch relayed via `markRelayed`.
    ///
    /// A no-op (returns `{.relayed = 0}` without touching `sink` or calling
    /// `markRelayed`) if `drainOutbox()` returns no rows.
    ///
    /// `markRelayed` runs only after `sink->flush()` returns normally. An
    /// `IActionLog` that cannot make the batch durable throws (see
    /// `IActionLog::flush`), which propagates out of here *before* the rows are
    /// marked — so they stay in the outbox and a later `relay()` retries them.
    /// This is what keeps the relay at-least-once instead of at-most-once: were
    /// the failure swallowed, the rows would be recorded as relayed while
    /// nothing reached the sink, and nothing would ever surface the loss.
    ///
    /// @return The number of rows relayed in this call.
    /// @throws std::exception propagated from `sink->append()` or `sink->flush()`;
    ///         the batch is left unmarked and therefore retryable.
    OutboxRelayResult relay() {
        logIfAnyDepNull();
        auto rows = drainOutbox();
        if (rows.empty()) {
            return {};
        }
        for (const auto& row : rows) {
            sink->append(row);
        }
        sink->flush();
        markRelayed(rows);
        return OutboxRelayResult{.relayed = rows.size()};
    }

private:
    // Logs any null member, mirroring ReconnectCoordinator::assertDepsNonNull —
    // a diagnostic, not a rejection; construction/assignment of OutboxRelay
    // itself is never intercepted (it is a plain aggregate), so the check runs
    // at the start of every relay() call instead.
    void logIfAnyDepNull() const {
        if (!drainOutbox) {
            ::morph::log::logError("[journal::OutboxRelay] null drainOutbox");
        }
        if (!markRelayed) {
            ::morph::log::logError("[journal::OutboxRelay] null markRelayed");
        }
        if (!sink) {
            ::morph::log::logError("[journal::OutboxRelay] null sink");
        }
        // This branch itself is unit-tested (test_outbox.cpp asserts the
        // warning fires). The crash relay() goes on to hit afterwards --
        // sink->append() dispatching through a null shared_ptr -- is real UB
        // and not exercised: no seam here turns it into something a portable
        // unit test can catch instead of taking down the process (see
        // LASTRADA-Software/morph#95, requesting either a catchable exception
        // for this case or an observability hook logIfAnyDepNull() could feed).
    }
};

}  // namespace morph::journal
