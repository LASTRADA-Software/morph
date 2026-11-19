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
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../core/bridge.hpp"
#include "../core/callback_scope.hpp"
#include "../core/completion.hpp"
#include "../core/executor.hpp"
#include "../reactive/scheduler.hpp"
#include "../table/state_store.hpp"
#include "interpreter.hpp"
#include "table_state_model.hpp"

/// @file
/// @brief The client half of persisted table state: `PreferencesTableStateStore`, which reaches the server's
///        `morph.tableState` model through the interpreter's `Dispatcher`, and `TableStates`, the states a session
///        keeps, with load batching and debounced saves.
///
/// Specified in `docs/spec/table/table.md`, "Persisted state".

namespace morph::ui {

/// @brief A table-state store over the server's `morph.tableState` model, so table state lives in the application's
///        preferences store under the person the server verified.
///
/// Every call is made, and every completion delivered, on the owner. Loads asked for in one turn of the owner go out
/// as one `load` call per `kMaxTableStateKeys` keys, and a key a full reply defers is asked for in the next call, so
/// one table's state never fails another's load; a document longer than the head the server answers is completed with
/// `read` calls by offset, started again when its revision changes meanwhile. A document over a part's size is saved
/// as parts under one upload id.
class PreferencesTableStateStore final : public table::ITableStateStore {
public:
    /// @brief A store calling through @p dispatcher.
    /// @param dispatcher Where model calls go; bound to `morph.tableState` once, at the first call. Borrowed: it must
    ///                   outlive the store.
    /// @param owner      The runtime's owner, where calls are made and completions delivered. Borrowed.
    PreferencesTableStateStore(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, exec::IExecutor& owner MORPH_LIFETIMEBOUND)
        : _dispatcher{&dispatcher}, _owner{&owner}, _random{std::random_device{}()} {}

    /// @brief Reads a stored document, in the owner's turn's one `load` call.
    /// @param key Which table's state, and which part.
    /// @return The stored document, or nothing when none is stored.
    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& key) override {
        auto [completion, promise] = async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_owner);
        queue(std::make_shared<Reading>(Reading{.key = key, .promise = std::move(promise)}));
        return std::move(completion);
    }

    /// @brief Stores @p json under the `expected` rule of `ITableStateStore::save`.
    /// @param key      Which table's state, and which part.
    /// @param json     The document.
    /// @param expected The revision the caller loaded, or empty.
    /// @return What the server answered.
    [[nodiscard]] async::Completion<table::SaveResult> save(table::TableStateKey const& key, std::string json,
                                                            std::string expected = {}) override {
        auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_owner);
        auto upload = std::make_shared<Upload>(Upload{.key = key,
                                                      .expected = std::move(expected),
                                                      .id = {},
                                                      .text = std::move(json),
                                                      .offset = 0,
                                                      .promise = std::move(promise)});
        if (upload->text.size() > kPartBytes) {
            upload->id = std::format("{:016x}", _random());
        }
        sendPart(std::move(upload));
        return std::move(completion);
    }

    /// @brief Forgets a stored document.
    /// @param key Which table's state, and which part.
    /// @return Whether a document was stored.
    [[nodiscard]] async::Completion<bool> remove(table::TableStateKey const& key) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(_owner);
        auto settle = std::make_shared<async::Completion<bool>::Promise>(std::move(promise));
        channel()
            ->execute("remove", model::ActionTraits<TableStateRemove>::toJson(TableStateRemove{.key = key}),
                      CallOptions{})
            .then(_scope,
                  [settle](std::string const& reply) {
                      try {
                          settle->resolve(model::ActionTraits<TableStateRemove>::resultFromJson(reply).removed);
                      } catch (...) {
                          settle->reject(std::current_exception());
                      }
                  })
            .onError(_scope, [settle](std::exception_ptr const& error) { settle->reject(error); });
        return std::move(completion);
    }

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
                                                               std::string expected) override {
        auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_owner);
        auto settle = std::make_shared<async::Completion<table::SaveResult>::Promise>(std::move(promise));
        TableStatePublish const request{
            .key = key, .operation = table::writeViewOperation(operation), .expected = std::move(expected)};
        channel()
            ->execute("publish", model::ActionTraits<TableStatePublish>::toJson(request), CallOptions{})
            .then(_scope,
                  [settle](std::string const& reply) {
                      try {
                          settle->resolve(model::ActionTraits<TableStatePublish>::resultFromJson(reply).result);
                      } catch (...) {
                          settle->reject(std::current_exception());
                      }
                  })
            .onError(_scope, [settle](std::exception_ptr const& error) { settle->reject(error); });
        return std::move(completion);
    }

private:
    /// The most bytes one `save` part carries: the server's default part size.
    static constexpr std::size_t kPartBytes = kTableStatePartBytes;
    /// How often one load starts again because the document changed while it was read.
    static constexpr int kMaxRestarts = 8;

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
    };

    [[nodiscard]] std::shared_ptr<ModelChannel> const& channel() {
        if (_channel == nullptr) {
            _channel = _dispatcher->bind(std::string{kTableStateModel}, bridge::BindSharing::Private, {});
        }
        return _channel;
    }

    /// Adds a load to the owner's turn's batch, posting the batch's call with the first.
    void queue(std::shared_ptr<Reading> reading) {
        _batch.push_back(std::move(reading));
        if (_batch.size() == 1) {
            _owner->post(_scope.token().guard([this] { flushLoads(); }));
        }
    }

    /// Sends the turn's loads, in calls of at most `kMaxTableStateKeys` keys.
    void flushLoads() {
        std::vector<std::shared_ptr<Reading>> all = std::move(_batch);
        _batch.clear();
        for (std::size_t first = 0; first < all.size(); first += kMaxTableStateKeys) {
            auto const last = std::min(all.size(), first + kMaxTableStateKeys);
            sendLoads(std::make_shared<std::vector<std::shared_ptr<Reading>>>(
                all.begin() + static_cast<std::ptrdiff_t>(first), all.begin() + static_cast<std::ptrdiff_t>(last)));
        }
    }

    void sendLoads(std::shared_ptr<std::vector<std::shared_ptr<Reading>>> batch) {
        TableStateLoad request;
        for (auto const& reading : *batch) {
            request.keys.push_back(reading->key);
        }
        channel()
            ->execute("load", model::ActionTraits<TableStateLoad>::toJson(request), CallOptions{})
            .then(_scope,
                  [this, batch](std::string const& reply) {
                      TableStateLoaded loaded;
                      try {
                          loaded = model::ActionTraits<TableStateLoad>::resultFromJson(reply);
                          if (loaded.entries.size() != batch->size()) {
                              throw std::runtime_error{"a table-state load answered " +
                                                       std::to_string(loaded.entries.size()) + " entries for " +
                                                       std::to_string(batch->size()) + " keys"};
                          }
                      } catch (...) {
                          for (auto const& reading : *batch) {
                              reading->promise.reject(std::current_exception());
                          }
                          return;
                      }
                      for (std::size_t at = 0; at < batch->size(); ++at) {
                          start((*batch)[at], std::move(loaded.entries[at]));
                      }
                  })
            .onError(_scope, [batch](std::exception_ptr const& error) {
                for (auto const& reading : *batch) {
                    reading->promise.reject(error);
                }
            });
    }

    void start(std::shared_ptr<Reading> const& reading, TableStateHead head) {
        if (head.deferred) {
            queue(reading);  // the reply was full: asked for again in the next call
            return;
        }
        if (!head.error.empty()) {
            reading->promise.reject(std::make_exception_ptr(std::runtime_error{head.error}));
            return;
        }
        if (!head.found) {
            reading->promise.resolve(std::nullopt);
            return;
        }
        reading->revision = std::move(head.revision);
        reading->size = head.size;
        reading->text = std::move(head.text);
        readMore(reading);
    }

    void readMore(std::shared_ptr<Reading> const& reading) {
        if (reading->text.size() >= reading->size) {
            reading->promise.resolve(
                table::StoredTableState{.json = std::move(reading->text), .revision = std::move(reading->revision)});
            return;
        }
        TableStateRead const request{
            .key = reading->key, .revision = reading->revision, .offset = reading->text.size()};
        channel()
            ->execute("read", model::ActionTraits<TableStateRead>::toJson(request), CallOptions{})
            .then(_scope,
                  [this, reading](std::string const& reply) {
                      TableStateChunk chunk;
                      try {
                          chunk = model::ActionTraits<TableStateRead>::resultFromJson(reply);
                      } catch (...) {
                          reading->promise.reject(std::current_exception());
                          return;
                      }
                      if (chunk.revision != reading->revision) {
                          if (++reading->restarts > kMaxRestarts) {
                              reading->promise.reject(std::make_exception_ptr(
                                  std::runtime_error{"a table's state kept changing while it was read"}));
                              return;
                          }
                          reading->text.clear();
                          queue(reading);
                          return;
                      }
                      if (chunk.text.empty()) {
                          reading->promise.reject(
                              std::make_exception_ptr(std::runtime_error{"a table's state ended before its size"}));
                          return;
                      }
                      reading->text += chunk.text;
                      readMore(reading);
                  })
            .onError(_scope, [reading](std::exception_ptr const& error) { reading->promise.reject(error); });
    }

    void sendPart(std::shared_ptr<Upload> upload) {
        std::string_view const rest = std::string_view{upload->text}.substr(upload->offset);
        std::size_t const length = upload->id.empty() ? rest.size() : detail::utf8Prefix(rest, kPartBytes);
        TableStateSave const request{.key = upload->key,
                                     .expected = upload->expected,
                                     .upload = upload->id,
                                     .offset = upload->offset,
                                     .text = std::string{rest.substr(0, length)},
                                     .last = length == rest.size()};
        channel()
            ->execute("save", model::ActionTraits<TableStateSave>::toJson(request), CallOptions{})
            .then(_scope,
                  [this, upload, length, last = request.last](std::string const& reply) {
                      table::SaveResult result;
                      try {
                          result = model::ActionTraits<TableStateSave>::resultFromJson(reply).result;
                      } catch (...) {
                          upload->promise.reject(std::current_exception());
                          return;
                      }
                      if (last) {
                          upload->promise.resolve(std::move(result));
                          return;
                      }
                      upload->offset += length;
                      sendPart(upload);
                  })
            .onError(_scope, [upload](std::exception_ptr const& error) { upload->promise.reject(error); });
    }

    Dispatcher* _dispatcher;
    exec::IExecutor* _owner;
    std::mt19937_64 _random;
    std::shared_ptr<ModelChannel> _channel;
    /// Loads asked for in this turn of the owner.
    std::vector<std::shared_ptr<Reading>> _batch;
    /// Last, so it is stopped first: a delivery after the store is gone runs nothing.
    async::CallbackScope _scope;
};

/// @brief The parts of a table's state a change touches: a bit set.
enum class StateParts : std::uint8_t {
    Layout = 1,   ///< The column layout, with the columns and groups carried for an absent column.
    Sort = 2,     ///< The sort chain, with the keys carried for an absent column.
    Filters = 4,  ///< The filters, with those carried for an absent column or group.
    View = 8,     ///< The saved view the arrangement was taken from.
};

/// @brief The union of two sets of parts.
/// @param a One set.
/// @param b The other.
/// @return Every part in either.
[[nodiscard]] constexpr StateParts operator|(StateParts a, StateParts b) noexcept {
    return static_cast<StateParts>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
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
class TableStates {
public:
    /// @brief The kept states of a session.
    /// @param store     Where states are loaded from and saved to.
    /// @param owner     The runtime's owner, where `load` answers. Borrowed.
    /// @param scheduler The debounce's timers; without one every change saves at once. Borrowed.
    /// @param debounce  How long a key must stay unchanged before its save goes out.
    /// @throws std::invalid_argument for a null store.
    TableStates(std::shared_ptr<table::ITableStateStore> store, exec::IExecutor& owner MORPH_LIFETIMEBOUND,
                reactive::Scheduler* scheduler, std::chrono::milliseconds debounce)
        : _store{std::move(store)}, _owner{&owner}, _scheduler{scheduler}, _debounce{debounce} {
        if (_store == nullptr) {
            throw std::invalid_argument{"TableStates: a null store"};
        }
    }

    TableStates(TableStates const&) = delete;
    TableStates& operator=(TableStates const&) = delete;
    TableStates(TableStates&&) = delete;
    TableStates& operator=(TableStates&&) = delete;
    ~TableStates() = default;

    /// @brief The kept copy of a key's state: what the store held when it was loaded, or what was last saved.
    /// @param key The key.
    /// @return It; nothing when the key was not loaded, or nothing is stored under it.
    [[nodiscard]] std::optional<table::StoredTableState> kept(table::TableStateKey const& key) const {
        auto const found = _entries.find(tie(key));
        return found == _entries.end() ? std::nullopt : found->second.kept;
    }

    /// @brief A key's stored state: the kept copy at once, with no call, when the key is known; otherwise from the
    ///        store, one load in flight per key however many ask.
    /// @param key The key.
    /// @return The stored document, or nothing; the store's failure when the load fails.
    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(table::TableStateKey const& key) {
        auto [completion, promise] = async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_owner);
        Entry& entry = _entries[tie(key)];
        if (entry.known) {
            promise.resolve(entry.kept);
            return std::move(completion);
        }
        entry.waiters.push_back(std::move(promise));
        if (!entry.loading) {
            startLoad(key, entry);
        }
        return std::move(completion);
    }

    /// @brief Marks @p parts of the key's state changed to those of @p document; a debounced save, one in flight per
    ///        key.
    /// @param key      The key.
    /// @param document The arrangement now; only @p parts of it are taken.
    /// @param parts    The parts that changed.
    void changed(table::TableStateKey const& key, table::StateDocument const& document, StateParts parts) {
        Entry& entry = _entries[tie(key)];
        if (entry.settled) {
            take(entry.current, document, parts);
        } else {
            take(entry.held, document, parts);
            entry.heldParts = entry.heldParts | parts;
            if (entry.known && !entry.loading) {
                settle(entry, entry.kept);
            } else if (!entry.loading) {
                startLoad(key, entry);
            }
        }
        entry.dirty = true;
        if (_scheduler == nullptr) {
            trySave(key);
            return;
        }
        entry.timer = _scheduler->after(_debounce, _scope.token().guard([this, key] { trySave(key); }));
    }

    /// @brief Saves a key's unsaved change now, as a table does when it unmounts; held until its load settles.
    /// @param key The key.
    void flush(table::TableStateKey const& key) {
        auto const found = _entries.find(tie(key));
        if (found == _entries.end()) {
            return;
        }
        found->second.timer.cancel();
        trySave(key);
    }

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
    void keep(table::TableStateKey const& key, table::StoredTableState state) {
        Entry& entry = _entries[tie(key)];
        entry.kept = std::move(state);
        entry.known = true;
        if (!entry.settled && !entry.loading) {
            settle(entry, entry.kept);
        }
    }

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
    static void take(table::StateDocument& into, table::StateDocument const& from, StateParts parts) {
        if (holds(parts, StateParts::Layout)) {
            into.layout = from.layout;
            into.carried.columns = from.carried.columns;
            into.carried.groups = from.carried.groups;
        }
        if (holds(parts, StateParts::Sort)) {
            into.sort = from.sort;
            into.carried.sort = from.carried.sort;
        }
        if (holds(parts, StateParts::Filters)) {
            into.filters = from.filters;
            into.carried.filters = from.carried.filters;
            into.carried.groupFilters = from.carried.groupFilters;
        }
        if (holds(parts, StateParts::View)) {
            into.view = from.view;
        }
    }

    void startLoad(table::TableStateKey const& key, Entry& entry) {
        entry.loading = true;
        _store->load(key)
            .then(_scope, [this, key](std::optional<table::StoredTableState> const& stored) { loaded(key, stored); })
            .onError(_scope, [this, key](std::exception_ptr const& error) { loadFailed(key, error); });
    }

    void loaded(table::TableStateKey const& key, std::optional<table::StoredTableState> const& stored) {
        Entry& entry = _entries[tie(key)];
        entry.loading = false;
        bool const wasKnown = entry.known;
        if (!wasKnown) {
            entry.kept = stored;
            entry.known = true;
        }
        if (!entry.settled || (entry.fromFailure && !wasKnown && !entry.dirty && !entry.saving)) {
            settle(entry, entry.kept);
        }
        for (auto& waiter : std::exchange(entry.waiters, {})) {
            waiter.resolve(entry.kept);
        }
        if (entry.dirty) {
            trySave(key);
        }
    }

    void loadFailed(table::TableStateKey const& key, std::exception_ptr const& error) {
        Entry& entry = _entries[tie(key)];
        entry.loading = false;
        if (!entry.settled) {
            settle(entry, std::nullopt);
            entry.fromFailure = true;
        }
        for (auto& waiter : std::exchange(entry.waiters, {})) {
            waiter.reject(error);
        }
        if (entry.dirty) {
            trySave(key);
        }
    }

    /// Decides the document changes are made to: @p stored as `readState` reads it, or an empty one, with the held
    /// changes applied.
    static void settle(Entry& entry, std::optional<table::StoredTableState> const& stored) {
        table::StateDocument base;
        if (stored) {
            if (auto read = table::readState(stored->json)) {
                base = std::move(*read);
            }
        }
        take(base, entry.held, entry.heldParts);
        entry.current = std::move(base);
        entry.held = {};
        entry.heldParts = {};
        entry.settled = true;
        entry.fromFailure = false;
    }

    void trySave(table::TableStateKey const& key) {
        auto const found = _entries.find(tie(key));
        if (found == _entries.end()) {
            return;
        }
        Entry& entry = found->second;
        if (!entry.settled || entry.saving || !entry.dirty || entry.timer.active()) {
            return;
        }
        std::string json = table::writeState(entry.current);
        entry.dirty = false;
        entry.saving = true;
        _store->save(key, json)
            .then(_scope, [this, key, json](table::SaveResult const& result) { saved(key, json, result); })
            .onError(_scope, [this, key](std::exception_ptr const& error) {
                saved(
                    key, {},
                    table::SaveResult{
                        .outcome = table::SaveOutcome::Refused, .revision = {}, .reason = detail::failureText(error)});
            });
    }

    void saved(table::TableStateKey const& key, std::string const& json, table::SaveResult const& result) {
        Entry& entry = _entries[tie(key)];
        entry.saving = false;
        if (result.outcome == table::SaveOutcome::Saved) {
            entry.kept = table::StoredTableState{.json = json, .revision = result.revision};
            entry.known = true;
        } else if (_onSaveFailed) {
            _onSaveFailed(key, result);
        }
        if (entry.dirty) {
            trySave(key);
        }
    }

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
