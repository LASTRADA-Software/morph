// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/view_cache.hpp
/// @brief The server's cache of computed views: the order and the undecided rows of a sort and filter, kept
///        while the rows and the order levels they were computed from are unchanged.
///
/// Sorting and filtering many rows is the expensive part of a reply, and every page of a scroll, every keyed
/// fetch and every watcher refetching after an arrival asks for the same view. A `ViewCache` keeps one entry per
/// table record, scope and canonical sort and filter, valid while the source's snapshot has the identity the entry
/// was computed from and each order level the generation it read. Requests that need an entry being computed wait
/// for that one computation.
///
/// See `docs/spec/table/table.md`, "The view cache".

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <morph/table/query.hpp>
#include <morph/table/runtime.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace morph::table {

/// @brief What names a cached view.
struct ViewKey {
    /// @brief The table's record name, `<model type>.<id of the table>`.
    std::string record;
    /// @brief `ServedTable::scope`: what about the caller changes the rows or their order.
    std::string scope;
    /// @brief The canonical JSON of the validated query's `sort` and `filters`.
    std::string query;

    /// @brief Member-wise ordering.
    /// @param other The key to compare with.
    /// @return The order of `(record, scope, query)`.
    [[nodiscard]] auto operator<=>(ViewKey const& other) const = default;
};

/// @brief What a computation returns: the view, and the order levels it found unloaded or failed.
struct ViewComputation {
    /// @brief The view.
    detail::ComputedView view;
    /// @brief The order levels unloaded for some row of the view or of the undecided rows.
    std::vector<std::string> awaiting;
    /// @brief The order levels failed for some such row.
    std::vector<std::string> failed;
    /// @brief The keys of the view's rows and of the undecided rows: what the query's order levels are wanted
    ///        for, kept with the view so that a repeated want is one a level can recognise. Null for a query with
    ///        no order level.
    std::shared_ptr<std::vector<RowId> const> orderKeys;
};

/// @brief One cached view: what it was computed from, and the lookups a keyed query reads, built at its first.
///
/// Held only by `std::shared_ptr`, never copied or moved.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- source rows are below the snapshot's row count
class MORPH_API ViewEntry {
public:
    /// @brief An entry of @p computation.
    /// @param snapshotIn    The rows the view was computed over.
    /// @param levelsIn      Each order level read, with the generation of its snapshot.
    /// @param computation   The view, `awaiting` and `failed`.
    ViewEntry(std::shared_ptr<RowSnapshot const> snapshotIn,
              std::vector<std::pair<std::string, std::uint64_t>> levelsIn, ViewComputation computation)
        : snapshot{std::move(snapshotIn)},
          levels{std::move(levelsIn)},
          view{std::move(computation.view)},
          awaiting{std::move(computation.awaiting)},
          failed{std::move(computation.failed)},
          orderKeys{std::move(computation.orderKeys)},
          _orderKeyBytes{keysBytes(orderKeys.get())} {}

    ViewEntry(ViewEntry const&) = delete;
    ViewEntry& operator=(ViewEntry const&) = delete;
    ViewEntry(ViewEntry&&) = delete;
    ViewEntry& operator=(ViewEntry&&) = delete;
    /// @brief Frees the view and the lookups.
    ~ViewEntry() = default;

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the entry is a record of one
    // computation, built whole and read by the cache's lookups; no member has an invariant of its own to guard.

    /// @brief The rows the view was computed over; held, so the address its identity names is not reused.
    std::shared_ptr<RowSnapshot const> snapshot;
    /// @brief Each order level the view read, with its snapshot's generation.
    std::vector<std::pair<std::string, std::uint64_t>> levels;
    /// @brief The view.
    detail::ComputedView view;
    /// @brief The order levels unloaded for some row of the view or of the undecided rows.
    std::vector<std::string> awaiting;
    /// @brief The order levels failed for some such row.
    std::vector<std::string> failed;
    /// @brief The keys of the view's rows and of the undecided rows, for the order levels' wants; null for a
    ///        query with no order level.
    std::shared_ptr<std::vector<RowId> const> orderKeys;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    /// @brief The view row of each source row, `detail::kNotInView` for a row not in the view.
    /// @return The table, built at the first call.
    [[nodiscard]] std::span<std::uint32_t const> viewRowOf() const;

    /// @brief The source row of a key.
    /// @param key The key.
    /// @return The row, or nothing when the snapshot has no row with that key.
    [[nodiscard]] std::optional<std::uint32_t> sourceRowOf(RowId const& key) const;

    /// @brief What the entry holds.
    /// @return 4 bytes a row of the view and of the undecided rows; the order keys, each by `rowIdBytes`, and
    ///         their vector's spare capacity; once the keyed lookups are built, 4 bytes a source row for the view
    ///         rows and one `(hash, row)` pair a source row for the key index.
    [[nodiscard]] std::size_t bytes() const;

private:
    // Counted once: the keys never change after the entry is made.
    [[nodiscard]] static std::size_t keysBytes(std::vector<RowId> const* keys) noexcept;

    void build() const;

    mutable std::once_flag _once;
    std::size_t _orderKeyBytes = 0;
    mutable std::atomic<bool> _built{false};
    mutable std::vector<std::uint32_t> _viewRowOf;
    mutable std::vector<std::pair<std::size_t, std::uint32_t>> _byHash;
};
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

/// @brief The server's computed views, across its tables, bounded by `ViewCacheOptions::maxBytes`.
///
/// Thread-safe. An entry is found under a mutex and computed outside it, on the calling thread; requests for the
/// same key, snapshot identity and order-level generations while it is computed wait for that computation. The
/// mutex is held for bookkeeping alone: an entry that leaves the cache is released after it.
///
/// `ViewCacheOptions::maxBytes` bounds what the entries themselves hold (`ViewEntry::bytes`), not the snapshots
/// they keep alive: an entry over an old snapshot holds that snapshot's rows until the entry is evicted or found
/// invalid.
class MORPH_API ViewCache {
public:
    /// @brief The result of a find.
    using Found = std::expected<std::shared_ptr<ViewEntry const>, TableError>;

    /// @brief A cache bounded as @p options says.
    /// @param options The bound and the computation hook.
    explicit ViewCache(ViewCacheOptions options = {}) : _options{std::move(options)} {}

    /// @brief The entry of @p key valid for @p snapshot and @p levels, computing it when there is none.
    ///
    /// An entry whose snapshot has the identity of @p snapshot and whose every level generation equals
    /// @p levels' is a hit. Any other entry of the key is dropped, unless the request is not at least as new as it
    /// in every part (its @p newness and each order level's generation): such a request computes its view without
    /// displacing the entry. A miss
    /// joins a running computation of the same key, identity and generations, or computes on the calling thread,
    /// calling `ViewCacheOptions::onCompute` first. A failed computation is not cached, and an entry replaces the
    /// key's only when it is at least as new. Entries this call evicts or drops are released after the mutex is.
    /// @param key      The view's key.
    /// @param snapshot The rows the view is of: the table's pinned source snapshot.
    /// @param levels   Each order level the query reads, with its snapshot's generation.
    /// @param compute  Computes the view over the request's snapshots.
    /// @param newness  How recently @p snapshot was taken, larger for later; 0 when unknown.
    /// @return The entry, or what the computation refused.
    [[nodiscard]] Found find(ViewKey const& key, std::shared_ptr<RowSnapshot const> const& snapshot,
                             std::span<std::pair<std::string, std::uint64_t> const> levels,
                             std::function<std::expected<ViewComputation, TableError>()> const& compute,
                             std::uint64_t newness = 0);

    /// @brief The counters.
    /// @return What the cache has done, and the bytes it holds.
    [[nodiscard]] ViewCacheStats stats() const {
        return ViewCacheStats{.hits = _hits.load(),
                              .misses = _misses.load(),
                              .computations = _computations.load(),
                              .waits = _waits.load(),
                              .evictions = _evictions.load(),
                              .invalidations = _invalidations.load(),
                              .bytes = _bytes.load()};
    }

private:
    struct Running {
        ViewKey key;
        void const* identity = nullptr;
        std::vector<std::pair<std::string, std::uint64_t>> levels;
        std::shared_ptr<RowSnapshot const> snapshot;
        std::uint64_t newness = 0;
        std::uint64_t order = 0;
        std::promise<Found> promise;
        std::shared_future<Found> done;
    };

    struct Slot {
        std::shared_ptr<ViewEntry const> entry;
        std::uint64_t newness = 0;
        std::uint64_t order = 0;
        std::size_t bytes = 0;
        std::list<ViewKey>::iterator recency;
    };

    using Entries = std::map<ViewKey, Slot>;

    // Ends @p running and caches its entry, unless the key's entry is newer: of a later snapshot, or of the same
    // newness and a later computation. What leaves the cache is released after the mutex.
    void finish(std::shared_ptr<Running> const& running, Found const& result);

    void remeasure(Slot& slot);

    // Whether (newness, levels) is at least (otherNewness, otherLevels) in every part: the rows were pinned no
    // earlier, and no order level the other read is at an older generation.
    [[nodiscard]] static bool atLeastAsNew(std::uint64_t newness,
                                           std::vector<std::pair<std::string, std::uint64_t>> const& levels,
                                           std::uint64_t otherNewness,
                                           std::vector<std::pair<std::string, std::uint64_t>> const& otherLevels);

    // Under the mutex: removes an entry, moving it into @p released for the caller to free after the mutex.
    void drop(Entries::iterator found, std::vector<std::shared_ptr<ViewEntry const>>& released);

    // Under the mutex: evicts the least recently used entries until the bound holds, into @p released.
    void bound(std::vector<std::shared_ptr<ViewEntry const>>& released);

    ViewCacheOptions _options;
    std::mutex _mutex;
    Entries _entries;
    std::list<ViewKey> _recency;  // most recent first
    std::vector<std::shared_ptr<Running>> _running;
    std::uint64_t _started = 0;
    std::size_t _held = 0;
    std::atomic<std::size_t> _hits{0};
    std::atomic<std::size_t> _misses{0};
    std::atomic<std::size_t> _computations{0};
    std::atomic<std::size_t> _waits{0};
    std::atomic<std::size_t> _evictions{0};
    std::atomic<std::size_t> _invalidations{0};
    std::atomic<std::size_t> _bytes{0};
};

}  // namespace morph::table
