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
/// This header includes only the executor, so `core/remote.hpp` reaches it
/// without the table headers.

#include <morph/attributes.hpp>
#include <morph/core/executor.hpp>
#include <morph/export.hpp>

namespace morph::table {

/// @brief What a server gives a table's actions for one dispatch.
struct TableRuntime {
    /// @brief Where a reply is built, off the model's strand; null builds it on the strand.
    exec::IExecutor* work = nullptr;
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
