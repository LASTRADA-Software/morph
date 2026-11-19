// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/owner_strand.hpp>
#include <morph/core/wire.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/preferences.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/value.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "ui_interpreter_support.hpp"

/// @file
/// @brief What table tests share: the test catalog of `Lims_SampleModel`'s table `samples`, builders of its
///        description and pages, `RowsFixture`, a session with a scripted dispatcher and no screen, for tests of
///        `TableRows`, and the stores table-state tests look into.

namespace morph::ui::testing {

/// @brief Reaches into a `WindowRows` for what a test checks and the rows do not show.
struct WindowRowsProbe {
    /// @brief The rows' window.
    /// @param rows The rows.
    /// @return Its window.
    [[nodiscard]] static table::PageWindow<table::GenericRow> const& window(WindowRows const& rows) {
        return rows._window;
    }
};

/// @brief The corpus catalog with the table `samples` of `Lims_SampleModel`: `Table.samples`, which holds its record
///        `Lims_SampleModel.samples` opaquely, and `Table.samples.describe`.
/// @return The catalog.
inline Catalog tableCatalog() {
    Catalog catalog = corpusCatalog();
    auto base = catalog.action;
    catalog.action = [base](std::string_view action) -> std::optional<ActionInfo> {
        auto info = base(action);
        if (info && action == "Table.samples") {
            info->modelType = "Lims_SampleModel";
            info->records = {ActionRecordInfo{.record = "Lims_SampleModel.samples",
                                              .keyField = "id",
                                              .paths = {},
                                              .open = {},
                                              .partial = {},
                                              .opaque = true}};
        }
        if (info && action == "Table.samples.describe") {
            info->modelType = "Lims_SampleModel";
        }
        return info;
    };
    return catalog;
}

/// @brief The description of `samples`: `id` (the Integer key), `name` (Text), then `c3` … Decimal.
/// @param columns The columns, at least 2.
/// @return The description, its revision set.
inline table::TableDescription samplesDescription(int columns) {
    table::TableDescription description;
    description.id = "samples";
    description.label = "Samples";
    description.key = "id";
    auto const add = [&description](std::string id, table::ColumnKind kind) {
        table::TableColumn column;
        column.info = table::ColumnInfo{.id = id, .kind = kind};
        column.label = std::move(id);
        description.columns.push_back(std::move(column));
    };
    add("id", table::ColumnKind::Integer);
    add("name", table::ColumnKind::Text);
    for (int at = 3; at <= columns; ++at) {
        add("c" + std::to_string(at), table::ColumnKind::Decimal);
    }
    return table::describe(std::move(description));
}

/// @brief A description's JSON, as `Table.<id>.describe` answers.
/// @param description The description.
/// @return The JSON.
inline std::string describeJson(table::TableDescription const& description) {
    return glz::write_json(description).value_or("");
}

/// @brief A page of `samples` as `Table.samples` answers, against a given revision: row keys `offset + i`, `id`
///        the key, `name` `"row-<key>"`, every other column the key.
/// @param revision  The description revision the page names.
/// @param offset    The first row's view index and key.
/// @param count     Rows.
/// @param total     The view's total.
/// @param columns   The columns the page holds.
/// @param kind      Whether the total is exact.
/// @param undecided Undecided rows.
/// @param awaiting  The order's awaited levels.
/// @return The page's JSON.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a page's numbers, in the order a page names them.
inline std::string pageJsonAt(std::string revision, std::int64_t offset, std::int64_t count, std::int64_t total,
                              std::vector<std::string> const& columns,
                              table::TotalKind kind = table::TotalKind::Exact, std::int64_t undecided = 0,
                              std::vector<std::string> awaiting = {}) {
    table::GenericPage page;
    page.revision = std::move(revision);
    page.columns = columns;
    page.total = total;
    page.totalKind = kind;
    page.offset = offset;
    page.undecided = undecided;
    page.awaiting = std::move(awaiting);
    for (std::int64_t key = offset; key < offset + count; ++key) {
        page.keys.emplace_back(key);
        table::GenericRow row;
        for (auto const& column : columns) {
            if (column == "name") {
                row.emplace_back(std::string{"row-"} + std::to_string(key));
            } else {
                row.emplace_back(key);
            }
        }
        page.rows.push_back(std::move(row));
    }
    return glz::write_json(page).value_or("");
}

/// @brief A page of `samples` against `samplesDescription(3)`'s revision; see `pageJsonAt`.
/// @param offset    The first row's view index and key.
/// @param count     Rows.
/// @param total     The view's total.
/// @param columns   The columns the page holds.
/// @param kind      Whether the total is exact.
/// @param undecided Undecided rows.
/// @param awaiting  The order's awaited levels.
/// @return The page's JSON.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a page's numbers, in the order a page names them.
inline std::string pageJson(std::int64_t offset, std::int64_t count, std::int64_t total,
                            std::vector<std::string> const& columns, table::TotalKind kind = table::TotalKind::Exact,
                            std::int64_t undecided = 0, std::vector<std::string> awaiting = {}) {
    return pageJsonAt(samplesDescription(3).revision, offset, count, total, columns, kind, undecided,
                      std::move(awaiting));
}

/// @brief A page of `samples` holding the given keys in order, against `samplesDescription(3)`'s revision; each
///        row as `pageJson` writes it.
/// @param offset  The first row's view index.
/// @param keys    The rows' keys, in view order.
/// @param total   The view's total.
/// @param columns The columns the page holds.
/// @return The page's JSON.
inline std::string pageJsonOfKeys(std::int64_t offset, std::vector<std::int64_t> const& keys, std::int64_t total,
                                  std::vector<std::string> const& columns) {
    table::GenericPage page;
    page.revision = samplesDescription(3).revision;
    page.columns = columns;
    page.total = total;
    page.offset = offset;
    for (auto const key : keys) {
        page.keys.emplace_back(key);
        table::GenericRow row;
        for (auto const& column : columns) {
            if (column == "name") {
                row.emplace_back(std::string{"row-"} + std::to_string(key));
            } else {
                row.emplace_back(key);
            }
        }
        page.rows.push_back(std::move(row));
    }
    return glz::write_json(page).value_or("");
}

/// @brief The view as a mount holds it: one entry per view row up to the total, the row's key as text, or empty for
///        a row not held.
/// @param rows The rows.
/// @return The entries.
inline std::vector<std::string> shownKeys(TableRows const& rows) {
    std::vector<std::string> out;
    auto const total = rows.total();
    for (std::int64_t at = 0; at < total.rows; ++at) {
        auto const row = rows.row(at);
        out.push_back(row ? std::visit([](auto const& key) { return std::format("{}", key); }, row->key)
                          : std::string{});
    }
    return out;
}

/// @brief A keyed reply of `samples`: each row's key and view row, its `name` `"renamed-<key>"`.
/// @param rows    The rows, as key and view row, in view order.
/// @param total   The view's total.
/// @param columns The columns.
/// @return The reply's JSON.
inline std::string keyedJson(std::vector<std::pair<std::int64_t, std::int64_t>> const& rows, std::int64_t total,
                             std::vector<std::string> const& columns) {
    table::GenericPage page;
    page.revision = samplesDescription(3).revision;
    page.columns = columns;
    page.total = total;
    for (auto const& [key, viewRow] : rows) {
        page.keys.emplace_back(key);
        page.indices.push_back(viewRow);
        table::GenericRow row;
        for (auto const& column : columns) {
            if (column == "name") {
                row.emplace_back(std::string{"renamed-"} + std::to_string(key));
            } else {
                row.emplace_back(key);
            }
        }
        page.rows.push_back(std::move(row));
    }
    return glz::write_json(page).value_or("");
}

/// @brief The formatting facts of a locale, built from the locale table as a session's locale scope builds them.
/// @param tag The BCP 47 tag.
/// @return The facts.
inline FormatContext formatFor(std::string_view tag) {
    LocaleFacts const& facts = localeFacts(tag);
    return FormatContext{.locale = std::string{tag},
                         .numeric = facts.numeric,
                         .currency = facts.currency,
                         .datePattern = std::string{facts.datePattern},
                         .dateTimePattern = std::string{facts.dateTimePattern}};
}

/// @brief A value's JSON text.
/// @param value The value.
/// @return The text.
inline std::string valueToJsonText(Value const& value) { return glz::write_json(valueToJson(value)).value_or(""); }

/// @brief A source of @p n rows `{id: i, name: "row-<i>"}`, keyed by `id`.
class TestRowsSource final : public table::DataSource {
public:
    /// @brief The rows.
    /// @param n Rows.
    explicit TestRowsSource(std::size_t n)
        : _rows{{table::ColumnInfo{.id = "id", .kind = table::ColumnKind::Integer},
                 table::ColumnInfo{.id = "name", .kind = table::ColumnKind::Text}}} {
        std::vector<table::RowId> ids;
        std::vector<std::vector<table::Cell>> cells;
        for (std::size_t at = 0; at < n; ++at) {
            auto const key = static_cast<std::int64_t>(at);
            ids.emplace_back(key);
            cells.push_back({table::Cell{key}, table::Cell{"row-" + std::to_string(at)}});
        }
        _rows.setRows(std::move(ids), std::move(cells));
    }

    [[nodiscard]] std::span<table::ColumnInfo const> columns() const override { return _rows.columns(); }
    [[nodiscard]] std::shared_ptr<table::RowSnapshot const> snapshot() const override { return _rows.snapshot(); }
    void subscribe(table::ChangeListener& listener) override { _rows.subscribe(listener); }
    void unsubscribe(table::ChangeListener& listener) override { _rows.unsubscribe(listener); }

    /// @brief Renames a row; notifies `Updated` for it.
    /// @param row  The source row.
    /// @param name Its new name.
    void rename(std::size_t row, std::string name) {
        _rows.updateRow(row, {table::Cell{static_cast<std::int64_t>(row)}, table::Cell{std::move(name)}});
    }

    /// @brief What reads a row's key and record.
    /// @return `{id, name}` of the row.
    [[nodiscard]] static RecordOf recordOf() {
        return [](table::RowSnapshot const& snapshot, std::size_t row) {
            ValueRecord fields;
            table::Cell const id = snapshot.cell(row, 0);
            if (auto const* const number = std::get_if<std::int64_t>(&id)) {
                fields.insert_or_assign("id", Value{*number});
            }
            fields.insert_or_assign("name", Value{table::displayText(snapshot.cell(row, 1))});
            return RowRecord{.key = snapshot.rowId(row), .row = Value{std::move(fields)}};
        };
    }

private:
    table::VectorSource _rows;
};

/// @brief A session over a scripted dispatcher with no screen: what a `TableRows` test needs.
///
/// The session's catalog is `tableCatalog()`, its clock the manual scheduler's, its watches answered at once, and its
/// locale `en`, so counts are grouped as the locale table groups them.
struct RowsFixture {
    RowsFixture() { session.setLocale("en"); }

    morph::testing::StepExecutor owner;
    reactive::Runtime runtime{owner};
    reactive::testing::ManualScheduler scheduler;
    ScriptedDispatcher dispatcher{owner};
    /// @brief Every watch request the session sent, as `kind body`.
    std::vector<std::string> watches;
    Session session{runtime, SessionOptions{.scheduler = &scheduler,
                                            .clock = [this] { return scheduler.now(); },
                                            .keepUnused = std::chrono::minutes{5},
                                            .catalog = tableCatalog(),
                                            .classify = {},
                                            .developmentTools = DevelopmentTools::Off,
                                            .watcher = [this](std::string const& kind, std::string const& body) {
                                                watches.push_back(kind + " " + body);
                                                auto [completion, promise] =
                                                    async::Completion<std::string>::makeSettleable(&owner);
                                                promise.resolve(std::string{"{}"});
                                                return std::move(completion);
                                            }}};
    /// @brief The channel of `Lims_SampleModel`.
    std::shared_ptr<ModelChannel> channel = dispatcher.bind("Lims_SampleModel", bridge::BindSharing::Private, "");
    /// @brief The description the latest rows were built with.
    table::TableDescription described = samplesDescription(3);
    /// @brief The first three columns of `samplesDescription(3)`.
    std::vector<std::string> columns = columnIds(0, 3);

    /// @brief Runs everything posted to the owner.
    void settle() { owner.runAll(); }

    /// @brief Advances the clock, then settles.
    /// @param delta How far.
    void advance(std::chrono::milliseconds delta) {
        scheduler.advance(delta);
        settle();
    }

    /// @brief The options of rows over a table of `Lims_SampleModel`.
    /// @param table     The table's id.
    /// @param restoring Whether the rows wait for `release`.
    /// @return The options.
    [[nodiscard]] WindowRowsOptions options(std::string const& table, bool restoring = false) const {
        return WindowRowsOptions{.channel = channel,
                                 .table = table,
                                 .record = "Lims_SampleModel." + table,
                                 .body = {},
                                 .pageSize = 200,
                                 .maxPages = 8,
                                 .maxKeys = 1000,
                                 .maxColumns = 96,
                                 .restoring = restoring};
    }

    /// @brief Rows over a table, its watch answered, @p description set and released.
    /// @param table       The table's id.
    /// @param description The description.
    /// @return The rows.
    std::shared_ptr<WindowRows> windowRows(std::string const& table,
                                           table::TableDescription description = samplesDescription(3)) {
        auto rows = restoringRows(table, std::move(description));
        rows->release();
        settle();
        return rows;
    }

    /// @brief Rows over a table, its watch answered and @p description set, still restoring.
    /// @param table       The table's id.
    /// @param description The description.
    /// @return The rows.
    std::shared_ptr<WindowRows> restoringRows(std::string const& table,
                                              table::TableDescription description = samplesDescription(3)) {
        described = description;
        auto rows = std::make_shared<WindowRows>(session, options(table, true));
        settle();
        rows->setDescription(std::move(description));
        settle();
        return rows;
    }

    /// @brief The ids of the held description's columns, in order.
    /// @param first The first column.
    /// @param count Columns.
    /// @return The ids.
    [[nodiscard]] std::vector<std::string> columnIds(std::size_t first, std::size_t count) const {
        std::vector<std::string> out;
        for (std::size_t at = first; at < first + count && at < described.columns.size(); ++at) {
            out.push_back(described.columns[at].info.id);
        }
        return out;
    }

    /// @brief The page a call asks for.
    /// @param index The call.
    /// @return Its `query.page.offset`.
    [[nodiscard]] std::int64_t offsetOf(std::size_t index) const {
        auto const json = parseJson(dispatcher.calls.at(index).body);
        REQUIRE(json.has_value());
        auto const value = valueFromJson(*json);
        REQUIRE(value.has_value());
        auto const* const query = value->field("query");
        REQUIRE(query != nullptr);
        auto const* const page = query->field("page");
        REQUIRE(page != nullptr);
        auto const* const offset = page->field("offset");
        REQUIRE(offset != nullptr);
        REQUIRE(offset->asInt() != nullptr);
        return *offset->asInt();
    }

    /// @brief Shows rows 0 to 29 and answers every page fetch that causes, each with its own page of @p total.
    /// @param rows  The rows.
    /// @param total The view's total.
    void answerFirstPage(WindowRows& rows, std::int64_t total) {
        rows.visible(0, 30, columns);
        settle();
        for (int round = 0; round < 8 && pending("Table.samples") > 0; ++round) {
            auto const index = oldest("Table.samples");
            dispatcher.reply(index, pageJson(offsetOf(index), 200, total, columns));
            settle();
        }
        REQUIRE(pending("Table.samples") == 0);
    }

    /// @brief The oldest unanswered call of an action.
    /// @param action The action.
    /// @return Its index.
    [[nodiscard]] std::size_t oldest(std::string_view action) const {
        for (std::size_t index = 0; index < dispatcher.calls.size(); ++index) {
            if (dispatcher.calls[index].action == action && !dispatcher.calls[index].answered) {
                return index;
            }
        }
        FAIL("no unanswered call of " << action);
        return 0;
    }

    /// @brief Answers the oldest unanswered `Table.samples` call, then settles.
    /// @param json The reply.
    void replyPage(std::string json) {
        dispatcher.reply(oldest("Table.samples"), std::move(json));
        settle();
    }

    /// @brief The unanswered calls of an action.
    /// @param action The action.
    /// @return How many.
    [[nodiscard]] std::size_t pending(std::string_view action) const {
        std::size_t out = 0;
        for (auto const& call : dispatcher.calls) {
            out += call.action == action && !call.answered ? 1U : 0U;
        }
        return out;
    }

    /// @brief Whether a call's stop was requested.
    /// @param index The call.
    /// @return `true` once stopped.
    [[nodiscard]] bool stopped(std::size_t index) const {
        return dispatcher.calls.at(index).options.stop.stop_requested();
    }

    /// @brief Delivers a change of `Lims_SampleModel.samples` with @p keys to the session, as a `changes` event
    ///        does, then settles.
    /// @param keys The changed rows' keys.
    void change(std::vector<std::int64_t> const& keys) {
        std::vector<glz::raw_json> raw;
        raw.reserve(keys.size());
        for (auto const key : keys) {
            raw.emplace_back(std::to_string(key));
        }
        wire::ChangesBody body;
        body.records.push_back(effects::RecordChange{.record = "Lims_SampleModel.samples", .keys = std::move(raw)});
        session.applyChanges(body);
        settle();
    }

    /// @brief The rows' window.
    /// @param rows The rows.
    /// @return Its window.
    [[nodiscard]] static table::PageWindow<table::GenericRow> const& window(WindowRows const& rows) {
        return WindowRowsProbe::window(rows);
    }

    /// @brief The keys of a range, which must be delivered by one settle and make no `Table.samples` call.
    /// @param rows  The rows.
    /// @param first The first view row.
    /// @param count Rows.
    /// @return The keys.
    std::vector<table::RowId> keysNow(TableRows& rows, std::int64_t first, std::int64_t count) {
        auto const calls = dispatcher.count("Table.samples");
        std::optional<std::expected<std::vector<table::RowId>, table::TableError>> got;
        auto keys = rows.keysOf(first, count);
        keys.then([&got](std::expected<std::vector<table::RowId>, table::TableError> const& result) { got = result; });
        settle();
        if (!got.has_value()) {
            FAIL("keysOf did not complete within one settle");
        }
        if (dispatcher.count("Table.samples") != calls) {
            FAIL("keysOf sent a fetch");
        }
        if (!got->has_value()) {
            FAIL("keysOf failed: " << got->error().message);
        }
        return **got;
    }
};

/// @brief A preferences store that keeps its entries in memory and lets a test list, read, erase and fail them, which
///        the table-state model, reaching the store by exact key alone, cannot.
///
/// Each call is a task on its own strand, as `MemoryPreferencesStore`'s are. A failed `set` or `remove` answers
/// `false` and changes nothing.
class ProbePreferencesStore final : public IPreferencesStore {
public:
    /// @brief A store whose strand runs on @p base.
    /// @param base Where the strand's tasks run. Borrowed.
    explicit ProbePreferencesStore(exec::IExecutor& base) : _strand{base} {}

    [[nodiscard]] async::Completion<std::optional<std::string>> get(PrefKey const& key) override {
        auto [completion, promise] = async::Completion<std::optional<std::string>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            std::scoped_lock const lock{_mutex};
            auto const found = _entries.find(entry);
            settle.resolve(found == _entries.end() ? std::nullopt : std::optional<std::string>{found->second});
        });
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<bool> set(PrefKey const& key, std::string json) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), text = std::move(json), settle = std::move(promise)] mutable {
            std::scoped_lock const lock{_mutex};
            ++_sets;
            if (_failSetsAfter && _sets > *_failSetsAfter) {
                settle.resolve(false);
                return;
            }
            _largest = std::max(_largest, text.size());
            _entries.insert_or_assign(std::move(entry), std::move(text));
            settle.resolve(true);
        });
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<bool> remove(PrefKey const& key) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            std::scoped_lock const lock{_mutex};
            ++_removes;
            if (_failRemovesAfter && _removes > *_failRemovesAfter) {
                settle.resolve(false);
                return;
            }
            settle.resolve(_entries.erase(entry) != 0);
        });
        return std::move(completion);
    }

    /// @brief Whether an entry is stored under @p key.
    /// @param key The key.
    /// @return `true` when one is.
    [[nodiscard]] bool has(PrefKey const& key) const {
        std::scoped_lock const lock{_mutex};
        return _entries.contains(tie(key));
    }

    /// @brief The entry under @p key.
    /// @param key The key.
    /// @return Its text; empty when none is stored.
    [[nodiscard]] std::string value(PrefKey const& key) const {
        std::scoped_lock const lock{_mutex};
        auto const found = _entries.find(tie(key));
        return found == _entries.end() ? std::string{} : found->second;
    }

    /// @brief Forgets the entry under @p key, as no caller of the store could; not counted as a `remove`.
    /// @param key The key.
    void erase(PrefKey const& key) {
        std::scoped_lock const lock{_mutex};
        _entries.erase(tie(key));
    }

    /// @brief The points of every stored entry with @p scope, of any person and application.
    /// @param scope The scope.
    /// @return The points.
    [[nodiscard]] std::set<std::string> pointsUnder(std::string const& scope) const {
        std::scoped_lock const lock{_mutex};
        std::set<std::string> out;
        for (auto const& entry : _entries) {
            if (std::get<2>(entry.first) == scope) {
                out.insert(std::get<3>(entry.first));
            }
        }
        return out;
    }

    /// @brief The longest value any `set` stored.
    /// @return Its length in bytes.
    [[nodiscard]] std::size_t largestValue() const {
        std::scoped_lock const lock{_mutex};
        return _largest;
    }

    /// @brief The `set` calls so far, failed ones included.
    /// @return How many.
    [[nodiscard]] std::size_t setCount() const {
        std::scoped_lock const lock{_mutex};
        return _sets;
    }

    /// @brief The `remove` calls so far, failed ones included.
    /// @return How many.
    [[nodiscard]] std::size_t removeCount() const {
        std::scoped_lock const lock{_mutex};
        return _removes;
    }

    /// @brief Fails every `set` after the @p n-th since the store was made.
    /// @param n The last `set` that still succeeds.
    void failSetsAfter(std::size_t n) {
        std::scoped_lock const lock{_mutex};
        _failSetsAfter = n;
    }

    /// @brief Fails every `remove` after the @p n-th since the store was made.
    /// @param n The last `remove` that still succeeds.
    void failRemovesAfter(std::size_t n) {
        std::scoped_lock const lock{_mutex};
        _failRemovesAfter = n;
    }

    /// @brief Lets every `set` succeed again.
    void healSets() {
        std::scoped_lock const lock{_mutex};
        _failSetsAfter.reset();
    }

    /// @brief Lets every `remove` succeed again.
    void healRemoves() {
        std::scoped_lock const lock{_mutex};
        _failRemovesAfter.reset();
    }

private:
    using Key = std::tuple<std::string, std::string, std::string, std::string>;

    [[nodiscard]] static Key tie(PrefKey const& key) { return Key{key.person, key.app, key.scope, key.point}; }

    mutable std::mutex _mutex;
    std::map<Key, std::string> _entries;
    std::size_t _largest = 0;
    std::size_t _sets = 0;
    std::size_t _removes = 0;
    std::optional<std::size_t> _failSetsAfter;
    std::optional<std::size_t> _failRemovesAfter;
    /// After the map, so it is destroyed first: a task running on it reads the map.
    exec::OwnerStrand _strand;
};

/// @brief A table-state store holding one document whose loads wait for `release()` or `fail()`; saves are answered
///        at once, and kept as what later loads read.
class GatedTableStateStore final : public table::ITableStateStore {
public:
    /// @brief A store holding @p json, delivering on @p executor.
    /// @param executor Where completions are delivered. Borrowed.
    /// @param json     The stored document; empty for none.
    GatedTableStateStore(exec::IExecutor& executor, std::string json) : _executor{&executor} {
        if (!json.empty()) {
            _stored = table::StoredTableState{.json = std::move(json), .revision = "1"};
        }
    }

    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& key) override {
        auto [completion, promise] =
            async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_executor);
        ++_loads;
        _lastLoadKey = key;
        if (_released) {
            promise.resolve(_stored);
        } else {
            _held.push_back(std::move(promise));
        }
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<table::SaveResult> save(table::TableStateKey const& /*key*/, std::string json,
                                                            std::string /*expected*/ = {}) override {
        auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_executor);
        ++_saves;
        _lastSaved = json;
        if (_refusal) {
            promise.resolve(
                table::SaveResult{.outcome = table::SaveOutcome::Refused, .revision = {}, .reason = *_refusal});
            return std::move(completion);
        }
        _stored = table::StoredTableState{.json = std::move(json), .revision = std::to_string(++_revision)};
        promise.resolve(
            table::SaveResult{.outcome = table::SaveOutcome::Saved, .revision = _stored->revision, .reason = {}});
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<bool> remove(table::TableStateKey const& /*key*/) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(_executor);
        promise.resolve(std::exchange(_stored, std::nullopt).has_value());
        return std::move(completion);
    }

    /// @brief Answers every held load with the stored document, and every later load at once.
    void release() {
        _released = true;
        for (auto& promise : std::exchange(_held, {})) {
            promise.resolve(_stored);
        }
    }

    /// @brief Fails every held load.
    void fail() {
        for (auto& promise : std::exchange(_held, {})) {
            promise.reject(std::make_exception_ptr(std::runtime_error{"the state store is unavailable"}));
        }
    }

    /// @brief Answers every later save `Refused` with @p reason.
    /// @param reason The reason.
    void refuseSaves(std::string reason) { _refusal = std::move(reason); }

    /// @brief The loads asked for.
    /// @return How many.
    [[nodiscard]] std::size_t loads() const { return _loads; }

    /// @brief The saves asked for.
    /// @return How many.
    [[nodiscard]] std::size_t saves() const { return _saves; }

    /// @brief The document the last save carried.
    /// @return Its text.
    [[nodiscard]] std::string const& lastSaved() const { return _lastSaved; }

    /// @brief The key of the last load.
    /// @return It.
    [[nodiscard]] table::TableStateKey const& lastLoadKey() const { return _lastLoadKey; }

private:
    exec::IExecutor* _executor;
    std::optional<table::StoredTableState> _stored;
    std::vector<async::Completion<std::optional<table::StoredTableState>>::Promise> _held;
    bool _released = false;
    std::optional<std::string> _refusal;
    std::size_t _loads = 0;
    std::size_t _saves = 0;
    std::uint64_t _revision = 1;
    std::string _lastSaved;
    table::TableStateKey _lastLoadKey;
};

/// @brief A non-owning `std::shared_ptr` to @p store, for an API that shares its store.
/// @param store The store, which must outlive every copy.
/// @return The pointer.
inline std::shared_ptr<table::ITableStateStore> sharedOf(table::ITableStateStore& store) {
    return {std::shared_ptr<void>{}, &store};
}

}  // namespace morph::ui::testing
