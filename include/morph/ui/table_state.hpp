// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "table_state_model.hpp"

/// @file
/// @brief The client half of persisted table state: `PreferencesTableStateStore`, which reaches the server's
///        `morph.tableState` model through the interpreter's `Dispatcher`, and `TableStates`, the states a session
///        keeps, with load batching and debounced saves.
///
/// Specified in `docs/spec/table/table.md`, "Persisted state".

namespace morph::ui {

class Dispatcher;
class ModelChannel;

/// @brief A table-state store over the server's `morph.tableState` model, so table state lives in the application's
///        preferences store under the person the server verified.
///
/// Every call is made, and every completion delivered, on the owner. Loads asked for in one turn of the owner go out
/// as one `load` call per `kMaxTableStateKeys` keys, and a key a full reply defers is asked for in the next call, so
/// one table's state never fails another's load; a document longer than the head the server answers is completed with
/// `read` calls by offset, started again when its revision changes meanwhile. A document over a part's size is saved
/// as parts under one upload id; one the server refuses because its person has `kMaxTableStateUploads` uploads open
/// is sent again after a short wait, so a screen of many large states unmounting at once saves them all.
class MORPH_API PreferencesTableStateStore final : public table::ITableStateStore {
public:
    /// @brief A store calling through @p dispatcher.
    /// @param dispatcher Where model calls go; bound to `morph.tableState` once, at the first call. Borrowed: it must
    ///                   outlive the store.
    /// @param owner      The runtime's owner, where calls are made and completions delivered. Borrowed.
    /// @param scheduler  The timers a save the server refused for its open uploads waits on before it is sent again;
    ///                   without one it is sent again on the owner's next turn. Borrowed.
    /// @param retryDelay How long such a save waits: a session's `tableStateTimeout`.
    PreferencesTableStateStore(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, exec::IExecutor& owner MORPH_LIFETIMEBOUND,
                               reactive::Scheduler* scheduler = nullptr,
                               std::chrono::milliseconds retryDelay = std::chrono::milliseconds{300})
        : _dispatcher{&dispatcher},
          _owner{&owner},
          _scheduler{scheduler},
          _retryDelay{retryDelay},
          _random{std::random_device{}()} {}

    /// @brief Sets the timers a save the server refused for its open uploads waits on, and how long it waits. A
    ///        `Session` calls it with its own scheduler and `tableStateTimeout` when its `tableState` is this store.
    /// @param scheduler  The timers. Borrowed: it must outlive the store.
    /// @param retryDelay How long such a save waits before it is sent again.
    void retryOn(reactive::Scheduler& scheduler, std::chrono::milliseconds retryDelay);

    /// @brief Reads a stored document, in the owner's turn's one `load` call.
    /// @param key Which table's state, and which part.
    /// @return The stored document, or nothing when none is stored.
    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& key) override;

    /// @brief Stores @p json under the `expected` rule of `ITableStateStore::save`.
    /// @param key      Which table's state, and which part.
    /// @param json     The document.
    /// @param expected The revision the caller loaded, or empty.
    /// @return What the server answered.
    [[nodiscard]] async::Completion<table::SaveResult> save(table::TableStateKey const& key, std::string json,
                                                            std::string expected = {}) override;

    /// @brief Forgets a stored document.
    /// @param key Which table's state, and which part.
    /// @return Whether a document was stored.
    [[nodiscard]] async::Completion<bool> remove(table::TableStateKey const& key) override;

    /// @brief Whether the store keeps the `shared` part for everyone.
    /// @return `true`: the server keeps it with no person.
    [[nodiscard]] bool shares() const override { return true; }

    /// @brief Applies one operation to the `shared` part, through the server's `publish` action, which the
    ///        application's authoriser admits or refuses by its id.
    /// @param key       Which table's state; its `part` is `"shared"`.
    /// @param operation The operation.
    /// @param expected  The revision the caller loaded, `table::kNoDocument` when it loaded nothing, or empty.
    /// @return What the server answered; the call's failure when the server refused it.
    [[nodiscard]] async::Completion<table::SaveResult> publish(table::TableStateKey const& key,
                                                               table::ViewOperation const& operation,
                                                               std::string expected) override;

private:
    /// The most bytes one `save` part carries: the server's default part size.
    static constexpr std::size_t kPartBytes = kTableStatePartBytes;
    /// How often one load starts again because the document changed while it was read.
    static constexpr int kMaxRestarts = 8;
    /// How often a save is sent again while the server refuses it for its open uploads: a hundred tries, about as
    /// long as the server keeps an abandoned upload.
    static constexpr int kMaxUploadRetries = 100;

    struct Reading {
        table::TableStateKey key;
        async::Completion<std::optional<table::StoredTableState>>::Promise promise;
        std::string revision;
        std::uint64_t size = 0;
        std::string text;
        int restarts = 0;
    };

    struct Upload {
        table::TableStateKey key;
        std::string expected;
        std::string id;
        std::string text;
        std::size_t offset = 0;
        async::Completion<table::SaveResult>::Promise promise;
        int retries = 0;
    };

    [[nodiscard]] std::shared_ptr<ModelChannel> const& channel();

    /// Adds a load to the owner's turn's batch, posting the batch's call with the first.
    void queue(std::shared_ptr<Reading> reading);

    /// Sends the turn's loads, in calls of at most `kMaxTableStateKeys` keys.
    void flushLoads();

    void sendLoads(
        std::shared_ptr<std::vector<std::shared_ptr<Reading>>> const&
            batch);  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the load registers are owned by the scope.

    void start(std::shared_ptr<Reading> const& reading, TableStateHead head);

    void readMore(
        std::shared_ptr<Reading> const&
            reading);  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the read registers are owned by the scope.

    /// Sends a save the server refused for its open uploads again, after `retryDelay`.
    void retryLater(std::shared_ptr<Upload> const& upload);

    void sendPart(
        std::shared_ptr<Upload> const&
            upload);  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the save registers are owned by the scope.

    Dispatcher* _dispatcher;
    exec::IExecutor* _owner;
    reactive::Scheduler* _scheduler;
    std::chrono::milliseconds _retryDelay;
    std::mt19937_64 _random;
    /// Saves waiting to be sent again, by a counter.
    std::map<std::uint64_t, reactive::TimerHandle> _retries;
    std::uint64_t _lastRetry = 0;
    std::shared_ptr<ModelChannel> _channel;
    /// Loads asked for in this turn of the owner.
    std::vector<std::shared_ptr<Reading>> _batch;
    /// Last, so it is stopped first: a delivery after the store is gone runs nothing.
    async::CallbackScope _scope;
};

/// @brief The parts of a table's state a change touches: a bit set.
enum class StateParts : std::uint8_t {
    None = 0,     ///< No part.
    Layout = 1,   ///< The column layout, with the columns and groups carried for an absent column.
    Sort = 2,     ///< The sort chain, with the keys carried for an absent column.
    Filters = 4,  ///< The filters, with those carried for an absent column or group.
    View = 8,     ///< The saved view the arrangement was taken from.
};

/// @brief The union of two sets of parts.
/// @param lhs One set.
/// @param rhs The other.
/// @return Every part in either.
[[nodiscard]] constexpr StateParts operator|(StateParts lhs, StateParts rhs) noexcept {
    return static_cast<StateParts>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

/// @brief Whether @p set holds @p part.
/// @param set  The set.
/// @param part One part.
/// @return `true` when @p part is in @p set.
[[nodiscard]] constexpr bool holds(StateParts set, StateParts part) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(part)) != 0;
}

/// @brief The table states one session keeps, loads and saves: one per `Session` that has a store.
///
/// Every document it loaded or saved is kept, by key, for its life, so a key loaded once is answered at once after.
/// Changes are debounced per key: a save goes out when a key has not changed for the debounce, at once on `flush`,
/// and one save per key is in flight, a change made during it being one more save after it. A save writes the
/// whole `state` document with no expected revision.
///
/// A change made before the key's load settles is held: when the load completes, the save writes the loaded document
/// with only the changed parts replaced, so a sort changed during a slow load never overwrites the stored filters. A
/// load that fails, or whose text `table::readState` refuses, gives an empty document to change: the stored text is
/// never merged into, so the first save after a change replaces it with a `table::kStateVersion` document holding
/// the changed parts, and nothing is written until a change.
///
/// Everything runs on the owner, where the store delivers its completions.
class MORPH_API TableStates {
public:
    /// @brief The kept states of a session.
    /// @param store     Where states are loaded from and saved to.
    /// @param owner     The runtime's owner, where `load` answers. Borrowed.
    /// @param scheduler The debounce's timers; without one every change saves at once. Borrowed.
    /// @param debounce  How long a key must stay unchanged before its save goes out.
    /// @throws std::invalid_argument for a null store.
    TableStates(std::shared_ptr<table::ITableStateStore> store, exec::IExecutor& owner MORPH_LIFETIMEBOUND,
                reactive::Scheduler* scheduler, std::chrono::milliseconds debounce);

    TableStates(TableStates const&) = delete;
    TableStates& operator=(TableStates const&) = delete;
    TableStates(TableStates&&) = delete;
    TableStates& operator=(TableStates&&) = delete;
    ~TableStates() = default;

    /// @brief The kept copy of a key's state: what the store held when it was loaded, or what was last saved.
    /// @param key The key.
    /// @return It; nothing when the key was not loaded, or nothing is stored under it.
    [[nodiscard]] std::optional<table::StoredTableState> kept(table::TableStateKey const& key) const;

    /// @brief A key's stored state: the kept copy at once, with no call, when the key is known; otherwise from the
    ///        store, one load in flight per key however many ask.
    /// @param key The key.
    /// @return The stored document, or nothing; the store's failure when the load fails.
    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(table::TableStateKey const& key);

    /// @brief Marks @p parts of the key's state changed to those of @p document; a debounced save, one in flight per
    ///        key.
    /// @param key      The key.
    /// @param document The arrangement now; only @p parts of it are taken.
    /// @param parts    The parts that changed.
    void changed(table::TableStateKey const& key, table::StateDocument const& document, StateParts parts);

    /// @brief Takes @p document as the key's state now, without saving it, as a table does when it starts from a view
    ///        while nothing is stored: the next change is saved on top of it, so the rest of what it started from is
    ///        kept. Ignored until the key's load settles, and once a change has been made.
    /// @param key      The key.
    /// @param document The state the table started from.
    void start(table::TableStateKey const& key, table::StateDocument document);

    /// @brief Saves a key's unsaved change now, as a table does when it unmounts; held until its load settles.
    /// @param key The key.
    void flush(table::TableStateKey const& key);

    /// @brief Sets the one handler told of every save that did not end `Saved`: the store's completion failed (as
    ///        `Refused` with the failure's text as `reason`), or it answered `Refused` or `Conflict`.
    /// @param handler Called on the owner with the key and the result; empty to tell no one.
    void onSaveFailed(std::function<void(table::TableStateKey const&, table::SaveResult const&)> handler) {
        _onSaveFailed = std::move(handler);
    }

    /// @brief The store it loads from and saves to, for a part this class does not debounce, such as saved views.
    /// @return The store.
    [[nodiscard]] table::ITableStateStore& store() const noexcept { return *_store; }

    /// @brief Replaces the kept copy of @p key after a save made through `store()`, so a later mount restores it.
    ///        A debounced save of the key still goes out, with its own document.
    /// @param key   The key.
    /// @param state What the store holds now.
    void keep(table::TableStateKey const& key, table::StoredTableState state);

private:
    using Key = std::tuple<std::string, std::string, std::string, std::string, std::string>;

    struct Entry {
        /// What the store holds, as far as this session knows.
        std::optional<table::StoredTableState> kept;
        /// Whether `kept` is known: loaded, saved or kept.
        bool known = false;
        bool loading = false;
        std::vector<async::Completion<std::optional<table::StoredTableState>>::Promise> waiters;
        /// Whether the document changes are made to is decided: the load settled.
        bool settled = false;
        /// Whether that document came from a failed load, and so a later load may still replace it.
        bool fromFailure = false;
        /// The document saves write, once settled.
        table::StateDocument current;
        /// Changes made before the load settled, and which parts they touch.
        table::StateDocument held;
        StateParts heldParts{};
        bool dirty = false;
        bool saving = false;
        reactive::TimerHandle timer;
    };

    [[nodiscard]] static Key tie(table::TableStateKey const& key) {
        return Key{key.application, key.model, key.table, key.qualifier, key.part};
    }

    /// Copies @p parts of @p from into @p into, with what each part carries for an absent column.
    static void take(table::StateDocument& into, table::StateDocument const& from, StateParts parts);

    void startLoad(
        table::TableStateKey const& key,
        Entry&
            entry);  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the load registers are owned by the scope.

    void loaded(table::TableStateKey const& key, std::optional<table::StoredTableState> const& stored);

    void loadFailed(table::TableStateKey const& key, std::exception_ptr const& error);

    /// Decides the document changes are made to: @p stored as `readState` reads it, or an empty one, with the held
    /// changes applied.
    static void settle(Entry& entry, std::optional<table::StoredTableState> const& stored);

    void trySave(
        table::TableStateKey const&
            key);  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the save registers are owned by the scope.

    void saved(table::TableStateKey const& key, std::string const& json, table::SaveResult const& result);

    std::shared_ptr<table::ITableStateStore> _store;
    exec::IExecutor* _owner;
    reactive::Scheduler* _scheduler;
    std::chrono::milliseconds _debounce;
    std::function<void(table::TableStateKey const&, table::SaveResult const&)> _onSaveFailed;
    std::map<Key, Entry> _entries;
    /// Last, so it is stopped first: a delivery or timer after the states are gone runs nothing.
    async::CallbackScope _scope;
};

}  // namespace morph::ui
