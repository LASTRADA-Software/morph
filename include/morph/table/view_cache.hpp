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
#include <morph/table/data_source.hpp>
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
    /// @brief The table's record name, `<model type>.<table id>`.
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
};

/// @brief One cached view: what it was computed from, and the lookups a keyed query reads, built at its first.
///
/// Held only by `std::shared_ptr`, never copied or moved.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- source rows are below the snapshot's row count
class ViewEntry {
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
          failed{std::move(computation.failed)} {}

    ViewEntry(ViewEntry const&) = delete;
    ViewEntry& operator=(ViewEntry const&) = delete;
    ViewEntry(ViewEntry&&) = delete;
    ViewEntry& operator=(ViewEntry&&) = delete;
    /// @brief Frees the view and the lookups.
    ~ViewEntry() = default;

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

    /// @brief The view row of each source row, `detail::kNotInView` for a row not in the view.
    /// @return The table, built at the first call.
    [[nodiscard]] std::span<std::uint32_t const> viewRowOf() const {
        build();
        return _viewRowOf;
    }

    /// @brief The source row of a key.
    /// @param key The key.
    /// @return The row, or nothing when the snapshot has no row with that key.
    [[nodiscard]] std::optional<std::uint32_t> sourceRowOf(RowId const& key) const {
        build();
        auto const hash = std::hash<RowId>{}(key);
        auto const [first, last] =
            std::ranges::equal_range(_byHash, hash, {}, &std::pair<std::size_t, std::uint32_t>::first);
        for (auto at = first; at != last; ++at) {
            if (snapshot->rowId(at->second) == key) {
                return at->second;
            }
        }
        return std::nullopt;
    }

    /// @brief What the entry holds.
    /// @return 4 bytes a row of the view and of the undecided rows; once the keyed lookups are built, 4 bytes a
    ///         source row for the view rows and one `(hash, row)` pair a source row for the key index.
    [[nodiscard]] std::size_t bytes() const {
        auto out = 4 * (view.rows.size() + view.undecided.size());
        if (_built.load()) {
            out += (sizeof(std::uint32_t) + sizeof(std::pair<std::size_t, std::uint32_t>)) * snapshot->rowCount();
        }
        return out;
    }

private:
    void build() const {
        std::call_once(_once, [this] {
            auto const rows = snapshot->rowCount();
            _viewRowOf.assign(rows, detail::kNotInView);
            for (std::size_t at = 0; at < view.rows.size(); ++at) {
                _viewRowOf[view.rows[at]] = static_cast<std::uint32_t>(at);
            }
            _byHash.reserve(rows);
            for (std::size_t row = 0; row < rows; ++row) {
                _byHash.emplace_back(std::hash<RowId>{}(snapshot->rowId(row)), static_cast<std::uint32_t>(row));
            }
            std::ranges::sort(_byHash);
            _built.store(true);
        });
    }

    mutable std::once_flag _once;
    mutable std::atomic<bool> _built{false};
    mutable std::vector<std::uint32_t> _viewRowOf;
    mutable std::vector<std::pair<std::size_t, std::uint32_t>> _byHash;
};
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

/// @brief The server's computed views, across its tables, bounded by `ViewCacheOptions::maxBytes`.
///
/// Thread-safe. An entry is found under a mutex and computed outside it, on the calling thread; requests for the
/// same key, snapshot identity and order-level generations while it is computed wait for that computation.
class ViewCache {
public:
    /// @brief The result of a find.
    using Found = std::expected<std::shared_ptr<ViewEntry const>, TableError>;

    /// @brief A cache bounded as @p options says.
    /// @param options The bound and the computation hook.
    explicit ViewCache(ViewCacheOptions options = {}) : _options{std::move(options)} {}

    /// @brief The entry of @p key valid for @p snapshot and @p levels, computing it when there is none.
    ///
    /// An entry whose snapshot has the identity of @p snapshot and whose every level generation equals
    /// @p levels' is a hit; any other entry of the key is dropped. A miss joins a running computation of the same
    /// key, identity and generations, or computes on the calling thread, calling `ViewCacheOptions::onCompute`
    /// first. A failed computation is not cached.
    /// @param key      The view's key.
    /// @param snapshot The rows the view is of: the table's pinned source snapshot.
    /// @param levels   Each order level the query reads, with its snapshot's generation.
    /// @param compute  Computes the view over the request's snapshots.
    /// @return The entry, or what the computation refused.
    [[nodiscard]] Found find(ViewKey const& key, std::shared_ptr<RowSnapshot const> const& snapshot,
                             std::span<std::pair<std::string, std::uint64_t> const> levels,
                             std::function<std::expected<ViewComputation, TableError>()> const& compute) {
        std::vector<std::pair<std::string, std::uint64_t>> const wanted(levels.begin(), levels.end());
        auto const* const identity = snapshot->identity();
        std::shared_ptr<Running> running;
        {
            std::unique_lock lock{_mutex};
            if (auto const found = _entries.find(key); found != _entries.end()) {
                auto const& slot = found->second;
                if (slot.entry->snapshot->identity() == identity && slot.entry->levels == wanted) {
                    _hits.fetch_add(1);
                    _recency.splice(_recency.begin(), _recency, slot.recency);
                    auto entry = slot.entry;
                    remeasure(found->second);
                    bound();
                    return entry;
                }
                drop(found);
                _invalidations.fetch_add(1);
            }
            _misses.fetch_add(1);
            for (auto const& other : _running) {
                if (other->key == key && other->identity == identity && other->levels == wanted) {
                    _waits.fetch_add(1);
                    auto done = other->done;
                    lock.unlock();
                    return done.get();
                }
            }
            running = std::make_shared<Running>();
            running->key = key;
            running->identity = identity;
            running->levels = wanted;
            running->snapshot = snapshot;
            running->order = ++_started;
            running->done = running->promise.get_future().share();
            _running.push_back(running);
            _computations.fetch_add(1);
        }
        Found result;
        try {
            if (_options.onCompute) {
                _options.onCompute();
            }
            auto computed = compute();
            if (computed) {
                result = std::make_shared<ViewEntry const>(snapshot, wanted, std::move(*computed));
            } else {
                result = std::unexpected(std::move(computed.error()));
            }
        } catch (...) {
            finish(running, {});
            running->promise.set_exception(std::current_exception());
            throw;
        }
        finish(running, result);
        running->promise.set_value(result);
        return result;
    }

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
        std::uint64_t order = 0;
        std::promise<Found> promise;
        std::shared_future<Found> done;
    };

    struct Slot {
        std::shared_ptr<ViewEntry const> entry;
        std::uint64_t order = 0;
        std::size_t bytes = 0;
        std::list<ViewKey>::iterator recency;
    };

    using Entries = std::map<ViewKey, Slot>;

    // Under the mutex: ends @p running and caches its entry, unless an entry of a later computation is there.
    void finish(std::shared_ptr<Running> const& running, Found const& result) {
        std::scoped_lock const lock{_mutex};
        std::erase(_running, running);
        if (!result || *result == nullptr) {
            return;
        }
        if (auto const found = _entries.find(running->key); found != _entries.end()) {
            if (found->second.order > running->order) {
                return;
            }
            drop(found);
        }
        _recency.push_front(running->key);
        auto [at, inserted] = _entries.emplace(
            running->key, Slot{.entry = *result, .order = running->order, .bytes = 0, .recency = _recency.begin()});
        static_cast<void>(inserted);
        remeasure(at->second);
        bound();
    }

    void remeasure(Slot& slot) {
        auto const now = slot.entry->bytes();
        _held = _held - slot.bytes + now;
        slot.bytes = now;
        _bytes.store(_held);
    }

    void drop(Entries::iterator found) {
        _held -= found->second.bytes;
        _bytes.store(_held);
        _recency.erase(found->second.recency);
        _entries.erase(found);
    }

    // Under the mutex: evicts the least recently used entries until the bound holds.
    void bound() {
        while (_held > _options.maxBytes && !_recency.empty()) {
            auto const found = _entries.find(_recency.back());
            drop(found);
            _evictions.fetch_add(1);
        }
    }

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
