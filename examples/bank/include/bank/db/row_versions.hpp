// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

/// @file
/// Per-row version counters, so a *stateful* model can tell when a row it is
/// holding in memory was changed by somebody else.
///
/// A keyed `AccountModel` instance owns one account and keeps its row in memory
/// — that is what makes reads free and what makes sharing the instance worth
/// anything. But the bank's ledger operations (transfer, bill payment, loan
/// disbursement) deliberately move money inside a single `SqlTransaction` owned
/// by a *different* model, because morph has no cross-instance transaction and
/// this example must not pretend otherwise (see
/// docs/planned/stateful_bank_example.md). Those writes land in SQLite behind
/// the cached row's back.
///
/// This is the smallest honest fix: every writer bumps the account's version,
/// and a cached reader re-hydrates when the version it captured is stale. It is
/// process-wide because the models it coordinates share one process; a real
/// deployment would use the store's own row version or an optimistic-concurrency
/// column instead.

namespace bank::db {

/// @brief Process-wide monotonic version counters keyed by account id.
///
/// Thread-safe: models run on their own strands, but different models run
/// concurrently, so bumps and reads genuinely race.
class RowVersions {
public:
    /// @brief Returns the process-wide instance.
    /// @return Reference to the singleton.
    static RowVersions& instance() {
        static RowVersions inst;
        return inst;
    }

    /// @brief Records that the row for @p accountId changed.
    /// @param accountId Account whose row was written.
    void bump(std::int64_t accountId) {
        std::scoped_lock const lock{_mtx};
        _versions[accountId] += 1;
    }

    /// @brief Current version of @p accountId's row.
    /// @param accountId Account to query.
    /// @return A counter that changes whenever the row is written; `0` if never written.
    [[nodiscard]] std::uint64_t version(std::int64_t accountId) {
        std::scoped_lock const lock{_mtx};
        auto iter = _versions.find(accountId);
        return iter == _versions.end() ? 0U : iter->second;
    }

private:
    std::mutex _mtx;
    std::unordered_map<std::int64_t, std::uint64_t> _versions;
};

/// @brief Convenience wrapper for `RowVersions::instance().bump(accountId)`.
/// @param accountId Account whose row was written.
inline void bumpRowVersion(std::int64_t accountId) { RowVersions::instance().bump(accountId); }

/// @brief Convenience wrapper for `RowVersions::instance().version(accountId)`.
/// @param accountId Account to query.
/// @return The account's current row version.
[[nodiscard]] inline std::uint64_t rowVersion(std::int64_t accountId) {
    return RowVersions::instance().version(accountId);
}

}  // namespace bank::db
