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
#include <morph/core/action_refusal.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/wire.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/preferences.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/testing/recording_backend.hpp>
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
///        `TableRows`, the stores table-state tests look into, and `TableHarness`, which mounts whole documents with
///        a model-backed table on a recording backend.

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
///        `Lims_SampleModel.samples` opaquely, and `Table.samples.describe`; no other table's actions.
/// @return The catalog.
inline Catalog tableCatalog() {
    Catalog catalog = corpusCatalog();
    auto const base = catalog.action;
    catalog.action = [base](std::string_view action) -> std::optional<ActionInfo> {
        if (action.starts_with("Table.") && action != "Table.samples" && action != "Table.samples.describe") {
            return std::nullopt;
        }
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

/// @brief The description of `samples`: `id` (the Integer key), `name` (Text), then `c3` … Decimal, each labelled as
///        a server labels a column from its id (`Id`, `Name`, `C3`).
/// @param columns The columns, at least 2.
/// @return The description, its revision set.
inline table::TableDescription samplesDescription(int columns) {
    table::TableDescription description;
    description.id = "samples";
    description.label = "Samples";
    description.key = "id";
    auto const add = [&description](std::string const& id, table::ColumnKind kind) {
        table::TableColumn column;
        column.info = table::ColumnInfo{.id = id, .kind = kind};
        column.label = forms::detail::inferTitle(id);
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
/// @return The page.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a page's numbers, in the order a page names them.
inline table::GenericPage pageAt(std::string revision, std::int64_t offset, std::int64_t count, std::int64_t total,
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
    return page;
}

/// @brief A page of `samples` as `Table.samples` answers, against a given revision; see `pageAt`.
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
                              std::vector<std::string> const& columns, table::TotalKind kind = table::TotalKind::Exact,
                              std::int64_t undecided = 0, std::vector<std::string> awaiting = {}) {
    return glz::write_json(
               pageAt(std::move(revision), offset, count, total, columns, kind, undecided, std::move(awaiting)))
        .value_or("");
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
                             std::vector<std::string> const& columns);

/// @brief A keyed reply of `samples` against @p revision; see `keyedJson`.
/// @param revision The description revision the reply names.
/// @param rows     The rows, as key and view row, in view order.
/// @param total    The view's total.
/// @param columns  The columns.
/// @return The reply's JSON.
inline std::string keyedJsonAt(std::string revision, std::vector<std::pair<std::int64_t, std::int64_t>> const& rows,
                               std::int64_t total, std::vector<std::string> const& columns) {
    table::GenericPage page;
    page.revision = std::move(revision);
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

inline std::string keyedJson(std::vector<std::pair<std::int64_t, std::int64_t>> const& rows, std::int64_t total,
                             std::vector<std::string> const& columns) {
    return keyedJsonAt(samplesDescription(3).revision, rows, total, columns);
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
                                            .watcher =
                                                [this](std::string const& kind, std::string const& body) {
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
                                 .action = {},
                                 .queryMember = {},
                                 .inferKinds = {},
                                 .records = {"Lims_SampleModel." + table},
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

protected:
    /// @brief A fixture whose session reads @p catalog and keeps table state in @p store, for application `lims`.
    /// @param catalog The session's catalog.
    /// @param store   The table-state store; null for none.
    RowsFixture(Catalog catalog, std::shared_ptr<table::ITableStateStore> store)
        : session{runtime, SessionOptions{.scheduler = &scheduler,
                                          .clock = [this] { return scheduler.now(); },
                                          .keepUnused = std::chrono::minutes{5},
                                          .catalog = std::move(catalog),
                                          .classify = {},
                                          .developmentTools = DevelopmentTools::Off,
                                          .watcher =
                                              [this](std::string const& kind, std::string const& body) {
                                                  watches.push_back(kind + " " + body);
                                                  auto [completion, promise] =
                                                      async::Completion<std::string>::makeSettleable(&owner);
                                                  promise.resolve(std::string{"{}"});
                                                  return std::move(completion);
                                              },
                                          .maxInlineFileBytes = kDefaultInlineFileBytes,
                                          .tableSettle = std::chrono::milliseconds{80},
                                          .tableAwaitedRefetch = std::chrono::milliseconds{250},
                                          .tableState = std::move(store),
                                          .tableStateTimeout = std::chrono::milliseconds{300},
                                          .tableStateDebounce = std::chrono::milliseconds{500},
                                          .application = "lims"}} {
        session.setLocale("en");
    }

public:
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

    /// @brief How often the runtime reported a misuse at @p site since the first call, which installs the recorder
    ///        that counts them in place of a debug build's assertion.
    /// @param site One of the `site::` constants.
    /// @return The count; 0 on the first call.
    std::size_t reported(std::string_view site) {
        if (!probe) {
            probe.emplace(owner.coreExecutor());
        }
        return probe->count(site);
    }

    /// @brief The recorder `reported` installs; last, so it is uninstalled first.
    std::optional<morph::testing::OwnerProbeRecorder> probe;
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
        note("get", key);
        auto [completion, promise] = async::Completion<std::optional<std::string>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            std::scoped_lock const lock{_mutex};
            if (++_gets == _holdGet) {
                _heldGet.emplace(HeldGet{.entry = std::move(entry), .settle = std::move(settle)});
                return;
            }
            auto const found = _entries.find(entry);
            settle.resolve(found == _entries.end() ? std::nullopt : std::optional<std::string>{found->second});
        });
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<bool> set(PrefKey const& key, std::string json) override {
        note("set", key);
        auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), text = std::move(json), settle = std::move(promise)] mutable {
            std::scoped_lock const lock{_mutex};
            ++_sets;
            if (_failSetsAfter && _sets > *_failSetsAfter) {
                settle.resolve(false);
                return;
            }
            if (_holdSet == _sets) {
                _held.emplace(Held{.entry = std::move(entry), .text = std::move(text), .settle = std::move(settle)});
                return;
            }
            _largest = std::max(_largest, text.size());
            _entries.insert_or_assign(std::move(entry), std::move(text));
            settle.resolve(true);
        });
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<bool> remove(PrefKey const& key) override {
        note("remove", key);
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

    /// @brief Holds the @p n-th `set` since the store was made, unanswered and unwritten, until `releaseHeld`.
    /// @param n The `set` to hold.
    void holdSet(std::size_t n) {
        std::scoped_lock const lock{_mutex};
        _holdSet = n;
    }

    /// @brief Whether a `set` is held.
    /// @return `true` while one is.
    [[nodiscard]] bool holding() const {
        std::scoped_lock const lock{_mutex};
        return _held.has_value();
    }

    /// @brief Writes and answers the held `set`.
    void releaseHeld() {
        std::scoped_lock const lock{_mutex};
        REQUIRE(_held.has_value());
        _largest = std::max(_largest, _held->text.size());
        _entries.insert_or_assign(std::move(_held->entry), std::move(_held->text));
        _held->settle.resolve(true);
        _held.reset();
        _holdSet = 0;
    }

    /// @brief The `get` calls answered or held so far.
    /// @return How many.
    [[nodiscard]] std::size_t getCount() const {
        std::scoped_lock const lock{_mutex};
        return _gets;
    }

    /// @brief Holds the @p n-th `get` since the store was made until `releaseHeldGet`, which reads the entry then.
    /// @param n The `get` to hold.
    void holdGet(std::size_t n) {
        std::scoped_lock const lock{_mutex};
        _holdGet = n;
    }

    /// @brief Whether a `get` is held.
    /// @return `true` while one is.
    [[nodiscard]] bool holdingGet() const {
        std::scoped_lock const lock{_mutex};
        return _heldGet.has_value();
    }

    /// @brief Answers the held `get` with what is stored now.
    void releaseHeldGet() {
        std::scoped_lock const lock{_mutex};
        REQUIRE(_heldGet.has_value());
        auto const found = _entries.find(_heldGet->entry);
        _heldGet->settle.resolve(found == _entries.end() ? std::nullopt : std::optional<std::string>{found->second});
        _heldGet.reset();
        _holdGet = 0;
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

    /// @brief Every call so far, as `verb person point`, the person in quotes so an empty one shows.
    /// @return The calls, in order.
    [[nodiscard]] std::vector<std::string> calls() const {
        std::scoped_lock const lock{_mutex};
        return _calls;
    }

private:
    using Key = std::tuple<std::string, std::string, std::string, std::string>;

    [[nodiscard]] static Key tie(PrefKey const& key) { return Key{key.person, key.app, key.scope, key.point}; }

    void note(std::string_view verb, PrefKey const& key) {
        std::scoped_lock const lock{_mutex};
        _calls.push_back(std::string{verb} + " \"" + key.person + "\" " + key.point);
    }

    mutable std::mutex _mutex;
    std::vector<std::string> _calls;
    std::map<Key, std::string> _entries;
    std::size_t _largest = 0;
    std::size_t _sets = 0;
    std::size_t _removes = 0;
    std::optional<std::size_t> _failSetsAfter;
    std::optional<std::size_t> _failRemovesAfter;
    /// @brief A held `set`.
    struct Held {
        Key entry;
        std::string text;
        async::Completion<bool>::Promise settle;
    };
    std::size_t _holdSet = 0;
    std::optional<Held> _held;
    /// @brief A held `get`.
    struct HeldGet {
        Key entry;
        async::Completion<std::optional<std::string>>::Promise settle;
    };
    std::size_t _gets = 0;
    std::size_t _holdGet = 0;
    std::optional<HeldGet> _heldGet;
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
        if (_holdingSaves) {
            _heldSaves.push_back(HeldSave{.json = std::move(json), .promise = std::move(promise)});
            return std::move(completion);
        }
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

    [[nodiscard]] async::Completion<table::SaveResult> publish(table::TableStateKey const& /*key*/,
                                                               table::ViewOperation const& /*operation*/,
                                                               std::string /*expected*/) override {
        return refusePublish(*_executor);
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

    /// @brief Holds every later save, unanswered and unstored, until `releaseSave`.
    void holdSaves() { _holdingSaves = true; }

    /// @brief Stores and answers the oldest held save.
    void releaseSave() {
        REQUIRE_FALSE(_heldSaves.empty());
        HeldSave held = std::move(_heldSaves.front());
        _heldSaves.erase(_heldSaves.begin());
        _stored = table::StoredTableState{.json = std::move(held.json), .revision = std::to_string(++_revision)};
        held.promise.resolve(
            table::SaveResult{.outcome = table::SaveOutcome::Saved, .revision = _stored->revision, .reason = {}});
    }

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
    struct HeldSave {
        std::string json;
        async::Completion<table::SaveResult>::Promise promise;
    };
    bool _holdingSaves = false;
    std::vector<HeldSave> _heldSaves;
};

// ── A screen with a model-backed table ──────────────────────────────────────

/// @brief Reaches into a `Screen` for what a test checks and the screen does not show.
struct ScreenProbe {
    /// @brief The mount's counters.
    /// @param screen The screen.
    /// @return `Mounted::stats()`.
    [[nodiscard]] static MountStats const& stats(Screen const& screen) { return screen._mounted->stats(); }

    /// @brief The screen's core.
    /// @param screen The screen.
    /// @return It.
    [[nodiscard]] static morph::ui::detail::ScreenCore& core(Screen& screen) { return *screen._core; }

    /// @brief An element of the screen's own scope.
    /// @param screen The screen.
    /// @param id     The element's id.
    /// @return Its state.
    [[nodiscard]] static morph::ui::detail::ElementState& element(Screen& screen, std::string_view id) {
        morph::ui::detail::NameRef const* const found = screen._scope->find(id);
        REQUIRE(found != nullptr);
        REQUIRE(found->element != nullptr);
        return *found->element;
    }
};

/// @brief Where a test's table-state store delivers: a step executor `TableHarness::settle` runs too, there before
///        the harness so a store can be built first.
/// @return The executor, one for the process.
inline morph::testing::StepExecutor& storeExecutor() {
    static morph::testing::StepExecutor executor;
    return executor;
}

/// @brief The actions a test adds to `TableHarness`'s catalog: action id, then its request schema's JSON text and
///        its `ActionInfo`. A base of the harness, so the catalog it builds can read them.
struct ExtraActions {
    /// @brief The actions, by id.
    std::map<std::string, std::pair<std::string, ActionInfo>, std::less<>> extraActions;
    /// @brief The view documents of actions, by action id: looked up before the table catalog's.
    std::map<std::string, std::string, std::less<>> extraViews;

    /// @brief A catalog that looks in `extraActions` first and in `tableCatalog()` after.
    /// @return The catalog; it reads this object, which must outlive it.
    [[nodiscard]] Catalog catalog() const {
        Catalog out = tableCatalog();
        out.action = [this, base = out.action](std::string_view id) -> std::optional<ActionInfo> {
            if (auto const found = extraActions.find(id); found != extraActions.end()) {
                return found->second.second;
            }
            return base ? base(id) : std::nullopt;
        };
        out.schema = [this, base = out.schema](std::string_view id) -> std::optional<std::string> {
            if (auto const found = extraActions.find(id); found != extraActions.end()) {
                return found->second.first;
            }
            return base ? base(id) : std::nullopt;
        };
        out.view = [this, base = out.view](std::string_view id) -> std::optional<std::string> {
            if (auto const found = extraViews.find(id); found != extraViews.end()) {
                return found->second;
            }
            return base ? base(id) : std::nullopt;
        };
        return out;
    }
};

/// @brief A host that keeps every `notify`, as `(text, role)`.
class NotifyingHost final : public InterpreterHost {
public:
    /// @brief Every message notified, oldest first.
    std::vector<std::pair<std::string, std::string>> notified;

    /// @brief Keeps the message.
    /// @param text The message.
    /// @param role Its role.
    /// @return `true`: shown.
    bool notify(std::string const& text, std::string const& role) override {
        notified.emplace_back(text, role);
        return true;
    }
};

/// @brief A screen with a model-backed table over `Table.samples`: a `RowsFixture` with an interpreter, a
///        `NotifyingHost` and a `RecordingBackend`.
///
/// The session's catalog, and the one `mount` loads against, look in `extraActions` first, so an action a test adds
/// before `mount` is known to both. The session's locale is `en`.
// NOLINTNEXTLINE(misc-multiple-inheritance): the harness adds the extra actions to the rows fixture; both are test helpers that share no state
struct TableHarness : ExtraActions, RowsFixture {
    /// @brief A harness whose session keeps no table state.
    TableHarness() : TableHarness(nullptr) {}

    /// @brief A harness whose session keeps table state in @p store.
    /// @param store The store; null for none.
    explicit TableHarness(std::shared_ptr<table::ITableStateStore> store)
        : RowsFixture{ExtraActions::catalog(), std::move(store)} {}

    /// @brief The host.
    NotifyingHost host;
    /// @brief The backend.
    RecordingBackend backend;
    /// @brief Every diagnostic reported, oldest first.
    std::vector<Refusal> diagnostics;
    /// @brief The screen `mount` mounted last, which `elementValue(h, …)` reads; the test owns it.
    Screen* mounted = nullptr;
    /// @brief The action each mutation of the document mounted last runs, by the mutation's name.
    std::map<std::string, std::string, std::less<>> mutationActions;
    /// @brief The interpreter.
    Interpreter interpreter{session, dispatcher,
                            InterpreterOptions{.host = &host, .diagnostics = [this](Refusal const& refusal) {
                                                   diagnostics.push_back(refusal);
                                               }}};

    /// @brief Loads and mounts a document, then settles.
    /// @param text    The document.
    /// @param params  The params.
    /// @param options The mount options.
    /// @return The screen.
    std::unique_ptr<Screen> mount(std::string_view text, ValueRecord const& params = {},
                                  MountOptions const& options = {}) {
        auto screen = interpreter.mount(loadOrFail(text, session.options().catalog), params, backend, options);
        if (!screen) {
            FAIL(screen.error().message());
        }
        mounted = screen->get();
        settle();
        return std::move(*screen);
    }

    /// @brief Runs everything posted to the owner and to `storeExecutor()` until both are idle.
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): the harness's `settle` also drains the store's executor, which the fixture's does not know of
    void settle() {
        for (int round = 0; round < 100; ++round) {
            std::size_t const ran = owner.runAll() + storeExecutor().runAll();
            if (ran == 0) {
                return;
            }
        }
        FAIL("the owner and the store did not settle");
    }

    /// @brief Advances the clock, then settles.
    /// @param delta How far.
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): the harness's `advance` settles through its own `settle`, which also drains the store's executor
    void advance(std::chrono::milliseconds delta) {
        scheduler.advance(delta);
        settle();
    }

    /// @brief Answers a call, then settles.
    /// @param index The call.
    /// @param json  The reply.
    void reply(std::size_t index, std::string json) {
        dispatcher.reply(index, std::move(json));
        settle();
    }

    /// @brief Fails a call, then settles.
    /// @param index The call.
    /// @param kind  The error kind.
    void fail(std::size_t index, std::string kind = "error") {
        dispatcher.fail(index, std::move(kind));
        settle();
    }

    /// @brief The first widget of a kind whose property has a value.
    /// @param kind  The kind.
    /// @param name  The property.
    /// @param value The value.
    /// @return Its id.
    int find(std::string_view kind, std::string_view name, std::string_view value) const {
        auto const found = backend.find(kind, name, value);
        if (!found) {
            FAIL("no " << kind << " with " << name << "=" << value << " in\n" << backend.dump());
        }
        return *found;
    }

    /// @brief The widget whose test id is @p testId.
    /// @param kind   The kind.
    /// @param testId The test id.
    /// @return Its id.
    int byTestId(std::string_view kind, std::string_view testId) const { return find(kind, "testId", testId); }

    /// @brief A text widget's text.
    /// @param testId The text widget's test id.
    /// @return Its text.
    std::string text(std::string_view testId) const { return backend.prop(byTestId("Text", testId), "text"); }
};

/// @brief Adds an action to the harness's catalog; before `mount`, so the loader and the session both know it.
/// @param h          The harness.
/// @param id         The action's id.
/// @param schemaJson Its request schema's JSON text.
/// @param info       Its catalog entry.
inline void addAction(TableHarness& h, std::string id, std::string schemaJson, ActionInfo info) {
    h.extraActions.insert_or_assign(std::move(id), std::pair{std::move(schemaJson), std::move(info)});
}

/// @brief A document value from JSON text.
/// @param json The text.
/// @return The value; the text must read.
inline Value value(std::string_view json) {
    auto parsed = parseJson(json);
    REQUIRE(parsed.has_value());
    auto read = valueFromJson(*parsed);
    REQUIRE(read.has_value());
    return std::move(*read);
}

/// @brief A record change event of the session, then settles.
/// @param h      The harness.
/// @param record The record.
/// @param keys   The changed keys; empty for a change of the whole record.
inline void fireEvent(TableHarness& h, std::string record, std::vector<std::int64_t> const& keys) {
    effects::RecordChange change{.record = std::move(record), .keys = std::nullopt};
    if (!keys.empty()) {
        std::vector<glz::raw_json> raw;
        for (auto const key : keys) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `raw` is a fixture list built once; sizing it first would not change what the case checks
            raw.emplace_back(std::to_string(key));
        }
        change.keys = std::move(raw);
    }
    wire::ChangesBody body;
    body.records.push_back(std::move(change));
    h.session.applyChanges(body);
    h.settle();
}

/// @brief Runs a mutation of the document through its button `run-<name>`, then settles.
/// @param h    The harness.
/// @param name The mutation's name.
inline void runMutation(TableHarness& h, std::string_view name) {
    h.backend.click(h.byTestId("Button", "run-" + std::string{name}));
    h.settle();
}

/// @brief The latest call of a mutation's action.
/// @param h    The harness.
/// @param name The mutation's name.
/// @return The call's index.
inline std::size_t mutationCall(TableHarness const& h, std::string_view name) {
    auto const found = h.mutationActions.find(name);
    REQUIRE(found != h.mutationActions.end());
    return h.dispatcher.last(found->second);
}

/// @brief Answers a mutation's latest call with @p replyJson, then settles.
/// @param h         The harness.
/// @param name      The mutation's name.
/// @param replyJson The reply.
inline void settleMutation(TableHarness& h, std::string_view name, std::string replyJson) {
    h.reply(mutationCall(h, name), std::move(replyJson));
}

/// @brief Refuses a mutation's latest call with an `ActionRefusal` of @p code, then settles.
/// @param h    The harness.
/// @param name The mutation's name.
/// @param code The refusal's code.
inline void refuseMutation(TableHarness& h, std::string_view name, std::string code) {
    auto& call = h.dispatcher.calls.at(mutationCall(h, name));
    REQUIRE_FALSE(call.answered);
    call.answered = true;
    call.promise.reject(std::make_exception_ptr(model::ActionRefusal{std::move(code), {}, "refused"}));
    h.settle();
}

/// @brief The test document: a table `list` of the table `samples` of the model alias `lab`, with `empty` content.
inline constexpr std::string_view kModelTable =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","empty":[{"kind":"text","text":"No samples"}]}})";

/// @brief A stored state: the sort `name` descending and a filter on `name` containing `a`.
inline constexpr std::string_view kStored =
    R"({"version":1,"sort":[{"column":"name","direction":"desc"}],"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}}})";

/// @brief A document's text with the table node's member @p member, written `"key":value`, replacing the member of
///        that name or added.
/// @param text   The document.
/// @param member The member.
/// @return The document.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `withTableKey(text, member)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
inline std::string withTableKey(std::string_view text, std::string_view member) {
    auto json = parseJson(text);
    REQUIRE(json.has_value());
    auto field = parseJson("{" + std::string{member} + "}");
    REQUIRE(field.has_value());
    auto& view = std::get<Json::object_t>(std::get<Json::object_t>(json->data).at("view").data);
    for (auto& [key, value] : std::get<Json::object_t>(field->data)) {
        view.insert_or_assign(key, std::move(value));
    }
    return glz::write_json(*json).value_or("");
}

/// @brief `kModelTable` sorted by the state `order`, bound: `id` ascending.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kBoundSort` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
inline std::string const kBoundSort =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
    R"("state":{"order":{"init":{"lit":[{"column":"id","direction":"asc"}]}}},)"
    R"("view":{"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","sort":{"ref":"order"},)"
    R"("empty":[{"kind":"text","text":"No samples"}]}})";

/// @brief `kModelTable` with `"persist": false`.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kNoPersist` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
inline std::string const kNoPersist =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
    R"("view":{"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","persist":false,)"
    R"("empty":[{"kind":"text","text":"No samples"}]}})";

/// @brief `kModelTable` with `"status": false`.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kNoStatus` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
inline std::string const kNoStatus =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
    R"("view":{"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","status":false,)"
    R"("empty":[{"kind":"text","text":"No samples"}]}})";

/// @brief The table widget of the test document.
/// @param h The harness.
/// @return Its id.
inline int tableOf(TableHarness const& h) { return h.byTestId("Table", "list"); }

/// @brief The JSON text of an element value of the screen mounted last.
/// @param screen The screen.
/// @param id     The element's id.
/// @param name   The value's name.
/// @return `readJson` of `{"ref": "<id>.<name>"}`.
inline std::string elementValue(Screen const& screen, std::string_view id, std::string_view name) {
    return readJson(screen, R"({"ref":")" + std::string{id} + "." + std::string{name} + R"("})");
}

/// @brief An element value of the screen the harness mounted last.
/// @param h    The harness.
/// @param id   The element's id.
/// @param name The value's name.
/// @return The value.
inline Value elementValue(TableHarness const& h, std::string_view id, std::string_view name) {
    REQUIRE(h.mounted != nullptr);
    return value(elementValue(*h.mounted, id, name));
}

/// @brief The oldest call of an action neither answered nor stopped.
/// @param h      The harness.
/// @param action The action.
/// @return Its index.
inline std::size_t oldestOpen(TableHarness const& h, std::string_view action) {
    for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
        auto const& call = h.dispatcher.calls[index];
        if (call.action == action && !call.answered && !call.options.stop.stop_requested()) {
            return index;
        }
    }
    FAIL("no open call of " << action);
    return 0;
}

/// @brief The open calls of an action: neither answered nor stopped.
/// @param h      The harness.
/// @param action The action.
/// @return How many.
inline std::size_t openCalls(TableHarness const& h, std::string_view action) {
    std::size_t out = 0;
    for (auto const& call : h.dispatcher.calls) {
        out += call.action == action && !call.answered && !call.options.stop.stop_requested() ? 1U : 0U;
    }
    return out;
}

/// @brief Answers the open `Table.samples.describe` call with @p description, then settles.
/// @param h           The harness.
/// @param description The description.
inline void replyDescribe(TableHarness& h, table::TableDescription const& description = samplesDescription(3)) {
    h.described = description;
    h.columns = h.columnIds(0, 3);
    h.reply(oldestOpen(h, "Table.samples.describe"), describeJson(table::describe(description)));
}

/// @brief Answers the oldest open `Table.samples` call, then settles.
/// @param h    The harness.
/// @param json The reply.
inline void replyPage(TableHarness& h, std::string json) { h.reply(oldestOpen(h, "Table.samples"), std::move(json)); }

/// @brief The query a `Table.samples` call carries.
/// @param h     The harness.
/// @param index The call.
/// @return The `TableQuery` read from the body's `query`.
inline table::TableQuery queryOf(TableHarness const& h, std::size_t index) {
    struct Body {
        table::TableQuery query;
    };
    Body body;
    auto const failed = glz::read<glz::opts{.error_on_unknown_keys = false}>(body, h.dispatcher.calls.at(index).body);
    if (failed) {
        FAIL("the call's body does not read: " << glz::format_error(failed, h.dispatcher.calls.at(index).body));
    }
    return body.query;
}

/// @brief The query the latest `Table.samples` call carries.
/// @param h The harness.
/// @return It.
inline table::TableQuery lastQuery(TableHarness const& h) { return queryOf(h, h.dispatcher.last("Table.samples")); }

/// @brief Answers every open `Table.samples` call with its own page of @p total rows, then settles, until none is
///        open; each reply holds `h.columns`.
/// @param h         The harness.
/// @param total     The view's total.
/// @param kind      Whether the total is exact.
/// @param undecided Undecided rows.
/// @param awaiting  The order's awaited levels.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a page's numbers, in the order a page names them.
inline void answerPages(TableHarness& h, std::int64_t total, table::TotalKind kind = table::TotalKind::Exact,
                        std::int64_t undecided = 0, std::vector<std::string> const& awaiting = {}) {
    for (int round = 0; round < 16 && openCalls(h, "Table.samples") > 0; ++round) {
        std::size_t const index = oldestOpen(h, "Table.samples");
        table::TableQuery const query = queryOf(h, index);
        std::int64_t const offset = query.page.offset;
        std::int64_t const count = std::clamp<std::int64_t>(total - offset, 0, 200);
        h.reply(index, pageJsonAt(h.described.revision, offset, count, total, h.columns, kind, undecided, awaiting));
    }
    REQUIRE(openCalls(h, "Table.samples") == 0);
}

/// @brief Mounts a document, answers its description and every page fetch it causes, and returns the screen.
/// @param h     The harness.
/// @param text  The document.
/// @param total The view's total.
/// @return The screen.
inline std::unique_ptr<Screen> mountShown(TableHarness& h, std::string_view text = kModelTable,
                                          std::int64_t total = 9093) {
    auto screen = h.mount(text);
    replyDescribe(h);
    answerPages(h, total);
    return screen;
}

/// @brief The status line the table widget was last given.
/// @param h The harness.
/// @return It.
inline TableStatus statusOf(TableHarness& h) { return h.backend.tableStatus(tableOf(h)); }

/// @brief The table widget's header captions.
/// @param h The harness.
/// @return They, in column order.
inline std::vector<std::string> headerLabels(TableHarness& h) {
    std::vector<std::string> out;
    for (TableColumn const& column : h.backend.tableColumns(tableOf(h))) {
        out.push_back(column.label);
    }
    return out;
}

/// @brief Answers the open `Table.samples.describe` call with @p json, then settles.
/// @param h    The harness.
/// @param json The description's JSON, its revision set.
inline void replyDescribe(TableHarness& h, std::string json) {
    h.reply(oldestOpen(h, "Table.samples.describe"), std::move(json));
}

/// @brief The reasons of every diagnostic reported, oldest first.
/// @param h The harness.
/// @return They.
inline std::vector<std::string> reportsOf(TableHarness const& h) {
    std::vector<std::string> out;
    for (Refusal const& refusal : h.diagnostics) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(refusal.reason);
    }
    return out;
}

/// @brief A table widget's header captions.
/// @param h      The harness.
/// @param testId The table's test id.
/// @return They, in column order.
inline std::vector<std::string> headerLabels(TableHarness& h, std::string_view testId) {
    std::vector<std::string> out;
    for (TableColumn const& column : h.backend.tableColumns(h.byTestId("Table", testId))) {
        out.push_back(column.label);
    }
    return out;
}

/// @brief The column with @p id the table `t` was given (`setColumns`).
/// @param h  The harness.
/// @param id The column's id.
/// @return The column.
inline TableColumn columnOf(TableHarness& h, std::string_view id) {
    for (TableColumn const& column : h.backend.tableColumns(h.byTestId("Table", "t"))) {
        if (column.id == id) {
            return column;
        }
    }
    FAIL("the table has no column " << id);
    return {};
}

/// @brief The index into the table `t`'s columns of the column with @p id.
/// @param h  The harness.
/// @param id The column's id.
/// @return The index.
inline std::size_t columnIndex(TableHarness& h, std::string_view id) {
    std::vector<TableColumn> const columns = h.backend.tableColumns(h.byTestId("Table", "t"));
    auto const found = std::ranges::find(columns, id, &TableColumn::id);
    if (found == columns.end()) {
        FAIL("the table has no column " << id);
    }
    return static_cast<std::size_t>(found - columns.begin());
}

/// @brief The text the cell of the row with @p key in @p column shows, in the table `t`.
/// @param h      The harness.
/// @param key    The row's key.
/// @param column The column's id.
/// @return The cell's text.
inline std::string cellText(TableHarness& h, std::int64_t key, std::string_view column) {
    int const table = h.byTestId("Table", "t");
    std::optional<int> const row = h.backend.tableRow(table, Key{key});
    if (!row) {
        FAIL("the table has no row " << key);
    }
    std::string const at = std::to_string(columnIndex(h, column));
    for (int const cell : h.backend.children(*row)) {
        if (h.backend.prop(cell, "column") == at) {
            return h.backend.prop(cell, "text");
        }
    }
    FAIL("the row " << key << " has no cell in " << column);
    return {};
}

/// @brief The marks the cell of the row with @p key in @p column draws, in the table `t`: the letters the recording
///        backend writes for them, empty for none.
/// @param h      The harness.
/// @param key    The row's key.
/// @param column The column's id.
/// @return The marks.
inline std::string cellMarks(TableHarness& h, std::int64_t key, std::string_view column) {
    std::optional<int> const row = h.backend.tableRow(h.byTestId("Table", "t"), Key{key});
    if (!row) {
        FAIL("the table has no row " << key);
    }
    return h.backend.prop(*row, "marks:" + std::to_string(columnIndex(h, column)));
}

/// @brief The accent the cell of the row with @p key in @p column was last given, in the table `t`.
/// @param h      The harness.
/// @param key    The row's key.
/// @param column The column's id.
/// @return The accent; `CellAccent{}` when none was given.
inline CellAccent cellAccent(TableHarness& h, std::int64_t key, std::string_view column) {
    return h.backend.cellAccent(h.byTestId("Table", "t"), Key{key}, columnIndex(h, column));
}

/// @brief The diagnostics whose reason contains @p text.
/// @param h    The harness.
/// @param text The text.
/// @return How many.
inline std::size_t diagnosticsMentioning(TableHarness const& h, std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        h.diagnostics, [text](Refusal const& refusal) { return refusal.reason.contains(text); }));
}

/// @brief Delivers a change of `Lims_SampleModel.samples` with @p keys, as a `changes` event does, then settles.
/// @param h    The harness.
/// @param keys The changed rows' keys; none for a change that names no keys, which changes every row.
inline void change(TableHarness& h, std::vector<std::int64_t> const& keys) {
    effects::RecordChange record{.record = "Lims_SampleModel.samples", .keys = std::nullopt};
    if (!keys.empty()) {
        std::vector<glz::raw_json> raw;
        for (auto const key : keys) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `raw` is a fixture list built once; sizing it first would not change what the case checks
            raw.emplace_back(std::to_string(key));
        }
        record.keys = std::move(raw);
    }
    wire::ChangesBody body;
    body.records.push_back(std::move(record));
    h.session.applyChanges(body);
    h.settle();
}

/// @brief Whether a widget is shown: neither it nor a column around it is hidden.
/// @param h      The harness.
/// @param widget The widget.
/// @return `true` when shown.
inline bool shownThroughColumns(TableHarness const& h, int widget) {
    for (std::optional<int> current = widget; current;) {
        if (h.backend.prop(*current, "visible") == "false") {
            return false;
        }
        std::optional<int> parent;
        for (int const column : h.backend.all("Column")) {
            if (std::ranges::contains(h.backend.children(column), *current)) {
                parent = column;
            }
        }
        current = parent;
    }
    return true;
}

/// @brief Whether a text with @p text is mounted and shown.
/// @param h    The harness.
/// @param text The text.
/// @return `true` when one is.
inline bool shownText(TableHarness const& h, std::string_view text) {
    auto const found = h.backend.find("Text", "text", text);
    return found.has_value() && shownThroughColumns(h, *found);
}

/// @brief The mount's counters of a screen.
/// @param screen The screen.
/// @return `Mounted::stats()`.
inline MountStats statsOf(Screen const& screen) { return ScreenProbe::stats(screen); }

/// @brief A screen's core.
/// @param screen The screen.
/// @return It.
inline morph::ui::detail::ScreenCore& coreOf(Screen& screen) { return ScreenProbe::core(screen); }

/// @brief An element of a screen's own scope.
/// @param screen The screen.
/// @param id     The element's id.
/// @return Its state.
inline morph::ui::detail::ElementState& elementOf(Screen& screen, std::string_view id) {
    return ScreenProbe::element(screen, id);
}

/// @brief A description with @p level on @p column, the level listed in its `levels`.
/// @param description The description.
/// @param column      The column's id.
/// @param level       The level.
/// @return The description, its revision set again.
inline table::TableDescription withLevel(table::TableDescription description, std::string_view column,
                                         std::string level) {
    for (auto& entry : description.columns) {
        if (entry.info.id == column) {
            entry.info.level = level;
        }
    }
    if (!std::ranges::contains(description.levels, level)) {
        description.levels.push_back(std::move(level));
    }
    return table::describe(std::move(description));
}

/// @brief A page against `h.described`'s revision with the names of rows @p first to `first + n - 1` written
///        `"renamed-<key>"`.
/// @param revision The revision the page names.
/// @param offset   The first row's view index and key.
/// @param count    Rows.
/// @param total    The view's total.
/// @param columns  The columns the page holds.
/// @param first    The first renamed row.
/// @param n        How many are renamed.
/// @return The page's JSON.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a page's numbers, in the order a page names them.
inline std::string pageJsonRenaming(std::string revision, std::int64_t offset, std::int64_t count, std::int64_t total,
                                    std::vector<std::string> const& columns, std::int64_t first, std::int64_t n) {
    table::GenericPage page = pageAt(std::move(revision), offset, count, total, columns);
    auto const name = std::ranges::find(columns, "name");
    REQUIRE(name != columns.end());
    auto const at = static_cast<std::size_t>(name - columns.begin());
    for (std::int64_t key = first; key < first + n; ++key) {
        page.rows.at(static_cast<std::size_t>(key - offset)).at(at) = table::Cell{"renamed-" + std::to_string(key)};
    }
    return glz::write_json(page).value_or("");
}

/// @brief A page against @p revision whose rows @p unloaded lack levels, and whose `failed` lists @p failed.
/// @param revision The revision the page names.
/// @param offset   The first row's view index and key.
/// @param count    Rows.
/// @param total    The view's total.
/// @param columns  The columns the page holds.
/// @param unloaded The rows with unloaded levels, by index into the page's rows.
/// @param failed   The levels whose load failed.
/// @return The page's JSON.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a page's numbers, in the order a page names them.
inline std::string pageJsonUnloaded(std::string revision, std::int64_t offset, std::int64_t count, std::int64_t total,
                                    std::vector<std::string> const& columns,
                                    std::vector<table::UnloadedRow> const& unloaded,
                                    std::vector<std::string> const& failed) {
    table::GenericPage page = pageAt(std::move(revision), offset, count, total, columns);
    page.unloadedRows = unloaded;
    page.failed = failed;
    return glz::write_json(page).value_or("");
}

/// @brief A non-owning `std::shared_ptr` to @p store, for an API that shares its store.
/// @param store The store, which must outlive every copy.
/// @return The pointer.
inline std::shared_ptr<table::ITableStateStore> sharedOf(table::ITableStateStore& store) {
    return {std::shared_ptr<void>{}, &store};
}

// ── A screen with a table over `rows` ───────────────────────────────────────

/// @brief A document's rows with the row of id `"b"` named `"zz"`, or the first row when no row has that id: what
///        `rowsDoc`'s button `rename` sets, so the list always changes.
/// @param itemsLiteral The rows, a JSON list.
/// @return The renamed list's JSON text.
inline std::string renamedB(std::string_view itemsLiteral) {
    auto json = parseJson(itemsLiteral);
    REQUIRE(json.has_value());
    auto& items = std::get<Json::array_t>(json->data);
    auto const isB = [](Json const& item) {
        auto const* const fields = std::get_if<Json::object_t>(&item.data);
        const auto* const id = fields == nullptr ? Json::object_t::const_iterator{} : fields->find("id");
        return fields != nullptr && id != fields->end() && std::get_if<std::string>(&id->second.data) != nullptr &&
               std::get<std::string>(id->second.data) == "b";
    };
    auto const found = std::ranges::find_if(items, isB);
    auto const renamed = found != items.end() ? found : items.begin();
    if (renamed != items.end()) {
        if (auto* const fields = std::get_if<Json::object_t>(&renamed->data)) {
            fields->insert_or_assign("name", Json{std::string{"zz"}});
        }
    }
    return glz::write_json(*json).value_or("");
}

/// @brief A `ui/1` document `samples` whose state `items` starts as @p itemsLiteral, showing a table (test id `list`)
///        over `items` keyed by the row's `id`, with the sortable columns `id`, `name` and `weight`, and a button
///        `rename` that sets `items` to `renamedB` of the list.
/// @param itemsLiteral The rows, a JSON list.
/// @param tableExtra   Members appended to the table node, each written `,"key":value`.
/// @param withId       Whether the table has the id `list`.
/// @return The document.
inline std::string rowsDocWith(std::string_view itemsLiteral, std::string_view tableExtra, bool withId) {
    return R"({"vocab":"ui/1","id":"samples","state":{"items":{"init":{"lit":)" + std::string{itemsLiteral} +
           R"(}}},"view":{"kind":"column","children":[{"kind":"table",)" + (withId ? R"("id":"list",)" : "") +
           R"("testId":"list","rows":{"ref":"items"},"key":{"row":"id"},"columns":[)"
           R"({"id":"id","label":"Id","sortable":true},{"id":"name","label":"Name","sortable":true},)"
           R"({"id":"weight","label":"Weight","sortable":true}])" +
           std::string{tableExtra} +
           R"(},{"kind":"button","testId":"rename","label":"Rename","onClick":[{"set":"items","value":{"lit":)" +
           renamedB(itemsLiteral) + R"(}}]}]}})";
}

/// @brief `rowsDocWith` with the table's `id`.
/// @param itemsLiteral The rows, a JSON list.
/// @param tableExtra   Members appended to the table node.
/// @return The document.
inline std::string rowsDoc(std::string_view itemsLiteral, std::string_view tableExtra = "") {
    return rowsDocWith(itemsLiteral, tableExtra, true);
}

/// @brief `rowsDoc` with no `id` on the table.
/// @param itemsLiteral The rows, a JSON list.
/// @param tableExtra   Members appended to the table node.
/// @return The document.
inline std::string rowsDocNoId(std::string_view itemsLiteral, std::string_view tableExtra = "") {
    return rowsDocWith(itemsLiteral, tableExtra, false);
}

/// @brief Three rows, `a`, `b` and `c`, each named as its id.
inline constexpr std::string_view kThree = R"([{"id":"a","name":"a"},{"id":"b","name":"b"},{"id":"c","name":"c"}])";

/// @brief The model-backed table a rows table is compared with.
inline constexpr std::string_view kRowsSamples = kModelTable;

/// @brief @p n rows `{"id": i, "name": "n<i>"}` as a JSON list.
/// @param n Rows.
/// @return The list's text.
inline std::string idRows(std::size_t n) {
    std::string out = "[";
    for (std::size_t at = 0; at < n; ++at) {
        out += (at == 0 ? "" : ",") + std::format(R"({{"id":{},"name":"n{}"}})", at, at);
    }
    return out + "]";
}

/// @brief 200 rows `{"id", "name", "weight"}` whose names mix accents and case and whose weights are decimals of two
///        places; every tenth name and weight is `null`.
/// @return The list's text.
inline std::string mixedRows() {
    static constexpr std::array<std::string_view, 14> kNames = {"Émile",  "emil",   "Zoë",   "zoe",   "Anna",
                                                                "anna",   "Ödön",   "oscar", "Ärger", "apple",
                                                                "éclair", "Eclair", "zebra", "Zebra"};
    std::string out = "[";
    for (std::size_t at = 0; at < 200; ++at) {
        bool const missing = at % 10 == 9;
        std::string const name = missing ? "null" : "\"" + std::string{kNames.at((at * 5) % kNames.size())} + "\"";
        std::string const weight = missing ? "null"
                                           : std::format(R"({{"num":{},"den":100,"dp":2}})",
                                                         static_cast<std::int64_t>((at * 389) % 2000) - 700);
        out += (at == 0 ? "" : ",") + std::format(R"({{"id":{},"name":{},"weight":{}}})", at, name, weight);
    }
    return out + "]";
}

/// @brief The cells of @p rows, a JSON list of `{id, name, weight}`, as a model's table holds them: `id` the key,
///        `name` text, `weight` an exact decimal, `null` empty.
/// @param rows The list's text.
/// @return The source.
inline std::unique_ptr<table::VectorSource> rowsAsSource(std::string_view rows) {
    auto source = std::make_unique<table::VectorSource>(
        std::vector<table::ColumnInfo>{table::ColumnInfo{.id = "id", .kind = table::ColumnKind::Integer},
                                       table::ColumnInfo{.id = "name", .kind = table::ColumnKind::Text},
                                       table::ColumnInfo{.id = "weight", .kind = table::ColumnKind::Decimal}});
    auto const list = parseJson(rows);
    REQUIRE(list.has_value());
    std::vector<table::RowId> ids;
    std::vector<std::vector<table::Cell>> cells;
    auto const integer = [](Json const& json) {
        return std::visit(
            []<typename Number>(Number const& number) -> std::int64_t {
                if constexpr (std::is_arithmetic_v<Number>) {
                    return static_cast<std::int64_t>(number);
                } else {
                    FAIL("not a number");
                    return 0;
                }
            },
            json.data);
    };
    for (Json const& item : std::get<Json::array_t>(list->data)) {
        auto const& fields = std::get<Json::object_t>(item.data);
        std::int64_t const id = integer(fields.at("id"));
        table::Cell name;
        if (auto const* const text = std::get_if<std::string>(&fields.at("name").data)) {
            name = *text;
        }
        table::Cell weight;
        if (auto const* const parts = std::get_if<Json::object_t>(&fields.at("weight").data)) {
            weight = math::Rational{math::Numerator{integer(parts->at("num"))},
                                    math::Denominator{integer(parts->at("den"))}, math::DecimalPlaces{2}};
        }
        ids.emplace_back(id);
        cells.push_back({table::Cell{id}, std::move(name), std::move(weight)});
    }
    source->setRows(std::move(ids), std::move(cells));
    return source;
}

/// @brief The description of a model's table over @p rows: `id` (the key), `name` and `weight`, sortable, of the
///        kinds a `ValueRowsSource` infers from those rows.
/// @param rows The list's text.
/// @return The description, its revision set.
inline table::TableDescription describedFrom(std::string_view rows) {
    auto const json = parseJson(rows);
    REQUIRE(json.has_value());
    auto const list = valueFromJson(*json);
    REQUIRE(list.has_value());
    table::TableDescription description;
    description.id = "samples";
    description.label = "Samples";
    description.key = "id";
    for (std::string_view const id : {"id", "name", "weight"}) {
        std::array<std::size_t, morph::ui::detail::kValueVotes> counts{};
        std::vector<morph::ui::detail::ValueVote> votes;
        for (Value const& row : *list->asList()) {
            Value const* const field = row.field(id);
            votes.push_back(field == nullptr ? morph::ui::detail::ValueVote::None : morph::ui::detail::voteOf(*field));
            ++counts.at(static_cast<std::size_t>(votes.back()));
        }
        table::TableColumn column;
        column.info = table::ColumnInfo{.id = std::string{id}, .kind = morph::ui::detail::inferredKind(counts, votes)};
        column.label = forms::detail::inferTitle(id);
        column.sortable = true;
        description.columns.push_back(std::move(column));
    }
    return table::describe(std::move(description));
}

/// @brief A model's answer to @p query over @p rows, ordered by `table::apply` with `ValueRowsCollator` as the
///        served collator, against `describedFrom(rows)`'s revision.
/// @param rows  The list's text.
/// @param query The query.
/// @return The generic page's JSON.
inline std::string pagesOf(std::string_view rows, table::TableQuery const& query) {
    auto const source = rowsAsSource(rows);
    table::ApplyOptions options;
    options.services.collator = std::make_shared<ValueRowsCollator>();
    auto const answered = table::apply(*source, query, options);
    REQUIRE(answered.has_value());
    auto const snapshot = source->snapshot();
    table::GenericPage page;
    page.revision = describedFrom(rows).revision;
    page.columns = query.columns.value_or(std::vector<std::string>{"id", "name", "weight"});
    page.total = answered->total;
    page.offset = answered->offset;
    for (std::size_t const row : answered->rows) {
        page.keys.push_back(snapshot->rowId(row));
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            auto const at = static_cast<std::size_t>(
                std::ranges::find(source->columns(), column, &table::ColumnInfo::id) - source->columns().begin());
            cells.push_back(snapshot->cell(row, at));
        }
        page.rows.push_back(std::move(cells));
    }
    return glz::write_json(page).value_or("");
}

/// @brief The keys of the realized data rows of the harness's first table, in child order, joined by spaces; a
///        text key is written without its quotes, and a placeholder row, which has none, is left out.
/// @param h The harness.
/// @return The keys.
inline std::string rowOrder(TableHarness const& h) {
    std::string out;
    for (int const row : h.backend.children(h.backend.all("Table").front())) {
        if (h.backend.prop(row, "heading") == "true") {
            continue;
        }
        std::string key = h.backend.prop(row, "rowKey");
        if (key.empty()) {
            continue;  // a placeholder row
        }
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
            key = key.substr(1, key.size() - 2);
        }
        out += (out.empty() ? "" : " ") + key;
    }
    return out;
}

/// @brief The keys of a table's realized rows that have one, in child order (the order the mount placed them), a
///        text key without its quotes: a collection keyed by text and a rows table keyed by integer compare equal.
/// @param h     The harness.
/// @param table The table widget.
/// @return The keys.
inline std::vector<std::string> rowKeys(TableHarness const& h, int table) {
    std::vector<std::string> out;
    for (int const row : h.backend.children(table)) {
        std::string key = h.backend.prop(row, "rowKey");
        if (key.empty()) {
            continue;
        }
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
            key = key.substr(1, key.size() - 2);
        }
        out.push_back(std::move(key));
    }
    return out;
}

/// @brief The Samples collection beside a rows table `t` over the same four rows.
inline constexpr std::string_view kCollectionBesideTable = R"({
  "vocab": "ui/1", "id": "both",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "rows": { "init": { "lit": [ { "id": 1, "name": "beta" }, { "id": 2, "name": "Alpha" },
                                          { "id": 3, "name": "álpha" }, { "id": 4, "name": "Gamma" } ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "collection", "model": "sample", "action": "Lims_ListSamples" },
    { "kind": "table", "id": "t", "testId": "t", "rows": { "ref": "rows" },
      "columns": [ { "id": "id", "label": "ID", "sortable": true },
                   { "id": "name", "label": "Name", "sortable": true } ] } ] } })";

/// @brief The Samples collection alone, on the screen `samples`.
inline constexpr std::string_view kSamplesCollection = R"({
  "vocab": "ui/1", "id": "samples",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "collection", "model": "sample", "action": "Lims_ListSamples" } })";

/// @brief A master-detail view of `Lims_ListSamples`: `ID` (hidden) and `Name`, its rows opening the capture form.
inline constexpr std::string_view kMasterDetailSamples =
    R"({"v-kind":"master-detail","v-title":"Samples","v-query":"Lims_ListSamples","v-rowKey":"id",
        "v-columns":[{"field":"id","label":"ID","kind":"integer","v-hidden":true},{"field":"name","label":"Name"}],
        "v-rowAction":{"action":"Lims_CaptureConcentration","bind":{"sampleId":"id"}}})";

/// @brief A view of `Lims_ListSamples` with the columns of `mixedRows`, in `rowsDoc`'s order: `id`, `name`, `weight`.
inline constexpr std::string_view kMixedSamplesView =
    R"({"v-kind":"collection","v-title":"Samples","v-query":"Lims_ListSamples","v-rowKey":"id",
        "v-columns":[{"field":"id","label":"Id","kind":"integer"},{"field":"name","label":"Name"},
                     {"field":"weight","label":"Weight","kind":"decimal"}]})";

/// @brief The engine that orders a table over `rows`.
/// @param screen The screen.
/// @param id     The table's element id.
/// @return The engine.
inline table::Engine& engineOf(Screen& screen, std::string_view id) {
    auto* const rows = dynamic_cast<EngineRows*>(ScreenProbe::element(screen, id).tableRows.get());
    REQUIRE(rows != nullptr);
    return rows->engine();
}

/// @brief A `rowsDoc` text with the column part @p column given the member @p member, written `"key":value`.
/// @param text   The document.
/// @param column The column's id.
/// @param member The member.
/// @return The document.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `withColumnKey(text, column, member)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
inline std::string withColumnKey(std::string_view text, std::string_view column, std::string_view member) {
    auto json = parseJson(text);
    REQUIRE(json.has_value());
    auto field = parseJson("{" + std::string{member} + "}");
    REQUIRE(field.has_value());
    auto& view = std::get<Json::object_t>(std::get<Json::object_t>(json->data).at("view").data);
    auto& children = std::get<Json::array_t>(view.at("children").data);
    auto& tableNode = std::get<Json::object_t>(children.front().data);
    for (auto& part : std::get<Json::array_t>(tableNode.at("columns").data)) {
        auto& fields = std::get<Json::object_t>(part.data);
        if (std::get<std::string>(fields.at("id").data) != column) {
            continue;
        }
        for (auto& [key, value] : std::get<Json::object_t>(field->data)) {
            fields.insert_or_assign(key, value);
        }
    }
    return glz::write_json(*json).value_or("");
}

/// @brief Whether a document loads against @p catalog.
/// @param text    The document.
/// @param catalog The catalog.
/// @return `true` when it loads.
inline bool loads(std::string_view text, Catalog const& catalog) {
    return loadDocumentText(text, LoadOptions{.catalog = catalog}).has_value();
}

/// @brief The reason a document is refused against the table catalog.
/// @param text The document.
/// @return The reason; empty when it loads.
inline std::string refusalOf(std::string_view text) {
    auto const loaded = loadDocumentText(text, LoadOptions{.catalog = tableCatalog()});
    return loaded ? std::string{} : loaded.error().reason;
}

/// @brief The reason a document is refused against a harness's catalog, the actions it added included.
/// @param h    The harness.
/// @param text The document.
/// @return The reason; empty when it loads.
inline std::string refusalOf(TableHarness const& h, std::string_view text) {
    auto const loaded = loadDocumentText(text, LoadOptions{.catalog = h.session.options().catalog});
    return loaded ? std::string{} : loaded.error().reason;
}

// ── A typed list action as a table ──────────────────────────────────────────

/// @brief A table `list` over the typed list action `Lims_PageSamples` of the model alias `lab`, keyed by `id`, with
///        the columns `id`, `name`, `weight`, `ratio` and `day`, the last of the literal kind `date`.
inline constexpr std::string_view kActionTable =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table","id":"list","testId":"list","model":"lab","action":"Lims_PageSamples","key":{"row":"id"},"columns":[{"id":"id"},{"id":"name"},{"id":"weight"},{"id":"ratio"},{"id":"day","kind":"date"}]}})";

/// @brief `kActionTable` over `Lims_ListSamples`, whose request schema in the corpus catalog has no member marked
///        `x-table`.
inline constexpr std::string_view kActionTableNoQuery =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table","id":"list","testId":"list","model":"lab","action":"Lims_ListSamples","key":{"row":"id"},"columns":[{"id":"id"},{"id":"name"}]}})";

/// @brief `kActionTable` without `columns`.
inline constexpr std::string_view kActionTableNoColumns =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table","id":"list","testId":"list","model":"lab","action":"Lims_PageSamples","key":{"row":"id"}}})";

/// @brief Adds `Lims_PageSamples` to the harness's catalog: a request with the query member `query`, marked
///        `x-table`, and a `site`; no view document and no record.
/// @param h The harness.
inline void addPageSamples(TableHarness& h) {
    addAction(h, "Lims_PageSamples",
              R"({"type":"object","properties":{"query":{"type":"object","x-table":true},"site":{"type":"string"}}})",
              ActionInfo{.modelType = "Lims_SampleModel"});
}

/// @brief Gives an action a view document in the harness's catalog; before `mount`.
/// @param h      The harness.
/// @param action The action's id.
/// @param view   The view document's JSON text.
inline void addView(TableHarness& h, std::string action, std::string view) {
    h.extraViews.insert_or_assign(std::move(action), std::move(view));
}

/// @brief The unit a model-backed table's description lists under @p id.
/// @param screen The screen.
/// @param id     The unit's id.
/// @return The unit; the test fails when it is not listed.
inline table::UnitInfo unitOf(Screen& screen, std::string_view id);

/// @brief The rows of a model-backed table.
/// @param screen The screen.
/// @param id     The table's element id.
/// @return Its `WindowRows`.
inline WindowRows& windowOf(Screen& screen, std::string_view id = "list") {
    WindowRows* const rows = ScreenProbe::element(screen, id).tableWindow.get();
    REQUIRE(rows != nullptr);
    return *rows;
}

/// @brief The kind of a column of a model-backed table's description.
/// @param screen The screen.
/// @param column The column's id.
/// @return Its kind.
inline table::ColumnKind kindOfColumn(Screen& screen, std::string_view column) {
    table::TableDescription const* const description = windowOf(screen).description();
    REQUIRE(description != nullptr);
    auto const found = std::ranges::find(description->columns, column,
                                         [](table::TableColumn const& c) -> auto const& { return c.info.id; });
    REQUIRE(found != description->columns.end());
    return found->info.kind;
}

inline table::UnitInfo unitOf(Screen& screen, std::string_view id) {
    table::TableDescription const* const description = windowOf(screen).description();
    REQUIRE(description != nullptr);
    auto const found = std::ranges::find(description->units, id, &table::UnitInfo::id);
    REQUIRE(found != description->units.end());
    return *found;
}

/// @brief A cell a model-backed table's window holds.
/// @param screen The screen.
/// @param row    The view row.
/// @param column The column's id.
/// @return The cell; the test fails when it is not held.
inline table::Cell const* cellOf(Screen& screen, std::int64_t row, std::string_view column) {
    table::Cell const* const cell = WindowRowsProbe::window(windowOf(screen)).cell(row, column);
    REQUIRE(cell != nullptr);
    return cell;
}

// ── Configuring a table ─────────────────────────────────────────────────────

/// @brief A rows table `t`: nine sortable columns c1…c9 labelled C1…C9, three rows keyed by `id`.
inline constexpr std::string_view kNine = R"({
  "vocab": "ui/1", "id": "nine",
  "state": { "items": { "init": { "lit": [
    { "id": 1, "c1": 3, "c2": 1, "c3": 2, "c4": 1, "c5": 2, "c6": 3, "c7": 1, "c8": 2, "c9": 3 },
    { "id": 2, "c1": 1, "c2": 2, "c3": 3, "c4": 2, "c5": 3, "c6": 1, "c7": 2, "c8": 3, "c9": 1 },
    { "id": 3, "c1": 2, "c2": 3, "c3": 1, "c4": 3, "c5": 1, "c6": 2, "c7": 3, "c8": 1, "c9": 2 } ] } } },
  "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
            "columns": [ { "id": "c1", "label": "C1", "sortable": true }, { "id": "c2", "label": "C2", "sortable": true },
                         { "id": "c3", "label": "C3", "sortable": true }, { "id": "c4", "label": "C4", "sortable": true },
                         { "id": "c5", "label": "C5", "sortable": true }, { "id": "c6", "label": "C6", "sortable": true },
                         { "id": "c7", "label": "C7", "sortable": true }, { "id": "c8", "label": "C8", "sortable": true },
                         { "id": "c9", "label": "C9", "sortable": true } ] } })";

/// @brief The model-backed table `t` over `Table.samples`; its description is what the test replies
/// (`h.replyDescribe`).
inline constexpr std::string_view kConfigModelTable = R"({
  "vocab": "ui/1", "id": "samples",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "table", "id": "t", "model": "sample", "table": "samples" } })";

/// @brief A header's menu, or the table's, as a user opening it sees it.
/// @param h      The harness.
/// @param table  The table widget.
/// @param column The column, an index into `Table::columns`; none for the table's menu.
/// @return The entries.
inline std::vector<MenuEntry> menuOf(TableHarness& h, int table, std::optional<std::size_t> column) {
    std::vector<MenuEntry> entries = h.backend.menuOf(table, column);
    h.settle();
    return entries;
}

/// @brief Chooses a menu entry by its labels from the top, then settles.
/// @param h      The harness.
/// @param table  The table widget.
/// @param column The column; none for the table's menu.
/// @param labels The entry's label at each level.
inline void chooseMenu(TableHarness& h, int table, std::optional<std::size_t> column,
                       std::vector<std::string_view> const& labels) {
    h.backend.chooseMenu(table, column, labels);
    h.settle();
}

/// @brief Drags a column's header to a place among the visible columns, then settles.
/// @param h        The harness.
/// @param table    The table widget.
/// @param column   The column.
/// @param position Its new place.
inline void moveColumn(TableHarness& h, int table, std::size_t column, std::size_t position) {
    h.backend.moveColumn(table, column, position);
    h.settle();
}

/// @brief Drags a column header's edge to a width, or double-clicks it, then settles.
/// @param h      The harness.
/// @param table  The table widget.
/// @param column The column.
/// @param units  The width; none for the double click.
inline void resizeColumn(TableHarness& h, int table, std::size_t column, std::optional<int> units) {
    h.backend.resizeColumn(table, column, units);
    h.settle();
}

/// @brief Presses a column's filter button, then settles.
/// @param h      The harness.
/// @param table  The table widget.
/// @param column The column.
inline void clickFilter(TableHarness& h, int table, std::size_t column) {
    h.backend.clickFilter(table, column);
    h.settle();
}

/// @brief Presses a column group's filter button, then settles.
/// @param h     The harness.
/// @param table The table widget.
/// @param group The group's index.
inline void clickGroupFilter(TableHarness& h, int table, std::size_t group) {
    h.backend.clickGroupFilter(table, group);
    h.settle();
}

/// @brief Asks the table to copy, as `Ctrl+C` does, then settles.
/// @param h     The harness.
/// @param table The table widget.
inline void pressCopy(TableHarness& h, int table) {
    h.backend.pressCopy(table);
    h.settle();
}

/// @brief The text last put on the clipboard.
/// @param h The harness.
/// @return It.
inline std::string clipboardText(TableHarness const& h) { return h.backend.clipboardText(); }

/// @brief A table widget's status line, as the mount last set it.
/// @param h     The harness.
/// @param table The table widget.
/// @return It.
inline TableStatus statusOf(TableHarness& h, int table) { return h.backend.tableStatus(table); }

/// @brief The last marks the recording backend got for a row of the table `testId` `t`.
/// @param h   The harness.
/// @param key The row's key.
/// @return The marks; none before any `setRowMarks` of that row.
inline std::optional<RowMarks> rowMarks(TableHarness& h, Key const& key) {
    return h.backend.rowMarks(h.byTestId("Table", "t"), key);
}

/// @brief The labels of a menu's entries that can be chosen or open a submenu, separators and headings left out.
/// @param entries The entries.
/// @return Their labels.
inline std::vector<std::string> labelsOf(std::vector<MenuEntry> const& entries) {
    std::vector<std::string> out;
    for (MenuEntry const& entry : entries) {
        if (!entry.separator && !entry.heading) {
            out.push_back(entry.label);
        }
    }
    return out;
}

/// @brief A menu's entry by its label.
/// @param entries The entries.
/// @param label   The label.
/// @return The entry; the test fails without one.
inline MenuEntry entryOf(std::vector<MenuEntry> const& entries, std::string_view label) {
    auto const found = std::ranges::find(entries, label, &MenuEntry::label);
    REQUIRE(found != entries.end());
    return *found;
}

// ── Stores for saved views ──────────────────────────────────────────────────

/// @brief One call a `RecordingTableStateStore` saw: what it was, and what the store answered.
struct StoreCall {
    /// @brief `"load"`, `"save"` or `"publish"`.
    std::string op;
    /// @brief The key's part.
    std::string part;
    /// @brief The `expected` revision of a save or publish; empty for a load.
    std::string expected;
    /// @brief A save's or publish's outcome; `Saved` for a load.
    table::SaveOutcome outcome = table::SaveOutcome::Saved;
    /// @brief The revision the answer named: a save's, or the loaded document's; empty for none.
    std::string revision;
};

/// @brief A map store under `ITableStateStore`'s `expected` rule, `kNoDocument` included, that records each call and
///        its answer in the order the calls were made. Revisions are `"1"`, `"2"`, … per store; completions are
///        delivered on the executor it was given. It keeps one document per `part` and ignores the rest of the key,
///        so it holds the state of one table. Built with `Sharing::Shared`, `publish` applies operations to the
///        `shared` part.
class RecordingTableStateStore final : public table::ITableStateStore {
public:
    /// @brief A store delivering on @p executor.
    /// @param executor Where completions are delivered. Borrowed.
    /// @param sharing  Whether it keeps the `shared` part for everyone.
    explicit RecordingTableStateStore(exec::IExecutor& executor, table::Sharing sharing = table::Sharing::Private)
        : _executor{&executor}, _sharing{sharing} {}

    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& key) override {
        auto [completion, promise] =
            async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_executor);
        std::optional<table::StoredTableState> const stored = storedOf(key.part);
        calls.push_back(StoreCall{.op = "load",
                                  .part = key.part,
                                  .expected = {},
                                  .outcome = {},
                                  .revision = stored ? stored->revision : std::string{}});
        promise.resolve(stored);
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<table::SaveResult> save(table::TableStateKey const& key, std::string json,
                                                            std::string expected = {}) override {
        return answer("save", key.part, expected, store(key.part, std::move(json), expected));
    }

    [[nodiscard]] async::Completion<bool> remove(table::TableStateKey const& key) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(_executor);
        promise.resolve(_stored.erase(key.part) != 0);
        return std::move(completion);
    }

    [[nodiscard]] bool shares() const override { return _sharing == table::Sharing::Shared; }

    [[nodiscard]] async::Completion<table::SaveResult> publish(table::TableStateKey const& key,
                                                               table::ViewOperation const& operation,
                                                               std::string expected) override {
        if (!shares()) {
            return refusePublish(*_executor);
        }
        table::SaveResult result;
        if (auto conflict = conflictOf(key.part, expected)) {
            result = *conflict;
        } else {
            table::ViewsDocument document;
            if (auto const stored = storedOf(key.part)) {
                document = table::readViews(stored->json).value_or(table::ViewsDocument{});
            }
            auto applied = table::applyViewOperation(std::move(document), operation, table::ViewsPart::Shared);
            result = applied ? store(key.part, table::writeViews(*applied, table::ViewsPart::Shared), expected)
                             : table::SaveResult{.outcome = table::SaveOutcome::Refused,
                                                 .revision = {},
                                                 .reason = table::viewErrorText(applied.error(), operation)};
        }
        return answer("publish", key.part, expected, std::move(result));
    }

    /// @brief The calls, in the order they were made.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): the store's call log, which the cases read
    std::vector<StoreCall> calls;

private:
    [[nodiscard]] std::optional<table::StoredTableState> storedOf(std::string const& part) const {
        auto const found = _stored.find(part);
        return found == _stored.end() ? std::nullopt : std::optional{found->second};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `conflictOf(part, expected)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    [[nodiscard]] std::optional<table::SaveResult> conflictOf(std::string const& part,
                                                              std::string const& expected) const {
        auto const stored = storedOf(part);
        bool const holds =
            expected.empty() || (expected == table::kNoDocument ? !stored : stored && stored->revision == expected);
        if (holds) {
            return std::nullopt;
        }
        return table::SaveResult{.outcome = table::SaveOutcome::Conflict,
                                 .revision = stored ? stored->revision : std::string{},
                                 .reason = {}};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `store(part, json, expected)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    table::SaveResult store(std::string const& part, std::string json, std::string const& expected) {
        if (auto conflict = conflictOf(part, expected)) {
            return *conflict;
        }
        std::string revision = std::to_string(++_revisions);
        _stored.insert_or_assign(part, table::StoredTableState{.json = std::move(json), .revision = revision});
        return table::SaveResult{.outcome = table::SaveOutcome::Saved, .revision = std::move(revision), .reason = {}};
    }

    async::Completion<table::SaveResult> answer(std::string op, std::string part, std::string expected,
                                                table::SaveResult result) {
        calls.push_back(StoreCall{.op = std::move(op),
                                  .part = std::move(part),
                                  .expected = std::move(expected),
                                  .outcome = result.outcome,
                                  .revision = result.revision});
        auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_executor);
        promise.resolve(std::move(result));
        return std::move(completion);
    }

    exec::IExecutor* _executor;
    table::Sharing _sharing;
    std::map<std::string, table::StoredTableState, std::less<>> _stored;
    std::uint64_t _revisions = 0;
};

/// @brief A map store that knows no sentinel: `expected` is compared with the stored revision as text, nothing stored
///        counting as revision `""`, so `kNoDocument` over nothing is a `Conflict` with revision `""`; an empty
///        `expected` replaces. Revisions are `"r1"`, `"r2"`, … per store. It keeps one document per `part` and ignores
///        the rest of the key, so it holds the state of one table.
class LiteralTableStateStore final : public table::ITableStateStore {
public:
    /// @brief A store delivering on @p executor.
    /// @param executor Where completions are delivered. Borrowed.
    explicit LiteralTableStateStore(exec::IExecutor& executor) : _executor{&executor} {}

    [[nodiscard]] async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& key) override {
        auto [completion, promise] =
            async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_executor);
        calls.push_back(StoreCall{.op = "load", .part = key.part, .expected = {}, .outcome = {}, .revision = {}});
        auto const found = _stored.find(key.part);
        std::optional<table::StoredTableState> const stored =
            found == _stored.end() ? std::nullopt : std::optional{found->second};
        calls.back().revision = stored ? stored->revision : std::string{};
        promise.resolve(stored);
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<table::SaveResult> save(table::TableStateKey const& key, std::string json,
                                                            std::string expected = {}) override {
        auto [completion, promise] = async::Completion<table::SaveResult>::makeSettleable(_executor);
        auto const found = _stored.find(key.part);
        std::string const current = found == _stored.end() ? std::string{} : found->second.revision;
        table::SaveResult result;
        if (!expected.empty() && expected != current) {
            result = table::SaveResult{.outcome = table::SaveOutcome::Conflict, .revision = current, .reason = {}};
        } else {
            std::string revision = "r" + std::to_string(++_revisions);
            _stored.insert_or_assign(key.part, table::StoredTableState{.json = std::move(json), .revision = revision});
            result =
                table::SaveResult{.outcome = table::SaveOutcome::Saved, .revision = std::move(revision), .reason = {}};
        }
        calls.push_back(StoreCall{.op = "save",
                                  .part = key.part,
                                  .expected = std::move(expected),
                                  .outcome = result.outcome,
                                  .revision = result.revision});
        promise.resolve(std::move(result));
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<bool> remove(table::TableStateKey const& key) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(_executor);
        promise.resolve(_stored.erase(key.part) != 0);
        return std::move(completion);
    }

    [[nodiscard]] async::Completion<table::SaveResult> publish(table::TableStateKey const& /*key*/,
                                                               table::ViewOperation const& /*operation*/,
                                                               std::string /*expected*/) override {
        return refusePublish(*_executor);
    }

    /// @brief The document stored under a part.
    /// @param part The part.
    /// @return It, or nothing.
    [[nodiscard]] std::optional<table::StoredTableState> stored(std::string const& part) const {
        auto const found = _stored.find(part);
        return found == _stored.end() ? std::nullopt : std::optional{found->second};
    }

    /// @brief The calls, in order.
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): the store's call log, which the cases read
    std::vector<StoreCall> calls;

private:
    exec::IExecutor* _executor;
    std::map<std::string, table::StoredTableState, std::less<>> _stored;
    std::uint64_t _revisions = 0;
};

}  // namespace morph::ui::testing
