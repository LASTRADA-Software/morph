// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/runtime.hpp
/// @brief Where a table's work runs: what a server installs around a dispatch,
///        for a table's action to read before its first suspension.
///
/// A table's function runs on the model's strand; the rest of a reply (the
/// sort, the filter, the page) runs on the server's table executor, so a sort
/// of many rows does not hold the strand. `RemoteServer` installs a
/// `TableRuntime` naming that executor for the length of each dispatch;
/// outside one there is none, and the reply is built where the action runs.
///
/// It also declares the server's view cache by name only (`ViewCache`, made by
/// `makeViewCache`), so a server holds one without its definition.
///
/// This header includes only the executor, so `core/remote.hpp` reaches it
/// without the table headers.

#include <cstddef>
#include <functional>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/executor.hpp>
#include <morph/export.hpp>
#include <string>
#include <string_view>

namespace morph::table {

/// @brief How a server's view cache is bounded.
struct ViewCacheOptions {
    /// @brief The most bytes the cached views hold, across the server's tables, before the least recently used
    ///        go. It counts the views, their order keys and their lookups, not the snapshots the views keep alive.
    std::size_t maxBytes = 64U << 20U;
    /// @brief Called on the computing thread at the start of each computation; empty calls nothing.
    std::function<void()> onCompute;
};

/// @brief What a view cache has done since it was made; `bytes` is what it holds now.
struct ViewCacheStats {
    /// @brief Finds that used a valid entry.
    std::size_t hits = 0;
    /// @brief Finds that found no valid entry.
    std::size_t misses = 0;
    /// @brief Views computed.
    std::size_t computations = 0;
    /// @brief Finds that waited for another request's computation.
    std::size_t waits = 0;
    /// @brief Entries evicted at the bound.
    std::size_t evictions = 0;
    /// @brief Entries dropped because the rows or an order level changed.
    std::size_t invalidations = 0;
    /// @brief The bytes the entries hold.
    std::size_t bytes = 0;
};

class ViewCache;

/// @brief A view cache, for a holder that does not see its definition.
/// @param options Its bound and computation hook.
/// @return The cache.
[[nodiscard]] MORPH_API std::shared_ptr<ViewCache> makeViewCache(ViewCacheOptions options);

/// @brief A view cache's counters, for a holder that does not see its definition.
/// @param cache The cache.
/// @return Its counters.
[[nodiscard]] MORPH_API ViewCacheStats viewCacheStats(ViewCache const& cache);

/// @brief What a server gives a table's actions for one dispatch.
struct TableRuntime {
    /// @brief Where a reply is built, off the model's strand; null builds it on the strand.
    exec::IExecutor* work = nullptr;
    /// @brief The server's view cache; null computes every view for its request.
    ViewCache* views = nullptr;
    /// @brief A serial executor a `resolve`'s completion belongs to: where it settles and its handlers run, so a
    ///        Task handler awaiting it from any strand finds one owner. Null runs `resolve` where it is called.
    exec::IExecutor* settle = nullptr;
    /// @brief Keeps `settle` alive while a `resolve`'s work is queued on `work`, which may outlive the server; the
    ///        server closes `settle` when it goes, so a delivery that comes later is dropped. Null when nothing does.
    std::shared_ptr<void const> settleKeep;
    /// @brief An action's input schema by its id, as the server serves it: what a table's description checks its
    ///        columns' edit actions against. Empty when the server offers none; an action it does not know answers
    ///        an empty text.
    std::function<std::string(std::string_view action)> schemaOf;
};

namespace detail {

/// @brief The calling thread's slot for the installed runtime.
/// @return The slot; null when no runtime is installed.
[[nodiscard]] MORPH_API TableRuntime const*& tableRuntimeSlot() noexcept;

}  // namespace detail

/// @brief The runtime installed on the calling thread.
/// @return The runtime, or null outside a `RemoteServer` dispatch.
[[nodiscard]] MORPH_API TableRuntime const* currentTableRuntime() noexcept;

/// @brief Installs a runtime on the calling thread for the scope, and restores the previous one.
class TableRuntimeScope {
public:
    /// @brief Installs @p runtime.
    /// @param runtime The runtime; must outlive the scope.
    explicit TableRuntimeScope(TableRuntime const& runtime MORPH_LIFETIMEBOUND) noexcept
        : _previous{detail::tableRuntimeSlot()} {
        detail::tableRuntimeSlot() = &runtime;
    }

    /// @brief Restores the runtime installed before this scope.
    ~TableRuntimeScope() { detail::tableRuntimeSlot() = _previous; }

    TableRuntimeScope(TableRuntimeScope const&) = delete;
    TableRuntimeScope& operator=(TableRuntimeScope const&) = delete;
    TableRuntimeScope(TableRuntimeScope&&) = delete;
    TableRuntimeScope& operator=(TableRuntimeScope&&) = delete;

private:
    TableRuntime const* _previous;
};

}  // namespace morph::table
