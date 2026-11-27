// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/level_cache.hpp
/// @brief The server's cache of a slow data level, and the source a reply reads a level through.
///
/// A `LevelCache` holds the values of one level, as its loader returned them, for every principal. A reply takes
/// one `LevelSnapshot` of each level it needs, reads the level's cells from it through a `LeveledSource`, and
/// then says which keys it needed with `want`; the cache loads the keys it neither holds nor is loading, off the
/// caller's thread, stores them, and announces them so that the clients showing those rows ask again.
///
/// See `docs/spec/table/table.md`, "The server level cache".

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/change_types.hpp>
#include <morph/export.hpp>
#include <morph/table/query.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table {

/// @brief What a level snapshot holds for one key.
enum class LevelRead : std::uint8_t {
    Loaded,    ///< The value is held; the cells were written.
    Unloaded,  ///< No value is held: never loaded, invalidated or evicted.
    Failed,    ///< The last load of the key returned no value.
};

/// @brief Why a query needs a level's keys.
enum class LevelUse : std::uint8_t {
    Display,  ///< The reply shows the keys' cells.
    Order,    ///< The query sorts or filters by the level, so it needs every candidate row's cells.
};

/// @brief One state of a level. Immutable: nothing it returns changes while it lives, and holding it blocks no one.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class LevelSnapshot {
public:
    virtual ~LevelSnapshot() = default;

    /// @brief Writes the row's cells for the level's columns, in the order of `LevelSource::columns()`.
    /// @param key The row's key.
    /// @param out One cell per level column; written only when the value is loaded.
    /// @return Whether the value is loaded, unloaded or failed.
    [[nodiscard]] virtual LevelRead read(RowId const& key, std::span<Cell> out) const = 0;

    /// @brief Differs between two snapshots that differ in any value.
    /// @return The snapshot's generation.
    [[nodiscard]] virtual std::uint64_t generation() const = 0;
};

/// @brief What the framework needs of a level, whatever the type of its values.
class LevelSource {
public:
    virtual ~LevelSource() = default;

    /// @brief The level's name, as a column's `ColumnInfo::level` names it.
    /// @return The name.
    [[nodiscard]] virtual std::string_view level() const = 0;

    /// @brief The level's columns. The same for every caller.
    /// @return The columns, in the order a snapshot writes a row's cells.
    [[nodiscard]] virtual std::span<ColumnInfo const> columns() const = 0;

    /// @brief The level as it is now.
    /// @return An immutable snapshot.
    [[nodiscard]] virtual std::shared_ptr<LevelSnapshot const> snapshot() const = 0;

    /// @brief Says the keys are needed.
    /// @param keys  The keys a reply needed.
    /// @param use   Why: to show them, or to sort or filter by them.
    /// @param retry Load the failed keys among @p keys now, without waiting out their retry delay.
    /// @return Nothing, or `Unavailable` when an order use cannot fit the cache's bound.
    virtual std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry = false) = 0;

    /// @brief Says a set of keys the caller keeps is needed: what a cached view wants for its order levels.
    ///
    /// The set is shared and never changed, so a level may tell from its address that a want repeats one it has
    /// served; the default wants its keys as `want` does.
    /// @param keys  The keys; not null.
    /// @param use   Display or order.
    /// @param retry Load the failed keys among @p keys now.
    /// @return Nothing, or `Unavailable` when an order use cannot fit the cache's bound.
    virtual std::expected<void, TableError> wantSet(std::shared_ptr<std::vector<RowId> const> const& keys,
                                                    LevelUse use, bool retry = false) {
        return want(*keys, use, retry);
    }

    /// @brief Takes back an order use's want of @p keys: the leases it gave end now. A reply calls it when a later
    ///        level refuses its order use, so the earlier levels do not hold rows for a query that was refused.
    ///        The default does nothing.
    /// @param keys The keys an order want named.
    virtual void unwant(std::span<RowId const> keys) { static_cast<void>(keys); }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief How a `LevelCache` is set up; fixed at construction.
struct LevelCacheOptions {
    /// @brief The table records it serves: one, or several that share rows.
    std::vector<std::string> records;
    /// @brief The level's name.
    std::string level;
    /// @brief The level's columns, for every caller, in a fixed order.
    std::vector<ColumnInfo> columns;
    /// @brief The most bytes the values may own before the least recently used are evicted.
    std::size_t maxBytes = 256U << 20U;
    /// @brief A value's size before any is stored; 0 takes 64 bytes a column.
    std::size_t expectedBytes = 0;
    /// @brief Records whose change invalidates values. Empty: `records`.
    std::vector<std::string> invalidatedBy;
    /// @brief Where the loader runs; never the server strand. Null runs it on the thread that wants the keys.
    exec::IExecutor* loads = nullptr;
    /// @brief The most keys handed to one loader call.
    std::size_t batch = 64;
    /// @brief How long an order use keeps its keys from eviction.
    std::chrono::seconds orderLease{60};
    /// @brief How long a failed key waits before it is loaded again; doubles with each failure in a row.
    std::chrono::seconds retryAfter{30};
    /// @brief The longest a failed key waits.
    std::chrono::seconds retryMax{600};
    /// @brief The cache's "now". Empty: the steady clock.
    std::function<std::chrono::steady_clock::time_point()> clock;
    /// @brief The unit registry the loader's measures are numbered in now. Empty: the level holds no measures.
    ///        Values are kept under a successor of the registry they were loaded with, whose numbers mean the same,
    ///        and dropped when a registry of another lineage is in place. Called from any thread that wants keys,
    ///        stores a load or takes a snapshot, with no lock held, so it must be safe to call concurrently with
    ///        whatever swaps the registry.
    std::function<std::shared_ptr<UnitRegistry const>()> units;
};

/// @brief What a `LevelCache` has done since it was made; `bytes` is what it holds now.
struct LevelCacheStats {
    /// @brief Keys a `want` found held.
    std::size_t hits = 0;
    /// @brief Keys a `want` found not held.
    std::size_t misses = 0;
    /// @brief Calls of the loader.
    std::size_t loaderCalls = 0;
    /// @brief Keys stored from a loader's return.
    std::size_t loadedKeys = 0;
    /// @brief Keys a loader call returned no value for, or that a throwing call failed.
    std::size_t failedLoads = 0;
    /// @brief Keys a load returned after an invalidation of them, and that were dropped.
    std::size_t droppedLoads = 0;
    /// @brief Held keys (values or failures) an invalidation removed.
    std::size_t invalidatedKeys = 0;
    /// @brief Values evicted at the bound.
    std::size_t evictedKeys = 0;
    /// @brief Keys an accepted order use leased.
    std::size_t leasedKeys = 0;
    /// @brief Order uses refused because they could not fit.
    std::size_t refusedOrders = 0;
    /// @brief The deep bytes of the values and failures held now, and of the keys of the order sets accepted.
    std::size_t bytes = 0;
};

template <typename Value>
class LevelCache;

namespace detail {

/// @brief The fewest shards a level cache spreads its keys over.
inline constexpr std::size_t kMinLevelShards = 16;

/// @brief The most entries a level cache's shard averages: the shard count, a power of two, doubles past it.
inline constexpr std::size_t kRowsPerShard = 64;

/// @brief The most keys a `want` takes under the cache's writer mutex at once; the mutex is released between such
///        chunks, so an invalidation waits for one chunk of a large want at most.
inline constexpr std::size_t kWantChunk = 1024;

/// @brief A cell vector's deep size: the vector, and what each cell owns.
/// @param cells The cells.
/// @return The bytes.
[[nodiscard]] MORPH_API std::size_t cellsBytes(std::vector<Cell> const& cells) noexcept;

/// @brief What fixtures and benchmarks reach of a `LevelCache` beyond its interface.
struct LevelCacheAccess {
    /// @brief Stores @p values as a load would, without a loader call and without announcing them.
    /// @tparam Value The cache's value type.
    /// @param cache  The cache.
    /// @param values The values by key.
    template <typename Value>
    static void store(LevelCache<Value>& cache, std::vector<std::pair<RowId, Value>> values) {
        cache._core->storeQuietly(std::move(values));
    }

    /// @brief How many shards stores, removals and observer calls have copied.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @return The count.
    template <typename Value>
    [[nodiscard]] static std::size_t shardCopies(LevelCache<Value> const& cache) {
        return cache._core->shardCopies.load();
    }

    /// @brief How many keys wait on the cache's queues.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @return The count.
    template <typename Value>
    [[nodiscard]] static std::size_t queuedEntries(LevelCache<Value> const& cache) {
        return cache._core->queuedEntries();
    }

    /// @brief How many shards the cache's published state has.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @return The count, a power of two.
    template <typename Value>
    [[nodiscard]] static std::size_t shardCount(LevelCache<Value> const& cache) {
        return cache._core->shardCount();
    }

    /// @brief How many order uses have been fitted: every order `want`, and every order `wantSet` that did not
    ///        repeat a set accepted within its lease.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @return The count.
    template <typename Value>
    [[nodiscard]] static std::size_t fits(LevelCache<Value> const& cache) {
        return cache._core->fits.load();
    }

    /// @brief Sets what `unwant` calls before each chunk of its keys, with no lock held.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param hook  The call, given the index of the chunk about to be taken; empty for none.
    template <typename Value>
    static void onReleaseChunk(LevelCache<Value>& cache, std::function<void(std::size_t)> hook) {
        cache._core->releaseHook = std::move(hook);
    }

    /// @brief Sets what a store calls between preparing its state and publishing it, with no lock held.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param hook  The call; empty for none. Set before any load runs.
    template <typename Value>
    static void onStorePrepared(LevelCache<Value>& cache, std::function<void()> hook) {
        cache._core->storeHook = std::move(hook);
    }

    /// @brief Sets what a want calls before each chunk of its keys, with no lock held: after an order use's fit and
    ///        before the first chunk, then between chunks.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param hook  The call, given the index of the chunk about to be taken; empty for none. Set before any want.
    template <typename Value>
    static void onWantChunk(LevelCache<Value>& cache, std::function<void(std::size_t)> hook) {
        cache._core->wantHook = std::move(hook);
    }

    /// @brief The end of a held value's order lease.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param key   The key.
    /// @return The lease's end in clock ticks; 0 when the key is not held or its value has no lease.
    template <typename Value>
    [[nodiscard]] static std::int64_t leaseEnd(LevelCache<Value> const& cache, RowId const& key) {
        auto const slot = LevelCache<Value>::findIn(*cache._core->publishedTable(), key);
        return slot != nullptr ? slot->leaseUntil.load() : 0;
    }

    /// @brief Whether a held key is kept from eviction by an order use: its own lease, or an accepted set.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param key   The key.
    /// @return Whether the key is held and kept.
    template <typename Value>
    [[nodiscard]] static bool kept(LevelCache<Value> const& cache, RowId const& key) {
        auto const slot = LevelCache<Value>::findIn(*cache._core->publishedTable(), key);
        auto const nowTicks = cache._core->now().time_since_epoch().count();
        return slot != nullptr && LevelCache<Value>::Core::kept(*slot, nowTicks);
    }

    /// @brief How many order sets the cache has accepted and not ended.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @return The count.
    template <typename Value>
    [[nodiscard]] static std::size_t acceptedSets(LevelCache<Value> const& cache) {
        std::scoped_lock const lock{cache._core->writer};
        return cache._core->sets.size();
    }

    /// @brief Whether a key not held yet has an order lease waiting for its store.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param key   The key.
    /// @return Whether the cache keeps a pending lease for @p key.
    template <typename Value>
    [[nodiscard]] static bool leasePending(LevelCache<Value> const& cache, RowId const& key) {
        std::scoped_lock const lock{cache._core->writer};
        return cache._core->pendingLeases.contains(key);
    }

    /// @brief Sets what each eviction scan calls first, with no lock held: how a test pauses a store mid-eviction.
    /// @tparam Value The cache's value type.
    /// @param cache The cache.
    /// @param hook  The call; empty for none. Set before any load runs.
    template <typename Value>
    static void onEvictionScan(LevelCache<Value>& cache, std::function<void()> hook) {
        cache._core->evictionHook = std::move(hook);
    }
};

}  // namespace detail

/// @brief The server's cache of one slow level: values loaded once, kept until invalidated or evicted, read by
///        every principal through immutable snapshots.
///
/// The values live in shards by key hash, each an immutable sorted vector behind a shared pointer; the shard
/// count is a power of two that doubles as the cache grows, so a shard averages at most `detail::kRowsPerShard`
/// entries. A published state is the shard pointers and a generation behind one shared pointer, and a snapshot
/// holds it. A store, a removal, an eviction and an observer call prepare their new state from the published one
/// with no lock held: the shard copies, the scans and sorts, the re-spread. They then take a writer mutex only to
/// check that nothing was published meanwhile (and prepare again if it was) and to publish, under a second mutex
/// held for the pointer swap alone. So neither a reader nor the server strand's observer call waits behind
/// another change's slow work. A `want` takes its keys a chunk at a time under the writer mutex, so a change waits
/// for one chunk of it at most. A value's recency and order lease are atomics beside it, and neither is part of a
/// snapshot's state.
///
/// The application's callbacks (the sink, the failure handler, `sizeOf`, `loads->post`) may throw: a throw is
/// logged, a key whose size could not be read is stored as failed, and a batch that could not be posted goes back
/// on the queue, so no callback stops the level from loading.
///
/// One loader call runs at a time. Wanted keys wait on two queues, `Display` before `Order`, each ordered by the
/// newest want first and, inside one want, in the order given; a key is queued once, and a want of a queued key
/// moves it to that want's place. A batch takes up to `batch` keys from the front. A
/// load remembers each key's generation and stores only the keys no invalidation touched meanwhile; it publishes
/// the values, then announces their keys to every record in `records`.
/// @tparam Value What the loader returns for one key.
template <typename Value>
class LevelCache final : public LevelSource, public effects::IChangeObserver {
public:
    /// @brief Reads the level for `keys`. Called on `options.loads`; may block. It returns a value for every key it
    ///        read, including a row that has nothing at this level, whose value is then empty. A key it returns no
    ///        value for failed; throwing fails every key of the call.
    using Loader = std::function<std::vector<std::pair<RowId, Value>>(std::span<RowId const> keys)>;
    /// @brief A value's deep bytes.
    using SizeOf = std::function<std::size_t(Value const&)>;
    /// @brief Writes a value's cells, in `LevelCacheOptions::columns` order.
    using Cells = std::function<void(Value const&, std::span<Cell> out)>;

    /// @brief A cache of @p options' level.
    /// @param options How it is bounded, batched and invalidated.
    /// @param loader  Reads values.
    /// @param sizeOf  A value's deep bytes.
    /// @param cells   A value's cells.
    LevelCache(LevelCacheOptions options, Loader loader, SizeOf sizeOf, Cells cells)
        : _core{std::make_shared<Core>(std::move(options), std::move(loader), std::move(sizeOf), std::move(cells))} {}

    /// @brief For `Value` = `std::vector<Cell>` only: the value is the cells, and its size is their deep size.
    /// @param options How it is bounded, batched and invalidated.
    /// @param loader  Reads values.
    LevelCache(LevelCacheOptions options, Loader loader)
        requires std::same_as<Value, std::vector<Cell>>
        : LevelCache{std::move(options), std::move(loader),
                     [](std::vector<Cell> const& value) { return detail::cellsBytes(value); },
                     [](std::vector<Cell> const& value, std::span<Cell> out) {
                         auto const count = std::min(value.size(), out.size());
                         std::copy_n(value.begin(), count, out.begin());
                         std::fill(out.begin() + static_cast<std::ptrdiff_t>(count), out.end(), Cell{});
                     }} {}

    /// @brief The level's name.
    /// @return `LevelCacheOptions::level`.
    [[nodiscard]] std::string_view level() const override { return _core->options.level; }

    /// @brief The level's columns.
    /// @return `LevelCacheOptions::columns`.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _core->options.columns; }

    /// @brief The values held now.
    ///
    /// When `LevelCacheOptions::units` gives a registry of another lineage than the one the values were loaded
    /// with, every value is invalidated first (`invalidateAll`, counted in `invalidatedKeys`): their unit numbers mean
    /// nothing under it. A `want` checks the same before it queues keys.
    /// @return An immutable snapshot: the shard pointers and the generation, copied under the mutex.
    [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const override {
        _core->checkLineage();
        return _core->snapshot();
    }

    /// @brief Says the keys are needed, and starts a load for those neither held, in flight, queued, nor failed
    ///        inside their retry delay.
    ///
    /// A held key is a hit and its recency is renewed. An order use is first fitted: the keys times the mean
    /// size of the values held (`expectedBytes` while none is), plus the bytes other keys' leases hold, against
    /// `maxBytes`; over it nothing changes. Otherwise every key is leased for `orderLease`, and the use is one more
    /// holder of each lease. The keys are taken `detail::kWantChunk` at a time under the writer mutex, each key
    /// classified, leased and queued at once, and the mutex is released between chunks.
    /// @param keys  The keys.
    /// @param use   Display or order.
    /// @param retry Load the failed keys among @p keys now.
    /// @return Nothing, or `Unavailable` for an order use that does not fit.
    std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry = false) override {
        _core->checkLineage();
        return Core::want(_core, keys, use, retry, Core::Pass::Lease);
    }

    /// @brief Says a kept set of keys is needed. An order use of a set, unretried, is held by the set: the first is
    ///        fitted and protects every key of the set from eviction in one pass; a repeat while the set's lease
    ///        runs is one more holder of the set and extends its lease, without touching a key, unless a protected
    ///        key was removed or a protected failure's delay has passed, when it also queues what the set misses.
    ///        An order use of a set whose fitting `want` another call is running waits for that call first.
    ///        Anything else is `want`.
    /// @param keys  The keys; not null.
    /// @param use   Display or order.
    /// @param retry Load the failed keys among @p keys now.
    /// @return Nothing, or `Unavailable` for an order use that does not fit.
    std::expected<void, TableError> wantSet(std::shared_ptr<std::vector<RowId> const> const& keys, LevelUse use,
                                            bool retry = false) override {
        _core->checkLineage();
        return Core::wantSet(_core, keys, use, retry);
    }

    /// @brief Takes back an order use's want of @p keys. Of a kept set, it is one holder fewer, and the set's
    ///        protection of its keys ends with its last holder; of other keys, it ends one holder's lease of each,
    ///        and a lease ends with its last holder.
    /// @param keys The keys.
    void unwant(std::span<RowId const> keys) override { _core->release(keys); }

    /// @brief Drops what @p changes made wrong. On the server strand; removes only.
    ///
    /// A change of a record in `invalidatedBy` that is one of `records` invalidates its keys, or the level when
    /// it has none or a key that is no row key. A change of any other record in `invalidatedBy` invalidates the
    /// level and announces each of `records` without keys.
    /// @param changes One fan-out's changes.
    void changed(effects::Changes const& changes) override { _core->changed(changes); }

    /// @brief Takes the server's announce sink.
    /// @param sink Where arrivals, failures and invalidations of the cache's own are reported.
    void bind(effects::AnnounceSink sink) override {
        std::scoped_lock const lock{_core->writer};
        _core->sink = std::move(sink);
    }

    /// @brief Removes the values and failures of @p keys; a load of them in flight does not land.
    /// @param keys The keys.
    void invalidate(std::span<RowId const> keys) { _core->invalidate(keys); }

    /// @brief Removes every value and failure; no load in flight lands.
    void invalidateAll() { _core->invalidateAll(); }

    /// @brief Sets what is called with the keys of each load that failed, after they are stored as failed.
    /// @param handler The handler; called on the loading thread.
    void onLoadFailed(std::function<void(std::span<RowId const>)> handler) {
        std::scoped_lock const lock{_core->writer};
        _core->failedHandler = std::move(handler);
    }

    /// @brief The counters.
    /// @return What the cache has done, and the bytes it holds.
    [[nodiscard]] LevelCacheStats stats() const { return _core->stats(); }

private:
    friend struct detail::LevelCacheAccess;

    using Clock = std::chrono::steady_clock;

    struct Slot {
        std::optional<Value> value;  // empty: the load failed
        std::uint32_t failures = 0;
        Clock::time_point failedAt;
        std::size_t bytes = 0;  // a value's deep bytes; a failure's own size
        mutable std::atomic<std::int64_t> lastUse{0};
        mutable std::atomic<std::int64_t> leaseUntil{0};    // clock ticks; 0: no lease
        mutable std::atomic<std::int32_t> leaseHolders{0};  // the order uses holding the lease
        mutable std::atomic<std::int32_t> setHolds{0};      // the accepted sets protecting the key
    };
    using SlotPtr = std::shared_ptr<Slot const>;
    using Entry = std::pair<RowId, SlotPtr>;
    using Shard = std::vector<Entry>;  // sorted by key

    // One published state: the shards, a power of two of them, the generation and the entry count.
    struct Table {
        std::vector<std::shared_ptr<Shard const>> shards;
        std::uint64_t generation = 0;
        std::size_t entries = 0;
    };

    [[nodiscard]] static std::size_t shardOf(RowId const& key, std::size_t count) noexcept {
        return std::hash<RowId>{}(key) & (count - 1);
    }

    [[nodiscard]] static SlotPtr findIn(Shard const& shard, RowId const& key) {
        auto const found = std::ranges::lower_bound(shard, key, {}, &Entry::first);
        return found != shard.end() && found->first == key ? found->second : nullptr;
    }

    [[nodiscard]] static SlotPtr findIn(Table const& table, RowId const& key) {
        return findIn(*table.shards[shardOf(key, table.shards.size())], key);
    }

    class Snapshot final : public LevelSnapshot {
    public:
        Snapshot(std::shared_ptr<Table const> table, std::shared_ptr<Cells const> cells)
            : _table{std::move(table)}, _cells{std::move(cells)} {}

        [[nodiscard]] LevelRead read(RowId const& key, std::span<Cell> out) const override {
            auto const slot = findIn(*_table, key);
            if (slot == nullptr) {
                return LevelRead::Unloaded;
            }
            if (!slot->value) {
                return LevelRead::Failed;
            }
            (*_cells)(*slot->value, out);
            return LevelRead::Loaded;
        }

        [[nodiscard]] std::uint64_t generation() const override { return _table->generation; }

    private:
        std::shared_ptr<Table const> _table;
        std::shared_ptr<Cells const> _cells;
    };

    // A key handed to a loader call: the level epoch and the sequence number its load started at.
    struct Loading {
        RowId key;
        std::uint64_t epoch = 0;
        std::uint64_t started = 0;
    };

    struct Queued {
        LevelUse use = LevelUse::Display;
        std::uint64_t priority = 0;
    };

    // A change prepared against one published state with no lock held: the shards it touches, copied once each,
    // and each slot it replaces or removes, for the accounting done when it is published.
    class Edit {
    public:
        Edit(Table const& base, std::atomic<std::size_t>& copies) : _base{&base}, _copies{&copies} {}

        [[nodiscard]] SlotPtr find(RowId const& key) const {
            auto const index = shardOf(key, _base->shards.size());
            if (auto const copied = _copied.find(index); copied != _copied.end()) {
                return findIn(*copied->second, key);
            }
            return findIn(*_base->shards[index], key);
        }

        void put(RowId const& key, SlotPtr slot) {
            auto& shard = copy(key);
            auto const found = std::ranges::lower_bound(shard, key, {}, &Entry::first);
            if (found != shard.end() && found->first == key) {
                changes.emplace_back(found->second, slot);
                found->second = std::move(slot);
            } else {
                changes.emplace_back(nullptr, slot);
                shard.insert(found, Entry{key, std::move(slot)});
                ++_added;
            }
        }

        void erase(RowId const& key) {
            auto& shard = copy(key);
            auto const found = std::ranges::lower_bound(shard, key, {}, &Entry::first);
            if (found != shard.end() && found->first == key) {
                changes.emplace_back(found->second, nullptr);
                shard.erase(found);
                ++_removed;
            }
        }

        [[nodiscard]] bool changed() const noexcept { return !_copied.empty(); }

        // The base with the copied shards in; the shard count doubles until a shard averages at most
        // `detail::kRowsPerShard` entries. Built with no lock held.
        [[nodiscard]] std::shared_ptr<Table const> table() const {
            auto out = std::make_shared<Table>();
            out->generation = _base->generation + 1;
            out->entries = _base->entries + _added - _removed;
            out->shards = _base->shards;
            for (auto const& [index, shard] : _copied) {
                out->shards[index] = shard;
            }
            auto count = out->shards.size();
            while (out->entries > detail::kRowsPerShard * count) {
                count *= 2;
            }
            if (count != out->shards.size()) {
                std::vector<Shard> spread(count);
                for (auto const& shard : out->shards) {
                    for (auto const& entry : *shard) {
                        spread[shardOf(entry.first, count)].push_back(entry);
                    }
                }
                out->shards.clear();
                out->shards.reserve(count);
                for (auto& shard : spread) {
                    std::ranges::sort(shard, {}, &Entry::first);
                    out->shards.push_back(std::make_shared<Shard const>(std::move(shard)));
                }
            }
            return out;
        }

        // (old, new) per changed key; null for none.
        // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): read by the cache when it publishes the edit.
        std::vector<std::pair<SlotPtr, SlotPtr>> changes;

    private:
        Shard& copy(RowId const& key) {
            auto const index = shardOf(key, _base->shards.size());
            auto& copied = _copied[index];
            if (copied == nullptr) {
                copied = std::make_shared<Shard>(*_base->shards[index]);
                _copies->fetch_add(1);
            }
            return *copied;
        }

        Table const* _base;
        std::atomic<std::size_t>* _copies;
        std::unordered_map<std::size_t, std::shared_ptr<Shard>> _copied;
        std::size_t _added = 0;
        std::size_t _removed = 0;
    };

    // What a store leaves to be done outside the mutex.
    struct Told {
        effects::AnnounceSink sink;
        std::function<void(std::span<RowId const>)> failedHandler;
        std::vector<RowId> stored;
        std::vector<RowId> failed;
        bool overBound = false;
    };

    struct Core {
        Core(LevelCacheOptions opts, Loader load, SizeOf size, Cells cellsOf)
            : options{std::move(opts)},
              loader{std::move(load)},
              sizeOf{std::move(size)},
              cells{std::make_shared<Cells const>(std::move(cellsOf))} {
            if (options.invalidatedBy.empty()) {
                options.invalidatedBy = options.records;
            }
            if (options.batch == 0) {
                options.batch = 1;
            }
            current = emptyTable(0);
            published = current;
        }

        [[nodiscard]] static std::shared_ptr<Table const> emptyTable(std::uint64_t generation) {
            auto out = std::make_shared<Table>();
            out->generation = generation;
            auto const empty = std::make_shared<Shard const>();
            out->shards.assign(detail::kMinLevelShards, empty);
            return out;
        }

        [[nodiscard]] Clock::time_point now() const { return options.clock ? options.clock() : Clock::now(); }

        // The clock, or the epoch when the clock throws: a failure's time is then already past its delay.
        [[nodiscard]] Clock::time_point nowOrEpoch() const noexcept {
            try {
                return now();
            } catch (...) {
                return Clock::time_point{};
            }
        }

        [[nodiscard]] static std::int64_t ticks(Clock::time_point when) noexcept {
            return when.time_since_epoch().count();
        }

        [[nodiscard]] Clock::duration retryDelay(std::uint32_t failures) const {
            // Doubled one step at a time and stopped at the ceiling: a delay doubled in one multiplication by 2^30
            // overflows the clock's tick count, and a wrapped delay is negative, so no failure would wait at all.
            Clock::duration delay = options.retryAfter;
            for (std::uint32_t doubled = 1; doubled < failures && delay < options.retryMax; ++doubled) {
                delay *= 2;
            }
            return std::min<Clock::duration>(delay, options.retryMax);
        }

        [[nodiscard]] std::size_t meanBytes() const {
            if (values > 0) {
                return valueBytes / values;
            }
            return options.expectedBytes != 0 ? options.expectedBytes : 64 * options.columns.size();
        }

        [[nodiscard]] std::shared_ptr<Table const> publishedTable() const {
            std::scoped_lock const lock{publishMutex};
            return published;
        }

        // Under the writer mutex: publishes a prepared table and accounts for its changes; the publish mutex is
        // held for the pointer swap alone.
        void publish(std::shared_ptr<Table const> next, Edit const& edit) {
            for (auto const& [old, slot] : edit.changes) {
                forget(old.get());
                if (slot != nullptr) {
                    remember(*slot);
                }
            }
            {
                std::scoped_lock const lock{publishMutex};
                published = next;
            }
            current = std::move(next);
            ++version;
        }

        [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const {
            return std::make_shared<Snapshot const>(publishedTable(), cells);
        }

        // ── The queue ──
        // Each use's queue is ordered by priority, the newest want highest and, inside one want, its first key
        // highest. A key is queued once; a want of a queued key gives it the want's priority, so the keys a user
        // asks for again come first. A key queued for display stays there when wanted for order.

        void enqueue(RowId const& key, LevelUse use, std::uint64_t priority) {
            auto const found = queued.find(key);
            if (found != queued.end()) {
                if (found->second.use == LevelUse::Display && use == LevelUse::Order) {
                    return;
                }
                queues.at(static_cast<std::size_t>(found->second.use)).erase(found->second.priority);
            }
            queued.insert_or_assign(key, Queued{.use = use, .priority = priority});
            queues.at(static_cast<std::size_t>(use)).emplace(priority, key);
        }

        // The priority of the `index`-th key of the next want.
        [[nodiscard]] std::uint64_t priorityOf(std::uint64_t want, std::size_t index) const noexcept {
            return (want << 32U) | (0xFFFF'FFFFULL - (static_cast<std::uint64_t>(index) & 0xFFFF'FFFFULL));
        }

        // Under the writer mutex: the next batch, display first, highest priority first, its keys marked in flight.
        [[nodiscard]] std::vector<Loading> takeBatch() {
            std::vector<Loading> out;
            auto const started = ++sequence;
            for (auto const use : {LevelUse::Display, LevelUse::Order}) {
                auto& queue = queues.at(static_cast<std::size_t>(use));
                while (out.size() < options.batch && !queue.empty()) {
                    auto const top = std::prev(queue.end());
                    auto key = std::move(top->second);
                    queue.erase(top);
                    queued.erase(key);
                    inFlight.insert_or_assign(key, std::pair{epoch, started});
                    out.push_back(Loading{.key = std::move(key), .epoch = epoch, .started = started});
                }
            }
            running = !out.empty();
            return out;
        }

        [[nodiscard]] bool inFlightNow(RowId const& key) const {
            auto const found = inFlight.find(key);
            if (found == inFlight.end() || found->second.first != epoch) {
                return false;
            }
            auto const invalidated = invalidatedAt.find(key);
            return invalidated == invalidatedAt.end() || invalidated->second < found->second.second;
        }

        [[nodiscard]] bool stillCurrent(Loading const& loading) const {
            if (loading.epoch != epoch) {
                return false;
            }
            auto const invalidated = invalidatedAt.find(loading.key);
            return invalidated == invalidatedAt.end() || invalidated->second < loading.started;
        }

        // ── want ──

        // An order set the cache accepted, by address: the set, held so that its address names it, the order uses
        // holding it, its lease's end, the value of `setEvents` it last looked at its keys for, the level's
        // generation it was last wanted at, and the bytes its keys own. While the lease runs and a use holds it,
        // its keys are protected.
        struct AcceptedSet {
            std::shared_ptr<std::vector<RowId> const> keys;
            std::int32_t holders = 0;
            std::int64_t until = 0;
            std::uint64_t seenEvents = 0;
            std::uint64_t generation = 0;
            std::size_t bytes = 0;
        };

        // The bytes a set's keys own, counted in the bound while the set is accepted.
        [[nodiscard]] static std::size_t setBytesOf(std::span<RowId const> keys) noexcept {
            auto out = sizeof(std::vector<RowId>);
            for (auto const& key : keys) {
                out += rowIdBytes(key);
            }
            return out;
        }

        // Under the writer mutex: takes a set out of the accepted sets and out of the bound; the caller unprotects
        // its keys.
        AcceptedSet takeSet(std::unordered_map<std::vector<RowId> const*, AcceptedSet>::iterator found) {
            auto out = std::move(found->second);
            sets.erase(found);
            setBytes -= std::min(setBytes, out.bytes);
            storeBytes();
            return out;
        }

        // How a want's pass treats an order key: a lease of its own, or protection by an accepted set.
        enum class Pass : std::uint8_t { Lease, Protect };

        // Ends a set's single flight: wakes the repeats that wait for it.
        class Settling {
        public:
            Settling(Core& core, std::vector<RowId> const* set) : _core{core}, _set{set} {}
            Settling(Settling const&) = delete;
            Settling& operator=(Settling const&) = delete;
            Settling(Settling&&) = delete;
            Settling& operator=(Settling&&) = delete;
            ~Settling() {
                {
                    std::scoped_lock const lock{_core.writer};
                    _core.settling.erase(_set);
                }
                _core.setSettled.notify_all();
            }

        private:
            Core& _core;
            std::vector<RowId> const* _set;
        };

        // An order use of a kept set. The first, fitted, protects every key of the set in one chunked pass; a
        // repeat while the set's lease runs is one more holder and a longer lease, under the lock, and touches no
        // key unless `setEvents` has moved since the set last looked (a protected key removed, a protected
        // failure's delay passed), when it queues the keys the set misses. A fit also ends the accepted sets that
        // no use has wanted since the level's generation moved past theirs: a view is found only at its level's
        // generation, so such a set's view is superseded.
        static std::expected<void, TableError> wantSet(std::shared_ptr<Core> const& core,
                                                       std::shared_ptr<std::vector<RowId> const> const& keys,
                                                       LevelUse use, bool retry) {
            if (use != LevelUse::Order || retry) {
                return want(core, *keys, use, retry, Pass::Lease);
            }
            auto const* const set = keys.get();
            auto const instant = core->now();
            auto const nowTicks = ticks(instant);
            auto const until = ticks(instant + core->options.orderLease);
            core->endExpiredSets(nowTicks);
            std::optional<AcceptedSet> lapsed;
            std::uint64_t events = 0;
            {
                std::unique_lock lock{core->writer};
                // A fitting want of this same set runs: wait for it, so that a repeat learns whether it was
                // accepted rather than fit the same keys again beside it.
                core->setSettled.wait(lock, [&] { return !core->settling.contains(set); });
                if (auto const found = core->sets.find(set); found != core->sets.end()) {
                    if (found->second.until > nowTicks) {
                        ++found->second.holders;
                        found->second.until = std::max(found->second.until, until);
                        found->second.generation = core->current->generation;
                        core->hits.fetch_add(keys->size());
                        if (nowTicks >= core->setsRecheckAt) {
                            // A protected failure's delay has passed: an event every set sees once.
                            ++core->setEvents;
                            core->setsRecheckAt = std::numeric_limits<std::int64_t>::max();
                        }
                        if (found->second.seenEvents == core->setEvents) {
                            return {};
                        }
                        found->second.seenEvents = core->setEvents;
                        lock.unlock();
                        core->requeue(core, *keys, instant);
                        return {};
                    }
                    lapsed = core->takeSet(found);
                }
                core->settling.insert(set);
                events = core->setEvents;
            }
            auto const bytes = setBytesOf(*keys);
            std::vector<AcceptedSet> superseded;
            std::expected<void, TableError> wanted;
            {
                Settling const settling{*core, set};
                if (lapsed) {
                    core->unprotect(*lapsed->keys);
                }
                wanted = want(core, *keys, use, retry, Pass::Protect, bytes);
                if (wanted) {
                    std::scoped_lock const lock{core->writer};
                    auto const generation = core->current->generation;
                    for (auto entry = core->sets.begin(); entry != core->sets.end();) {
                        auto const next = std::next(entry);
                        if (entry->second.generation < generation) {
                            superseded.push_back(core->takeSet(entry));
                        }
                        entry = next;
                    }
                    core->sets.insert_or_assign(set, AcceptedSet{.keys = keys,
                                                                 .holders = 1,
                                                                 .until = until,
                                                                 .seenEvents = events,
                                                                 .generation = generation,
                                                                 .bytes = bytes});
                    core->setBytes += bytes;
                    core->storeBytes();
                }
            }
            // After the new set protects its keys, so that a key both sets hold is never unprotected between, and
            // after the repeats waiting for the set are woken, so that none waits for this pass.
            for (auto const& old : superseded) {
                core->unprotect(*old.keys);
            }
            return wanted;
        }

        // An order use is fitted first against the published state with no lock held: the fit is an estimate.
        // The keys are then taken a chunk of `detail::kWantChunk` at a time under the writer mutex, which is
        // released between chunks, so an invalidation or a store waits for one chunk at most. Inside a chunk a
        // key is classified, leased or protected, and queued at once: a held key's slot is marked where an
        // eviction's publish check sees it, and a key not held gets a pending lease, or a protection count, that
        // its store moves onto its slot.
        // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
        static std::expected<void, TableError> want(std::shared_ptr<Core> const& core, std::span<RowId const> keys,
                                                    LevelUse use, bool retry, Pass pass, std::size_t keyBytes = 0) {
            auto const instant = core->now();
            auto const nowTicks = ticks(instant);
            auto const until = ticks(instant + core->options.orderLease);
            if (use == LevelUse::Order) {
                core->fits.fetch_add(1);
                if (auto fitted = core->fitOrder(keys, nowTicks, keyBytes); !fitted) {
                    return fitted;
                }
            }
            auto const protect = use == LevelUse::Order && pass == Pass::Protect;
            std::uint64_t want = 0;
            std::size_t hit = 0;
            std::vector<Loading> batch;
            for (std::size_t first = 0;; first += detail::kWantChunk) {
                if (core->wantHook) {
                    core->wantHook(first / detail::kWantChunk);
                }
                std::scoped_lock const lock{core->writer};
                if (first == 0) {
                    want = ++core->wants;
                }
                auto const& table = *core->current;
                auto const last = std::min(keys.size(), first + detail::kWantChunk);
                for (auto index = first; index < last; ++index) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < last <= keys.size()
                    auto const& key = keys[index];
                    auto const slot = findIn(table, key);
                    if (protect) {
                        auto const count = ++core->protectedKeys[key];
                        if (slot != nullptr) {
                            slot->setHolds.store(count);
                        }
                    }
                    if (slot != nullptr && slot->value) {
                        ++hit;
                        slot->lastUse.store(core->useCounter.fetch_add(1) + 1);
                        if (use == LevelUse::Order && !protect) {
                            lease(*slot, until, nowTicks);
                        }
                        continue;
                    }
                    if (slot != nullptr && !retry && instant < slot->failedAt + core->retryDelay(slot->failures)) {
                        if (protect) {
                            core->recheckAt(slot->failedAt + core->retryDelay(slot->failures));
                        }
                        continue;
                    }
                    if (use == LevelUse::Order && !protect) {
                        auto& pending = core->pendingLeases[key];
                        pending.holders = pending.until > nowTicks ? pending.holders + 1 : 1;
                        pending.until = std::max(pending.until, until);
                    }
                    if (!core->inFlightNow(key)) {
                        core->enqueue(key, use, core->priorityOf(want, index));
                    }
                }
                if (last == keys.size()) {
                    if (!core->running) {
                        batch = core->takeBatch();
                    }
                    break;
                }
            }
            core->hits.fetch_add(hit);
            core->misses.fetch_add(keys.size() - hit);
            if (use == LevelUse::Order) {
                core->leasedKeys.fetch_add(keys.size());
            }
            dispatch(core, std::move(batch));
            return {};
        }

        // Under the writer mutex: a repeat of an accepted set must look at its keys again once `at` has passed.
        void recheckAt(Clock::time_point when) { setsRecheckAt = std::min(setsRecheckAt, ticks(when)); }

        // Queues the keys of an accepted set that are neither held, in flight, nor failed inside their delay, a
        // chunk at a time under the writer mutex. Their protection is kept by key, so it needs no change.
        static void requeue(std::shared_ptr<Core> const& core, std::span<RowId const> keys, Clock::time_point when) {
            std::uint64_t want = 0;
            std::vector<Loading> batch;
            for (std::size_t first = 0;; first += detail::kWantChunk) {
                std::scoped_lock const lock{core->writer};
                if (first == 0) {
                    want = ++core->wants;
                }
                auto const& table = *core->current;
                auto const last = std::min(keys.size(), first + detail::kWantChunk);
                for (auto index = first; index < last; ++index) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < last <= keys.size()
                    auto const& key = keys[index];
                    auto const slot = findIn(table, key);
                    if (slot != nullptr && slot->value) {
                        continue;
                    }
                    if (slot != nullptr && when < slot->failedAt + core->retryDelay(slot->failures)) {
                        core->recheckAt(slot->failedAt + core->retryDelay(slot->failures));
                        continue;
                    }
                    if (!core->inFlightNow(key)) {
                        core->enqueue(key, LevelUse::Order, core->priorityOf(want, index));
                    }
                }
                if (last == keys.size()) {
                    if (!core->running) {
                        batch = core->takeBatch();
                    }
                    break;
                }
            }
            dispatch(core, std::move(batch));
        }

        // Takes one protection of each key away, a chunk at a time under the writer mutex; a held key's slot keeps
        // the count its key has.
        void unprotect(std::span<RowId const> keys) {
            for (std::size_t first = 0; first < keys.size(); first += detail::kWantChunk) {
                if (releaseHook) {
                    releaseHook(first / detail::kWantChunk);
                }
                std::scoped_lock const lock{writer};
                auto const last = std::min(keys.size(), first + detail::kWantChunk);
                for (auto index = first; index < last; ++index) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < last <= keys.size()
                    auto const& key = keys[index];
                    auto const found = protectedKeys.find(key);
                    if (found == protectedKeys.end()) {
                        continue;
                    }
                    auto const count = --found->second;
                    if (count <= 0) {
                        protectedKeys.erase(found);
                    }
                    if (auto const slot = findIn(*current, key); slot != nullptr) {
                        slot->setHolds.store(std::max(count, 0));
                    }
                }
            }
        }

        // Ends the accepted sets whose lease has run out, and their protection.
        void endExpiredSets(std::int64_t nowTicks) {
            std::vector<AcceptedSet> expired;
            {
                std::scoped_lock const lock{writer};
                for (auto entry = sets.begin(); entry != sets.end();) {
                    auto const next = std::next(entry);
                    if (entry->second.until <= nowTicks) {
                        expired.push_back(takeSet(entry));
                    }
                    entry = next;
                }
            }
            for (auto const& set : expired) {
                unprotect(*set.keys);
            }
        }

        // Whether an order use of `keys` fits: the keys times the mean value size, plus the bytes leased or
        // protected for keys other than these, plus the accepted sets' key bytes and `keyBytes`, what the use's own
        // set of keys will hold, against `maxBytes`. Read from the published state with no lock held.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a time in ticks, then a byte count, in the order the names read.
        [[nodiscard]] std::expected<void, TableError> fitOrder(std::span<RowId const> keys, std::int64_t nowTicks,
                                                               std::size_t keyBytes) {
            std::shared_ptr<Table const> base;
            std::size_t mean = 0;
            std::size_t setsHeld = 0;
            {
                std::scoped_lock const lock{writer};
                base = current;
                mean = meanBytes();
                setsHeld = setBytes;
            }
            std::size_t ownLeased = 0;
            for (auto const& key : keys) {
                if (auto const slot = findIn(*base, key); slot != nullptr && slot->value && kept(*slot, nowTicks)) {
                    ownLeased += slot->bytes;
                }
            }
            auto const leasedAll = leasedBytes(*base, nowTicks);
            auto const leasedOther = leasedAll - std::min(ownLeased, leasedAll);
            if ((keys.size() * mean) + leasedOther + setsHeld + keyBytes <= options.maxBytes) {
                return {};
            }
            refusedOrders.fetch_add(1);
            return std::unexpected(
                TableError{.code = TableErrorCode::Unavailable,
                           .column = {},
                           .message = "the level " + options.level + " cannot hold every row this order needs"});
        }

        // Whether an order use keeps a slot from eviction: its own lease runs, or an accepted set protects it.
        [[nodiscard]] static bool kept(Slot const& slot, std::int64_t nowTicks) noexcept {
            return slot.leaseUntil.load() > nowTicks || slot.setHolds.load() > 0;
        }

        // Gives a held value an order lease until `until`, one more holder while its lease runs.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the lease's end, then the current time, as every caller reads them.
        static void lease(Slot const& slot, std::int64_t until, std::int64_t nowTicks) {
            if (slot.leaseUntil.load() > nowTicks) {
                slot.leaseHolders.fetch_add(1);
            } else {
                slot.leaseHolders.store(1);
            }
            auto current = slot.leaseUntil.load();
            while (current < until && !slot.leaseUntil.compare_exchange_weak(current, until)) {
            }
        }

        // The bytes the order uses keep from eviction in `table`. Run with no lock held.
        [[nodiscard]] static std::size_t leasedBytes(Table const& table, std::int64_t nowTicks) {
            std::size_t out = 0;
            for (auto const& shard : table.shards) {
                for (auto const& [key, slot] : *shard) {
                    if (kept(*slot, nowTicks)) {
                        out += slot->bytes;
                    }
                }
            }
            return out;
        }

        // Takes back an order use's want. Of an accepted set, found by its keys' address, it is one holder fewer;
        // the set's protection ends with its last holder. Of anything else, it ends one holder's lease of each key,
        // a chunk at a time under the writer mutex: on a held value's slot while its lease runs, otherwise on the
        // key's pending lease.
        void release(std::span<RowId const> keys) {
            bool ofSet = false;
            {
                std::scoped_lock const lock{writer};
                auto const found = std::ranges::find_if(sets, [&keys](auto const& entry) {
                    return entry.first->data() == keys.data() && entry.first->size() == keys.size();
                });
                if (found != sets.end()) {
                    if (--found->second.holders > 0) {
                        return;
                    }
                    static_cast<void>(takeSet(found));
                    ofSet = true;
                }
            }
            if (ofSet) {
                unprotect(keys);
                return;
            }
            auto const nowTicks = ticks(now());
            for (std::size_t first = 0; first < keys.size(); first += detail::kWantChunk) {
                if (releaseHook) {
                    releaseHook(first / detail::kWantChunk);
                }
                std::scoped_lock const lock{writer};
                auto const last = std::min(keys.size(), first + detail::kWantChunk);
                for (auto index = first; index < last; ++index) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < last <= keys.size()
                    auto const& key = keys[index];
                    if (auto const slot = findIn(*current, key);
                        slot != nullptr && slot->leaseUntil.load() > nowTicks) {
                        if (slot->leaseHolders.fetch_sub(1) <= 1) {
                            slot->leaseHolders.store(0);
                            slot->leaseUntil.store(0);
                        }
                    } else if (auto const pending = pendingLeases.find(key); pending != pendingLeases.end()) {
                        if (--pending->second.holders <= 0) {
                            pendingLeases.erase(pending);
                        }
                    }
                }
            }
        }

        // ── loading ──

        static void dispatch(std::shared_ptr<Core> const& core, std::vector<Loading> batch) {
            if (batch.empty()) {
                return;
            }
            if (core->options.loads == nullptr) {
                while (!batch.empty()) {
                    batch = core->load(std::move(batch));
                }
                return;
            }
            try {
                core->options.loads->post([core, batch]() mutable { dispatch(core, core->load(std::move(batch))); });
            } catch (...) {
                // Nothing will load the batch: its keys go back on the display queue, and the next want dispatches.
                core->unpost(batch);
            }
        }

        void unpost(std::vector<Loading> const& batch) {
            std::scoped_lock const lock{writer};
            auto const want = ++wants;
            std::size_t index = 0;
            for (auto const& loading : batch) {
                inFlight.erase(loading.key);
                enqueue(loading.key, LevelUse::Display, priorityOf(want, index++));
            }
            running = false;
        }

        [[nodiscard]] std::vector<Loading> load(std::vector<Loading> batch) {
            std::vector<RowId> keys;
            keys.reserve(batch.size());
            for (auto const& loading : batch) {
                keys.push_back(loading.key);
            }
            loaderCalls.fetch_add(1);
            // The registry the loader numbers its measures in is the one in place when it is called.
            auto const lineage = unitLineage();
            std::vector<std::pair<RowId, Value>> loaded;
            try {
                loaded = loader(keys);
            } catch (...) {
                loaded.clear();
            }
            auto told = store(batch, loaded, true, lineage);
            std::vector<Loading> next;
            {
                std::scoped_lock const lock{writer};
                next = takeBatch();
            }
            tell(told);
            return next;
        }

        // Stores a load's values and failures, then applies the bound. The new state and the eviction are
        // prepared with no lock held; the writer mutex is taken only to check that nothing changed since and to
        // publish. A change published meanwhile makes the store prepare again. `lineage` is the unit lineage the
        // load was issued under: a load whose registry was swapped for another lineage while it ran is dropped, as
        // an invalidated one is, since its unit numbers mean nothing now.
        // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
        [[nodiscard]] Told store(std::vector<Loading> const& batch, std::vector<std::pair<RowId, Value>>& loaded,
                                 bool counted, std::uint64_t lineage) {
            // Sizes are read once, before any lock: a throwing `sizeOf` fails its key.
            std::unordered_map<RowId, std::pair<Value*, std::size_t>> returned;
            returned.reserve(loaded.size());
            for (auto& [key, value] : loaded) {
                try {
                    returned.emplace(key, std::pair{&value, sizeOf(value)});
                } catch (...) {
                    // Not returned: the key is stored as failed.
                    returned.erase(key);
                }
            }
            auto const now = nowOrEpoch();
            checkLineage();
            bool const otherLineage = lineage != unitLineage();
            Told told;
            bool leasedStored = false;
            for (;;) {
                std::shared_ptr<Table const> base;
                std::uint64_t seen = 0;
                std::vector<bool> valid(batch.size());
                {
                    std::scoped_lock const lock{writer};
                    base = current;
                    seen = version;
                    for (std::size_t index = 0; index < batch.size(); ++index) {
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < batch.size() and valid has one flag per batch entry
                        valid[index] = !otherLineage && stillCurrent(batch[index]);
                    }
                }
                Edit edit{*base, shardCopies};
                std::vector<RowId> stored;
                std::vector<RowId> failed;
                std::size_t dropped = 0;
                for (std::size_t index = 0; index < batch.size(); ++index) {
                    auto const& key = batch[index].key;
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < batch.size() and valid has one flag per batch entry
                    if (!valid[index]) {
                        ++dropped;
                        continue;
                    }
                    auto const old = edit.find(key);
                    auto slot = std::make_shared<Slot>();
                    if (auto const found = returned.find(key); found != returned.end()) {
                        slot->value = *found->second.first;
                        slot->bytes = found->second.second;
                        stored.push_back(key);
                    } else {
                        slot->bytes = sizeof(Slot);
                        slot->failures = old != nullptr && !old->value ? old->failures + 1 : 1;
                        slot->failedAt = now;
                        failed.push_back(key);
                    }
                    edit.put(key, std::move(slot));
                }
                auto next = edit.table();
                if (storeHook) {
                    storeHook();
                }
                {
                    std::scoped_lock const lock{writer};
                    if (version != seen) {
                        continue;  // a change was published meanwhile: prepare again over it
                    }
                    for (auto const& loading : batch) {
                        if (auto const flight = inFlight.find(loading.key);
                            flight != inFlight.end() && flight->second == std::pair{loading.epoch, loading.started}) {
                            inFlight.erase(flight);
                        }
                        invalidatedAt.erase(loading.key);
                    }
                    for (auto const& key : stored) {
                        auto const slot = findIn(*next, key);
                        slot->lastUse.store(useCounter.fetch_add(1) + 1);
                        if (auto const lease = pendingLeases.find(key); lease != pendingLeases.end()) {
                            if (lease->second.until > ticks(now)) {
                                slot->leaseHolders.store(lease->second.holders);
                                slot->leaseUntil.store(lease->second.until);
                            }
                            pendingLeases.erase(lease);
                            leasedStored = true;
                        }
                        if (auto const kept = protectedKeys.find(key); kept != protectedKeys.end()) {
                            slot->setHolds.store(kept->second);
                            leasedStored = true;
                        }
                    }
                    for (auto const& key : failed) {
                        pendingLeases.erase(key);
                        if (auto const kept = protectedKeys.find(key); kept != protectedKeys.end()) {
                            auto const slot = findIn(*next, key);
                            slot->setHolds.store(kept->second);
                            recheckAt(slot->failedAt + retryDelay(slot->failures));
                        }
                    }
                    if (edit.changed()) {
                        publish(std::move(next), edit);
                    }
                    if (!stored.empty()) {
                        storedLineage.store(lineage);
                    }
                    if (counted) {
                        loadedKeys.fetch_add(stored.size());
                    }
                    failedLoads.fetch_add(failed.size());
                    droppedLoads.fetch_add(dropped);
                    told.sink = sink;
                    told.failedHandler = failedHandler;
                }
                told.stored = std::move(stored);
                told.failed = std::move(failed);
                break;
            }
            // Leased bytes grow only when a leased value is stored; only then can they have passed the bound.
            told.overBound = leasedStored && dropLeasesOverBound(now);
            evict(told.stored, told.failed, now);
            return told;
        }

        // Under the writer mutex: what `stats().bytes` reads, the values, the failures and the accepted sets' keys.
        void storeBytes() { bytes.store(valueBytes + failureBytes + setBytes); }

        void forget(Slot const* slot) {
            if (slot == nullptr) {
                return;
            }
            if (slot->value) {
                valueBytes -= slot->bytes;
                --values;
            } else {
                failureBytes -= slot->bytes;
            }
            storeBytes();
        }

        void remember(Slot const& slot) {
            if (slot.value) {
                valueBytes += slot.bytes;
                ++values;
            } else {
                failureBytes += slot.bytes;
            }
            storeBytes();
        }

        // When the leased and protected values, with the accepted sets' keys, exceed the bound, stops loading for
        // order, drops every lease and ends every accepted set. The bytes are summed and the slots cleared over the
        // published state with no lock held; the queue, the pending leases and the sets are changed under it.
        bool dropLeasesOverBound(Clock::time_point now) {
            endExpiredSets(ticks(now));
            auto const table = publishedTable();
            std::size_t setsHeld = 0;
            {
                std::scoped_lock const lock{writer};
                setsHeld = setBytes;
            }
            if (leasedBytes(*table, ticks(now)) + setsHeld <= options.maxBytes) {
                return false;
            }
            {
                std::scoped_lock const lock{writer};
                queues.at(static_cast<std::size_t>(LevelUse::Order)).clear();
                std::erase_if(queued, [](auto const& entry) { return entry.second.use == LevelUse::Order; });
                pendingLeases.clear();
                endAllSets();  // their keys lose their protection: a repeat of a set must be fitted again
            }
            for (auto const& shard : table->shards) {
                for (auto const& [key, slot] : *shard) {
                    slot->leaseUntil.store(0);
                    slot->setHolds.store(0);
                }
            }
            return true;
        }

        // Evicts the least recently used unleased slots until the bound holds. The scan, the sort and the new
        // state are prepared with no lock held, over the published state; the writer mutex is taken only to check
        // that nothing changed and to publish. This batch's keys, stored or failed, are not candidates, nor is a
        // failure still inside its retry delay; when the bound still does not hold, the stored keys are marked to
        // go first.
        // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
        void evict(std::vector<RowId> const& stored, std::vector<RowId> const& failed, Clock::time_point now) {
            auto const nowTicks = ticks(now);
            endExpiredSets(nowTicks);
            std::unordered_set<RowId> fresh(stored.begin(), stored.end());
            fresh.insert(failed.begin(), failed.end());
            for (int attempt = 0;; ++attempt) {
                if (attempt == 8) {
                    ::morph::log::logError(
                        "[levelCache] {}: eviction lost to other changes eight times; the level "
                        "stays over its bound until the next store",
                        options.level);
                    return;
                }
                std::shared_ptr<Table const> base;
                std::uint64_t seen = 0;
                std::size_t held = 0;
                {
                    std::scoped_lock const lock{writer};
                    base = current;
                    seen = version;
                    held = valueBytes + failureBytes + setBytes;
                }
                if (held <= options.maxBytes) {
                    return;
                }
                if (evictionHook) {
                    evictionHook();
                }
                std::vector<std::tuple<std::int64_t, RowId, std::size_t>> candidates;
                for (auto const& shard : base->shards) {
                    for (auto const& [key, slot] : *shard) {
                        if (kept(*slot, nowTicks) || fresh.contains(key)) {
                            continue;
                        }
                        if (!slot->value && now < slot->failedAt + retryDelay(slot->failures)) {
                            continue;
                        }
                        candidates.emplace_back(slot->value ? slot->lastUse.load() : std::int64_t{-1}, key,
                                                slot->bytes);
                    }
                }
                std::ranges::sort(candidates);
                Edit edit{*base, shardCopies};
                auto remaining = held;
                std::size_t evicted = 0;
                for (auto const& [use, key, size] : candidates) {
                    if (remaining <= options.maxBytes) {
                        break;
                    }
                    edit.erase(key);
                    remaining -= std::min(remaining, size);
                    ++evicted;
                }
                auto next = edit.changed() ? edit.table() : nullptr;
                {
                    std::scoped_lock const lock{writer};
                    if (version != seen) {
                        continue;  // a change was published meanwhile: scan again over it
                    }
                    // A lease given since the scan protects its value: scan again.
                    bool leased = false;
                    for (auto const& [old, slot] : edit.changes) {
                        leased = leased || kept(*old, nowTicks);
                    }
                    if (leased) {
                        continue;
                    }
                    if (next != nullptr) {
                        publish(std::move(next), edit);
                    }
                    evictedKeys.fetch_add(evicted);
                    if (valueBytes + failureBytes > options.maxBytes) {
                        for (auto const& key : stored) {
                            if (auto const slot = findIn(*current, key); slot != nullptr) {
                                slot->lastUse.store(0);
                            }
                        }
                    }
                }
                return;
            }
        }

        template <typename Call>
        static void guarded(Call&& call) noexcept {
            try {
                std::forward<Call>(call)();
            } catch (std::exception const& exc) {
                ::morph::log::logError("[levelCache] a callback threw: {}", exc.what());
            } catch (...) {
                ::morph::log::logError("[levelCache] a callback threw");
            }
        }

        void tell(Told const& told) const {
            if (told.sink) {
                for (auto const& record : options.records) {
                    if (!told.stored.empty()) {
                        guarded([&] { told.sink(record, changeKeys(told.stored)); });
                    }
                    if (!told.failed.empty()) {
                        guarded([&] { told.sink(record, changeKeys(told.failed)); });
                    }
                }
            }
            if (!told.failed.empty() && told.failedHandler) {
                guarded([&] { told.failedHandler(told.failed); });
            }
            if (told.overBound && told.sink) {
                for (auto const& record : options.records) {
                    guarded([&] { told.sink(record, std::nullopt); });
                }
            }
        }

        void storeQuietly(std::vector<std::pair<RowId, Value>> loaded) {
            std::vector<Loading> batch;
            {
                std::scoped_lock const lock{writer};
                auto const started = ++sequence;
                batch.reserve(loaded.size());
                for (auto const& [key, value] : loaded) {
                    batch.push_back(Loading{.key = key, .epoch = epoch, .started = started});
                }
            }
            static_cast<void>(store(batch, loaded, false, unitLineage()));
        }

        // Removes keys. The new state is prepared with no lock held; the writer mutex is taken to check that
        // nothing changed, mark the keys in flight as invalidated, and publish.
        void invalidate(std::span<RowId const> keys) {
            for (;;) {
                std::shared_ptr<Table const> base;
                std::uint64_t seen = 0;
                {
                    std::scoped_lock const lock{writer};
                    base = current;
                    seen = version;
                }
                Edit edit{*base, shardCopies};
                for (auto const& key : keys) {
                    if (findIn(*base, key) != nullptr) {
                        edit.erase(key);
                    }
                }
                auto next = edit.changed() ? edit.table() : nullptr;
                std::scoped_lock const lock{writer};
                if (version != seen) {
                    continue;
                }
                bool recorded = false;
                for (auto const& key : keys) {
                    if (inFlight.contains(key)) {
                        invalidatedAt.insert_or_assign(key, ++sequence);
                        recorded = true;
                    }
                    pendingLeases.erase(key);
                    // A protected key removed: every accepted set's next repeat queues what its set misses.
                    if (protectedKeys.contains(key)) {
                        ++setEvents;
                    }
                }
                if (next != nullptr) {
                    invalidatedKeys.fetch_add(edit.changes.size());
                    publish(std::move(next), edit);
                } else if (recorded) {
                    // A store preparing a key in flight checks `version` before it publishes: it must see this.
                    ++version;
                }
                return;
            }
        }

        // Under the writer mutex: ends every accepted set and every key's protection; the caller clears the slots
        // it does not drop.
        void endAllSets() {
            sets.clear();
            protectedKeys.clear();
            setBytes = 0;
            storeBytes();
            setsRecheckAt = std::numeric_limits<std::int64_t>::max();
        }

        // The lineage of the registry `options.units` gives now; 0 without one.
        [[nodiscard]] std::uint64_t unitLineage() const {
            if (!options.units) {
                return 0;
            }
            auto const registry = options.units();
            return registry != nullptr ? registry->lineage() : 0;
        }

        // Drops every value when the registry is of another lineage than the one they were loaded with. Read with
        // no lock: a snapshot never waits for a writer to learn that nothing changed.
        void checkLineage() {
            if (!options.units) {
                return;
            }
            auto const stored = storedLineage.load();
            if (stored == 0 || stored == unitLineage()) {
                return;
            }
            invalidateAll();
        }

        void invalidateAll() {
            std::scoped_lock const lock{writer};
            ++epoch;
            storedLineage.store(0);
            invalidatedKeys.fetch_add(current->entries);
            auto next = emptyTable(current->generation + 1);
            {
                std::scoped_lock const swap{publishMutex};
                published = next;
            }
            current = std::move(next);
            ++version;
            invalidatedAt.clear();
            pendingLeases.clear();
            endAllSets();
            valueBytes = 0;
            failureBytes = 0;
            values = 0;
            bytes.store(0);
        }

        void changed(effects::Changes const& changes) {
            bool other = false;
            for (auto const& change : changes.records) {
                if (std::ranges::find(options.invalidatedBy, change.record) == options.invalidatedBy.end()) {
                    continue;
                }
                if (std::ranges::find(options.records, change.record) == options.records.end()) {
                    other = true;
                    invalidateAll();
                    continue;
                }
                if (!change.keys) {
                    invalidateAll();
                    continue;
                }
                std::vector<RowId> keys;
                keys.reserve(change.keys->size());
                bool readable = true;
                for (auto const& key : *change.keys) {
                    auto rowId = rowIdOf(key);
                    if (!rowId) {
                        readable = false;
                        break;
                    }
                    keys.push_back(std::move(*rowId));
                }
                if (readable) {
                    invalidate(keys);
                } else {
                    invalidateAll();
                }
            }
            if (!other) {
                return;
            }
            effects::AnnounceSink told;
            {
                std::scoped_lock const lock{writer};
                told = sink;
            }
            if (told) {
                for (auto const& record : options.records) {
                    guarded([&] { told(record, std::nullopt); });
                }
            }
        }

        [[nodiscard]] std::size_t queuedEntries() const {
            std::scoped_lock const lock{writer};
            return std::get<0>(queues).size() + std::get<1>(queues).size();
        }

        [[nodiscard]] std::size_t shardCount() const { return publishedTable()->shards.size(); }

        [[nodiscard]] LevelCacheStats stats() const {
            return LevelCacheStats{.hits = hits.load(),
                                   .misses = misses.load(),
                                   .loaderCalls = loaderCalls.load(),
                                   .loadedKeys = loadedKeys.load(),
                                   .failedLoads = failedLoads.load(),
                                   .droppedLoads = droppedLoads.load(),
                                   .invalidatedKeys = invalidatedKeys.load(),
                                   .evictedKeys = evictedKeys.load(),
                                   .leasedKeys = leasedKeys.load(),
                                   .refusedOrders = refusedOrders.load(),
                                   .bytes = bytes.load()};
        }

        LevelCacheOptions options;
        Loader loader;
        SizeOf sizeOf;
        std::shared_ptr<Cells const> cells;
        // Called at each eviction scan, with no lock held: what a test pauses a store with.
        std::function<void()> evictionHook;
        // Called by a store between preparing and publishing, and by a want and a release before each chunk of
        // their keys with the chunk's index; with no lock held. What a test interleaves a change with.
        std::function<void()> storeHook;
        std::function<void(std::size_t)> wantHook;
        std::function<void(std::size_t)> releaseHook;
        // The order sets accepted, by address, and the bytes their keys own; the protection count of each of their
        // keys, held or not; the events a set's keys may need a look for, counted (a protected key removed, a
        // protected failure's delay passed); the earliest end of a protected failure's delay not yet counted.
        // Under `writer`.
        std::unordered_map<std::vector<RowId> const*, AcceptedSet> sets;
        std::size_t setBytes = 0;
        std::unordered_map<RowId, std::int32_t> protectedKeys;
        std::uint64_t setEvents = 0;
        std::int64_t setsRecheckAt = std::numeric_limits<std::int64_t>::max();
        // The sets a `wantSet` is fitting, and what wakes the repeats of them that wait; under `writer`.
        std::unordered_set<std::vector<RowId> const*> settling;
        std::condition_variable setSettled;
        std::atomic<std::size_t> fits{0};

        // Orders every change; guards everything below but `published`. Held only to read or publish state,
        // never across a scan, a sort or a shard copy.
        mutable std::mutex writer;
        // Held only to read or swap `published`.
        mutable std::mutex publishMutex;
        std::shared_ptr<Table const> published;
        std::shared_ptr<Table const> current;  // what `published` is, read under `writer`
        std::uint64_t version = 0;             // advances with every published change
        std::uint64_t epoch = 0;
        std::atomic<std::uint64_t> storedLineage{0};  // the unit lineage the values held were loaded under; 0: none
        std::uint64_t sequence = 0;
        std::uint64_t wants = 0;
        // Keys of the running batch invalidated after it started, by the sequence number of the invalidation.
        std::unordered_map<RowId, std::uint64_t> invalidatedAt;
        std::unordered_map<RowId, std::pair<std::uint64_t, std::uint64_t>> inFlight;  // epoch, started
        std::array<std::map<std::uint64_t, RowId>, 2> queues;                         // by priority
        std::unordered_map<RowId, Queued> queued;
        bool running = false;
        // Order leases of keys not held yet, by key, until a clock tick; a held key's lease is on its slot.
        struct PendingLease {
            std::int64_t until = 0;
            std::int32_t holders = 0;
        };
        std::unordered_map<RowId, PendingLease> pendingLeases;
        std::size_t valueBytes = 0;
        std::size_t failureBytes = 0;
        std::size_t values = 0;
        std::atomic<std::int64_t> useCounter{0};
        effects::AnnounceSink sink;
        std::function<void(std::span<RowId const>)> failedHandler;

        std::atomic<std::size_t> hits{0};
        std::atomic<std::size_t> misses{0};
        std::atomic<std::size_t> loaderCalls{0};
        std::atomic<std::size_t> loadedKeys{0};
        std::atomic<std::size_t> failedLoads{0};
        std::atomic<std::size_t> droppedLoads{0};
        std::atomic<std::size_t> invalidatedKeys{0};
        std::atomic<std::size_t> evictedKeys{0};
        std::atomic<std::size_t> leasedKeys{0};
        std::atomic<std::size_t> refusedOrders{0};
        std::atomic<std::size_t> bytes{0};
        std::atomic<std::size_t> shardCopies{0};
    };

    std::shared_ptr<Core> _core;
};

/// @brief The level cache whose value is a row's cells, in the level's column order.
using CellLevelCache = LevelCache<std::vector<Cell>>;

/// @brief One level a reply reads: its source and the snapshot taken at the reply's start.
struct LevelInput {
    /// @brief The level.
    std::shared_ptr<LevelSource> source;
    /// @brief The level's state the reply reads.
    std::shared_ptr<LevelSnapshot const> snapshot;
};

/// @brief One request's immutable view of a table: base-level columns from the table's source, level columns
///        from the level snapshots the request took.
///
/// A level column is read from its level's snapshot at the column's place in `LevelSource::columns()`, found by
/// id, so a caller whose description holds the level's columns in another order or only some of them reads each
/// under its own id. A level column is `Unloaded` unless its snapshot reads the row `Loaded`; a level no input
/// serves is `Unloaded` in every row.
class MORPH_API LeveledSource final : public DataSource {
public:
    /// @brief The view of @p base and @p levels under @p columns.
    /// @param base    The table's rows; its snapshot is taken here, once.
    /// @param columns The columns the view has: the description's.
    /// @param levels  The levels the request read, each with its snapshot.
    LeveledSource(std::shared_ptr<DataSource const> const& base, std::vector<ColumnInfo> columns,
                  std::vector<LevelInput> levels);

    LeveledSource(LeveledSource const&) = delete;
    LeveledSource& operator=(LeveledSource const&) = delete;
    LeveledSource(LeveledSource&&) = delete;
    LeveledSource& operator=(LeveledSource&&) = delete;
    ~LeveledSource() override = default;

    /// @brief The view's columns.
    /// @return The columns given.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The view's one snapshot.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override { return _snapshot; }

    /// @brief The base source's declared groups, copied when the view was made.
    /// @return The groups.
    [[nodiscard]] std::span<ColumnGroupInfo const> groups() const override { return _groups; }

    /// @brief Does nothing: the view never changes.
    /// @param listener Ignored.
    void subscribe(ChangeListener& listener) override { static_cast<void>(listener); }

    /// @brief Does nothing: the view never changes.
    /// @param listener Ignored.
    void unsubscribe(ChangeListener& listener) override { static_cast<void>(listener); }

    /// @brief What a level holds for a row.
    /// @param row   Row index of the view's snapshot.
    /// @param level The level.
    /// @return The level snapshot's read of the row's key; `Unloaded` for a level no input serves.
    [[nodiscard]] LevelRead state(std::size_t row, std::string_view level) const {
        return _snapshot->state(row, level);
    }

private:
    struct Plan {
        std::optional<std::size_t> base;
        std::optional<std::size_t> level;
        std::size_t place = 0;
    };

    struct Level {
        std::string name;
        std::shared_ptr<LevelSnapshot const> snapshot;
        std::size_t width = 0;
    };

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- column and level indices come from the plans built against the same lists
    class MORPH_API Snapshot final : public RowSnapshot {
    public:
        Snapshot(std::shared_ptr<RowSnapshot const> base, std::vector<Plan> plans, std::vector<Level> levels)
            : _base{std::move(base)}, _plans{std::move(plans)}, _levels{std::move(levels)} {}

        [[nodiscard]] std::size_t rowCount() const override { return _base->rowCount(); }

        [[nodiscard]] RowId rowId(std::size_t row) const override { return _base->rowId(row); }

        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): row, then column, as DataSource::cell declares them.
        [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override;

        void readColumn(std::size_t column, ColumnSink& sink, std::size_t first, std::size_t last) const override;

        [[nodiscard]] MORPH_PROCESS_UNIQUE LevelRead state(std::size_t row, std::string_view level) const;

    private:
        std::shared_ptr<RowSnapshot const> _base;
        std::vector<Plan> _plans;
        std::vector<Level> _levels;
    };
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    std::vector<ColumnInfo> _columns;
    std::vector<ColumnGroupInfo> _groups;
    std::shared_ptr<Snapshot const> _snapshot;
};

}  // namespace morph::table
