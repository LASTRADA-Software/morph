// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/table_state.hpp>.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/table_state_model.hpp>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace morph::ui {

void PreferencesTableStateStore::retryOn(reactive::Scheduler& scheduler, std::chrono::milliseconds retryDelay) {
    _scheduler = &scheduler;
    _retryDelay = retryDelay;
}

async::Completion<std::optional<table::StoredTableState>> PreferencesTableStateStore::load(
    table::TableStateKey const& key) {
    auto [completion, promise] = async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_owner);
    queue(std::make_shared<Reading>(Reading{.key = key, .promise = std::move(promise)}));
    return std::move(completion);
}

async::Completion<table::SaveResult> PreferencesTableStateStore::save(table::TableStateKey const& key,
                                                                      std::string json, std::string expected) {
    auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_owner);
    auto upload = std::make_shared<Upload>(Upload{.key = key,
                                                  .expected = std::move(expected),
                                                  .id = {},
                                                  .text = std::move(json),
                                                  .offset = 0,
                                                  .promise = std::move(promise),
                                                  .retries = 0});
    if (upload->text.size() > kPartBytes) {
        upload->id = std::format("{:016x}", _random());
    }
    sendPart(upload);
    return std::move(completion);
}

async::Completion<bool> PreferencesTableStateStore::remove(table::TableStateKey const& key) {
    auto [completion, promise] = async::Completion<bool>::makeSettleable(_owner);
    auto const settle = std::make_shared<async::Completion<bool>::Promise>(std::move(promise));
    channel()
        ->execute("remove", model::ActionTraits<TableStateRemove>::toJson(TableStateRemove{.key = key}), CallOptions{})
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

async::Completion<table::SaveResult> PreferencesTableStateStore::publish(table::TableStateKey const& key,
                                                                         table::ViewOperation const& operation,
                                                                         std::string expected) {
    auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_owner);
    auto const settle = std::make_shared<async::Completion<table::SaveResult>::Promise>(std::move(promise));
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

std::shared_ptr<ModelChannel> const& PreferencesTableStateStore::channel() {
    if (_channel == nullptr) {
        _channel = _dispatcher->bind(std::string{kTableStateModel}, bridge::BindSharing::Private, {});
    }
    return _channel;
}

void PreferencesTableStateStore::queue(std::shared_ptr<Reading> reading) {
    _batch.push_back(std::move(reading));
    if (_batch.size() == 1) {
        _owner->post(_scope.token().guard([this] { flushLoads(); }));
    }
}

void PreferencesTableStateStore::flushLoads() {
    std::vector<std::shared_ptr<Reading>> all = std::move(_batch);
    _batch.clear();
    for (std::size_t first = 0; first < all.size(); first += kMaxTableStateKeys) {
        auto const last = std::min(all.size(), first + kMaxTableStateKeys);
        sendLoads(std::make_shared<std::vector<std::shared_ptr<Reading>>>(
            all.begin() + static_cast<std::ptrdiff_t>(first), all.begin() + static_cast<std::ptrdiff_t>(last)));
    }
}

void PreferencesTableStateStore::sendLoads(std::shared_ptr<std::vector<std::shared_ptr<Reading>>> const& batch) {
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
                      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): loaded.entries.size() == batch->size(), checked above
                      start((*batch)[at], std::move(loaded.entries[at]));
                  }
              })
        .onError(_scope, [batch](std::exception_ptr const& error) {
            for (auto const& reading : *batch) {
                reading->promise.reject(error);
            }
        });
}

void PreferencesTableStateStore::start(std::shared_ptr<Reading> const& reading, TableStateHead head) {
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

void PreferencesTableStateStore::readMore(std::shared_ptr<Reading> const& reading) {
    if (reading->text.size() >= reading->size) {
        reading->promise.resolve(
            table::StoredTableState{.json = std::move(reading->text), .revision = std::move(reading->revision)});
        return;
    }
    TableStateRead const request{.key = reading->key, .revision = reading->revision, .offset = reading->text.size()};
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
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the function object owns the callable it heap-allocates for the capture.

void PreferencesTableStateStore::retryLater(std::shared_ptr<Upload> const& upload) {
    auto send = _scope.token().guard([this, upload] { sendPart(upload); });
    if (_scheduler == nullptr) {
        _owner->post(std::move(send));
        return;
    }
    std::uint64_t const token = ++_lastRetry;
    _retries.insert_or_assign(token, _scheduler->after(_retryDelay, _scope.token().guard([this, token, send] mutable {
        _retries.erase(token);
        send();
    })));
}

void PreferencesTableStateStore::sendPart(std::shared_ptr<Upload> const& upload) {
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
                  if (upload->offset == 0 && result.outcome == table::SaveOutcome::Refused &&
                      result.reason == kTableStateUploadsBusy && upload->retries < kMaxUploadRetries) {
                      ++upload->retries;
                      retryLater(upload);
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
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the function object owns the callable it heap-allocates for the capture.

TableStates::TableStates(std::shared_ptr<table::ITableStateStore> store, exec::IExecutor& owner MORPH_LIFETIMEBOUND,
                         reactive::Scheduler* scheduler, std::chrono::milliseconds debounce)
    : _store{std::move(store)}, _owner{&owner}, _scheduler{scheduler}, _debounce{debounce} {
    if (_store == nullptr) {
        throw std::invalid_argument{"TableStates: a null store"};
    }
}

std::optional<table::StoredTableState> TableStates::kept(table::TableStateKey const& key) const {
    auto const found = _entries.find(tie(key));
    return found == _entries.end() ? std::nullopt : found->second.kept;
}

async::Completion<std::optional<table::StoredTableState>> TableStates::load(table::TableStateKey const& key) {
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

void TableStates::changed(table::TableStateKey const& key, table::StateDocument const& document, StateParts parts) {
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

void TableStates::start(table::TableStateKey const& key, table::StateDocument document) {
    auto const found = _entries.find(tie(key));
    if (found == _entries.end() || !found->second.settled || found->second.dirty) {
        return;
    }
    found->second.current = std::move(document);
}

void TableStates::flush(table::TableStateKey const& key) {
    auto const found = _entries.find(tie(key));
    if (found == _entries.end()) {
        return;
    }
    found->second.timer.cancel();
    trySave(key);
}

void TableStates::keep(table::TableStateKey const& key, table::StoredTableState state) {
    Entry& entry = _entries[tie(key)];
    entry.kept = std::move(state);
    entry.known = true;
    if (!entry.settled && !entry.loading) {
        settle(entry, entry.kept);
    }
}

void TableStates::take(table::StateDocument& into, table::StateDocument const& from, StateParts parts) {
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

void TableStates::startLoad(table::TableStateKey const& key, Entry& entry) {
    entry.loading = true;
    _store->load(key)
        .then(_scope, [this, key](std::optional<table::StoredTableState> const& stored) { loaded(key, stored); })
        .onError(_scope, [this, key](std::exception_ptr const& error) { loadFailed(key, error); });
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the function object owns the callable it heap-allocates for the capture.

void TableStates::loaded(table::TableStateKey const& key, std::optional<table::StoredTableState> const& stored) {
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

void TableStates::loadFailed(table::TableStateKey const& key, std::exception_ptr const& error) {
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

void TableStates::settle(Entry& entry, std::optional<table::StoredTableState> const& stored) {
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

void TableStates::trySave(table::TableStateKey const& key) {
    auto const found = _entries.find(tie(key));
    if (found == _entries.end()) {
        return;
    }
    Entry& entry = found->second;
    if (!entry.settled || entry.saving || !entry.dirty || entry.timer.active()) {
        return;
    }
    std::string const json = table::writeState(entry.current);
    entry.dirty = false;
    entry.saving = true;
    _store->save(key, json)
        .then(_scope, [this, key, json](table::SaveResult const& result) { saved(key, json, result); })
        .onError(_scope, [this, key](std::exception_ptr const& error) {
            saved(key, {},
                  table::SaveResult{
                      .outcome = table::SaveOutcome::Refused, .revision = {}, .reason = detail::failureText(error)});
        });
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the function object owns the callable it heap-allocates for the capture.

void TableStates::saved(table::TableStateKey const& key, std::string const& json, table::SaveResult const& result) {
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

}  // namespace morph::ui
