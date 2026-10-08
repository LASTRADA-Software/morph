// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

#include "../attributes.hpp"
#include "../core/executor.hpp"
#include "detail/graph.hpp"

/// @file
/// @brief `morph::reactive::Runtime`: the owner, batching and flush scheduling of a signal graph.
///
/// Specified in `docs/spec/reactive/signals.md`.

namespace morph::reactive {

/// @brief How a `Runtime` bounds and reports its flushes.
struct RuntimeOptions {
    /// @brief How many times one Effect may run in a single flush before the flush is treated as a
    ///        write cycle, reported, and stopped.
    std::size_t maxEffectRunsPerFlush = detail::kDefaultMaxEffectRunsPerFlush;
    /// @brief How many flushes in a row are re-posted after an Effect threw. Each re-post runs what is still
    ///        queued, so a defect does not stall it until the next write; past the bound it waits for that
    ///        write, so an Effect that throws on every run cannot loop for ever. A write from outside a flush
    ///        starts a new count.
    std::size_t maxThrowReposts = detail::kDefaultMaxThrowReposts;
    /// @brief Called on the owner after every flush that processed a queued Effect; a frontend
    ///        schedules its redraw here. May be empty. An exception it throws is reported
    ///        (`detail::site::kAfterFlushThrew`) and dropped, so it never reaches the owner executor.
    std::function<void()> afterFlush;
};

/// @brief The owner of a signal graph: every node takes one, and every operation runs on its owner.
///
/// Writes are batched. When the outermost batch ends with effects queued, the
/// runtime posts **one** flush to the owner; no Effect ever runs inside `set()`.
/// Destroying a Runtime while nodes made from it are alive is reported
/// (`detail::site::kRuntimeOutlived`); the nodes stay valid and any flush does nothing.
///
/// Off the owner, `batch`, `widgetEvent` and `untracked` are reported (`detail::site::kOffOwner`) and
/// refused: the body still runs, because the caller may need what it returns, but the runtime's state
/// (batch depth, widget-event depth, tracking frame) is left alone, and each graph operation inside the
/// body is checked, reported and refused on its own.
class Runtime {
public:
    /// @param owner The executor the graph belongs to (the GUI or loop executor). It must be serial
    ///        (`IExecutor::isSerial`), queue what is posted and run it later, in order, as `Completion`
    ///        requires of its owner: one that runs a post inline would run Effects inside `set()`, and a pool
    ///        would run two flushes at once. Borrowed: it must outlive the Runtime and every node made from it.
    /// @param options Flush bounds and after-flush hook.
    /// @throws std::invalid_argument when @p owner is not serial.
    explicit Runtime(exec::IExecutor& owner MORPH_LIFETIMEBOUND, RuntimeOptions options = {})
        : _core{makeCore(owner)} {
        _core->configure(detail::FlushBounds{.maxEffectRunsPerFlush = options.maxEffectRunsPerFlush,
                                             .maxThrowReposts = options.maxThrowReposts},
                         std::move(options.afterFlush));
    }

    /// @brief Reports `detail::site::kRuntimeOutlived` when nodes made from it are still alive, then
    ///        detaches the core, so a flush still posted does nothing.
    ~Runtime() {
        if (_core->liveNodes() != 0) {
            _core->report(detail::site::kRuntimeOutlived);
        }
        _core->detach();
    }

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// @brief Runs @p fn as one batch: every write inside it is one flush.
    /// @tparam F A callable taking no arguments.
    /// @param fn The body. Writes made before it throws still flush.
    /// @return Whatever @p fn returns.
    template <typename F>
    decltype(auto) batch(F&& fn) {
        if (!_core->checkOwner()) {
            return std::invoke(std::forward<F>(fn));
        }
        detail::BatchScope const scope{*_core};
        return std::invoke(std::forward<F>(fn));
    }

    /// @brief Runs a widget callback as one batch. A flush that falls due while it runs, or that a nested
    ///        event loop inside it (a modal dialog) picks up, is deferred: the end of the outermost widget
    ///        event posts it. So a remount never destroys a widget whose native handler is on the stack, and a
    ///        nested loop does not spin on a flush it may not run.
    /// @tparam F A callable taking no arguments.
    /// @param fn The callback.
    /// @return Whatever @p fn returns.
    template <typename F>
    decltype(auto) widgetEvent(F&& fn) {
        if (!_core->checkOwner()) {
            return std::invoke(std::forward<F>(fn));
        }
        detail::WidgetEventScope const event{*_core};
        return batch(std::forward<F>(fn));
    }

    /// @brief Runs @p fn with tracking off: reads inside it subscribe nothing.
    /// @tparam F A callable taking no arguments.
    /// @param fn The body.
    /// @return Whatever @p fn returns.
    template <typename F>
    decltype(auto) untracked(F&& fn) const {
        if (!_core->checkOwner()) {
            return std::invoke(std::forward<F>(fn));
        }
        detail::TrackingFrame const frame{*_core, nullptr};
        return std::invoke(std::forward<F>(fn));
    }

    /// @brief The owner executor.
    /// @return The executor passed at construction.
    [[nodiscard]] exec::IExecutor& owner() const noexcept { return _core->owner(); }

    /// @brief Whether a flush is posted and has not run yet.
    /// @return True between the first write that queued an Effect and the flush.
    [[nodiscard]] bool isFlushRequested() const noexcept { return _core->isFlushRequested(); }

    /// @brief The shared core every node holds.
    /// @return The core.
    [[nodiscard]] std::shared_ptr<detail::RuntimeCore> const& core() const noexcept { return _core; }

private:
    [[nodiscard]] static std::shared_ptr<detail::RuntimeCore> makeCore(exec::IExecutor& owner) {
        if (!owner.isSerial()) {
            throw std::invalid_argument{"morph::reactive::Runtime: the owner executor must be serial"};
        }
        return std::make_shared<detail::RuntimeCore>(owner);
    }

    std::shared_ptr<detail::RuntimeCore> _core;
};

}  // namespace morph::reactive
