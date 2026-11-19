// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/engine/collection.hpp>
#include <morph/forms/engine/collection_view.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <morph/forms/engine/wizard_view.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/chart_scale.hpp>
#include <morph/ui/dev_tools.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/message.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

static_assert(kMaxInlineFileBytes == (wire::kMaxEnvelopeBytes - (std::size_t{64} * 1024)) * 3 / 4,
              "the inline cap must stay tied to the wire envelope");

class BridgeDispatcher::Channel final : public ModelChannel {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the type id, then the key, as `bind` takes them.
    Channel(bridge::Bridge& bridge, exec::IExecutor* cbExec, Evaluator evaluator, std::string const& typeId,
            bridge::BindSharing sharing, std::string const& instanceKey)
        : _cbExec{cbExec},
          _evaluator{std::move(evaluator)},
          _typeId{typeId},
          _handler{bridge, cbExec, typeId, sharing, instanceKey} {}

    [[nodiscard]] async::Completion<std::string> execute(std::string const& actionId, std::string const& bodyJson,
                                                         CallOptions options) override;

    [[nodiscard]] ChannelFacts facts() const override;

private:
    exec::IExecutor* _cbExec;
    Evaluator _evaluator;
    std::string _typeId;
    bridge::RawHandler _handler;
};

namespace detail {

/// @brief The watches a session made without entries, shared with each registration so that a holder that outlives
///        its session finds nothing to release.
struct WatchRegistry {
    /// @brief One watch's record and handler.
    struct Watch {
        std::string record;
        std::function<void(effects::RecordChange const&)> handler;
    };
    /// @brief A `watchRecord` whose watch is not answered yet.
    struct Waiter {
        std::string record;
        async::Completion<WatchHolder>::Promise promise;
        WatchHolder holder;
    };

    /// @brief The session; null once it is being destroyed.
    Session* session = nullptr;
    /// @brief The last watch id given.
    std::uint64_t next = 0;
    /// @brief The watches, by id.
    std::map<std::uint64_t, Watch> watches;
    /// @brief The records whose `watch` is sent and not answered.
    std::set<std::string, std::less<>> inFlight;
    /// @brief The `watchRecord` calls waiting for an answer.
    std::vector<Waiter> waiters;

    /// @brief A `watch` naming @p records was answered: each waiting `watchRecord` for one of them resolves.
    /// @param records The records.
    void answered(std::vector<std::string> const& records) {
        for (std::string const& record : records) {
            inFlight.erase(record);
        }
        // Moved out first: resolving may release a registration, which reaches back into this registry.
        std::vector<Waiter> ready;
        std::erase_if(waiters, [&](Waiter& waiter) {
            if (std::ranges::find(records, waiter.record) == records.end()) {
                return false;
            }
            ready.push_back(std::move(waiter));
            return true;
        });
        for (Waiter& waiter : ready) {
            waiter.promise.resolve(waiter.holder);
        }
    }
};

/// @brief One watch of `Session::watchRecord`, released when the last `WatchHolder` copy dies.
struct WatchRegistration {
    WatchRegistration() = default;
    WatchRegistration(WatchRegistration const&) = delete;
    WatchRegistration& operator=(WatchRegistration const&) = delete;
    WatchRegistration(WatchRegistration&&) = delete;
    WatchRegistration& operator=(WatchRegistration&&) = delete;

    ~WatchRegistration() {
        auto const held = registry.lock();
        if (!held) {
            return;
        }
        held->watches.erase(id);
        if (held->session == nullptr) {
            return;
        }
        try {
            held->session->countRecord(record, -1);
            held->session->flushWatches();
        } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report a failed unwatch to.
        }
    }

    /// @brief The session's registry.
    std::weak_ptr<WatchRegistry> registry;
    /// @brief The watch's id there.
    std::uint64_t id = 0;
    /// @brief The record.
    std::string record;
};

}  // namespace detail

namespace detail {

namespace {

// The shortest text that reads back as @p number, in fixed notation: `0.0001`, never `1e-04`, so that every value
// a decimal can hold is read as one. The buffer holds the longest fixed form of a double (about 330 characters).
Value decimalOrText(double number) {
    std::array<char, 512> buffer{};
    auto const [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number,
                                            std::chars_format::fixed);
    if (error == std::errc{}) {
        if (auto const decimal = ::morph::table::parseDecimal(std::string_view{buffer.data(), end})) {
            return Value{Decimal{*decimal}};
        }
    }
    return Value{std::format("{}", number)};
}

}  // namespace

std::expected<Value, std::string> tableValueFromJson(Json const& json) {
    return structuralFromJson(json, &decimalOrText);
}

}  // namespace detail

Session::~Session() {
    if (_watchRegistry) {
        _watchRegistry->session = nullptr;
        _watchRegistry->waiters.clear();
    }
}

namespace detail {

ElementState::ElementState(reactive::Runtime& runtime)
    : draft{runtime},
      committed{runtime},
      edited{runtime, false},
      touched{runtime, false},
      focused{runtime, false},
      open{runtime, false},
      submitted{runtime, false},
      selection{runtime, Value{ValueList{}}},
      sort{runtime, Value{ValueList{}}},
      activeKey{runtime},
      checksVersion{runtime, 0},
      inner{runtime, nullptr} {}

ElementState::~ElementState() = default;

[[nodiscard]] Value evalIn(RtScope const& scope, Bindings const* bindings, ExprPtr const& expr) {
    if (!expr) {
        return Value{};
    }
    ScopeEnv const env{scope, bindings};
    return evaluate(*expr, env);
}

[[nodiscard]] bool engaged(Value const& value) {
    if (value.isNull()) {
        return false;
    }
    if (std::string const* const text = value.asString()) {
        return !text->empty();
    }
    if (ValueList const* const items = value.asList()) {
        return !items->empty();
    }
    return true;
}

[[nodiscard]] std::string textOf(Value const& value, FormatContext const& format) {
    if (value.isNull()) {
        return {};
    }
    if (std::string const* const text = value.asString()) {
        return *text;
    }
    if (bool const* const flag = value.asBool()) {
        return *flag ? "true" : "false";
    }
    // A number that cannot be rounded at its own places shows as its exact JSON rather than as a clamped value.
    if (Quantity const* const quantity = value.asQuantity()) {
        auto number = formatNumber(value, withoutGrouping(format.numeric));
        return number ? *number + " " + unitDisplay(quantity->unit, format.units) : canonicalJson(value);
    }
    if (auto number = formatNumber(value, withoutGrouping(format.numeric))) {
        return *std::move(number);
    }
    if (time::DateTime const* const instant = value.asTimestamp()) {
        return instant->toIso8601();
    }
    return canonicalJson(value);
}

namespace {

/// @brief A value as a view key: an `int` or a string as itself, anything else as its JSON text.
/// @param value The value.
/// @return The key.
[[nodiscard]] Key keyOf(Value const& value) {
    if (std::int64_t const* const number = value.asInt()) {
        return Key{*number};
    }
    if (std::string const* const text = value.asString()) {
        return Key{*text};
    }
    return Key{canonicalJson(value)};
}

}  // namespace

[[nodiscard]] Value valueOfKey(Key const& key) {
    if (std::int64_t const* const number = std::get_if<std::int64_t>(&key)) {
        return Value{*number};
    }
    return Value{std::get<std::string>(key)};
}

namespace {

/// @brief A list of strings out of a value: a string, a list of strings, or nothing.
/// @param value The value.
/// @return The strings.
[[nodiscard]] std::vector<std::string> stringsOf(Value const& value) {
    std::vector<std::string> out;
    if (std::string const* const text = value.asString()) {
        out.push_back(*text);
    } else if (ValueList const* const items = value.asList()) {
        for (Value const& item : *items) {
            if (std::string const* const element = item.asString()) {
                out.push_back(*element);
            }
        }
    }
    return out;
}

/// @brief A list of strings as a value.
/// @param texts The strings.
/// @return The list.
[[nodiscard]] Value listOfStrings(std::vector<std::string> const& texts) {
    ValueList items;
    items.reserve(texts.size());
    for (std::string const& text : texts) {
        items.emplace_back(text);
    }
    return Value{std::move(items)};
}

/// @brief A command context without its scope: what a callback the host keeps may hold, so that the host never
///        keeps a scope alive past its screen. The callback holds the scope weakly and puts it back.
/// @param ctx The context.
/// @return The context, its scope dropped.
[[nodiscard]] CommandCtx detached(CommandCtx const& ctx) {
    CommandCtx kept = ctx;
    kept.scope = nullptr;
    return kept;
}

/// @brief Collects the `onClick` lists of menu entries, submenus included.
/// @param entries The entries.
/// @param lists Where they go, in document order.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
void menuCommandLists(std::span<DocNodePtr const> entries, std::vector<Commands const*>& lists) {
    for (DocNodePtr const& entry : entries) {
        if (Commands const* const commands = entry->event("onClick")) {
            lists.push_back(commands);
        }
        menuCommandLists(entry->partList("items"), lists);
    }
}

/// @brief Collects the command lists of a screen's head: its actions', its menu entries' and its tools'.
/// @param doc The screen's document.
/// @param lists Where they go, in document order.
void headCommandLists(Document const& doc, std::vector<Commands const*>& lists) {
    for (DocNodePtr const& action : doc.actions) {
        if (Commands const* const commands = action->event("onClick")) {
            lists.push_back(commands);
        }
    }
    menuCommandLists(doc.menu, lists);
    for (DocNodePtr const& tool : doc.tools) {
        for (std::string_view const key : {"onChange", "onSubmit", "onChoose"}) {
            if (Commands const* const commands = tool->event(key)) {
                lists.push_back(commands);
            }
        }
    }
}

/// @brief A kind's key spec, from the kind table.
/// @param kind The kind spec.
/// @param key  The key.
/// @return The spec, or `nullptr` when the kind has no such key of its own.
[[nodiscard]] KeySpec const* keySpec(KindSpec const& kind, std::string_view key) {
    for (KeySpec const& spec : kind.keys) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

/// @brief Whether a node kind keeps per-element state even without an id: the inputs, dialogs, tables and the forms
///        engine's screens.
/// @param kind The kind.
/// @return It.
[[nodiscard]] bool statefulKind(std::string_view kind) {
    static constexpr std::array<std::string_view, 12> kStateful = {
        "textInput", "checkbox", "select", "dateTimeInput", "slider",     "filePicker",
        "dialog",    "drawer",   "table",  "form",          "collection", "wizard",
    };
    return listed(kStateful, kind);
}

}  // namespace

// NOLINTNEXTLINE(modernize-use-string-view): callers keep the key as an owned string, in the log and in tests.
[[nodiscard]] std::string commandKey(CommandKind kind) {
    switch (kind) {
        case CommandKind::Set:
            return "set";
        case CommandKind::Reset:
            return "reset";
        case CommandKind::Run:
            return "run";
        case CommandKind::Refetch:
            return "refetch";
        case CommandKind::Open:
            return "open";
        case CommandKind::Close:
            return "close";
        case CommandKind::Navigate:
            return "navigate";
        case CommandKind::Return:
            return "return";
        case CommandKind::Focus:
            return "focus";
        case CommandKind::Host:
            return "host";
        case CommandKind::SwitchBackend:
            return "switchBackend";
        case CommandKind::ResetPrefs:
            return "resetPrefs";
        case CommandKind::SetLocale:
            return "setLocale";
        case CommandKind::SetContext:
            return "setContext";
        case CommandKind::Notify:
            return "notify";
        case CommandKind::Seq:
            return "seq";
        case CommandKind::All:
            return "all";
        case CommandKind::If:
            return "if";
        case CommandKind::CancelClose:
            return "cancelClose";
        case CommandKind::Retry:
            return "retry";
        case CommandKind::SignIn:
            return "signIn";
        case CommandKind::SignOut:
            return "signOut";
        case CommandKind::Upload:
            return "upload";
        case CommandKind::Download:
            return "download";
        default:
            return "unknown";
    }
}

}  // namespace detail

namespace detail {

[[nodiscard]] std::string failureText(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& failure) {
        return failure.what();
    } catch (...) {
        return "unknown error";
    }
}

namespace {

/// @brief A failure's action refusal, as a document reads it.
/// @param error The failure.
/// @return `{"code": c, "args": [{"name": n, "value": v}, ...]}` when it is an `ActionRefusal`, else `null`.
[[nodiscard]] Value refusalValue(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (model::ActionRefusal const& refusal) {
        ValueList args;
        for (model::RefusalArg const& arg : refusal.args()) {
            args.emplace_back(ValueRecord{{"name", Value{arg.name}}, {"value", Value{arg.value}}});
        }
        return Value{ValueRecord{{"code", Value{refusal.code()}}, {"args", Value{std::move(args)}}}};
    } catch (...) {
        return Value{};
    }
}

}  // namespace

// NOLINTNEXTLINE(modernize-use-string-view): callers keep the kind as an owned string, in a reply's `kind`.
[[nodiscard]] std::string defaultErrorKind(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (backend::DisconnectedError const&) {
        return "offline";
    } catch (backend::TimeoutError const&) {
        return "timeout";
    } catch (backend::ClientTimeoutError const&) {
        return "timeout";
    } catch (backend::BackendChangedError const&) {
        return "backendChanged";
    } catch (model::ActionRefusal const&) {
        // An action's own refusal is an answer about its data, whatever its message says, never the authoriser's.
        return "error";
    } catch (wire::UnauthorizedError const&) {
        // A refused control request (register, attach): its text carries the request's name before the refusal.
        return "denied";
    } catch (std::runtime_error const& refused) {
        // The authoriser says no to an execute with this one text; no exception type tells it from another failure.
        return std::string_view{refused.what()} == wire::kUnauthorizedMessage ? "denied" : "error";
    } catch (...) {
        return "error";
    }
}

[[nodiscard]] std::string canonicalJson(Value const& value) {
    return glz::write_json(valueToJson(value)).value_or(std::string{});
}

namespace {

/// @brief A value with the field at @p path replaced, creating records along the way.
/// @param base  The value written into.
/// @param path  The field path.
/// @param value The new value at the path.
/// @return The new value.
// NOLINTNEXTLINE(misc-no-recursion): one level per path segment
[[nodiscard]] Value withPath(Value const& base, std::span<std::string const> path, Value value) {
    if (path.empty()) {
        return value;
    }
    ValueRecord fields;
    if (ValueRecord const* const record = base.asRecord()) {
        fields = *record;
    }
    Value const inner = base.field(path.front()) != nullptr ? *base.field(path.front()) : Value{};
    fields.insert_or_assign(path.front(), withPath(inner, path.subspan(1), std::move(value)));
    return Value{std::move(fields)};
}

/// @brief A row's identity for `patch` and `optimistic`: its `id` field.
/// @param row The row.
/// @return The `id` field, or `null`.
[[nodiscard]] Value rowId(Value const& row) {
    Value const* const found = row.field("id");
    return found == nullptr ? Value{} : *found;
}

}  // namespace

[[nodiscard]] Value mergeRow(Value const& row, Value const& with) {
    ValueRecord const* const fields = row.asRecord();
    ValueRecord const* const over = with.asRecord();
    if (fields == nullptr || over == nullptr) {
        return with;
    }
    ValueRecord merged = *fields;
    for (auto const& [name, field] : *over) {
        merged.insert_or_assign(name, field);
    }
    return Value{std::move(merged)};
}

[[nodiscard]] std::expected<Value, PatchMiss> patchRows(
    Value const& value, std::string_view listMember,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a list, a key and a row are distinct roles of one type.
    Value const& key, Value const& with) {
    ValueRecord const* const record = listMember.empty() ? nullptr : value.asRecord();
    if (!listMember.empty() && (record == nullptr || !record->contains(listMember))) {
        return std::unexpected(PatchMiss::NoList);
    }
    Value const* const rows = record == nullptr ? &value : &record->find(listMember)->second;
    ValueList const* const items = rows->asList();
    if (items == nullptr) {
        return std::unexpected(PatchMiss::NoList);
    }
    if (key.isNull()) {
        return std::unexpected(PatchMiss::NoRow);
    }
    ValueList out = *items;
    bool found = false;
    for (Value& row : out) {
        if (rowId(row) == key) {
            row = mergeRow(row, with);
            found = true;
        }
    }
    if (!found) {
        return std::unexpected(PatchMiss::NoRow);
    }
    if (record == nullptr) {
        return Value{std::move(out)};
    }
    ValueRecord patched = *record;
    patched.insert_or_assign(std::string{listMember}, Value{std::move(out)});
    return Value{std::move(patched)};
}

namespace {

/// @brief What a patch that found no list reports.
/// @param query      The query it patches.
/// @param listMember The record member it names; empty when none.
/// @return The diagnostic's reason.
[[nodiscard]] std::string patchMissReason(std::string_view query, std::string_view listMember) {
    if (listMember.empty()) {
        return "the value of `" + std::string{query} +
               "` is not a list, so the patch changes nothing; `at` names the record member that holds the list";
    }
    return "the value of `" + std::string{query} + "` has no list at `" + std::string{listMember} +
           "`, so the patch changes nothing";
}

}  // namespace

// NOLINTNEXTLINE(misc-no-recursion): one level per nested list or record of the value.
[[nodiscard]] Value withoutFields(Value const& value, std::vector<std::string> const& fields) {
    if (fields.empty()) {
        return value;
    }
    if (ValueRecord const* const record = value.asRecord()) {
        ValueRecord kept = *record;
        for (std::string const& field : fields) {
            kept.erase(field);
        }
        return Value{std::move(kept)};
    }
    if (ValueList const* const items = value.asList()) {
        ValueList kept;
        kept.reserve(items->size());
        for (Value const& item : *items) {
            kept.push_back(withoutFields(item, fields));
        }
        return Value{std::move(kept)};
    }
    return value;
}

namespace {

/// @brief The members of a catalog record path: `"header.rows"` is `header`, then `rows`; `""` is none.
/// @param path The path.
/// @return Its members in order.
[[nodiscard]] std::vector<std::string> pathMembers(std::string_view path) {
    std::vector<std::string> members;
    for (auto const part : std::views::split(path, '.')) {
        if (!part.empty()) {
            members.emplace_back(part.begin(), part.end());
        }
    }
    return members;
}

/// @brief The rows of a keyed record that a value holds at a path.
///
/// A list on the way is looked through, as the result walker looks through a `std::vector`. At the path's end a
/// record with the key field is a row, and a record without it is a map whose values are the rows.
/// @param value    The value.
/// @param path     The members still to follow.
/// @param keyField The record's key field.
/// @param out      Where the rows go.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
void keyedRowsAt(Value const& value, std::span<std::string const> path, std::string_view keyField,
                 std::vector<Value const*>& out) {
    if (ValueList const* const items = value.asList()) {
        for (Value const& item : *items) {
            keyedRowsAt(item, path, keyField, out);
        }
        return;
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr) {
        return;
    }
    if (!path.empty()) {
        if (auto const found = record->find(path.front()); found != record->end()) {
            keyedRowsAt(found->second, path.subspan(1), keyField, out);
        }
        return;
    }
    if (record->contains(keyField)) {
        out.push_back(&value);
        return;
    }
    for (auto const& [name, item] : *record) {
        static_cast<void>(name);
        if (ValueRecord const* const row = item.asRecord(); row != nullptr && row->contains(keyField)) {
            out.push_back(&item);
        }
    }
}

/// @brief Merges new rows over the rows with the same key that a value holds at a path.
/// @param value    The value.
/// @param path     The members still to follow.
/// @param keyField The record's key field.
/// @param rows     The new rows.
/// @return The new value, or nothing when no row matched.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
[[nodiscard]] std::optional<Value> patchKeyedRows(Value const& value, std::span<std::string const> path,
                                                  std::string_view keyField, RowsByKey const& rows) {
    if (ValueList const* const items = value.asList()) {
        ValueList out = *items;
        bool changed = false;
        for (Value& item : out) {
            if (auto patched = patchKeyedRows(item, path, keyField, rows)) {
                item = std::move(*patched);
                changed = true;
            }
        }
        return changed ? std::optional<Value>{Value{std::move(out)}} : std::nullopt;
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr) {
        return std::nullopt;
    }
    if (!path.empty()) {
        auto const found = record->find(path.front());
        if (found == record->end()) {
            return std::nullopt;
        }
        auto patched = patchKeyedRows(found->second, path.subspan(1), keyField, rows);
        if (!patched) {
            return std::nullopt;
        }
        ValueRecord out = *record;
        out.insert_or_assign(path.front(), std::move(*patched));
        return Value{std::move(out)};
    }
    if (auto const key = record->find(keyField); key != record->end()) {
        auto const row = rows.find(canonicalJson(key->second));
        return row == rows.end() ? std::nullopt : std::optional<Value>{mergeRow(value, *row->second)};
    }
    ValueRecord out = *record;
    bool changed = false;
    for (auto& [name, item] : out) {
        static_cast<void>(name);
        if (ValueRecord const* const row = item.asRecord(); row != nullptr && row->contains(keyField)) {
            if (auto patched = patchKeyedRows(item, {}, keyField, rows)) {
                item = std::move(*patched);
                changed = true;
            }
        }
    }
    return changed ? std::optional<Value>{Value{std::move(out)}} : std::nullopt;
}

/// @brief Whether a member at a path is set: a non-empty list, a record, `true`, or a number other than zero.
///
/// A list on the way is looked through: the member is set when it is set in any element.
/// @param value The value.
/// @param path  The members still to follow.
/// @return `true` when set.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
[[nodiscard]] bool memberSetAt(Value const& value, std::span<std::string const> path) {
    if (!path.empty()) {
        if (ValueList const* const items = value.asList()) {
            return std::ranges::any_of(*items, [&](Value const& item) { return memberSetAt(item, path); });
        }
        ValueRecord const* const record = value.asRecord();
        if (record == nullptr) {
            return false;
        }
        auto const found = record->find(path.front());
        return found != record->end() && memberSetAt(found->second, path.subspan(1));
    }
    if (ValueList const* const items = value.asList()) {
        return !items->empty();
    }
    if (value.asRecord() != nullptr) {
        return true;
    }
    if (bool const* const flag = value.asBool()) {
        return *flag;
    }
    if (std::int64_t const* const number = value.asInt()) {
        return *number != 0;
    }
    if (Decimal const* const decimal = value.asDecimal()) {
        return !decimal->value.isZero();
    }
    return false;
}

/// @brief Whether any member at @p paths is set in @p value.
/// @param value The value.
/// @param paths The members' paths.
/// @return `true` when one is.
[[nodiscard]] bool anyMemberSet(Value const& value, std::vector<std::string> const& paths) {
    return std::ranges::any_of(
        paths, [&](std::string const& path) { return memberSetAt(value, pathMembers(path)); });
}

}  // namespace

[[nodiscard]] std::vector<Value const*> keyedRowsOf(Value const& value, std::vector<std::string> const& paths,
                                                    std::string_view keyField) {
    std::vector<Value const*> rows;
    for (std::string const& path : paths) {
        std::vector<std::string> const members = pathMembers(path);
        keyedRowsAt(value, members, keyField, rows);
    }
    return rows;
}

[[nodiscard]] std::optional<Value> patchHeldRows(Value const& value, std::vector<std::string> const& paths,
                                                 std::string_view keyField, RowsByKey const& rows) {
    std::optional<Value> patched;
    for (std::string const& path : paths) {
        std::vector<std::string> const members = pathMembers(path);
        auto next = patched ? patchKeyedRows(*patched, members, keyField, rows)
                            : patchKeyedRows(value, members, keyField, rows);
        if (next) {
            patched = std::move(next);
        }
    }
    return patched;
}

}  // namespace detail

std::shared_ptr<ModelChannel> BridgeDispatcher::bind(std::string const& typeId, bridge::BindSharing sharing,
                                                     std::string const& instanceKey) {
    return std::make_shared<Channel>(*_bridge, _cbExec, _evaluator, typeId, sharing, instanceKey);
}

async::Completion<std::string> BridgeDispatcher::Channel::execute(std::string const& actionId,
                                                                  std::string const& bodyJson, CallOptions options) {
    if (options.evaluate) {
        if (_evaluator) {
            return _evaluator(_typeId, actionId, bodyJson);
        }
        auto [refused, promise] = async::Completion<std::string>::makeSettleable(_cbExec);
        promise.reject(std::make_exception_ptr(std::runtime_error{"evaluate needs the UI service"}));
        return std::move(refused);
    }
    if (options.stop.stop_possible()) {
        return _handler.execute(actionId, bodyJson, std::move(options.stop));
    }
    return _handler.execute(actionId, bodyJson);
}

ChannelFacts BridgeDispatcher::Channel::facts() const {
    return ChannelFacts{.instanceId = _handler.instanceId(), .primary = _handler.primary()};
}
// NOLINTBEGIN(performance-unnecessary-value-param,bugprone-easily-swappable-parameters): every default below ignores
// its arguments, so each keeps the parameter types of the interface it implements.
bool InterpreterHost::navigate(NavigateRequest const& request, std::function<void(Value)> onResult,
                               std::function<void()> onCancel) {
    static_cast<void>(request);
    static_cast<void>(onResult);
    static_cast<void>(onCancel);
    return false;
}

bool InterpreterHost::closeSelf(std::string const& screen, std::optional<Value> result) {
    static_cast<void>(screen);
    static_cast<void>(result);
    return false;
}

bool InterpreterHost::hostCommand(std::string const& name, Value const& args) {
    static_cast<void>(name);
    static_cast<void>(args);
    return false;
}

bool InterpreterHost::switchBackend(std::string const& name,
                                    std::function<void(std::expected<void, std::string>)> done) {
    static_cast<void>(name);
    static_cast<void>(done);
    return false;
}

void InterpreterHost::resetPrefs(std::string const& screen, std::string const& point) {
    static_cast<void>(screen);
    static_cast<void>(point);
}

bool InterpreterHost::notify(std::string const& text, std::string const& role) {
    static_cast<void>(text);
    static_cast<void>(role);
    return false;
}

bool InterpreterHost::signIn(Value const& reply) {
    static_cast<void>(reply);
    return false;
}

bool InterpreterHost::setAppValue(std::string const& name, Value const& value) {
    static_cast<void>(name);
    static_cast<void>(value);
    return false;
}

bool InterpreterHost::upload(Value const& file, std::function<void(Value)> progress,
                             std::function<void(std::expected<Value, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(progress);
    static_cast<void>(done);
    return false;
}

bool InterpreterHost::uploadFor(UploadRequest const& request, std::function<void(Value)> progress,
                                std::function<void(std::expected<Value, std::string>)> done) {
    return upload(request.file, std::move(progress), std::move(done));
}

bool InterpreterHost::download(Value const& file, std::string const& option, Value const& name,
                               std::function<void(std::expected<Value, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(option);
    static_cast<void>(name);
    static_cast<void>(done);
    return false;
}

bool InterpreterHost::readFile(Value const& file, std::size_t maxBytes,
                               std::function<void(std::expected<InlineFile, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(maxBytes);
    static_cast<void>(done);
    return false;
}

bool InterpreterHost::saveFile(InlineFile const& file, std::string const& option,
                               std::function<void(std::expected<Value, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(option);
    static_cast<void>(done);
    return false;
}

void InterpreterHost::preferenceChanged(std::string const& screen, std::string const& point, Value const& value) {
    static_cast<void>(screen);
    static_cast<void>(point);
    static_cast<void>(value);
}
// NOLINTEND(performance-unnecessary-value-param,bugprone-easily-swappable-parameters)

bool Session::can(std::string_view action) const {
    static constexpr std::string_view kHostPrefix = "host:";
    if (action.starts_with(kHostPrefix)) {
        return _hostCommandNames.get().contains(action.substr(kHostPrefix.size()));
    }
    if (action == "context") {
        return _contextOffered.get();
    }
    return _permitted.get().contains(action);
}

void Session::setFormatLocale(std::string tag) {
    _formatLocale = std::move(tag);
    applyFacts(_format.peek()->locale);
}

std::shared_ptr<FormatContext const> Session::scopedFormatting(std::string_view tag) const {
    FormatContext next = *_format.get();
    LocaleFacts const& facts = localeFacts(tag);
    next.locale = std::string{tag};
    next.numeric = facts.numeric;
    next.currency = facts.currency;
    next.datePattern = _declaredDate.empty() ? std::string{facts.datePattern} : _declaredDate;
    next.dateTimePattern = _declaredDateTime.empty() ? std::string{facts.dateTimePattern} : _declaredDateTime;
    return std::make_shared<FormatContext const>(std::move(next));
}

bool Session::firstUnlistedLocale(std::string const& tag) {
    return findLocaleFacts(tag) == nullptr && _unlistedLocales.insert(tag).second;
}

void Session::setFormatting(FormatContext format) {
    // One batch, so an effect never sees the new `env.locale` beside the old facts, and runs once.
    _runtime->batch([this, &format] {
        setEnvironmentValue("locale", Value{format.locale});
        _format.set(std::make_shared<FormatContext const>(std::move(format)));
    });
}

void Session::setDatePatterns(std::string date, std::string dateTime) {
    _declaredDate = std::move(date);
    _declaredDateTime = std::move(dateTime);
    FormatContext next = *_format.peek();
    next.datePattern = _declaredDate.empty() ? _localeDate : _declaredDate;
    next.dateTimePattern = _declaredDateTime.empty() ? _localeDateTime : _declaredDateTime;
    setFormatting(std::move(next));
}

void Session::setTranslations(render::TranslationProvider translations) {
    FormatContext next = *_format.peek();
    next.translations = std::move(translations);
    setFormatting(std::move(next));
}

void Session::setEnvironment(ValueRecord values) {
    values.insert_or_assign("locale", Value{_format.peek()->locale});
    _env.set(std::move(values));
}

void Session::setEnvironmentValue(std::string const& name, Value value) {
    ValueRecord env = _env.peek();
    env.insert_or_assign(name, std::move(value));
    _env.set(std::move(env));
}

void Session::setHostCommands(HostCommands commands) {
    _hostCommandRunner = std::move(commands.run);
    _hostCommandNames.set(std::move(commands.available));
}

std::function<HostCommandResult(std::string const&, Value const&)> const& Session::hostCommandRunner() const noexcept {
    return _hostCommandRunner;
}

Value Session::client(ExprKind form, std::string_view name) const {
    ValueRecord const& values = [&] -> ValueRecord const& {
        if (form == ExprKind::Env) {
            return _env.get();
        }
        return form == ExprKind::Session ? _sessionValues.get() : _hostValues.get();
    }();
    auto const found = values.find(name);
    return found == values.end() ? Value{} : found->second;
}

void Session::clear() {
    _runtime->batch([this] {
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            ++entry->generation;
            entry->evict.cancel();
            countTopics(*entry, -1);
        }
        _entries.clear();
        for (detail::QueryRuntime* const query : _mounted) {
            detachQuery(*query);
        }
    });
}

void Session::prefetch(std::shared_ptr<ModelChannel> const& channel, std::string const& token,
                       std::string const& typeId, std::string const& action, Value const& body) {
    auto const entry = acquire(token, typeId, action, false, body);
    if (!entry->pending.peek() && (!entry->hasValue.peek() || entry->stale)) {
        fetch(entry, channel);
    }
    release(entry);
}

void Session::asUser(std::function<void()> const& body) {
    ++_userDepth;
    try {
        body();
    } catch (...) {
        --_userDepth;
        throw;
    }
    --_userDepth;
}

std::string Session::cacheKey(std::string const& token, std::string const& typeId, std::string const& action,
                              bool evaluate, std::string const& body) {
    return typeId + '\x1f' + token + '\x1f' + (evaluate ? "evaluate:" : "") + action + '\x1f' + body;
}

std::shared_ptr<detail::CacheEntry> Session::acquire(std::string const& token, std::string const& typeId,
                                                     std::string const& action, bool evaluate, Value const& body) {
    std::string const text = detail::canonicalJson(body);
    std::string key = cacheKey(token, typeId, action, evaluate, text);
    auto found = _entries.find(key);
    if (found == _entries.end()) {
        auto entry = std::make_shared<detail::CacheEntry>(*_runtime);
        entry->key = key;
        entry->typeId = typeId;
        entry->action = action;
        entry->body = text;
        entry->evaluate = evaluate;
        entry->instanceToken = token;
        if (_options.catalog.action) {
            if (auto const info = _options.catalog.action(action)) {
                entry->secretFields = info->secretFields;
                if (!evaluate) {
                    entry->records = info->records;
                }
            }
        }
        countTopics(*entry, +1);
        found = _entries.emplace(std::move(key), std::move(entry)).first;
    }
    ++found->second->users;
    found->second->evict.cancel();
    return found->second;
}

void Session::release(std::shared_ptr<detail::CacheEntry> const& entry) {
    if (--entry->users > 0 || _options.scheduler == nullptr) {
        return;
    }
    std::weak_ptr<detail::CacheEntry> const weak = entry;
    entry->evict = _options.scheduler->after(_options.keepUnused, [this, weak] {
        auto const held = weak.lock();
        if (held && held->users == 0) {
            countTopics(*held, -1);
            _entries.erase(held->key);
            flushWatches();
        }
    });
}

void Session::fetch(std::shared_ptr<detail::CacheEntry> const& entry, std::shared_ptr<ModelChannel> const& channel) {
    // The watch goes out before the read, on the same connection, so a change committed after the read is
    // seen as an event.
    flushWatches();
    std::uint64_t const generation = ++entry->generation;
    if (channel == nullptr) {
        settleFailure(*entry, "the model alias has no instance", "error");
        return;
    }
    entry->pending.set(true);
    std::weak_ptr<detail::CacheEntry> const weak = entry;
    try {
        auto completion = channel->execute(entry->action, entry->body, CallOptions{.evaluate = entry->evaluate});
        completion
            .then([this, weak, generation](std::string const& reply) {
                auto const held = weak.lock();
                if (held && held->generation == generation) {
                    settleReply(*held, reply);
                }
            })
            .onError([this, weak, generation](std::exception_ptr const& error) {
                auto const held = weak.lock();
                if (held && held->generation == generation) {
                    settleFailure(*held, detail::failureText(error), classify(error), detail::refusalValue(error));
                }
            });
    } catch (...) {
        std::exception_ptr const error = std::current_exception();
        settleFailure(*entry, detail::failureText(error), classify(error), detail::refusalValue(error));
    }
}

void Session::confirm(detail::CacheEntry& entry, Value const& confirmed) {
    entry.confirmed = detail::withoutFields(confirmed, entry.secretFields);
    showLayers(entry);
}

void Session::showLayers(detail::CacheEntry& entry) {
    Value shown = entry.confirmed;
    for (detail::OptimisticLayer const& layer : entry.layers) {
        if (auto patched = detail::patchRows(shown, layer.at, layer.key, layer.with)) {
            shown = std::move(*patched);
        }
    }
    noteWrite();
    entry.value.set(std::move(shown));
}

std::string Session::classify(std::exception_ptr const& error) const {
    return _options.classify ? _options.classify(error) : detail::defaultErrorKind(error);
}

std::chrono::milliseconds Session::now() const {
    if (_options.clock) {
        return _options.clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

void Session::markStale(detail::CacheEntry& entry) {
    entry.stale = true;
    if (entry.pending.peek()) {
        entry.refetchAfter = true;
    }
}

SessionOptions Session::checkedOptions(SessionOptions options) {
    if (options.maxInlineFileBytes == 0 || options.maxInlineFileBytes > kMaxInlineFileBytes) {
        throw std::invalid_argument{"morph::ui::Session: maxInlineFileBytes is " +
                                    std::to_string(options.maxInlineFileBytes) + "; it is 1 to " +
                                    std::to_string(kMaxInlineFileBytes) + " (kMaxInlineFileBytes)"};
    }
    return options;
}

void Session::settleReply(detail::CacheEntry& entry, std::string const& reply) {
    // A table's page holds its record opaquely, and its Number cells are numbers with fractions, which the strict
    // rule refuses; every other reply keeps the strict rule.
    bool const opaque = std::ranges::any_of(entry.records, &ActionRecordInfo::opaque);
    auto json = parseJson(reply);
    auto value = json ? (opaque ? detail::tableValueFromJson(*json) : valueFromJson(*json))
                      : std::unexpected(json.error());
    if (!value) {
        settleFailure(entry, "the reply is not a document value: " + value.error(), "error");
        return;
    }
    _runtime->batch([&] {
        confirm(entry, *value);
        entry.hasValue.set(true);
        entry.error.set({});
        entry.errorKind.set({});
        entry.refusal.set(Value{});
        entry.pending.set(false);
        entry.fetchedAt = now();
        entry.stale = entry.refetchAfter;
        if (entry.refetchAfter) {
            entry.refetchAfter = false;
            refetchMounted(entry);
        }
    });
}

void Session::settleFailure(detail::CacheEntry& entry, std::string text, std::string kind, Value refusal) {
    _runtime->batch([&] {
        noteWrite();
        entry.error.set(std::move(text));
        entry.errorKind.set(std::move(kind));
        entry.refusal.set(std::move(refusal));
        entry.pending.set(false);
    });
}

void Session::applyFacts(std::string locale) {
    LocaleFacts const& facts = localeFacts(_formatLocale.empty() ? locale : _formatLocale);
    _localeDate = std::string{facts.datePattern};
    _localeDateTime = std::string{facts.dateTimePattern};
    FormatContext next = *_format.peek();
    next.locale = std::move(locale);
    next.numeric = facts.numeric;
    next.currency = facts.currency;
    next.datePattern = _declaredDate.empty() ? _localeDate : _declaredDate;
    next.dateTimePattern = _declaredDateTime.empty() ? _localeDateTime : _declaredDateTime;
    setFormatting(std::move(next));
}

Screen::Screen() = default;

Screen::~Screen() {
    try {
        if (Commands const* const commands = lifecycleList("onUnmount")) {
            runCommands(*commands);
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
    }
    _core->focusElement = {};
    _mounted.reset();
    _headActions.clear();
    _headStatus.clear();
    _headMenu.clear();
    _headTools.clear();
    _scope.reset();
}

Widget& Screen::root() const {
    if (!_mounted) {
        throw std::logic_error{"the screen is not mounted"};
    }
    return _mounted->root();
}

Value Screen::read(Json const& expression) const {
    auto parsed = parseExpression(expression);
    if (!parsed) {
        return Value{};
    }
    return detail::evalIn(*_scope, nullptr, *parsed);
}

auto Screen::boundAliases() const -> std::vector<BoundAlias> {
    std::vector<BoundAlias> bound;
    for (auto const& alias : _scope->models) {
        if (alias->channel == nullptr) {
            continue;
        }
        ChannelFacts facts = alias->channel->facts();
        if (facts.instanceId != 0) {
            bound.push_back(BoundAlias{.decl = alias->decl, .facts = std::move(facts)});
        }
    }
    return bound;
}

std::vector<HeadBadge> Screen::status() const {
    std::vector<HeadBadge> shown;
    for (detail::BadgeModel const& badge : _headStatus) {
        if (badge.visible.evaluate()) {
            shown.push_back(HeadBadge{
                .text = badge.text.evaluate(),
                .tone = badge.tone.evaluate(),
                .icon = badge.icon.evaluate(),
            });
        }
    }
    return shown;
}

std::vector<HeadAction> Screen::actions() const {
    std::vector<HeadAction> shown;
    for (detail::ActionModel const& action : _headActions) {
        if (action.visible.evaluate()) {
            shown.push_back(HeadAction{
                .id = action.id,
                .label = action.label.evaluate(),
                .icon = action.icon.evaluate(),
                .iconOnly = action.iconOnly.evaluate(),
                .emphasis = action.emphasis,
                .enabled = action.enabled.evaluate(),
            });
        }
    }
    return shown;
}

bool Screen::runAction(std::string_view actionId) {
    auto const found = std::ranges::find(_headActions, actionId, &detail::ActionModel::id);
    if (found == _headActions.end()) {
        _core->report("/actions", "head action " + std::string{actionId} + " is unknown");
        return false;
    }
    auto const [visible, enabled] =
        runtime().untracked([&] { return std::pair{found->visible.evaluate(), found->enabled.evaluate()}; });
    if (!visible || !enabled) {
        _core->report(found->path, "head action " + found->id + (visible ? " is disabled" : " is hidden"));
        return false;
    }
    if (found->run) {
        found->run();
    }
    return true;
}

std::string Screen::menuLabel() const {
    if (!_core->doc().menuLabel) {
        return "More actions";
    }
    return detail::textOf(detail::evalIn(*_scope, nullptr, _core->doc().menuLabel), _scope->formatting());
}

bool Screen::chooseMenuEntry(std::string_view entryId) {
    FoundEntry const found = runtime().untracked([&] { return findHeadEntry(_headMenu, entryId, FoundEntry{}); });
    if (found.model == nullptr) {
        _core->report("/menu", "head menu entry " + std::string{entryId} + " is unknown");
        return false;
    }
    if (!found.visible || !found.enabled) {
        _core->report(found.model->path,
                      "head menu entry " + found.model->id + (found.visible ? " is disabled" : " is hidden"));
        return false;
    }
    if (found.model->item.onSelect) {
        found.model->item.onSelect();
    }
    return true;
}

std::vector<HeadTool> Screen::tools() const {
    std::vector<HeadTool> shown;
    for (detail::HeadToolModel const& tool : _headTools) {
        if (!tool.visible.evaluate()) {
            continue;
        }
        bool const search = tool.kind == HeadToolKind::Search;
        shown.push_back(HeadTool{
            .id = tool.id,
            .kind = tool.kind,
            .label = tool.label.evaluate(),
            .value = search ? tool.value.evaluate() : std::string{},
            .placeholder = search ? tool.placeholder.evaluate() : std::string{},
            .options = search ? std::vector<SelectOption>{} : tool.options.evaluate(),
            .selected = search ? std::nullopt : tool.selected.evaluate(),
            .width = tool.width,
            .enabled = tool.enabled.evaluate(),
        });
    }
    return shown;
}

bool Screen::editTool(std::string_view toolId, Value const& value) {
    detail::HeadToolModel const* const tool = usableTool(toolId);
    if (tool == nullptr) {
        return false;
    }
    if (tool->kind == HeadToolKind::Search && value.asString() == nullptr) {
        _core->report(tool->path, "head tool " + tool->id + " is a search, edited with text");
        return false;
    }
    tool->edit(value);
    return true;
}

bool Screen::submitTool(std::string_view toolId) {
    detail::HeadToolModel const* const tool = usableTool(toolId);
    if (tool == nullptr) {
        return false;
    }
    if (tool->kind != HeadToolKind::Search) {
        _core->report(tool->path, "head tool " + tool->id + " is a select, which is not submitted");
        return false;
    }
    tool->submit(Value{runtime().untracked([&] { return tool->value.evaluate(); })});
    return true;
}

bool Screen::dirty() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().dirty);
    bool const* const flag = value.asBool();
    return flag != nullptr && *flag;
}

bool Screen::requestClose() {
    std::shared_ptr<bool const> const cancelled = beginClose();
    return cancelled == nullptr || !*cancelled;
}

std::shared_ptr<bool const> Screen::beginClose() {
    Commands const* const commands = lifecycleList("onClose");
    if (commands == nullptr) {
        return nullptr;
    }
    return runCommandsFlag(*commands);
}

void Screen::runLifecycle(std::string_view key) {
    if (Commands const* const commands = lifecycleList(key)) {
        runCommands(*commands);
    }
}

bool Screen::setPreference(std::string const& point, Value value) {
    auto const found = _core->doc().customize.find(point);
    if (found == _core->doc().customize.end() || !fits(found->second, value)) {
        return false;
    }
    ValueRecord prefs = _core->prefs.peek();
    prefs.insert_or_assign(point, std::move(value));
    _core->prefs.set(std::move(prefs));
    return true;
}

std::map<std::string, detail::ScreenCore::KeptState, std::less<>> Screen::ownState() const {
    std::map<std::string, detail::ScreenCore::KeptState, std::less<>> states;
    for (auto const& slot : _scope->states) {
        if (slot->value) {
            states.insert_or_assign(slot->decl->name, detail::ScreenCore::KeptState{
                                                          .type = slot->decl->type.name(),
                                                          .value = slot->value->peek(),
                                                      });
        }
    }
    return states;
}

Commands const* Screen::lifecycleList(std::string_view key) const {
    auto const found = _core->doc().lifecycle.find(key);
    return found == _core->doc().lifecycle.end() ? nullptr : &found->second;
}

std::shared_ptr<bool const> Screen::runCommandsFlag(Commands const& commands) {
    detail::CommandCtx const ctx{
        .scope = _scope,
        .bindings = std::make_shared<detail::Bindings>(),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = nullptr,
    };
    _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
    return ctx.cancelClose;
}

bool Screen::fits(CustomizePoint const& point, Value const& value) {
    if (point.kind == "choice") {
        std::string const* const text = value.asString();
        return text != nullptr && listed(std::span<std::string const>{point.options}, *text);
    }
    if (point.kind == "value") {
        return point.type.admits(value);
    }
    return true;
}

bool Screen::listed(std::span<std::string const> options, std::string const& text) {
    return std::ranges::find(options, text) != options.end();
}

detail::HeadToolModel const* Screen::usableTool(std::string_view toolId) const {
    auto const found = std::ranges::find(_headTools, toolId, &detail::HeadToolModel::id);
    if (found == _headTools.end()) {
        _core->report("/tools", "head tool " + std::string{toolId} + " is unknown");
        return nullptr;
    }
    auto const [visible, enabled] =
        runtime().untracked([&] { return std::pair{found->visible.evaluate(), found->enabled.evaluate()}; });
    if (!visible || !enabled) {
        _core->report(found->path, "head tool " + found->id + (visible ? " is disabled" : " is hidden"));
        return nullptr;
    }
    return &*found;
}
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
auto Screen::findHeadEntry(std::vector<detail::HeadMenuModel> const& level, std::string_view entryId,
                           FoundEntry const& outer) -> FoundEntry {
    for (detail::HeadMenuModel const& model : level) {
        FoundEntry const here{
            .model = &model,
            .visible = outer.visible && model.item.visible.evaluate(),
            .enabled = outer.enabled && model.item.enabled.evaluate(),
        };
        if (!model.id.empty() && model.id == entryId) {
            return here;
        }
        if (FoundEntry const inner = findHeadEntry(model.items, entryId, here); inner.model != nullptr) {
            return inner;
        }
    }
    return FoundEntry{};
}
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<HeadMenuEntry> Screen::shownHeadMenu(std::vector<detail::HeadMenuModel> const& level) {
    std::vector<HeadMenuEntry> shown;
    for (detail::HeadMenuModel const& model : level) {
        MenuItem const& item = model.item;
        if (item.separator) {
            if (!shown.empty() && !shown.back().separator) {
                shown.push_back(HeadMenuEntry{.separator = true});
            }
            continue;
        }
        if (!item.visible.evaluate()) {
            continue;
        }
        shown.push_back(HeadMenuEntry{
            .id = model.id,
            .label = item.label.evaluate(),
            .icon = item.icon,
            .keys = item.keys,
            .checked = item.checked.evaluate(),
            .enabled = item.enabled.evaluate(),
            .items = shownHeadMenu(model.items),
            .separator = false,
            .description = item.description.evaluate(),
        });
    }
    if (!shown.empty() && shown.back().separator) {
        shown.pop_back();
    }
    return shown;
}

AppRoot::~AppRoot() {
    try {
        if (auto const found = _core->doc().lifecycle.find("onUnmount"); found != _core->doc().lifecycle.end()) {
            runCommands(found->second);
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
    }
    // The counts read the scope, so they go before it.
    _navigationBadges.reset();
    _navigation.clear();
    _core->session.setAppScope(nullptr);
    _scope.reset();
}

reactive::Computed<std::vector<NavigationBadge>> const& AppRoot::navigationBadges() const {
    return *_navigationBadges;
}

PrincipalPlacement AppRoot::principalPlacement() const noexcept {
    return _core->doc().principalPlacement == "head" ? PrincipalPlacement::Head : PrincipalPlacement::Sidebar;
}

Value AppRoot::read(Json const& expression) const {
    auto parsed = parseExpression(expression);
    if (!parsed) {
        return Value{};
    }
    return detail::evalIn(*_scope, nullptr, *parsed);
}

auto AppRoot::query(std::string_view name) const -> QueryRead {
    detail::NameRef const* const found = _scope->find(name);
    if (found == nullptr || found->category != detail::NameCategory::Query) {
        return QueryRead{};
    }
    detail::QueryRuntime const& runtime = *found->query;
    Value const error = runtime.error();
    Value const kind = runtime.errorKind();
    std::string const* const kindText = kind.asString();
    return QueryRead{
        .value = runtime.value(),
        .pending = runtime.pending(),
        .failed = !error.isNull(),
        .declared = true,
        .denied = kindText != nullptr && *kindText == "denied",
    };
}

bool AppRoot::setState(std::string_view name, Value value) {
    detail::NameRef const* const found = _scope->find(name);
    if (found == nullptr || found->category != detail::NameCategory::State) {
        return false;
    }
    detail::StateSlot& slot = *found->state;
    slot.scope->core.initState(*slot.scope, slot);
    _core->session.noteWrite();
    slot.value->set(std::move(value));
    return true;
}

bool AppRoot::runMutation(std::string_view name, Value body, std::function<void(bool succeeded)> done) {
    detail::NameRef const* const found = _scope->find(name);
    if (found == nullptr || found->category != detail::NameCategory::Mutation) {
        return false;
    }
    detail::MutationRuntime& mutation = *found->mutation;
    _core->runtime().batch([&] {
        mutation.run(std::move(body), std::make_shared<detail::Bindings>(),
                     [done = std::move(done)](bool succeeded, Value const& /*reply*/) {
                         if (done) {
                             done(succeeded);
                         }
                     });
    });
    return true;
}

void AppRoot::runCommands(Commands const& commands) {
    detail::CommandCtx const ctx{
        .scope = _scope,
        .bindings = std::make_shared<detail::Bindings>(),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = nullptr,
    };
    _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
}

namespace detail {

ActionRecordInfo const* CacheEntry::holding(std::string_view record) const {
    auto const found = std::ranges::find(records, record, &ActionRecordInfo::record);
    return found == records.end() ? nullptr : &*found;
}

void Marks::add(CacheEntry& entry) {
    if (std::ranges::find(entries, &entry) == entries.end()) {
        entries.push_back(&entry);
    }
}

CheckRuntime::~CheckRuntime() {
    for (ElementState* const target : targets) {
        std::erase(target->checks, this);
        target->checksVersion.set(target->checksVersion.peek() + 1);
    }
}

NameRef const* RtScope::find(std::string_view name) const {
    for (RtScope const* scope = this; scope != nullptr; scope = scope->parent.get()) {
        auto const found = scope->names.find(name);
        if (found != scope->names.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

reactive::Signal<Value> const* RtScope::innermostRow() const {
    for (RtScope const* scope = this; scope != nullptr; scope = scope->parent.get()) {
        if (scope->row != nullptr) {
            return scope->row;
        }
    }
    return nullptr;
}

RtScope const& RtScope::policyScope() const {
    RtScope const* scope = this;
    while (scope->submitted == nullptr && scope->parent != nullptr) {
        scope = scope->parent.get();
    }
    return *scope;
}

Value ScopeEnv::row(std::span<std::string const> path) const {
    if (_row != nullptr) {
        return walk(*_row, path);
    }
    reactive::Signal<Value> const* const current = _scope->innermostRow();
    return current == nullptr ? Value{} : walk(current->get(), path);
}

void QueryRuntime::invalidate() {
    if (auto const& current = _entry.peek()) {
        current->stale = true;
    }
    refetch();
}

void QueryRuntime::detach() {
    _entry.set(nullptr);
    _carried.set(Value{});
    _target = {};
}

bool QueryRuntime::Target::operator==(Target const& other) const {
    return idle == other.idle && token == other.token && bodyText == other.bodyText;
}

ScreenCore::ScreenCore(std::shared_ptr<Document const> doc, Session& owner, Dispatcher& calls,
                       InterpreterOptions const& options, MountOptions const& mount)
    : session{owner},
      dispatcher{calls},
      host{options.host},
      onDiagnostic{options.diagnostics},
      borrowed{mount.borrowed},
      depth{mount.depth},
      prefs{session.runtime()},
      submitted{session.runtime(), false},
      pendingWork{session.runtime(), 0},
      recording{owner.options().developmentTools == DevelopmentTools::On},
      _doc{std::move(doc)} {
    ValueRecord defaults;
    for (auto const& [name, point] : _doc->customize) {
        defaults.insert_or_assign(name, point.defaultValue);
    }
    prefDefaults = defaults;
    prefs.set(std::move(defaults));
    session.attachScreen(*this);
}

void ScreenCore::runLifecycleOf(std::string_view key) {
    auto const found = _doc->lifecycle.find(key);
    auto const scope = rootScope.lock();
    if (found == _doc->lifecycle.end() || !scope) {
        return;
    }
    CommandCtx const ctx{
        .scope = scope,
        .bindings = std::make_shared<Bindings>(),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = nullptr,
    };
    runList(found->second, ctx, {});
}

void ScreenCore::report(std::string_view path, std::string_view reason) {
    if (inspecting) {
        return;
    }
    std::string key = std::string{path} + '\x1f' + std::string{reason};
    if (!reported.insert(std::move(key)).second) {
        return;
    }
    Refusal const refusal{.document = _doc->id, .path = std::string{path}, .reason = std::string{reason}};
    diagnostics.push_back(refusal);
    if (onDiagnostic) {
        onDiagnostic(refusal);
    }
}

void ScreenCore::initState(RtScope const& scope, StateSlot& slot) const {
    if (slot.value || slot.initializing) {
        return;
    }
    if (std::optional<Value> keptValue = keptState(scope, *slot.decl)) {
        slot.value = std::make_unique<reactive::Signal<Value>>(runtime(), std::move(*keptValue));
        return;
    }
    slot.initializing = true;
    Value init = runtime().untracked([&] { return evalIn(scope, nullptr, slot.decl->init); });
    slot.value = std::make_unique<reactive::Signal<Value>>(runtime(), std::move(init));
    slot.initializing = false;
}

void ScreenCore::runEvent(Commands const* commands, LowerCtx const& ctx, Value event) {
    if (commands == nullptr || commands->empty()) {
        return;
    }
    auto bindings = std::make_shared<Bindings>(*ctx.bindings);
    bindings->push_back(Binding{.name = "event", .read = [event = std::move(event)] { return event; }});
    CommandCtx const command{
        .scope = ctx.scope,
        .bindings = std::move(bindings),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = ctx.boundary,
    };
    session.asUser([&] { runtime().batch([&] { runList(*commands, command, {}); }); });
}

std::optional<Value> ScreenCore::keptState(RtScope const& scope, StateDecl const& decl) const {
    if (keptStates.empty() || rootScope.lock().get() != &scope) {
        return std::nullopt;
    }
    auto const found = keptStates.find(decl.name);
    if (found == keptStates.end() || found->second.type != decl.type.name() ||
        !decl.type.admits(found->second.value)) {
        return std::nullopt;
    }
    return found->second.value;
}

bool ScreenCore::unsupported(Command const& command, std::string_view what) {
    report(command.path, std::string{what} + " is not supported by the host");
    return false;
}

Value ScreenCore::once(LowerCtx const& ctx, ExprPtr const& expr) const {
    return runtime().untracked([&] { return evalIn(*ctx.scope, ctx.bindings.get(), expr); });
}

void ScreenCore::logList(Commands const& commands) {
    CommandLogEntry entry{.event = {}, .commands = {}, .batch = runtime().core()->currentBatch()};
    std::string_view const first = commands.front().path;
    entry.event = std::string{first.substr(0, first.rfind('/') == std::string_view::npos ? 0 : first.rfind('/'))};
    for (Command const& command : commands) {
        entry.commands.push_back(
            LoggedCommand{.kind = commandKey(command.kind), .target = command.target, .path = command.path});
    }
    if (commandLog.size() == kCommandLogSize) {
        commandLog.pop_front();
    }
    commandLog.push_back(std::move(entry));
}

}  // namespace detail

void Session::backendChanged(std::string const& backend) {
    _runtime->batch([this, &backend] {
        std::vector<detail::ScreenCore*> const screens = _screens;
        for (detail::ScreenCore* const screen : screens) {
            if (std::ranges::find(_screens, screen) != _screens.end()) {
                screen->runLifecycleOf("onBackendChange");
            }
        }
        ValueRecord env = _env.peek();
        env.insert_or_assign("backend", Value{backend});
        _env.set(std::move(env));
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            markStale(*entry);
        }
        std::vector<detail::QueryRuntime*> const mounted = _mounted;
        for (detail::QueryRuntime* const query : mounted) {
            query->refetchCurrent();
        }
    });
}

void Session::refetchMounted(detail::CacheEntry const& entry) {
    std::vector<detail::QueryRuntime*> const mounted = _mounted;
    for (detail::QueryRuntime* const query : mounted) {
        if (_runtime->untracked([query] { return query->entry(); }).get() == &entry) {
            query->refetchCurrent();
        }
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a model type and an action id, as every caller names them.
void Session::applyReplyEffects(std::string const& typeId, std::string const& actionId, Value const& reply,
                                ReplyUse use, std::string const& instanceToken) {
    if (!_options.catalog.action) {
        return;
    }
    std::optional<ActionInfo> const info = _options.catalog.action(actionId);
    if (!info || !info->effects) {
        return;
    }
    detail::Marks marks;
    for (ActionEffect const& effect : *info->effects) {
        if (effect.kind == "changesInstance") {
            for (auto const& [key, entry] : _entries) {
                static_cast<void>(key);
                if (!entry->evaluate && entry->typeId == typeId && entry->instanceToken == instanceToken) {
                    marks.add(*entry);
                }
            }
        } else if (effect.record && (effect.kind == "updates" || effect.kind == "changes")) {
            applyRecordEffect(effect, *effect.record, *info, reply, use, marks);
        }
    }
    markAndRefetch(marks);
}

void Session::applyRecordEffect(ActionEffect const& effect, std::string const& record, ActionInfo const& info,
                                Value const& reply, ReplyUse use, detail::Marks& marks) {
    // The reply's rows of the record, by key; none when the reply does not hold it keyed, or must only mark.
    auto const own = std::ranges::find(info.records, record, &ActionRecordInfo::record);
    std::optional<std::string> const ownKey =
        effect.kind == "updates" && use == ReplyUse::Applied && own != info.records.end() ? own->keyField
                                                                                          : std::nullopt;
    detail::RowsByKey rows;
    if (ownKey) {
        for (Value const* const row : detail::keyedRowsOf(reply, own->paths, *ownKey)) {
            rows.insert_or_assign(detail::canonicalJson(*row->field(*ownKey)), row);
        }
    }
    for (auto const& [key, entry] : _entries) {
        static_cast<void>(key);
        ActionRecordInfo const* const held = entry->holding(record);
        if (held == nullptr) {
            continue;
        }
        // A merge replaces rows by key: it cannot find an opaque entry's rows, nor update what an open or partial
        // member says about them by position, so such an entry is marked and fetched again.
        bool const unmergeable = held->opaque || detail::anyMemberSet(entry->confirmed, held->open) ||
                                 detail::anyMemberSet(entry->confirmed, held->partial);
        if (!ownKey || !held->keyField || unmergeable) {
            marks.add(*entry);
            continue;
        }
        if (auto patched = detail::patchHeldRows(entry->confirmed, held->paths, *held->keyField, rows)) {
            confirm(*entry, *patched);
        }
        // A fetch in flight may have read before the write, and would settle over the patch.
        if (entry->pending.peek()) {
            markStale(*entry);
        }
    }
}

void Session::markAndRefetch(detail::Marks const& marks) {
    for (detail::CacheEntry* const entry : marks.entries) {
        markStale(*entry);
    }
    for (detail::CacheEntry const* const entry : marks.entries) {
        refetchMounted(*entry);
    }
}

void Session::countTopics(detail::CacheEntry const& entry, int delta) {
    if (entry.evaluate) {
        return;
    }
    // One topic: its count, and the queues a change of the count from or to zero goes to.
    auto const count = [delta](auto& refs, auto const& topic, auto& watches, auto& unwatches) {
        int& refsOf = refs[topic];
        refsOf += delta;
        if (delta > 0 && refsOf == 1 && unwatches.erase(topic) == 0) {
            watches.insert(topic);
        } else if (refsOf <= 0) {
            refs.erase(topic);
            if (watches.erase(topic) == 0) {
                unwatches.insert(topic);
            }
        }
    };
    if (!entry.records.empty()) {
        for (ActionRecordInfo const& record : entry.records) {
            countRecord(record.record, delta);
        }
        return;
    }
    constexpr std::string_view kShared = "shared:";
    if (entry.instanceToken.starts_with(kShared)) {
        count(_instanceRefs, std::pair{entry.typeId, entry.instanceToken.substr(kShared.size())}, _watchInstances,
              _unwatchInstances);
    }
}

void Session::countRecord(std::string const& record, int delta) {
    int& refs = _recordRefs[record];
    refs += delta;
    if (delta > 0 && refs == 1 && _unwatchRecords.erase(record) == 0) {
        _watchRecords.insert(record);
    } else if (refs <= 0) {
        _recordRefs.erase(record);
        if (_watchRecords.erase(record) == 0) {
            _unwatchRecords.insert(record);
        }
    }
}

void Session::sendWatch(std::string_view kind, wire::WatchBody const& body) const {
    auto completion = _options.watcher(std::string{kind}, glz::write_json(body).value_or(std::string{}));
    if (kind != wire::kWatchKind || body.records.empty()) {
        completion.then([](std::string const&) {}).onError([](std::exception_ptr const&) {});
        return;
    }
    // The records' watch is in flight until its answer: a `watchRecord` for one of them resolves then.
    std::weak_ptr<detail::WatchRegistry> const weak = _watchRegistry;
    if (_watchRegistry) {
        _watchRegistry->inFlight.insert(body.records.begin(), body.records.end());
    }
    auto const answered = [weak, records = body.records] {
        if (auto const registry = weak.lock()) {
            registry->answered(records);
        }
    };
    completion.then([answered](std::string const&) { answered(); })
        .onError([answered](std::exception_ptr const&) { answered(); });
}

void Session::flushWatches() {
    if (!_options.watcher) {
        _watchRecords.clear();
        _unwatchRecords.clear();
        _watchInstances.clear();
        _unwatchInstances.clear();
        return;
    }
    auto const take = [](auto& records, auto& instances) {
        wire::WatchBody body;
        body.records.assign(records.begin(), records.end());
        for (auto const& [modelType, primary] : instances) {
            body.instances.push_back(wire::InstanceTopic{.modelType = modelType, .primary = primary, .instance = 0});
        }
        records.clear();
        instances.clear();
        return body;
    };
    if (!_watchRecords.empty() || !_watchInstances.empty()) {
        static_cast<void>(watchRegistry());
        sendWatch(wire::kWatchKind, take(_watchRecords, _watchInstances));
    }
    if (!_unwatchRecords.empty() || !_unwatchInstances.empty()) {
        sendWatch(wire::kUnwatchKind, take(_unwatchRecords, _unwatchInstances));
    }
}

wire::WatchBody Session::watchSet() const {
    wire::WatchBody body;
    for (auto const& [record, refs] : _recordRefs) {
        static_cast<void>(refs);
        body.records.push_back(record);
    }
    for (auto const& [topic, refs] : _instanceRefs) {
        static_cast<void>(refs);
        body.instances.push_back(
            wire::InstanceTopic{.modelType = topic.first, .primary = topic.second, .instance = 0});
    }
    return body;
}

void Session::markAllStale() {
    _runtime->batch([this] {
        detail::Marks marks;
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            if (!entry->evaluate) {
                marks.add(*entry);
            }
        }
        markAndRefetch(marks);
    });
}

void Session::reconnected() {
    // The whole set replaces what the server holds, so the queued changes are superseded.
    _watchRecords.clear();
    _unwatchRecords.clear();
    _watchInstances.clear();
    _unwatchInstances.clear();
    if (_options.watcher) {
        wire::WatchBody body = watchSet();
        body.replace = true;
        sendWatch(wire::kWatchKind, body);
    }
    markAllStale();
    if (_watchRegistry) {
        std::set<std::string, std::less<>> records;
        for (auto const& [id, watch] : _watchRegistry->watches) {
            static_cast<void>(id);
            records.insert(watch.record);
        }
        for (std::string const& record : records) {
            notifyWatchers(effects::RecordChange{.record = record, .keys = std::nullopt});
        }
    }
}

void Session::deliverEvent(wire::EventBody const& event) {
    if (event.topic == wire::kResyncTopic) {
        reconnected();
        return;
    }
    if (event.topic != wire::kChangesTopic) {
        return;
    }
    if (auto changes = wire::readChangesBody(event.payload.str)) {
        applyChanges(*changes);
    }
}

void Session::applyChanges(wire::ChangesBody const& changes) {
    _runtime->batch([&] {
        detail::Marks marks;
        for (effects::RecordChange const& change : changes.records) {
            markRecordChange(change, marks);
        }
        for (wire::InstanceTopic const& instance : changes.instances) {
            markInstanceChange(instance, marks);
        }
        markAndRefetch(marks);
    });
    for (effects::RecordChange const& change : changes.records) {
        notifyWatchers(change);
    }
}

void Session::notifyWatchers(effects::RecordChange const& change) {
    if (!_watchRegistry) {
        return;
    }
    // A handler may drop its own watch, or another, so the handlers are copied out first.
    std::vector<std::function<void(effects::RecordChange const&)>> handlers;
    for (auto const& [id, watch] : _watchRegistry->watches) {
        static_cast<void>(id);
        if (watch.record == change.record) {
            handlers.push_back(watch.handler);
        }
    }
    for (auto const& handler : handlers) {
        handler(change);
    }
}

detail::WatchRegistry& Session::watchRegistry() {
    if (!_watchRegistry) {
        _watchRegistry = std::make_shared<detail::WatchRegistry>();
        _watchRegistry->session = this;
    }
    return *_watchRegistry;
}

async::Completion<WatchHolder> Session::watchRecord(std::string const& record,
                                                    std::function<void(effects::RecordChange const&)> handler) {
    auto& registry = watchRegistry();
    auto const id = ++registry.next;
    registry.watches.emplace(id, detail::WatchRegistry::Watch{.record = record, .handler = std::move(handler)});
    auto registration = std::make_shared<detail::WatchRegistration>();
    registration->registry = _watchRegistry;
    registration->id = id;
    registration->record = record;
    WatchHolder holder{std::move(registration)};
    countRecord(record, +1);
    flushWatches();
    auto [completion, promise] = async::Completion<WatchHolder>::makeSettleable(&_runtime->owner());
    if (registry.inFlight.contains(record)) {
        registry.waiters.push_back(
            detail::WatchRegistry::Waiter{.record = record, .promise = std::move(promise), .holder = std::move(holder)});
    } else {
        promise.resolve(std::move(holder));
    }
    return std::move(completion);
}

async::Completion<std::string> Session::read(std::shared_ptr<ModelChannel> const& channel, std::string const& actionId,
                                             Value const& body, ::core::async::StopToken stop) {
    using Promise = async::Completion<std::string>::Promise;
    auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_runtime->owner());
    auto const settle = std::make_shared<Promise>(std::move(promise));
    if (channel == nullptr) {
        settle->reject(std::make_exception_ptr(std::runtime_error{"the model alias has no instance"}));
        return std::move(completion);
    }
    if (stop.stop_requested()) {
        settle->reject(std::make_exception_ptr(::core::async::OperationCancelled{}));
        return std::move(completion);
    }
    // A stop settles the read at once; the reply that may still come is dropped, since the first settlement wins.
    using OnStop = ::core::async::StopCallback<std::function<void()>>;
    std::shared_ptr<OnStop> onStop;
    if (stop.stop_possible()) {
        onStop = std::make_shared<OnStop>(stop, std::function<void()>{[settle] {
                                              settle->reject(
                                                  std::make_exception_ptr(::core::async::OperationCancelled{}));
                                          }});
    }
    try {
        auto sent = channel->execute(actionId, detail::canonicalJson(body),
                                     CallOptions{.evaluate = false, .queueOffline = false, .stop = stop});
        sent.then([settle, onStop, stop](std::string const& reply) {
                if (!stop.stop_requested()) {
                    settle->resolve(reply);
                }
            })
            .onError([settle, onStop](std::exception_ptr const& error) { settle->reject(error); });
    } catch (...) {
        settle->reject(std::current_exception());
    }
    return std::move(completion);
}

void Session::markRecordChange(effects::RecordChange const& change, detail::Marks& marks) {
    // The keys' canonical text, so a key the server wrote is compared with the rows' own.
    std::set<std::string, std::less<>> keys;
    for (glz::raw_json const& key : change.keys.value_or(std::vector<glz::raw_json>{})) {
        auto json = parseJson(key.str);
        auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
        if (value) {
            keys.insert(detail::canonicalJson(*value));
        }
    }
    for (auto const& [name, entry] : _entries) {
        static_cast<void>(name);
        ActionRecordInfo const* const held = entry->holding(change.record);
        if (held == nullptr) {
            continue;
        }
        // An entry still waiting for its first value may come to hold a key; keyless rows cannot be told apart. An
        // opaque entry's rows cannot be found by key, and while an open member is set a row the entry does not hold
        // may come to belong to it.
        if (!change.keys || !held->keyField || !entry->hasValue.peek() || held->opaque ||
            detail::anyMemberSet(entry->confirmed, held->open)) {
            marks.add(*entry);
            continue;
        }
        std::string const& keyField = *held->keyField;
        bool const holdsKey = std::ranges::any_of(
            detail::keyedRowsOf(entry->confirmed, held->paths, keyField),
            [&](Value const* row) { return keys.contains(detail::canonicalJson(*row->field(keyField))); });
        if (holdsKey) {
            marks.add(*entry);
        }
    }
}

void Session::markInstanceChange(wire::InstanceTopic const& instance, detail::Marks& marks) {
    std::string const shared = "shared:" + instance.primary;
    for (auto const& [name, entry] : _entries) {
        static_cast<void>(name);
        if (entry->evaluate || entry->typeId != instance.modelType) {
            continue;
        }
        // Without a primary key, the instance is one of the connection's own private ones.
        bool const same =
            instance.primary.empty() ? !entry->instanceToken.starts_with("shared") : entry->instanceToken == shared;
        if (same) {
            marks.add(*entry);
        }
    }
}

void Session::actionSucceeded(std::string const& typeId, std::string const& actionId, Value const& reply, ReplyUse use,
                              std::string const& instanceToken) {
    _runtime->batch([&] { applyReplyEffects(typeId, actionId, reply, use, instanceToken); });
}

void Session::detachQuery(detail::QueryRuntime& query) { query.detach(); }

namespace detail {

[[nodiscard]] std::string instanceKeyText(Value const& key) {
    if (key.isNull()) {
        return {};
    }
    if (std::int64_t const* const number = key.asInt()) {
        return std::to_string(*number);
    }
    if (std::string const* const text = key.asString()) {
        return *text;
    }
    return canonicalJson(key);
}

namespace {

/// @brief The channel an alias is bound to for its current token, binding it when the token changed.
/// @param alias The alias.
/// @param core  The screen.
/// @return The channel, or `nullptr` while the alias is unbound.
[[nodiscard]] std::shared_ptr<ModelChannel> channelOf(ModelAlias& alias, ScreenCore& core) {
    std::string const token = core.runtime().untracked([&] { return alias.token->get(); });
    if (token.empty()) {
        return nullptr;
    }
    if (alias.channel && alias.boundToken == token) {
        return alias.channel;
    }
    std::string const& type = alias.decl->type;
    switch (alias.mode) {
        case InstanceMode::Borrowed: {
            auto const found = core.borrowed.find(alias.decl->name);
            alias.channel = found == core.borrowed.end() ? nullptr : found->second.channel;
            break;
        }
        case InstanceMode::Shared:
            // `shared` is for a stateless model, whose instances are interchangeable: its calls go to an instance of
            // the alias's own, and what it shares with other screens is the query cache, through the token.
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Private, {});
            break;
        case InstanceMode::Keyed:
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Shared, token.substr(7));
            break;
        case InstanceMode::Private:
        case InstanceMode::Default:
        default:
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Private, {});
            break;
    }
    alias.boundToken = token;
    return alias.channel;
}
}  // namespace

RtScope::RtScope(ScreenCore& screen, std::shared_ptr<RtScope> enclosing, reactive::Signal<Value> const* current,
                 std::size_t depth)
    : core{screen}, parent{std::move(enclosing)}, row{current}, nodes{screen.runtime(), depth} {
    if (core.recording) {
        core.scopes.push_back(this);
    }
}

RtScope::~RtScope() {
    if (core.recording) {
        std::erase(core.scopes, this);
    }
    if (owner != nullptr) {
        core.addPending(-1);
        if (owner->inner.peek() == this) {
            owner->inner.set(nullptr);
        }
    }
}

// NOLINTNEXTLINE(misc-no-recursion): an element's value can read a nested scope's readiness, one level per nesting.
bool RtScope::ready() const {
    for (auto const& element : elements) {
        Value const shown = core.elementValue(*element, "visible");
        bool const* const visible = shown.asBool();
        ValueList const* const errors = element->errors->get().asList();
        if ((visible == nullptr || *visible) && errors != nullptr && !errors->empty()) {
            return false;
        }
    }
    for (auto const& check : checks) {
        if (check->decl->blocking && check->failing->get()) {
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): an `app.` name is read in the root scope with its head consumed, once per segment.
Value ScopeEnv::ref(std::string_view head, std::span<std::string const> path) const {
    if (head == "app") {
        RtScope const* const app = _scope->core.session.appScope();
        if (app == nullptr || path.empty()) {
            return Value{};
        }
        return ScopeEnv{*app, nullptr}.ref(path.front(), path.subspan(1));
    }
    if (_bindings != nullptr) {
        for (Binding const& bound : std::views::reverse(*_bindings)) {
            if (bound.name == head) {
                return walk(bound.read(), path);
            }
        }
    }
    NameRef const* const name = _scope->find(head);
    if (name == nullptr) {
        return Value{};
    }
    switch (name->category) {
        case NameCategory::Param:
            return walk(name->param, path);
        case NameCategory::State:
            _scope->core.initState(*name->state->scope, *name->state);
            // State inits are acyclic (`loadDocument` checks it), so the value is made by now.
            return walk(name->state->value->get(), path);
        case NameCategory::Let:
            return walk(name->let->get(), path);
        case NameCategory::Query:
            return walk(name->query->value(), path);
        case NameCategory::Element:
            if (path.empty()) {
                return Value{};
            }
            return walk(_scope->core.elementValue(*name->element, path.front()), path.subspan(1));
        case NameCategory::Transfer: {
            if (path.empty()) {
                return Value{};
            }
            TransferState const& transfer = *name->transfer;
            Value value;
            if (path.front() == "progress") {
                value = transfer.progress.get();
            } else if (path.front() == "pending") {
                value = Value{transfer.pending.get()};
            } else if (path.front() == "error") {
                value = transfer.error.get();
            }
            return walk(value, path.subspan(1));
        }
        case NameCategory::Model:
        case NameCategory::Mutation:
        default:
            return Value{};
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per control form and kind of name.
Value ScopeEnv::control(ExprKind form, std::string_view name) const {
    Session const& session = _scope->core.session;
    NameRef const* const found = _scope->find(name);
    if (form == ExprKind::Can) {
        if (found != nullptr && found->category == NameCategory::Query) {
            return session.can(found->query->decl().action);
        }
        if (found != nullptr && found->category == NameCategory::Mutation) {
            return session.can(found->mutation->decl().action);
        }
        return session.can(name);
    }
    if (found != nullptr && found->category == NameCategory::Query) {
        QueryRuntime const& query = *found->query;
        if (form == ExprKind::Query) {
            return query.value();
        }
        if (form == ExprKind::Pending) {
            return query.pending();
        }
        if (form == ExprKind::Error) {
            return query.error();
        }
        if (form == ExprKind::Refusal) {
            return query.refusal();
        }
        return form == ExprKind::ErrorKind ? query.errorKind() : Value{};
    }
    if (found != nullptr && found->category == NameCategory::Mutation) {
        MutationRuntime const& mutation = *found->mutation;
        if (form == ExprKind::Result) {
            return mutation.result();
        }
        if (form == ExprKind::Pending) {
            return mutation.pending();
        }
        if (form == ExprKind::Error) {
            return mutation.error();
        }
        if (form == ExprKind::Refusal) {
            return mutation.refusal();
        }
        return form == ExprKind::ErrorKind ? mutation.errorKind() : Value{};
    }
    return Value{};
}

Value ScopeEnv::pref(std::string_view point, std::span<std::string const> path) const {
    ValueRecord const& prefs = _scope->core.prefs.get();
    auto const found = prefs.find(point);
    return found == prefs.end() ? Value{} : walk(found->second, path);
}

Value ScopeEnv::client(ExprKind form, std::string_view name) const { return _scope->core.session.client(form, name); }

FormatContext const& RtScope::formatting() const {
    for (RtScope const* scope = this; scope != nullptr;
         scope = scope->parent != nullptr ? scope->parent.get() : scope->formatSource.get()) {
        if (scope->localeFormat != nullptr) {
            return *scope->localeFormat->get();
        }
    }
    return core.session.formatting();
}

FormatContext const& ScopeEnv::formatting() const { return _scope->formatting(); }

FormatContext const& ScopeEnv::entryFormatting() const { return _scope->core.session.formatting(); }

void ScopeEnv::report(std::string_view path, std::string_view reason) const { _scope->core.report(path, reason); }

// --- Queries ---------------------------------------------------------------------------------------------------

QueryRuntime::QueryRuntime(QueryDecl const& decl, RtScope& scope, ModelAlias* alias)
    : _decl{&decl},
      _typeId{alias->decl->type},
      _scope{&scope},
      _alias{alias},
      _entry{scope.core.runtime()},
      _carried{scope.core.runtime()},
      _waiting{scope.core.runtime(), false} {
    scope.core.session.attachQuery(*this);
}

QueryRuntime::~QueryRuntime() {
    _debounce.cancel();
    _refresh.cancel();
    _scope->core.session.forgetQuery(*this);
    if (auto const& current = _entry.peek()) {
        _scope->core.session.release(current);
    }
}

std::shared_ptr<ModelChannel> QueryRuntime::channel() const { return channelOf(*_alias, _scope->core); }

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the query's key, its fetch and its refresh triggers.
void QueryRuntime::start() {
    RtScope& scope = *_scope;
    auto& key = scope.nodes.computed([this] -> Target {
        Target target;
        if (_decl->when) {
            Value const when = evalIn(*_scope, nullptr, _decl->when);
            bool const* const flag = when.asBool();
            if (flag == nullptr || !*flag) {
                return target;
            }
        }
        std::string token = _alias->token->get();
        if (token.empty()) {
            return target;
        }
        Value body = _decl->body ? evalIn(*_scope, nullptr, _decl->body) : Value{ValueRecord{}};
        if (body.isNull()) {
            return target;
        }
        target.idle = false;
        target.token = std::move(token);
        target.bodyText = canonicalJson(body);
        target.body = std::move(body);
        return target;
    });
    _key = &key;
    reactive::Scheduler* const scheduler = scope.core.session.options().scheduler;
    std::weak_ptr<bool> const alive = _alive;
    scope.nodes.effect([this, &key, scheduler, alive, first = true] mutable {
        Target target = key.get();
        _scope->core.runtime().untracked([&] {
            if (first || target.idle || _decl->debounce <= 0 || scheduler == nullptr) {
                first = false;
                _debounce.cancel();
                _waiting.set(false);
                switchTo(target);
                return;
            }
            _pendingTarget = std::move(target);
            _waiting.set(true);
            _debounce = scheduler->after(std::chrono::milliseconds{_decl->debounce}, [this, alive] {
                if (alive.expired()) {
                    return;
                }
                _scope->core.runtime().batch([this] {
                    _waiting.set(false);
                    switchTo(_pendingTarget);
                });
            });
        });
    });
    if (_decl->refreshEvery > 0 && scheduler != nullptr) {
        _refresh = scheduler->every(std::chrono::milliseconds{_decl->refreshEvery}, [this, alive] {
            if (alive.expired()) {
                return;
            }
            auto const current = _entry.peek();
            if (!current || _waiting.peek() || current->pending.peek()) {
                return;
            }
            _scope->core.runtime().batch([&] { _scope->core.session.fetch(current, channel()); });
        });
    }
}

void QueryRuntime::switchTo(Target const& target) {
    Session& session = _scope->core.session;
    std::shared_ptr<CacheEntry> const old = _entry.peek();
    if (old && target == _target) {
        return;
    }
    Value shown = old && old->hasValue.peek() ? old->value.peek() : _carried.peek();
    _target = target;
    if (target.idle) {
        _carried.set(Value{});
        _entry.set(nullptr);
        if (old) {
            session.release(old);
        }
        return;
    }
    auto const fresh = session.acquire(target.token, _typeId, _decl->action, _decl->evaluate, target.body);
    _carried.set(std::move(shown));
    _entry.set(fresh);
    if (old) {
        session.release(old);
    }
    bool const young = fresh->fetchedAt.has_value() &&
                       session.now() - *fresh->fetchedAt < std::chrono::milliseconds{_decl->staleTime};
    if (!fresh->pending.peek() && (!fresh->hasValue.peek() || fresh->stale || !young)) {
        session.fetch(fresh, channel());
    }
}

void QueryRuntime::refetch() {
    _scope->core.runtime().batch([this] {
        bool switched = false;
        if (_waiting.peek()) {
            _debounce.cancel();
            _waiting.set(false);
            switchTo(_pendingTarget);
            switched = true;
        }
        auto const current = _entry.peek();
        if (current && !(switched && current->pending.peek())) {
            _scope->core.session.fetch(current, channel());
        }
    });
}

void QueryRuntime::refetchCurrent() {
    if (_key == nullptr) {
        refetch();
        return;
    }
    _scope->core.runtime().batch([this] {
        _debounce.cancel();
        _waiting.set(false);
        Target const target = _key->peek();
        if (!(_entry.peek() && target == _target)) {
            switchTo(target);
            return;
        }
        if (auto const current = _entry.peek(); current && !current->pending.peek()) {
            _scope->core.session.fetch(current, channel());
        }
    });
}

Value QueryRuntime::value() const {
    auto const& current = _entry.get();
    if (current) {
        if (current->hasValue.get()) {
            return current->value.get();
        }
        if (!current->error.get().empty()) {
            return Value{};
        }
    }
    return _carried.get();
}

bool QueryRuntime::pending() const {
    if (_waiting.get()) {
        return true;
    }
    auto const& current = _entry.get();
    return current && current->pending.get();
}

Value QueryRuntime::error() const {
    auto const& current = _entry.get();
    if (!current || current->error.get().empty()) {
        return Value{};
    }
    return Value{current->error.get()};
}

Value QueryRuntime::errorKind() const {
    auto const& current = _entry.get();
    if (!current || current->errorKind.get().empty()) {
        return Value{};
    }
    return Value{current->errorKind.get()};
}

Value QueryRuntime::refusal() const {
    auto const& current = _entry.get();
    return current ? current->refusal.get() : Value{};
}

bool QueryRuntime::settled() const {
    auto const& current = _entry.get();
    return !current || current->hasValue.get() || !current->error.get().empty();
}

// --- Mutations -------------------------------------------------------------------------------------------------

MutationRuntime::MutationRuntime(MutationDecl const& decl, RtScope& scope, ModelAlias* alias)
    : _decl{&decl},
      _scope{&scope},
      _alias{alias},
      _typeId{alias->decl->type},
      _core{&scope.core},
      _inFlight{scope.core.runtime(), 0},
      _result{scope.core.runtime()},
      _error{scope.core.runtime()},
      _errorKind{scope.core.runtime()},
      _refusal{scope.core.runtime()} {}

MutationRuntime::~MutationRuntime() {
    *_alive = false;
    // A run whose reply will now never be read is no longer pending.
    if (int const running = _inFlight.peek(); running > 0) {
        _core->addPending(-running);
    }
}

// NOLINTBEGIN(misc-no-recursion): a mutation's commands can run another mutation, and a serial queue sends its next
// run as the last one settles.
void MutationRuntime::run(Value body, std::shared_ptr<Bindings const> bindings, RunDone done) {
    auto const run = std::make_shared<Run>();
    run->body = std::move(body);
    run->bindings = std::move(bindings);
    run->done = std::move(done);
    if (_decl->concurrency == Concurrency::Exclusive && _inFlight.peek() > 0) {
        run->done(false, Value{});
        return;
    }
    _inFlight.set(_inFlight.peek() + 1);
    _core->addPending(1);
    if (_decl->concurrency == Concurrency::Serial) {
        // The key reads the run's body, so a run keys the same whichever command started it.
        Bindings withBody = *run->bindings;
        withBody.push_back(Binding{.name = "body", .read = [body = run->body] { return body; }});
        Value const key = evalIn(*_scope, &withBody, _decl->serialKey);
        run->serialKey = key.isNull() ? std::string{} : canonicalJson(key);
        if (_busy.contains(run->serialKey)) {
            _queues[run->serialKey].push_back(run);
            return;
        }
        _busy.insert(run->serialKey);
    }
    send(run);
}

void MutationRuntime::send(std::shared_ptr<Run> const& run) {
    run->id = ++_runs;
    _latest = run->id;
    applyOptimistic(*run);
    std::shared_ptr<ModelChannel> const channel = channelOf(*_alias, _scope->core);
    if (channel == nullptr) {
        settle(run, std::nullopt, std::make_exception_ptr(std::runtime_error{"the model alias has no instance"}));
        return;
    }
    std::weak_ptr<bool> const alive = _alive;
    try {
        auto completion = channel->execute(_decl->action, canonicalJson(run->body),
                                           CallOptions{.queueOffline = _decl->offlineQueue});
        completion
            .then([this, alive, run](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                if (!value) {
                    settle(run, std::nullopt,
                           std::make_exception_ptr(
                               std::runtime_error{"the reply is not a document value: " + value.error()}));
                    return;
                }
                settle(run, std::move(*value), nullptr);
            })
            .onError([this, alive, run](std::exception_ptr const& error) {
                if (!alive.expired()) {
                    settle(run, std::nullopt, error);
                }
            });
    } catch (...) {
        settle(run, std::nullopt, std::current_exception());
    }
}

void MutationRuntime::applyOptimistic(Run& run) {
    if (!_decl->optimistic) {
        return;
    }
    NameRef const* const target = _scope->find(_decl->optimistic->query);
    if (target == nullptr || target->category != NameCategory::Query) {
        return;
    }
    auto entry = _scope->core.runtime().untracked([&] { return target->query->entry(); });
    if (!entry) {
        return;
    }
    Bindings bindings = *run.bindings;
    bindings.push_back(Binding{.name = "body", .read = [body = run.body] { return body; }});
    Value key = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->key); });
    Value with = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->with); });
    // Before a value has arrived there is no list to miss.
    if (entry->hasValue.peek() &&
        patchRows(entry->confirmed, _decl->optimistic->at, key, with).error_or(PatchMiss::NoRow) ==
            PatchMiss::NoList) {
        _scope->core.report(pointerAppend(_decl->path, "optimistic"),
                            patchMissReason(_decl->optimistic->query, _decl->optimistic->at));
    }
    entry->layers.push_back(
        OptimisticLayer{.run = run.id, .at = _decl->optimistic->at, .key = std::move(key), .with = std::move(with)});
    _scope->core.session.showLayers(*entry);
    run.layered.push_back(std::move(entry));
}

void MutationRuntime::dropLayers(Run& run) {
    for (auto const& entry : run.layered) {
        std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run.id; });
        _scope->core.session.showLayers(*entry);
    }
    run.layered.clear();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome a reply can have.
void MutationRuntime::settle(std::shared_ptr<Run> const& run, std::optional<Value> reply,
                             std::exception_ptr const& error) {
    ScreenCore& core = _scope->core;
    Session& session = core.session;
    bool const applied = _decl->concurrency != Concurrency::Latest || run->id == _latest;
    std::shared_ptr<RtScope> const scope = _scope->shared_from_this();
    auto commandCtx = [&](Bindings extra) {
        return CommandCtx{
            .scope = scope,
            .bindings = std::make_shared<Bindings>(std::move(extra)),
            .cancelClose = std::make_shared<bool>(false),
            .boundary = nullptr,
        };
    };
    Value delivered;
    bool succeeded = false;
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome a reply can have.
    core.runtime().batch([&] {
        _inFlight.set(_inFlight.peek() - 1);
        _core->addPending(-1);
        if (reply) {
            succeeded = true;
            // The run's optimistic writes are confirmed by the reply: they become part of the confirmed rows.
            for (auto const& entry : run->layered) {
                Value confirmed = entry->confirmed;
                for (OptimisticLayer const& layer : entry->layers) {
                    if (layer.run == run->id) {
                        if (auto patched = patchRows(confirmed, layer.at, layer.key, layer.with)) {
                            confirmed = std::move(*patched);
                        }
                    }
                }
                std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run->id; });
                session.confirm(*entry, confirmed);
            }
            run->layered.clear();
            std::vector<std::string> secret;
            if (session.options().catalog.action) {
                if (auto const info = session.options().catalog.action(_decl->action)) {
                    secret = info->secretFields;
                }
            }
            delivered = withoutFields(*reply, secret);
            std::string const token =
                _alias == nullptr ? std::string{} : core.runtime().untracked([&] { return _alias->token->get(); });
            if (!applied) {
                session.actionSucceeded(_typeId, _decl->action, *reply, ReplyUse::Discarded, token);
                return;
            }
            _result.set(*reply);
            _error.set(Value{});
            _errorKind.set(Value{});
            _refusal.set(Value{});
            session.actionSucceeded(_typeId, _decl->action, *reply, ReplyUse::Applied, token);
            core.runList(_decl->onSuccess, commandCtx(*run->bindings), {});
            if (!secret.empty()) {
                _result.set(delivered);
            }
            return;
        }
        dropLayers(*run);
        std::string const kind = session.classify(error);
        if (applied) {
            _error.set(Value{failureText(error)});
            _errorKind.set(Value{kind});
            _refusal.set(detail::refusalValue(error));
            core.runList(_decl->onError, commandCtx(*run->bindings), {});
        }
        if (kind == "denied" && core.host != nullptr) {
            core.host->permissionsStale();
        }
    });
    if (_decl->concurrency == Concurrency::Serial) {
        next(run->serialKey);
    }
    run->done(succeeded, delivered);
}

void MutationRuntime::next(std::string const& serialKey) {
    auto const found = _queues.find(serialKey);
    if (found == _queues.end() || found->second.empty()) {
        _queues.erase(serialKey);
        _busy.erase(serialKey);
        return;
    }
    std::shared_ptr<Run> const queued = found->second.front();
    found->second.pop_front();
    _scope->core.runtime().batch([&] { send(queued); });
}
// NOLINTEND(misc-no-recursion)

}  // namespace detail

namespace detail {
namespace {

// --- Declaring scopes ------------------------------------------------------------------------------------------

/// @brief The element a node of a scope keeps its state in.
/// @param scope The scope that declares it.
/// @param node  The node.
/// @return The element, or `nullptr` for a node that keeps none.
[[nodiscard]] ElementState* elementFor(RtScope const& scope, DocNode const& node) {
    // A scope that declares nothing (a locale scope and the scope of its mounted content) leaves its children's
    // elements to the enclosing scope, where `collect` registered them, so the lookup continues there.
    for (RtScope const* at = &scope; at != nullptr; at = at->transparent ? at->parent.get() : nullptr) {
        for (auto const& element : at->elements) {
            if (element->node == &node) {
                return element.get();
            }
        }
    }
    return nullptr;
}

/// @brief The transfers of the `upload` commands in a list, nested lists included.
/// @param commands The list.
/// @param out      Where the transfer ids go.
// NOLINTNEXTLINE(misc-no-recursion): one level per nested command list
void transfersOf(Commands const& commands, std::vector<std::string>& out) {
    for (Command const& command : commands) {
        if (command.kind == CommandKind::Upload && !command.transfer.empty()) {
            out.push_back(command.transfer);
        }
        transfersOf(command.commands, out);
        transfersOf(command.alternative, out);
    }
}
}  // namespace

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes and parts, and collecting descends it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of node and part.
void ScreenCore::collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands) {
    KindSpec const* const spec = findKind(node.kind, false);
    if (!node.id.empty() || statefulKind(node.kind)) {
        auto element = std::make_unique<ElementState>(runtime());
        element->node = &node;
        element->scope = &scope;
        if (!node.id.empty()) {
            NameRef name;
            name.category = NameCategory::Element;
            name.element = element.get();
            scope.names.insert_or_assign(node.id, name);
        }
        wireErrors(*element);
        scope.elements.push_back(std::move(element));
    }
    bool const createsScope = spec != nullptr && (spec->flags & kScope) != 0;
    for (auto const& [key, list] : node.events) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (!createsScope || keyed == nullptr || (keyed->flags & kInner) == 0) {
            commands.push_back(&list);
        }
    }
    for (auto const& [chord, list] : node.keys) {
        static_cast<void>(chord);
        commands.push_back(&list);
    }
    for (auto const& [key, nodes] : node.children) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (createsScope && keyed != nullptr && (keyed->flags & kInner) != 0) {
            continue;
        }
        for (DocNodePtr const& child : nodes) {
            collect(*child, scope, commands);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            KindSpec const* const partSpec = findKind(part->kind, true);
            if (partSpec != nullptr && (partSpec->flags & kScope) != 0) {
                continue;
            }
            for (auto const& [event, list] : part->events) {
                static_cast<void>(event);
                commands.push_back(&list);
            }
            for (auto const& [childKey, nodes] : part->children) {
                KeySpec const* const keyed = partSpec == nullptr ? nullptr : keySpec(*partSpec, childKey);
                if (keyed != nullptr && (keyed->flags & kInner) != 0) {
                    continue;
                }
                for (DocNodePtr const& child : nodes) {
                    collect(*child, scope, commands);
                }
            }
        }
    }
}
// NOLINTEND(misc-no-recursion)

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of declaration.
void ScreenCore::declare(RtScope& scope, ScopeDecl const* decl, std::vector<DocNode const*> const& content,
                         std::vector<Commands const*> const& commands) {
    std::vector<Commands const*> lists = commands;
    for (DocNode const* const node : content) {
        collect(*node, scope, lists);
    }
    std::vector<std::string> transferIds;
    for (Commands const* const list : lists) {
        transfersOf(*list, transferIds);
    }
    for (std::string const& transferId : transferIds) {
        if (scope.names.contains(transferId)) {
            continue;
        }
        scope.transfers.push_back(std::make_unique<TransferState>(runtime()));
        NameRef name;
        name.category = NameCategory::Transfer;
        name.transfer = scope.transfers.back().get();
        scope.names.insert_or_assign(transferId, name);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            auto alias = std::make_unique<ModelAlias>();
            alias->decl = &model;
            alias->mode = model.instance;
            if (alias->mode == InstanceMode::Default) {
                std::optional<ModelInfo> const info =
                    session.options().catalog.model ? session.options().catalog.model(model.type) : std::nullopt;
                alias->mode = info && info->stateless ? InstanceMode::Shared : InstanceMode::Private;
            }
            ModelAlias* const raw = alias.get();
            switch (raw->mode) {
                case InstanceMode::Keyed:
                    raw->token = &scope.nodes.computed([&scope, raw] -> std::string {
                        std::string const key = instanceKeyText(evalIn(scope, nullptr, raw->decl->key));
                        return key.empty() ? std::string{} : "shared:" + key;
                    });
                    break;
                case InstanceMode::Shared:
                    raw->token = &scope.nodes.computed([] { return std::string{"shared"}; });
                    break;
                case InstanceMode::Borrowed: {
                    auto const found = borrowed.find(model.name);
                    std::string token = found == borrowed.end() ? std::string{} : found->second.token;
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
                case InstanceMode::Private:
                case InstanceMode::Default:
                default: {
                    std::string token = session.privateToken();
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
            }
            NameRef name;
            name.category = NameCategory::Model;
            name.model = raw;
            scope.names.insert_or_assign(model.name, name);
            scope.models.push_back(std::move(alias));
        }
        for (LetDecl const& let : decl->lets) {
            NameRef name;
            name.category = NameCategory::Let;
            name.letExpr = let.value;
            name.let = &scope.nodes.computed([&scope, expr = let.value] { return evalIn(scope, nullptr, expr); });
            scope.names.insert_or_assign(let.name, name);
        }
        for (StateDecl const& state : decl->state) {
            auto slot = std::make_unique<StateSlot>();
            slot->decl = &state;
            slot->scope = &scope;
            NameRef name;
            name.category = NameCategory::State;
            name.state = slot.get();
            scope.names.insert_or_assign(state.name, name);
            scope.states.push_back(std::move(slot));
        }
        auto const aliasOf = [&scope](std::string const& model) -> ModelAlias* {
            NameRef const* const found = scope.find(model);
            return found != nullptr && found->category == NameCategory::Model ? found->model : nullptr;
        };
        for (QueryDecl const& query : decl->queries) {
            scope.queries.push_back(std::make_unique<QueryRuntime>(query, scope, aliasOf(query.model)));
            NameRef name;
            name.category = NameCategory::Query;
            name.query = scope.queries.back().get();
            scope.names.insert_or_assign(query.name, name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            scope.mutations.push_back(std::make_unique<MutationRuntime>(mutation, scope, aliasOf(mutation.model)));
            NameRef name;
            name.category = NameCategory::Mutation;
            name.mutation = scope.mutations.back().get();
            scope.names.insert_or_assign(mutation.name, name);
        }
    }
    for (auto const& slot : scope.states) {
        initState(scope, *slot);
    }
    for (auto const& element : scope.elements) {
        wireElement(*element);
    }
    if (decl == nullptr) {
        return;
    }
    for (CheckDecl const& check : decl->checks) {
        auto made = std::make_unique<CheckRuntime>();
        made->decl = &check;
        made->failing = &scope.nodes.computed([&scope, expr = check.assertion] {
            Value const asserted = evalIn(scope, nullptr, expr);
            bool const* const flag = asserted.asBool();
            return flag != nullptr && !*flag;
        });
        made->message = &scope.nodes.computed(
            [&scope, expr = check.message] { return textOf(evalIn(scope, nullptr, expr), scope.formatting()); });
        for (std::string const& target : check.targets) {
            NameRef const* const found = scope.find(target);
            if (found != nullptr && found->category == NameCategory::Element) {
                found->element->checks.push_back(made.get());
                made->targets.push_back(found->element);
                found->element->checksVersion.set(found->element->checksVersion.peek() + 1);
            }
        }
        scope.checks.push_back(std::move(made));
    }
    for (auto const& query : scope.queries) {
        query->start();
    }
    for (WatchDecl const& watch : decl->watches) {
        auto& source = scope.nodes.computed([&scope, expr = watch.on] { return evalIn(scope, nullptr, expr); });
        scope.nodes.effect([this, &scope, &source, watched = &watch, first = true] mutable {
            static_cast<void>(source.get());
            if (first) {
                first = false;
                return;
            }
            bool const user = session.lastWriteByUser();
            if (watched->source == WatchSource::User && !user) {
                return;
            }
            runtime().untracked([&] {
                CommandCtx const ctx{
                    .scope = scope.shared_from_this(),
                    .bindings = std::make_shared<Bindings>(),
                    .cancelClose = std::make_shared<bool>(false),
                    .boundary = nullptr,
                };
                if (user) {
                    session.asUser([&] { runList(watched->body, ctx, {}); });
                } else {
                    runList(watched->body, ctx, {});
                }
            });
        });
    }
}

namespace {

/// @brief What a date input reads from a value: the instant it shows, and whether the value names one at all.
struct DateReading {
    /// @brief The instant shown; empty for an empty field.
    std::optional<time::DateTime> instant;
    /// @brief Whether the value names a date, an instant or nothing; `false` for text that names no date.
    bool readable = true;
};

/// @brief Reads what a date input shows for @p value, in a display zone @p offsetMinutes from UTC.
///
/// The value is read as the date functions read it (`readDateValue`): a timestamp is its instant; a `yyyy-MM-dd`
/// string is midnight of that date in the display zone; a string `DateTime::fromIso8601` reads is its instant; `null`
/// and `""` are empty. Any other value is empty and unreadable.
/// @param value         The value.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return The instant, or nothing, and whether the value was readable.
[[nodiscard]] DateReading dateInstantOf(Value const& value, std::int32_t offsetMinutes) {
    DateValue const reading = readDateValue(value);
    switch (reading.form) {
        case DateForm::Absent:
            return {};
        case DateForm::Date:
            return {
                .instant = time::DateTime{std::chrono::sys_days{reading.date} - std::chrono::minutes{offsetMinutes}},
            };
        case DateForm::Timestamp:
        case DateForm::InstantText:
            return {.instant = reading.instant};
        case DateForm::Unreadable:
        default:
            return {.instant = std::nullopt, .readable = false};
    }
}

/// @brief A date input's date text for @p instant in a display zone @p offsetMinutes from UTC.
/// @param instant       The instant.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return `yyyy-MM-dd`.
[[nodiscard]] std::string dateTextOf(time::DateTime const& instant, std::int32_t offsetMinutes) {
    auto const local = instant.value + std::chrono::minutes{offsetMinutes};
    std::chrono::year_month_day const date{std::chrono::floor<std::chrono::days>(local)};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()));
}

/// @brief A value as the places a number is entered with.
/// @param value  The value.
/// @param format The formatting facts.
/// @return The places, 0 to the most a decimal holds; -1 for a value that is not a non-negative integer.
[[nodiscard]] int placesOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);  // Only to fit `bindProp`'s converter signature.
    std::int64_t const* const whole = value.asInt();
    if (whole == nullptr || *whole < 0) {
        return -1;
    }
    return static_cast<int>(std::min<std::int64_t>(*whole, math::kMaxDecimalPlaces));
}

/// @brief The fraction digits of a number as typed in the active locale.
/// @param text   The typed text.
/// @param format The formatting facts.
/// @return The digits after the decimal separator; 0 for a text that is not a number, which no places rule judges.
[[nodiscard]] std::size_t fractionDigits(std::string_view text, FormatContext const& format) {
    std::optional<std::string> const canonical = render::normalizeLocaleNumber(text, withoutGrouping(format.numeric));
    if (!canonical.has_value()) {
        return 0;
    }
    auto const dot = canonical->find('.');
    return dot == std::string::npos ? 0 : canonical->size() - dot - 1;
}

/// @brief Whether two values show the same in an input: by text for a text input, by instant for a date input, by
///        value otherwise.
/// @param kind   The input's kind.
/// @param lhs    One value.
/// @param rhs    The other.
/// @param format The formatting facts.
/// @return It.
[[nodiscard]] bool sameShown(std::string_view kind, Value const& lhs, Value const& rhs, FormatContext const& format) {
    if (kind == "textInput" || kind == "filePicker") {
        return textOf(lhs, format) == textOf(rhs, format);
    }
    if (kind == "dateTimeInput") {
        return dateInstantOf(lhs, format.utcOffsetMinutes).instant ==
               dateInstantOf(rhs, format.utcOffsetMinutes).instant;
    }
    return lhs == rhs;
}

/// @brief The key of a select option: a record's `key`, `id` or `value` field, or the option itself.
/// @param option The option.
/// @return The key.
[[nodiscard]] Value optionKey(Value const& option) {
    if (option.asRecord() == nullptr) {
        return option;
    }
    for (std::string_view const field : {"key", "id", "value"}) {
        if (Value const* const found = option.field(field)) {
            return *found;
        }
    }
    return Value{};
}
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one rule per source of an error.
void ScreenCore::wireErrors(ElementState& element) {
    RtScope& scope = *element.scope;
    element.errors = &scope.nodes.computed([this, &element] -> Value {
        static_cast<void>(element.checksVersion.get());
        DocNode const& field = *element.node;
        RtScope const& owner = *element.scope;
        std::vector<std::string> errors;
        // A group's `required` is its legend's mark only: a group has no value, so no `Required` is derived from it.
        if (ExprPtr const required = field.kind != "group" ? field.prop("required") : nullptr) {
            Value const requiredNow = evalIn(owner, nullptr, required);
            bool const* const flag = requiredNow.asBool();
            if (flag != nullptr && *flag && !engaged(inputValue(element))) {
                errors.emplace_back("Required");
            }
        }
        for (std::string& text : stringsOf(evalIn(owner, nullptr, field.prop("errors")))) {
            errors.push_back(std::move(text));
        }
        if (ExprPtr const places = field.kind == "textInput" ? field.prop("decimals") : nullptr) {
            int const allowed = placesOf(evalIn(owner, nullptr, places), session.formatting());
            if (allowed >= 0 && fractionDigits(textOf(inputValue(element), session.formatting()),
                                               session.formatting()) > static_cast<std::size_t>(allowed)) {
                // The key and fallback of a form field's `too-precise`, so both read alike in any language.
                errors.emplace_back(render::resolveText(session.formatting().translations, session.formatting().locale,
                                                        std::nullopt, "morph.forms.error.too-precise",
                                                        "Too many decimal places"));
            }
        }
        if (!field.id.empty()) {
            std::vector<std::string> const path{"errors", field.id};
            for (auto const& query : owner.queries) {
                for (std::string& text : stringsOf(walk(query->value(), path))) {
                    errors.push_back(std::move(text));
                }
            }
            for (auto const& mutation : owner.mutations) {
                for (std::string& text : stringsOf(walk(mutation->result(), path))) {
                    errors.push_back(std::move(text));
                }
            }
        }
        for (CheckRuntime const* const check : element.checks) {
            if (check->failing->get()) {
                errors.push_back(check->message->get());
            }
        }
        return listOfStrings(errors);
    });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of element.
void ScreenCore::wireElement(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string_view const kind = node.kind;
    if (kind == "form") {
        wireForm(element);
        return;
    }
    if (kind == "collection" || kind == "wizard") {
        wireComposite(element);
        return;
    }
    bool const input = kind == "textInput" || kind == "checkbox" || kind == "select" || kind == "dateTimeInput" ||
                       kind == "slider" || kind == "filePicker";
    if (input) {
        std::string_view key = "value";
        if (kind == "checkbox") {
            key = "checked";
        } else if (kind == "select") {
            key = "selected";
        }
        ExprPtr const expr = kind == "filePicker" ? nullptr : node.prop(key);
        bool const controlled = expr && (expr->kind != ExprKind::Literal || !expr->literal.isNull());
        if (controlled) {
            element.bound = &scope.nodes.computed([&scope, expr] { return evalIn(scope, nullptr, expr); });
            element.draft.set(runtime().untracked([&] { return element.bound->get(); }));
            scope.nodes.effect([this, &element, kind] {
                Value const bound = element.bound->get();
                runtime().untracked([&] {
                    if (!element.edited.peek()) {
                        element.draft.set(bound);
                    } else if (sameShown(kind, element.draft.peek(), bound, session.formatting())) {
                        element.edited.set(false);
                    }
                });
            });
        } else if (kind == "checkbox") {
            element.draft.set(false);
        }
    }
    if (kind == "select") {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("options")] { return evalIn(scope, nullptr, expr); });
        if (element.bound == nullptr && node.name("onStale") == "clear") {
            scope.nodes.effect([this, &element] {
                Value const options = element.rows->get();
                runtime().untracked([&] {
                    Value const selected = element.draft.peek();
                    if (selected.isNull()) {
                        return;
                    }
                    ValueList const* const items = options.asList();
                    bool const kept = items != nullptr && std::ranges::any_of(*items, [&](Value const& option) {
                                          return optionKey(option) == selected;
                                      });
                    if (!kept) {
                        session.noteWrite();
                        element.draft.set(Value{});
                    }
                });
            });
        }
    }
    if (kind == "table" && node.prop("rows")) {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("rows")] { return evalIn(scope, nullptr, expr); });
        // A bound selection is controlled, as a select's `selected` is: any expression but a literal `null` binds it.
        ExprPtr const selection = node.prop("selection");
        if (selection && (selection->kind != ExprKind::Literal || !selection->literal.isNull())) {
            element.bound = &scope.nodes.computed([&scope, selection] { return evalIn(scope, nullptr, selection); });
        }
        // A bound sort is controlled as the selection is.
        ExprPtr const sort = node.prop("sort");
        if (sort && (sort->kind != ExprKind::Literal || !sort->literal.isNull())) {
            element.sortBound = &scope.nodes.computed([&scope, sort] { return evalIn(scope, nullptr, sort); });
        }
        element.view = &scope.nodes.computed([this, &element] { return tableView(element, nullptr); });
    }
}

namespace {

/// @brief A table's sort as its element value reads it: the bound `sort` when the key binds one, else the table's
///        own.
/// @param element The table's element.
/// @return The sort. Tracked.
[[nodiscard]] Value tableSort(ElementState const& element) {
    return element.sortBound != nullptr ? element.sortBound->get() : element.sort.get();
}

/// @brief A table's page values: `viewCount` (the rows it orders, every page), `page`, `pageCount`, and
///        `pageFirst` and `pageLast`, the 1-based positions of the page's first and last rows, both 0 when there are
///        none. A table that does not page reads page 0 of 1, over every row.
/// @param element The table's element.
/// @param name    The value's name.
/// @return The value. Tracked.
[[nodiscard]] Value tablePageValue(ElementState const& element, std::string_view name) {
    if (element.view == nullptr) {
        return name == "pageCount" ? std::int64_t{1} : std::int64_t{0};
    }
    TableView const& view = element.view->get();
    if (name == "viewCount") {
        return static_cast<std::int64_t>(view.rows.size());
    }
    if (name == "page") {
        return view.page;
    }
    if (name == "pageCount") {
        return view.pageCount;
    }
    bool const none = view.first == view.last;
    if (name == "pageFirst") {
        return none ? std::int64_t{0} : static_cast<std::int64_t>(view.first + 1);
    }
    return static_cast<std::int64_t>(view.last);
}

/// @brief The selected keys of the rows a table holds on pages other than the one shown, in their order: what an
///        `Extend` report keeps, since the backend holds the shown page alone.
/// @param view      The table's rows and page.
/// @param selection The selection, a list of keys.
/// @param rowKey    A row's key.
/// @return The keys.
[[nodiscard]] ValueList selectedOffPage(TableView const& view, Value const& selection,
                                        std::function<Key(Value const&)> const& rowKey) {
    std::unordered_set<Key> onPage;
    std::unordered_set<Key> offPage;
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        (index >= view.first && index < view.last ? onPage : offPage).insert(rowKey(view.rows.at(index)));
    }
    ValueList kept;
    if (ValueList const* const selected = selection.asList()) {
        for (Value const& item : *selected) {
            Key const key = keyOf(item);
            if (offPage.contains(key) && !onPage.contains(key)) {
                kept.push_back(item);
            }
        }
    }
    return kept;
}

/// @brief The sortable column part of a table with the id @p columnId.
/// @param table    The table.
/// @param columnId The column's id.
/// @return The part, or `nullptr` when no sortable column has that id.
[[nodiscard]] DocNode const* sortableColumn(DocNode const& table, std::string_view columnId) {
    for (DocNodePtr const& column : table.partList("columns")) {
        if (column->id == columnId && column->flag("sortable")) {
            return column.get();
        }
    }
    return nullptr;
}

/// @brief One key of a table's sort: the column part that orders the rows, and whether it is descending.
struct SortKey {
    /// @brief The sortable column.
    DocNode const* column = nullptr;
    /// @brief Whether the key sorts descending.
    bool descending = false;
};

/// @brief A table's sort read as its keys: a list of `{"column", "direction"}` records naming sortable columns.
/// @param table The table.
/// @param sort  The sort; `null` is the source order, as `[]` is.
/// @return The keys, most significant first; `nullopt` when @p sort is not such a list.
[[nodiscard]] std::optional<std::vector<SortKey>> sortKeysOf(DocNode const& table, Value const& sort) {
    std::vector<SortKey> keys;
    if (sort.isNull()) {
        return keys;
    }
    ValueList const* const items = sort.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    for (Value const& item : *items) {
        Value const* const column = item.field("column");
        Value const* const direction = item.field("direction");
        std::string const* const columnId = column == nullptr ? nullptr : column->asString();
        std::string const* const way = direction == nullptr ? nullptr : direction->asString();
        DocNode const* const part = columnId == nullptr ? nullptr : sortableColumn(table, *columnId);
        if (part == nullptr || way == nullptr || (*way != "asc" && *way != "desc")) {
            return std::nullopt;
        }
        keys.push_back(SortKey{.column = part, .descending = *way == "desc"});
    }
    return keys;
}
}  // namespace

void ScreenCore::groupRows(ElementState const& element, Bindings const* bindings, TableView& view) {
    auto const parts = element.node->partList("group");
    if (parts.empty()) {
        return;
    }
    ExprPtr const groupBy = parts.front()->prop("by");
    RtScope const& scope = *element.scope;
    ScopeEnv const tableEnv{scope, bindings};
    Evaluator runs{tableEnv};
    // A heading's key is its run's key and its ordinal among the runs with an equal key, since adjacent grouping
    // can give two runs one key (samples A, B, A); the ordinal stays put across a sort, which moves no run.
    std::map<std::string, std::size_t, std::less<>> ordinals;
    Value key;
    ValueList run;
    auto const close = [&] {
        std::string const json = canonicalJson(key);
        Value const record = runRecord(key, std::exchange(run, {}));
        ValueRecord entry = *record.asRecord();
        entry.insert_or_assign(std::string{kGroupHeadingField},
                               Value{"\x1Fgroup:" + json + ":" + std::to_string(ordinals[json]++)});
        view.runs.emplace_back(std::move(entry));
    };
    for (Value const& row : view.rows) {
        Value value = evaluate(*groupBy, ScopeEnv{scope, bindings, &row});
        if (!run.empty() && !runs.sameRun(key, value)) {
            close();
        }
        if (run.empty()) {
            key = std::move(value);
        }
        run.push_back(row);
        view.runOf.push_back(view.runs.size());
    }
    if (!run.empty()) {
        close();
    }
}

void ScreenCore::sortedRows(ElementState const& element, Bindings const* bindings, TableView& view) {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    Value const source = bindings == nullptr ? element.rows->get() : evalIn(scope, bindings, node.prop("rows"));
    ValueList const* const items = source.asList();
    if (items == nullptr) {
        return;
    }
    view.rows.assign(items->begin(), items->end());
    groupRows(element, bindings, view);
    Value const sort = tableSort(element);
    std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, sort);
    std::string const sortPath = pointerAppend(node.path, "sort");
    if (!keys) {
        report(
            sortPath,
            "sort is " + canonicalJson(sort) +
                R"(; a table's sort is a list of {"column": <a sortable column's id>, "direction": "asc" or "desc"})");
        return;
    }
    if (keys->empty()) {
        return;
    }
    // Each row's keys are read once, then the rows are ordered by them: a change to the sort, the rows, or anything
    // a `sortBy` reads sorts again. A grouped table orders by the group first, so every group keeps its place in
    // the source order and its rows are sorted within it.
    bool const grouped = !view.runOf.empty();
    std::vector<std::vector<Value>> rowKeys;
    rowKeys.reserve(view.rows.size());
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        Value const& row = view.rows.at(index);
        ScopeEnv const env{scope, bindings, &row};
        std::vector<Value> values;
        values.reserve(keys->size() + 1);
        if (grouped) {
            values.emplace_back(static_cast<std::int64_t>(view.runOf.at(index)));
        }
        for (SortKey const& key : *keys) {
            if (ExprPtr const sortBy = key.column->prop("sortBy")) {
                values.push_back(evaluate(*sortBy, env));
            } else {
                Value const* const field = row.field(key.column->id);
                values.push_back(field == nullptr ? Value{} : *field);
            }
        }
        rowKeys.push_back(std::move(values));
    }
    std::vector<bool> descending;
    descending.reserve(keys->size() + 1);
    if (grouped) {
        descending.push_back(false);
    }
    for (SortKey const& key : *keys) {
        descending.push_back(key.descending);
    }
    ScopeEnv const env{scope, bindings};
    std::optional<std::vector<std::size_t>> const order = Evaluator{env}.sortRows(rowKeys, descending);
    if (!order) {
        report(sortPath, "the rows' values for the sort are not of one comparable kind; the rows keep their order");
        return;
    }
    std::vector<Value> sorted;
    std::vector<std::size_t> runOf;
    sorted.reserve(view.rows.size());
    for (std::size_t const index : *order) {
        sorted.push_back(view.rows.at(index));
        if (grouped) {
            runOf.push_back(view.runOf.at(index));
        }
    }
    view.rows = std::move(sorted);
    view.runOf = std::move(runOf);
}

TableView ScreenCore::tableView(ElementState const& element, Bindings const* bindings) {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    TableView view;
    sortedRows(element, bindings, view);
    view.last = view.rows.size();
    ExprPtr const sizeExpr = node.prop("pageSize");
    if (!sizeExpr) {
        return view;
    }
    // A `pageSize` of `null` does not page; one that is no positive integer is reported and does not page either.
    Value const size = evalIn(scope, bindings, sizeExpr);
    std::int64_t const* const perPage = size.asInt();
    if (size.isNull() || perPage == nullptr || *perPage < 1) {
        if (!size.isNull()) {
            report(pointerAppend(node.path, "pageSize"),
                   "pageSize is " + canonicalJson(size) + "; a page holds a positive whole number of rows");
        }
        return view;
    }
    // A `page` of `null` is the first; one that is negative or not a whole number is reported and shows the first.
    // One past the last is the last: a filter that shrinks the rows under the user is normal, not an error.
    std::int64_t asked = 0;
    if (ExprPtr const pageExpr = node.prop("page")) {
        Value const page = evalIn(scope, bindings, pageExpr);
        std::int64_t const* const index = page.asInt();
        if (index != nullptr && *index >= 0) {
            asked = *index;
        } else if (!page.isNull()) {
            report(pointerAppend(node.path, "page"),
                   "page is " + canonicalJson(page) + "; a page is a whole number from 0");
        }
    }
    auto const total = static_cast<std::int64_t>(view.rows.size());
    view.pageCount = std::max<std::int64_t>(1, (total + *perPage - 1) / *perPage);
    view.page = std::min(asked, view.pageCount - 1);
    view.first = static_cast<std::size_t>(std::min(total, view.page * *perPage));
    view.last = static_cast<std::size_t>(std::min(total, (view.page + 1) * *perPage));
    return view;
}

namespace {

/// @brief A table's selection as its element value reads it: the bound `selection` when the key binds one, else
///        the table's own. A bound value that is not a list (`null` while state starts empty) selects nothing.
/// @param element The table's element.
/// @return A list of keys. Tracked.
[[nodiscard]] Value tableSelection(ElementState const& element) {
    if (element.bound == nullptr) {
        return element.selection.get();
    }
    Value const bound = element.bound->get();
    return bound.asList() != nullptr ? bound : Value{ValueList{}};
}
}  // namespace

[[nodiscard]] Value jsonTextValue(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return Value{};
    }
    auto value = valueFromJson(*json);
    return value ? *std::move(value) : Value{};
}

namespace {

/// @brief One field of a form as a record: `value` (the encoded value, `null` while blank or invalid), `draft` (the
///        text), `valid`, `required`, `visible`, `readonly` and `errors`. Tracked.
/// @param field The field.
/// @return The record.
[[nodiscard]] Value formFieldValue(forms::FieldState const& field) {
    ValueRecord record;
    forms::Encoding const& encoding = field.encoded().get();
    Value value;
    if (encoding.has_value() && encoding->has_value()) {
        value = (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
    }
    std::optional<forms::FieldError> const& error = field.error().get();
    record.insert_or_assign("value", std::move(value));
    record.insert_or_assign("draft", Value{field.spec().kind == forms::FieldKind::Object ||
                                                   field.spec().kind == forms::FieldKind::ObjectArray
                                               ? std::string{}
                                               : field.draft()});
    record.insert_or_assign("valid", Value{!error.has_value()});
    record.insert_or_assign("required", Value{field.required().get()});
    record.insert_or_assign("visible", Value{field.visible().get()});
    record.insert_or_assign("readonly", Value{field.readonly().get()});
    record.insert_or_assign("errors",
                            error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}});
    return Value{std::move(record)};
}

/// @brief A form's element values (spec 5 §6): `ready`, `body` (the record the form would submit, `null` while not
///        ready), `submitted` (whether a submission was sent), `pending`, and each field by its wire name. Tracked.
/// @param element The form's element.
/// @param name    The value's name.
/// @return The value; `null` for a name the form does not have, or a form without a session.
[[nodiscard]] Value formValue(ElementState const& element, std::string_view name) {
    forms::FormSession const* const form = element.form.get();
    if (form == nullptr) {
        return Value{};
    }
    if (name == "ready") {
        return form->ready();
    }
    if (name == "body") {
        std::optional<std::string> const body = form->body();
        return body.has_value() ? jsonTextValue(*body) : Value{};
    }
    if (name == "submitted") {
        return form->submitAttempts() > 0;
    }
    if (name == "pending") {
        return form->pending();
    }
    for (auto const& field : form->fields()) {
        if (field->spec().name == name) {
            return formFieldValue(*field);
        }
    }
    return Value{};
}
}  // namespace

Value ScreenCore::inputValue(ElementState const& element) const {
    std::string_view const kind = element.node->kind;
    if (kind == "checkbox") {
        return elementValue(element, "checked");
    }
    if (kind == "select") {
        return elementValue(element, "selected");
    }
    return elementValue(element, "value");
}

// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): one case per element value.
Value ScreenCore::elementValue(ElementState const& element, std::string_view name) const {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    std::string_view const kind = node.kind;
    auto const flagOf = [&](std::string_view key, bool fallback) -> Value {
        ExprPtr const expr = node.prop(key);
        if (!expr) {
            return fallback;
        }
        Value const value = evalIn(scope, nullptr, expr);
        bool const* const flag = value.asBool();
        return flag != nullptr && *flag;
    };
    auto const errors = [&] -> Value { return element.errors->get(); };
    auto const count = [](Value const& list) -> Value {
        ValueList const* const items = list.asList();
        return static_cast<std::int64_t>(items == nullptr ? 0 : items->size());
    };
    if (name == "visible" || name == "enabled") {
        return flagOf(name, true);
    }
    if (name == "errors") {
        return errors();
    }
    if (name == "valid") {
        Value const current = errors();
        ValueList const* const items = current.asList();
        return items == nullptr || items->empty();
    }
    if (name == "shownErrors") {
        RtScope const& policy = scope.policyScope();
        std::string const mode = policy.showErrors.empty() ? _doc->showErrors : policy.showErrors;
        bool const submittedNow = policy.submitted != nullptr && policy.submitted->get();
        if (mode == "always" || (mode == "submit" && submittedNow) ||
            (mode != "submit" && (submittedNow || element.touched.get() || touchedWithin(element)))) {
            return errors();
        }
        return Value{ValueList{}};
    }
    if (kind == "dialog" || kind == "drawer") {
        if (name == "open") {
            return node.prop("open") ? flagOf("open", false) : Value{element.open.get()};
        }
        if (name == "ready") {
            RtScope const* const inner = element.inner.get();
            return inner != nullptr && inner->ready();
        }
        if (name == "submitted") {
            return element.submitted.get();
        }
        return Value{};
    }
    if (kind == "table") {
        if (name == "selection") {
            return tableSelection(element);
        }
        if (name == "activeKey") {
            return element.activeKey.get();
        }
        if (name == "selectedCount") {
            return count(tableSelection(element));
        }
        if (name == "sort") {
            return tableSort(element);
        }
        if (name == "viewCount" || name == "page" || name == "pageCount" || name == "pageFirst" ||
            name == "pageLast") {
            return tablePageValue(element, name);
        }
        if (name == "sourceCount") {
            return element.rows == nullptr ? Value{std::int64_t{0}} : count(element.rows->get());
        }
        ExprPtr const rows = node.prop("rows");
        if ((name == "pending" || name == "error") && rows && rows->kind == ExprKind::Query) {
            ScopeEnv const env{scope, nullptr};
            return env.control(name == "pending" ? ExprKind::Pending : ExprKind::Error, rows->name);
        }
        return name == "pending" ? Value{false} : Value{};
    }
    if (kind == "tabs") {
        // The shown tab's id: the bound one, else the user's own pick, else the first tab's.
        ExprPtr const selected = node.prop("selected");
        if (name != "selected") {
            return Value{};
        }
        if (selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull())) {
            return evalIn(scope, nullptr, selected);
        }
        if (Value picked = element.draft.get(); !picked.isNull()) {
            return picked;
        }
        auto const tabs = node.partList("tabs");
        return tabs.empty() || tabs.front()->id.empty() ? Value{} : Value{tabs.front()->id};
    }
    if (kind == "form") {
        return formValue(element, name);
    }
    if (name == "draft") {
        return element.draft.get();
    }
    if (name == "dirty") {
        return element.edited.get();
    }
    if (name == "focused") {
        return element.focused.get();
    }
    if (name == "touched") {
        return element.touched.get();
    }
    if (name == "checked" || name == "selected") {
        return element.bound != nullptr ? element.bound->get() : element.draft.get();
    }
    if (name == "selectedOption") {
        Value const selected = elementValue(element, "selected");
        ValueList const* const options = element.rows == nullptr ? nullptr : element.rows->get().asList();
        if (options != nullptr && !selected.isNull()) {
            for (Value const& option : *options) {
                if (optionKey(option) == selected) {
                    return option;
                }
            }
        }
        return Value{};
    }
    if (name == "value") {
        if (element.bound != nullptr) {
            return element.bound->get();
        }
        return element.committed.get();
    }
    return Value{};
}

bool ScreenCore::touchedWithin(ElementState const& element) {
    if (element.node->kind != "group") {
        return false;
    }
    std::string const inside = element.node->path + "/";
    return std::ranges::any_of(element.scope->elements, [&inside](std::unique_ptr<ElementState> const& other) {
        return other->node->path.starts_with(inside) && other->touched.get();
    });
}

// --- Commands --------------------------------------------------------------------------------------------------

// NOLINTBEGIN(misc-no-recursion): command lists nest
void ScreenCore::runList(Commands const& commands, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    if (commands.empty()) {
        done(true);
        return;
    }
    struct Join {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    if (recording) {
        logList(commands);
    }
    auto const join = std::make_shared<Join>(Join{.left = commands.size() + 1, .ok = true, .done = std::move(done)});
    auto const finish = [join](bool succeeded) {
        join->ok = join->ok && succeeded;
        if (--join->left == 0) {
            join->done(join->ok);
        }
    };
    for (Command const& command : commands) {
        runOne(command, ctx, finish);
    }
    finish(true);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
void ScreenCore::runOne(Command const& command, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    RtScope const& scope = *ctx.scope;
    auto const eval = [&](ExprPtr const& expr) { return evalIn(scope, ctx.bindings.get(), expr); };
    auto const finish = [&done](bool succeeded) { done(succeeded); };
    switch (command.kind) {
        case CommandKind::Set:
            runSet(command, ctx, done);
            return;
        case CommandKind::Reset:
            runReset(command, ctx, done);
            return;
        case CommandKind::Run:
            runMutation(command, ctx, std::move(done));
            return;
        case CommandKind::Refetch: {
            // A loaded document's `refetch` names a query in scope (`loadDocument` checks it), as `run` names a
            // mutation and `open` a dialog.
            scope.find(command.target)->query->refetch();
            finish(true);
            return;
        }
        case CommandKind::Open:
        case CommandKind::Close: {
            ElementState* target = nullptr;
            if (command.kind == CommandKind::Close && command.target == "self") {
                // `self` is the innermost dialog or drawer whose content holds the command, found lexically. A
                // component's fallback scope has no parent, so the walk stops at its boundary as its names do.
                RtScope const* walk = &scope;
                while (walk->owner == nullptr && walk->parent) {
                    walk = walk->parent.get();
                }
                target = walk->owner;
                if (target == nullptr && walk != rootScope.lock().get()) {
                    report(command.path, "`close: self` inside a component names no dialog or drawer");
                    finish(false);
                    return;
                }
                if (target == nullptr) {
                    finish((host != nullptr && host->closeSelf(_doc->id, std::nullopt)) ||
                           unsupported(command, "`close: self`"));
                    return;
                }
            } else {
                target = scope.find(command.target)->element;
            }
            ElementState& element = *target;
            if (element.node->prop("open")) {
                report(command.path, "the dialog's `open` is bound to an expression");
                finish(false);
                return;
            }
            bool const opening = command.kind == CommandKind::Open;
            if (opening && !element.open.peek()) {
                element.submitted.set(false);
            }
            session.noteWrite();
            element.open.set(opening);
            finish(true);
            return;
        }
        case CommandKind::Navigate: {
            NavigateRequest const request{
                .screen = command.target,
                .route = eval(command.extra),
                .params = eval(command.value),
                .as = command.option,
                .call = false,
            };
            if (host == nullptr) {
                finish(unsupported(command, "`navigate`"));
                return;
            }
            // A navigation without `onResult` and `onCancel` asks for no answer, so it files no call: the screen it
            // opens answers nobody, and its `return` is the application's.
            if (command.commands.empty() && command.alternative.empty()) {
                finish(host->navigate(request, {}, {}) || unsupported(command, "`navigate`"));
                return;
            }
            std::weak_ptr<RtScope> const weak = ctx.scope;
            CommandCtx const kept = detached(ctx);
            auto onResult = [this, weak, kept, list = &command.commands](Value const& result) {
                if (auto const held = weak.lock()) {
                    auto const bindings = std::make_shared<Bindings>(*kept.bindings);
                    bindings->push_back(Binding{.name = "event", .read = [result] { return result; }});
                    CommandCtx const next{
                        .scope = held,
                        .bindings = bindings,
                        .cancelClose = kept.cancelClose,
                        .boundary = kept.boundary,
                    };
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            auto onCancel = [this, weak, kept, list = &command.alternative] {
                if (auto const held = weak.lock()) {
                    CommandCtx next = kept;
                    next.scope = held;
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            finish(host->navigate(request, std::move(onResult), std::move(onCancel)) ||
                   unsupported(command, "`navigate`"));
            return;
        }
        case CommandKind::Return:
            finish(host != nullptr && host->closeSelf(_doc->id, eval(command.value))
                       ? true
                       : unsupported(command, "`return`"));
            return;
        case CommandKind::Focus:
            if (focusTarget(command, scope, ctx.bindings.get())) {
                finish(true);
                return;
            }
            report(command.path, "`" + command.target + "` is not mounted");
            finish(false);
            return;
        case CommandKind::Host:
            runHostCommand(command, ctx, done);
            return;
        case CommandKind::SwitchBackend: {
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto committed = [this, weak, target = command.target, path = command.path,
                              done](std::expected<void, std::string> const& outcome) {
                if (!weak.lock()) {
                    return;
                }
                if (!outcome) {
                    report(path, "backendSwitchFailed: " + outcome.error());
                    done(false);
                    return;
                }
                session.backendChanged(target);
                done(true);
            };
            if (host != nullptr && host->switchBackend(command.target, std::move(committed))) {
                return;
            }
            report(command.path, "backend `" + command.target + "` is not listed");
            finish(false);
            return;
        }
        case CommandKind::ResetPrefs: {
            ValueRecord prefsNow = prefs.peek();
            if (command.target == "all") {
                prefsNow = prefDefaults;
            } else if (auto const found = prefDefaults.find(command.target); found != prefDefaults.end()) {
                prefsNow.insert_or_assign(command.target, found->second);
            }
            prefs.set(std::move(prefsNow));
            if (host != nullptr) {
                host->resetPrefs(_doc->id, command.target);
            }
            finish(true);
            return;
        }
        case CommandKind::SetLocale:
            session.setLocale(command.target);
            if (host != nullptr) {
                host->setLocale(command.target);
            }
            finish(true);
            return;
        case CommandKind::SetContext: {
            // The shell checks the value against the switcher's options; with no switcher it is unsupported.
            Value const requested = eval(command.value);
            if (session.requestContext(requested)) {
                finish(true);
            } else {
                report(command.path,
                       "`setContext` was refused: " + std::string{session.can("context")
                                                                      ? "the value is not one of the scope's options"
                                                                      : "no scope switcher is declared"});
                finish(false);
            }
            return;
        }
        case CommandKind::Notify: {
            std::string const text = textOf(eval(command.value), ctx.scope->formatting());
            finish(host != nullptr && host->notify(text, command.option) ? true : unsupported(command, "`notify`"));
            return;
        }
        case CommandKind::Seq: {
            // A step that waits for a reply keeps the rest of the sequence in that reply's handler, which the
            // dispatcher may hold past the screen: the sequence holds its scope weakly and stops once it is gone.
            CommandCtx const inner = detached(CommandCtx{
                .scope = nullptr,
                .bindings = std::make_shared<Bindings>(*ctx.bindings),
                .cancelClose = ctx.cancelClose,
                .boundary = ctx.boundary,
            });
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto const step = std::make_shared<std::function<void(std::size_t)>>();
            *step = [this, inner, weak, list = &command.commands, done = std::move(done),
                     weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
                auto const held = weak.lock();
                if (index == list->size() || !held) {
                    done(held != nullptr);
                    return;
                }
                auto const self = weakStep.lock();
                CommandCtx current = inner;
                current.scope = held;
                runOne(list->at(index), current, [self, index, done](bool succeeded) {
                    if (!succeeded) {
                        done(false);
                        return;
                    }
                    (*self)(index + 1);
                });
            };
            (*step)(0);
            return;
        }
        case CommandKind::All: {
            CommandCtx const inner{
                .scope = ctx.scope,
                .bindings = std::make_shared<Bindings>(*ctx.bindings),
                .cancelClose = ctx.cancelClose,
                .boundary = ctx.boundary,
            };
            runList(command.commands, inner, std::move(done));
            return;
        }
        case CommandKind::If: {
            Value const condition = eval(command.value);
            bool const* const flag = condition.asBool();
            if (flag == nullptr) {
                finish(true);
                return;
            }
            runList(*flag ? command.commands : command.alternative, ctx, std::move(done));
            return;
        }
        case CommandKind::CancelClose:
            *ctx.cancelClose = true;
            finish(true);
            return;
        case CommandKind::Retry:
            // `retry` is valid only inside a boundary's `error`, which is lowered with its boundary.
            for (QueryRuntime* const query : ctx.boundary->queries) {
                if (!runtime().untracked([&] { return query->error(); }).isNull()) {
                    query->refetch();
                }
            }
            finish(true);
            return;
        case CommandKind::SignIn: {
            NameRef const* const found = scope.find(command.target);
            Value const reply = found != nullptr && found->category == NameCategory::Mutation
                                    ? runtime().untracked([&] { return found->mutation->result(); })
                                    : Value{};
            session.clear();
            finish(host != nullptr && host->signIn(reply) ? true : unsupported(command, "`signIn`"));
            return;
        }
        case CommandKind::SignOut:
            session.clear();
            if (host != nullptr) {
                host->signOut();
            }
            finish(true);
            return;
        case CommandKind::Upload:
        case CommandKind::Download:
        default:
            runTransfer(command, ctx, std::move(done));
            return;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a single run, or a run per list item.
void ScreenCore::runMutation(Command const& command, CommandCtx const& ctx, Done done) const {
    RtScope const& scope = *ctx.scope;
    MutationRuntime& mutation = *scope.find(command.target)->mutation;
    RtScope const& policy = scope.policyScope();
    if (policy.submitted != nullptr) {
        policy.submitted->set(true);
    }
    if (policy.owner != nullptr && !runtime().untracked([&] { return policy.ready(); })) {
        done(false);
        return;
    }
    RtScope const& declaring = *[&] -> RtScope const* {
        for (RtScope const* candidate = &scope; candidate != nullptr; candidate = candidate->parent.get()) {
            if (candidate->names.contains(command.target)) {
                return candidate;
            }
        }
        return &scope;
    }();
    auto const bodyFor = [&](Bindings const& bindings) {
        if (command.value) {
            return evalIn(scope, &bindings, command.value);
        }
        if (mutation.decl().body) {
            return evalIn(declaring, &bindings, mutation.decl().body);
        }
        return Value{ValueRecord{}};
    };
    if (!command.extra) {
        Value body = bodyFor(*ctx.bindings);
        mutation.run(std::move(body), std::make_shared<Bindings const>(*ctx.bindings),
                     [bindings = ctx.bindings, bindAs = command.binding, done = std::move(done)](bool succeeded,
                                                                                                 Value const& reply) {
                         if (succeeded && !bindAs.empty()) {
                             bindings->push_back(Binding{.name = bindAs, .read = [reply] { return reply; }});
                         }
                         done(succeeded);
                     });
        return;
    }
    Value const each = evalIn(scope, ctx.bindings.get(), command.extra);
    ValueList const items = each.asList() == nullptr ? ValueList{} : *each.asList();
    if (items.empty()) {
        done(true);
        return;
    }
    struct Each {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    auto const state = std::make_shared<Each>(Each{.left = items.size(), .ok = true, .done = std::move(done)});
    std::vector<Value> bodies;
    std::vector<std::shared_ptr<Bindings const>> bound;
    for (Value const& item : items) {
        Bindings bindings = *ctx.bindings;
        if (!command.binding.empty()) {
            bindings.push_back(Binding{.name = command.binding, .read = [item] { return item; }});
        }
        bodies.push_back(bodyFor(bindings));
        bound.push_back(std::make_shared<Bindings const>(std::move(bindings)));
    }
    if (mutation.decl().concurrency != Concurrency::Exclusive) {
        for (std::size_t index = 0; index < bodies.size(); ++index) {
            mutation.run(bodies.at(index), bound.at(index), [state](bool succeeded, Value const&) {
                state->ok = state->ok && succeeded;
                if (--state->left == 0) {
                    state->done(state->ok);
                }
            });
        }
        return;
    }
    // An exclusive mutation refuses a run while another is in flight, so its elements go one after another.
    auto const step = std::make_shared<std::function<void(std::size_t)>>();
    *step = [&mutation, bodies, bound, state,
             weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
        if (index == bodies.size()) {
            state->done(state->ok);
            return;
        }
        auto const self = weakStep.lock();
        mutation.run(bodies.at(index), bound.at(index), [self, state, index](bool succeeded, Value const&) {
            if (!succeeded) {
                state->done(false);
                return;
            }
            (*self)(index + 1);
        });
    };
    (*step)(0);
}

// NOLINTEND(misc-no-recursion)
void ScreenCore::writeSlot(StateSlot& slot, std::span<std::string const> within, Value value, Command const& command,
                           Done const& done) {
    slot.scope->core.initState(*slot.scope, slot);
    Type const& type = slot.decl->type;
    if (within.empty() && !type.isAny() && !type.admits(value)) {
        report(command.path, "`set` writes a value that is not a " + type.name());
        done(false);
        return;
    }
    Value next = withPath(slot.value->peek(), within, std::move(value));
    session.noteWrite();
    slot.value->set(std::move(next));
    done(true);
}

void ScreenCore::runSetApp(Command const& command, std::span<std::string const> rest, Value value, Done const& done) {
    if (RtScope const* const app = session.appScope(); app != nullptr && !rest.empty()) {
        NameRef const* const named = app->find(rest.front());
        if (named != nullptr && named->category == NameCategory::State) {
            writeSlot(*named->state, rest.subspan(1), std::move(value), command, done);
            return;
        }
        report(command.path, "`app." + rest.front() + "` is not state of the app document");
        done(false);
        return;
    }
    std::string name;
    for (std::string const& segment : rest) {
        name += name.empty() ? segment : "." + segment;
    }
    done(host != nullptr && host->setAppValue(name, value) ? true : unsupported(command, "`set` of `app`"));
}

void ScreenCore::runSet(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    auto path = splitPath(command.target);
    if (!path || path->empty()) {
        done(false);
        return;
    }
    std::string const head = path->front();
    std::span<std::string const> const rest = std::span<std::string const>{*path}.subspan(1);
    Value value = evalIn(scope, ctx.bindings.get(), command.value);
    if (head == "app") {
        runSetApp(command, rest, std::move(value), done);
        return;
    }
    NameRef const* const found = scope.find(head);
    if (found != nullptr && found->category == NameCategory::State) {
        writeSlot(*found->state, rest, std::move(value), command, done);
        return;
    }
    if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        session.noteWrite();
        element.draft.set(value);
        if (element.bound == nullptr) {
            element.committed.set(std::move(value));
        }
        done(true);
        return;
    }
    report(command.path, "`set` names nothing writable here");
    done(false);
}

void ScreenCore::runReset(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    NameRef const* const found = scope.find(command.target);
    auto const resetStates = [this](RtScope const& owner) {
        for (auto const& slot : owner.states) {
            if (slot->value) {
                Value init = runtime().untracked([&] { return evalIn(owner, nullptr, slot->decl->init); });
                slot->value->set(std::move(init));
            }
        }
        for (auto const& element : owner.elements) {
            element->draft.set(element->bound != nullptr ? runtime().untracked([&] { return element->bound->get(); })
                                                         : Value{});
            element->committed.set(Value{});
            element->edited.set(false);
            element->touched.set(false);
        }
    };
    bool known = true;
    session.noteWrite();
    if (found != nullptr && found->category == NameCategory::State) {
        StateSlot& slot = *found->state;
        initState(*slot.scope, slot);
        slot.value->set(runtime().untracked([&] { return evalIn(*slot.scope, nullptr, slot.decl->init); }));
    } else if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        element.submitted.set(false);
        if (RtScope const* const inner = element.inner.peek()) {
            resetStates(*inner);
        }
    } else {
        report(command.path, "`reset` names no state or dialog here");
        known = false;
    }
    done(known);
}

void ScreenCore::runHostCommand(Command const& command, CommandCtx const& ctx, Done const& done) {
    Value const args = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (auto const& run = session.hostCommandRunner()) {
        HostCommandResult const result = run(command.target, args);
        if (result.kind == HostCommandResult::Kind::Missing) {
            report(command.path, "host command `" + command.target + "` is missing");
        } else if (result.kind == HostCommandResult::Kind::Refused) {
            report(command.path, result.reason);
        }
        done(result.kind == HostCommandResult::Kind::Ran);
        return;
    }
    bool const known = host != nullptr && host->hostCommand(command.target, args);
    if (!known) {
        report(command.path, "host command `" + command.target + "` is missing");
    }
    done(known);
}

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a download and an upload, each settled.
/// @brief A transfer's share of its screen's pending count.
///
/// A transfer is pending from the host accepting it to its settle: its completion carries on the command list that
/// started it, which the close protocol and save-and-close wait for. The count is taken once the host accepts, so a
/// refused transfer never counts, and a host that settles before it returns leaves nothing taken. It is given back
/// once, also when the transfer settles after its scope is gone, but not after the screen itself is gone, since the
/// count went with it.
class TransferHold {
public:
    /// @brief A hold that counts nothing yet.
    /// @param core  The screen whose count it moves.
    /// @param alive Expires with the screen.
    TransferHold(ScreenCore& core, std::weak_ptr<bool> alive) : _core{&core}, _alive{std::move(alive)} {}

    /// @brief Counts the transfer, unless it has already settled.
    void take() {
        if (_state == State::NotTaken) {
            _state = State::Taken;
            _core->addPending(1);
        }
    }

    /// @brief Gives the count back, if one was taken and the screen is still there; the transfer counts no more.
    void release() {
        if (_state == State::Taken && !_alive.expired()) {
            _core->addPending(-1);
        }
        _state = State::Released;
    }

private:
    enum class State : std::uint8_t { NotTaken, Taken, Released };

    ScreenCore* _core;
    std::weak_ptr<bool> _alive;
    State _state = State::NotTaken;
};

/// @brief Bytes as base64 (RFC 4648, with padding): how a document carries an inline file.
/// @param bytes The bytes.
/// @return The text.
[[nodiscard]] std::string base64Encode(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        std::size_t const left = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t group = 0;
        for (std::size_t offset = 0; offset < 3; ++offset) {
            group <<= 8U;
            if (offset < left) {
                group |= static_cast<unsigned char>(bytes.at(index + offset));
            }
        }
        for (std::size_t digit = 0; digit < 4; ++digit) {
            out.push_back(digit <= left ? kAlphabet.at((group >> (18U - (6U * digit))) & 0x3FU) : '=');
        }
    }
    return out;
}
}  // namespace

[[nodiscard]] std::optional<std::string> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    auto const digitOf = [](char chr) -> int {
        if (chr >= 'A' && chr <= 'Z') {
            return chr - 'A';
        }
        if (chr >= 'a' && chr <= 'z') {
            return chr - 'a' + 26;
        }
        if (chr >= '0' && chr <= '9') {
            return chr - '0' + 52;
        }
        if (chr == '+') {
            return 62;
        }
        return chr == '/' ? 63 : -1;
    };
    std::string out;
    out.reserve((text.size() * 3) / 4);
    std::uint32_t group = 0;
    int bits = 0;
    for (char const chr : text) {
        int const digit = digitOf(chr);
        if (digit < 0) {
            return std::nullopt;
        }
        group = (group << 6U) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((group >> static_cast<std::uint32_t>(bits)) & 0xFFU));
        }
    }
    return out;
}

[[nodiscard]] std::size_t base64Size(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    return (text.size() * 3) / 4;
}

[[nodiscard]] std::string sizeText(std::size_t bytes) {
    constexpr std::size_t kKiB = 1024;
    if (bytes < kKiB) {
        return std::to_string(bytes) + " bytes";
    }
    bool const mebi = bytes >= kKiB * kKiB;
    std::size_t const unit = mebi ? kKiB * kKiB : kKiB;
    // One decimal place, rounded half up, and none when it is zero.
    std::size_t const tenths = ((bytes * 10) + (unit / 2)) / unit;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        text += "." + std::to_string(tenths % 10);
    }
    return text + (mebi ? " MiB" : " KiB");
}

[[nodiscard]] std::expected<std::string, std::string> inlineBytes(Value const& value) {
    if (std::string const* const text = value.asString()) {
        if (std::optional<std::string> bytes = base64Decode(*text)) {
            return *std::move(bytes);
        }
        return std::unexpected(std::string{"the text is not base64"});
    }
    if (ValueList const* const list = value.asList()) {
        std::string bytes;
        bytes.reserve(list->size());
        for (Value const& item : *list) {
            std::int64_t const* const byte = item.asInt();
            if (byte == nullptr || *byte < 0 || *byte > 255) {
                return std::unexpected(std::string{"a byte list holds integers 0 to 255"});
            }
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(*byte)));
        }
        return bytes;
    }
    return std::unexpected(std::string{"inline bytes are base64 text or a list of bytes"});
}

void ScreenCore::runTransfer(Command const& command, CommandCtx const& ctx, Done done) {
    Value const file = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (command.kind == CommandKind::Download) {
        runDownload(command, ctx, file, std::move(done));
    } else {
        runUpload(command, ctx, file, std::move(done));
    }
}

void ScreenCore::runDownload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    Value const name = evalIn(*ctx.scope, ctx.bindings.get(), command.extra);
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    auto const settled = [this, weak = std::weak_ptr<RtScope>{ctx.scope}, target = command.target,
                          preview = command.option == "preview", done,
                          hold](std::expected<Value, std::string> const& reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            bool const succeeded = reply.has_value();
            NameRef const* const found = succeeded && preview ? held->find(target) : nullptr;
            if (found != nullptr && found->category == NameCategory::State) {
                initState(*found->state->scope, *found->state);
                session.noteWrite();
                found->state->value->set(*reply);
            }
            done(succeeded);
            hold->release();
        });
    };
    if (command.bytes) {
        std::optional<InlineFile> const inlined = inlineDownload(command, ctx, file, name);
        if (!inlined) {
            done(false);
            return;
        }
        if (host == nullptr || !host->saveFile(*inlined, command.option, settled)) {
            unsupported(command, "`download`");
            done(false);
            return;
        }
        hold->take();
        return;
    }
    if (host == nullptr || !host->download(file, command.option, name, settled)) {
        unsupported(command, "`download`");
        done(false);
        return;
    }
    hold->take();
}

// Inline bytes are checked before the host is asked: content over the cap, or that does not decode, never reaches it.
std::optional<InlineFile> ScreenCore::inlineDownload(
    Command const& command, CommandCtx const& ctx,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the command's `file` and `name`, in its order.
    Value const& file, Value const& name) {
    std::size_t const cap = session.options().maxInlineFileBytes;
    std::size_t size = 0;
    if (std::string const* const text = file.asString()) {
        size = base64Size(*text);
    } else if (ValueList const* const list = file.asList()) {
        size = list->size();
    }
    std::expected<std::string, std::string> bytes = size > cap ? std::unexpected(std::string{}) : inlineBytes(file);
    if (size > cap || (bytes && bytes->size() > cap)) {
        report(command.path, "download: " + sizeText(bytes ? bytes->size() : size) + " is over the " + sizeText(cap) +
                                 " inline cap; serve it through the file store");
        return std::nullopt;
    }
    if (!bytes) {
        report(command.path, "download: " + bytes.error());
        return std::nullopt;
    }
    return InlineFile{
        .name = textOf(name, ctx.scope->formatting()),
        .mediaType = command.mediaType
                         ? textOf(evalIn(*ctx.scope, ctx.bindings.get(), command.mediaType), ctx.scope->formatting())
                         : std::string{},
        .bytes = *std::move(bytes),
    };
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome an upload can have.
void ScreenCore::runUpload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    RtScope const& scope = *ctx.scope;
    std::weak_ptr<RtScope> const weak = ctx.scope;
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    NameRef const* const transferName = command.transfer.empty() ? nullptr : scope.find(command.transfer);
    TransferState* const transfer =
        transferName != nullptr && transferName->category == NameCategory::Transfer ? transferName->transfer : nullptr;
    auto const progress = [weak, transfer](Value fraction) {
        if (weak.lock() && transfer != nullptr) {
            transfer->progress.set(std::move(fraction));
        }
    };
    // Settles with the members the mutation's body gets beside its own (the reference or the inline file), or the
    // failure.
    auto const settled = [this, weak, kept = detached(ctx), transfer, command = &command, done,
                          hold](std::expected<ValueRecord, std::string> reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            if (transfer != nullptr) {
                transfer->pending.set(false);
                transfer->error.set(reply ? Value{} : Value{reply.error()});
            }
            if (!reply) {
                done(false);
                hold->release();
                return;
            }
            NameRef const* const found = held->find(command->target);
            // The mutation's body cannot name the reference (a body is checked where the mutation is declared),
            // so the reference is added to it under `as`, `file` by default.
            MutationRuntime& mutation = *found->mutation;
            Value const base = evalIn(*held, kept.bindings.get(), mutation.decl().body);
            ValueRecord fields = base.asRecord() == nullptr ? ValueRecord{} : *base.asRecord();
            for (auto& [member, value] : *reply) {
                fields.insert_or_assign(member, std::move(value));
            }
            // The run takes its own count before this one is given back, so pending never reads zero in between.
            mutation.run(Value{std::move(fields)}, std::make_shared<Bindings const>(*kept.bindings),
                         [done](bool succeeded, Value const&) { done(succeeded); });
            hold->release();
        });
    };
    if (host == nullptr) {
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    if (transfer != nullptr) {
        transfer->pending.set(true);
        transfer->progress.set(Value{std::int64_t{0}});
        transfer->error.set(Value{});
    }
    NameRef const* const target = scope.find(command.target);
    UploadRequest request{.file = file, .modelType = {}, .action = {}};
    if (target != nullptr && target->category == NameCategory::Mutation && target->mutation != nullptr) {
        request.modelType = target->mutation->typeId();
        request.action = target->mutation->decl().action;
    }
    std::string const member = command.binding.empty() ? std::string{"file"} : command.binding;
    bool started = false;
    if (command.bytes) {
        std::size_t const cap = session.options().maxInlineFileBytes;
        started = host->readFile(
            file, cap,
            [this, settled, progress, cap, member, command = &command](std::expected<InlineFile, std::string> read) {
                if (!read) {
                    settled(std::unexpected(read.error()));
                    return;
                }
                if (read->bytes.size() > cap) {
                    // The host should have refused it; the cap holds whether or not it did.
                    std::string why = "upload: " + sizeText(read->bytes.size()) + " is over the " + sizeText(cap) +
                                      " inline cap; send it through the file store";
                    report(command->path, why);
                    settled(std::unexpected(std::move(why)));
                    return;
                }
                progress(Value{std::int64_t{1}});
                ValueRecord added;
                added.insert_or_assign(member, Value{base64Encode(read->bytes)});
                if (!command->nameAs.empty()) {
                    added.insert_or_assign(command->nameAs, Value{read->name});
                }
                if (!command->typeAs.empty()) {
                    added.insert_or_assign(command->typeAs, Value{read->mediaType});
                }
                settled(std::move(added));
            });
    } else {
        started = host->uploadFor(request, progress, [settled, member](std::expected<Value, std::string> reference) {
            if (!reference) {
                settled(std::unexpected(reference.error()));
                return;
            }
            ValueRecord added;
            added.insert_or_assign(member, *std::move(reference));
            settled(std::move(added));
        });
    }
    if (!started) {
        if (transfer != nullptr) {
            transfer->pending.set(false);
            transfer->progress.set(Value{});
        }
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    hold->take();
}

}  // namespace detail

namespace detail {
namespace {

// --- Lowering --------------------------------------------------------------------------------------------------

/// @brief A value as a `bool` property: `true` only for `true`.
/// @param value  The value.
/// @param format Unused.
/// @return It.
[[nodiscard]] bool boolOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);
    bool const* const flag = value.asBool();
    return flag != nullptr && *flag;
}

/// @brief A value as a text property.
/// @param value  The value.
/// @param format The formatting facts.
/// @return Its text.
[[nodiscard]] std::string stringOf(Value const& value, FormatContext const& format) { return textOf(value, format); }
}  // namespace

[[nodiscard]] std::string componentText(Value const& value, std::string_view type, FormatContext const& format) {
    if (type == "json") {
        return canonicalJson(value);
    }
    if (type == "int" || type == "exact") {
        if (std::int64_t const* const whole = value.asInt()) {
            return std::to_string(*whole);
        }
        // A value that cannot be rounded at its own places is written as its exact JSON, never clamped.
        if (Decimal const* const number = value.asDecimal()) {
            return detail::decimalText(number->value, static_cast<std::uint32_t>(number->value.decimalPlaces.value))
                .value_or(canonicalJson(value));
        }
        if (Quantity const* const quantity = value.asQuantity()) {
            auto const text =
                detail::decimalText(quantity->value, static_cast<std::uint32_t>(quantity->value.decimalPlaces.value));
            return text ? *text + " " + quantity->unit : canonicalJson(value);
        }
    }
    return textOf(value, format);
}

namespace {

/// @brief An exact rational as the nearest double, unrounded.
///
/// `Rational::toDouble` rounds to the value's own places, which is a display reading: `29 / 100` keeps its operands'
/// 0 places and would read as 0. The view tree wants the magnitude, so the lowering reads it here.
/// @param value The rational.
/// @return Its numerator divided by its denominator.
[[nodiscard]] double realOf(math::Rational const& value) noexcept {
    return static_cast<double>(value.numerator) / static_cast<double>(value.denominator);
}
}  // namespace

[[nodiscard]] double componentNumber(Value const& value) {
    if (std::int64_t const* const whole = value.asInt()) {
        return static_cast<double>(*whole);
    }
    if (Decimal const* const number = value.asDecimal()) {
        return realOf(number->value);
    }
    if (Quantity const* const quantity = value.asQuantity()) {
        return realOf(quantity->value);
    }
    return 0.0;
}

[[nodiscard]] Value componentPayload(std::string const& payload, Type const* type) {
    std::string_view const text{payload};
    if (text.starts_with("i:")) {
        std::int64_t whole = 0;
        std::string_view const digits = text.substr(2);
        char const* const last = std::to_address(digits.end());
        auto const [end, error] = std::from_chars(std::to_address(digits.begin()), last, whole);
        if (error == std::errc{} && end == last) {
            return Value{whole};
        }
    }
    if (text.starts_with("s:")) {
        return Value{std::string{text.substr(2)}};
    }
    if (auto json = parseJson(text); json && type != nullptr) {
        if (auto value = valueFromJson(*json, *type)) {
            return std::move(*value);
        }
    }
    return Value{payload};
}

[[nodiscard]] std::int64_t intOf(Value const& value, std::int64_t fallback) {
    if (std::int64_t const* const number = value.asInt()) {
        return *number;
    }
    if (Decimal const* const number = value.asDecimal()) {
        // Integer division truncates toward zero, and reads the exact value rather than one rounded to its places.
        return number->value.numerator / number->value.denominator;
    }
    return fallback;
}

namespace {

/// @brief A text role by name.
/// @param name `normal`, `muted`, `heading` (or `title`), `section`, `error`, `success`, `mono`, `display`, `small`,
/// `overline`, `caption`, `label`, `headline`.
/// @return The role; `nullopt` for any other name.
[[nodiscard]] std::optional<TextRole> roleNamed(std::string_view name) {
    if (name == "normal") {
        return TextRole::Normal;
    }
    if (name == "muted") {
        return TextRole::Muted;
    }
    if (name == "heading" || name == "title") {
        return TextRole::Heading;
    }
    if (name == "section") {
        return TextRole::Section;
    }
    if (name == "error") {
        return TextRole::Error;
    }
    if (name == "success") {
        return TextRole::Success;
    }
    if (name == "mono") {
        return TextRole::Mono;
    }
    if (name == "display") {
        return TextRole::Display;
    }
    if (name == "small") {
        return TextRole::Small;
    }
    if (name == "overline") {
        return TextRole::Overline;
    }
    if (name == "caption") {
        return TextRole::Caption;
    }
    if (name == "label") {
        return TextRole::Label;
    }
    if (name == "headline") {
        return TextRole::Headline;
    }
    return std::nullopt;
}

/// @brief What a role that names none is told: the names `roleNamed` reads.
constexpr std::string_view kRoleNames =
    "a role is normal, muted, heading, section, error, success, mono, display, small, overline, caption, label or "
    "headline";

/// @brief A text role by name, as `roleNamed` reads it.
/// @param value  The name.
/// @param format Unused.
/// @return The role; `Normal` for any other name.
[[nodiscard]] TextRole roleOf(Value const& value, FormatContext const& format) {
    return roleNamed(textOf(value, format)).value_or(TextRole::Normal);
}

/// @brief A tone by name.
/// @param value  `neutral`, `info`, `ok` (or `success`), `warn` (or `warning`), `err` (or `error`).
/// @param format Unused.
/// @return The tone; `Neutral` for anything else.
[[nodiscard]] Tone toneOf(Value const& value, FormatContext const& format) {
    std::string const text = textOf(value, format);
    if (text == "info") {
        return Tone::Info;
    }
    if (text == "ok" || text == "success") {
        return Tone::Ok;
    }
    if (text == "warn" || text == "warning") {
        return Tone::Warn;
    }
    if (text == "err" || text == "error") {
        return Tone::Err;
    }
    return Tone::Neutral;
}
}  // namespace

[[nodiscard]] std::optional<Tone> toneNamed(Value const& value) {
    if (value.isNull()) {
        return Tone::Neutral;
    }
    static constexpr std::array<std::pair<std::string_view, Tone>, 5> kTones = {
        {
            {"neutral", Tone::Neutral},
            {"info", Tone::Info},
            {"ok", Tone::Ok},
            {"warn", Tone::Warn},
            {"err", Tone::Err},
        },
    };
    if (std::string const* const text = value.asString()) {
        // A loop, not std::ranges::find: an array's iterator is a pointer on some standard libraries and not on
        // others, so `auto` there reads differently to each.
        for (auto const& [name, tone] : kTones) {
            if (name == *text) {
                return tone;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<TextSpan> spansOf(Value const& value, FormatContext const& format, bool& dropped,
                                            bool* badTone) {
    std::vector<TextSpan> spans;
    if (value.isNull()) {
        return spans;
    }
    ValueList const* const runs = value.asList();
    if (runs == nullptr) {
        dropped = true;
        return spans;
    }
    auto const flag = [](Value const* field) {
        bool const* const set = field != nullptr ? field->asBool() : nullptr;
        return set != nullptr && *set;
    };
    for (Value const& run : *runs) {
        if (run.asRecord() == nullptr) {
            if (run.isNull() || run.asList() != nullptr) {
                dropped = true;
            } else {
                spans.push_back(TextSpan{.text = textOf(run, format)});
            }
            continue;
        }
        Value const* const text = run.field("text");
        if (text == nullptr) {
            dropped = true;
            continue;
        }
        Tone tone = Tone::Neutral;
        if (Value const* const named = run.field("tone")) {
            if (std::optional<Tone> const known = toneNamed(*named)) {
                tone = *known;
            } else if (badTone != nullptr) {
                *badTone = true;
            }
        }
        spans.push_back(TextSpan{
            .text = textOf(*text, format),
            .strong = flag(run.field("strong")),
            .mono = flag(run.field("mono")),
            .tone = tone,
            .action = flag(run.field("action")),
        });
    }
    return spans;
}

namespace {

/// @brief Whether a scope declares any name, so a node in it reads names the screen's own scope lacks.
/// @param scope The scope.
/// @return True when it declares a model, state, let, query, mutation, watch or check.
[[nodiscard]] bool declaresNames(ScopeDecl const& scope) noexcept {
    return !(scope.models.empty() && scope.state.empty() && scope.lets.empty() && scope.queries.empty() &&
             scope.mutations.empty() && scope.watches.empty() && scope.checks.empty());
}

/// @brief What a span list with a run it drops is told.
constexpr std::string_view kSpansReason =
    "spans is a list of runs, each a value or a record with `text`; a list, a null or a record without `text` is "
    "dropped";

/// @brief What a tone of no known name is told.
constexpr std::string_view kToneNames = "a tone is neutral, info, ok, warn or err, or null for none";

/// @brief What a span list with a run of a tone of no known name is told.
constexpr std::string_view kSpanToneReason =
    "a run's tone is neutral, info, ok, warn or err, or null for none; a run of another tone is drawn untoned";

/// @brief A splitter's `sizes`: a list of numbers, each read as `intOf` reads it.
/// @param value The value.
/// @return The sizes; `nullopt` for anything but a list whose every item is an `int` or a decimal.
[[nodiscard]] std::optional<std::vector<int>> splitterSizesOf(Value const& value) {
    ValueList const* const items = value.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    std::vector<int> sizes;
    sizes.reserve(items->size());
    for (Value const& item : *items) {
        if (item.asInt() == nullptr && item.asDecimal() == nullptr) {
            return std::nullopt;
        }
        sizes.push_back(static_cast<int>(intOf(item, 0)));
    }
    return sizes;
}
}  // namespace

[[nodiscard]] Sizing sizingOf(Value const& value) {
    if (Value const* const fixed = value.field("fixed")) {
        return Sizing::fixed(static_cast<int>(intOf(*fixed, 0)));
    }
    if (Value const* const percent = value.field("percent")) {
        return Sizing::percent(static_cast<int>(intOf(*percent, 1)));
    }
    if (Value const* const stretch = value.field("stretch")) {
        return Sizing::stretch(static_cast<int>(intOf(*stretch, 1)));
    }
    return Sizing::content();
}

[[nodiscard]] bool isSizing(Value const& value) {
    if (std::string const* const text = value.asString()) {
        return *text == "content";
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr || record->size() != 1 || record->begin()->second.asInt() == nullptr) {
        return false;
    }
    if (record->contains("percent")) {
        std::int64_t const share = *record->begin()->second.asInt();
        return share >= 1 && share <= 100;
    }
    return record->contains("fixed") || record->contains("stretch");
}

namespace {

/// @brief An alignment by name, as the loader checked it: `start`, `center`, `end`, or stretch for none.
/// @param name The name; empty for none.
/// @return The alignment.
[[nodiscard]] Align alignOf(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return Align::Stretch;
}

/// @brief A table cell's place by name: across its column (`align`) or down its row (`valign`).
/// @param name `start`, `center` or `end`.
/// @return The place; `nullopt` for any other name, `stretch` included.
[[nodiscard]] std::optional<Align> cellPlaceNamed(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return std::nullopt;
}

/// @brief A justification by name, as the loader checked it: `center`, `end`, `between`, or start for none.
/// @param name The name; empty for none.
/// @return The justification.
[[nodiscard]] Justify justifyOf(std::string_view name) {
    if (name == "center") {
        return Justify::Center;
    }
    if (name == "end") {
        return Justify::End;
    }
    if (name == "between") {
        return Justify::Between;
    }
    return Justify::Start;
}

/// @brief An axis by name: `horizontal`, or vertical for anything else.
/// @param value The value.
/// @return The axis.
[[nodiscard]] Axis axisOf(Value const& value) {
    std::string const* const text = value.asString();
    return text != nullptr && *text == "horizontal" ? Axis::Horizontal : Axis::Vertical;
}

/// @brief A step's state by name: `active`, `done`, `failed`, or pending for anything else.
/// @param name The name.
/// @return The state.
[[nodiscard]] StepState stepStateOf(std::string_view name) {
    if (name == "active") {
        return StepState::Active;
    }
    if (name == "done") {
        return StepState::Done;
    }
    return name == "failed" ? StepState::Failed : StepState::Pending;
}

/// @brief The options of a select, from a list of scalars or of records with a key and a `label`.
/// @param options The list.
/// @param format  The formatting facts.
/// @return The options.
[[nodiscard]] std::vector<SelectOption> selectOptions(Value const& options, FormatContext const& format) {
    std::vector<SelectOption> out;
    if (ValueList const* const items = options.asList()) {
        for (Value const& option : *items) {
            Value const key = optionKey(option);
            Value const* const label = option.field("label");
            out.push_back(SelectOption{.key = keyOf(key), .label = textOf(label != nullptr ? *label : key, format)});
        }
    }
    return out;
}

/// @brief What a percent sizes, as a refusal or a report says it.
constexpr std::string_view kPercentPlaces =
    "`percent` sizes a width along a row or a horizontal splitter; nothing here takes a share";
/// @brief Why a percent inside a case or an overlay's content is not a share.
constexpr std::string_view kPercentInHolder = "size the `switch` or the overlay itself; its content fills it";

/// @brief The default spacing step between the children of a document panel or collapsible, the mockups' 8 px.
constexpr int kPanelGap = 2;
/// @brief The default spacing step between the children of a document dialog or drawer, the mockups' 12 px.
constexpr int kDialogGap = 3;

/// @brief A node's size request.
/// @param node The node; may be null.
/// @return Its layout hints, or none for a null node or a kind that has no `Common`.
[[nodiscard]] LayoutHints layoutHintsOf(Node const& node) {
    if (!node) {
        return {};
    }
    return std::visit(
        [](auto const& spec) -> LayoutHints {
            if constexpr (requires { spec.common.layout; }) {
                return spec.common.layout;
            } else {
                return {};
            }
        },
        node->kind);
}

/// @brief A node with its width sizing replaced; a node of a kind with no layout is returned as it is.
/// @param node  The node.
/// @param width The sizing.
/// @return The node.
[[nodiscard]] Node withWidth(Node const& node, Sizing width) {
    if (!node) {
        return node;
    }
    auto copy = std::make_shared<NodeData>(*node);
    std::visit(
        [&width](auto& spec) {
            if constexpr (requires { spec.common.layout; }) {
                spec.common.layout.width = width;
            }
        },
        copy->kind);
    return copy;
}

/// @brief The size request of a holder of @p sizings: a stretch by the largest weight where any is a stretch, and
///        content otherwise (a share of a row is not a holder's: `listColumn` passes a lone child's on).
/// @param sizings One dimension's sizing of each child.
/// @return The holder's sizing.
[[nodiscard]] Sizing holderSizing(std::span<Sizing const> sizings) {
    int weight = 0;
    for (Sizing const& sizing : sizings) {
        if (sizing.kind == Sizing::Kind::Stretch) {
            weight = std::max(weight, sizing.amount);
        }
    }
    return weight > 0 ? Sizing::stretch(weight) : Sizing::content();
}

/// @brief The column that holds a node list. It stretches along an axis when one of its children does, so the
///        content means what its nodes would mean written inline in the holder's parent.
/// @param children The content.
/// @param gap      The spacing step between children.
/// @return The column; bounds and `alignSelf` stay unset, because they belong to a child.
[[nodiscard]] Column listColumn(std::vector<Node> children, int gap = 0) {
    std::vector<Sizing> widths;
    std::vector<Sizing> heights;
    for (Node const& child : children) {
        LayoutHints const hints = layoutHintsOf(child);
        widths.push_back(hints.width);
        heights.push_back(hints.height);
    }
    // A lone child's share of its row is the holder's, and the child fills the holder, so the share is taken once.
    if (children.size() == 1 && widths.front().kind == Sizing::Kind::Percent) {
        Sizing const share = widths.front();
        children.front() = withWidth(children.front(), Sizing::stretch(1));
        Column column{.children = std::move(children), .gap = gap};
        column.common.layout.width = share;
        column.common.layout.height = holderSizing(heights);
        return column;
    }
    Column column{.children = std::move(children), .gap = gap};
    column.common.layout.width = holderSizing(widths);
    column.common.layout.height = holderSizing(heights);
    return column;
}

/// @brief Content whose scope is made when it mounts and released when it unmounts: a one-row `forEach` whose
///        row view builds it.
/// @param build Builds the content, its scope included.
/// @param hints The size request of the content. A slot reads the sizing of its direct child alone, which is this
///        `forEach`, so the content's stretch reaches the slot only through these.
/// @return The node.
[[nodiscard]] Node lazy(std::function<Node()> build, LayoutHints const& hints = {}) {
    Common common;
    common.layout = hints;
    return ui::forEach<int>([] { return std::vector<int>{0}; }, [](int const&) { return Key{std::int64_t{0}}; },
                            [build = std::move(build)](reactive::Signal<int> const&) { return build(); },
                            Axis::Vertical, 0, common);
}

/// @brief A column of content that holds its scope for as long as it is mounted: its `visible` is a binding
///        that keeps the scope alive and reads nothing.
/// @param children The content.
/// @param scope    The scope.
/// @param keys     The chords of the content.
/// @param gap      The spacing step between the children.
/// @return The node.
[[nodiscard]] Node holding(std::vector<Node> children, std::shared_ptr<RtScope> scope,
                           std::vector<KeyBinding> keys = {}, int gap = 0) {
    Column column = listColumn(std::move(children), gap);
    column.common.visible = Prop<bool>{[held = std::move(scope)] { return held != nullptr; }};
    column.common.keys = std::move(keys);
    return ui::column(std::move(column));
}

/// @brief Whether an expression reads a name in @p names.
/// @param expr  The expression.
/// @param names The names.
/// @return Whether any form of the expression names one.
// NOLINTBEGIN(misc-no-recursion): an expression nests its arguments and fields, and the walk follows them.
[[nodiscard]] bool readsAny(ExprPtr const& expr, std::set<std::string, std::less<>> const& names) {
    if (!expr) {
        return false;
    }
    if (!expr->name.empty() && names.contains(expr->name)) {
        return true;
    }
    return std::ranges::any_of(expr->args, [&](ExprPtr const& arg) { return readsAny(arg, names); }) ||
           std::ranges::any_of(expr->fields, [&](auto const& field) { return readsAny(field.second, names); });
}
// NOLINTEND(misc-no-recursion)

/// @brief Collects the element ids of a document subtree.
/// @param node The root.
/// @param out  Receives the ids.
// NOLINTNEXTLINE(misc-no-recursion): a node's children and parts are nodes, and the walk follows them.
void collectIds(DocNode const& node, std::set<std::string, std::less<>>& out) {
    if (!node.id.empty()) {
        out.insert(node.id);
    }
    for (auto const* group : {&node.children, &node.parts}) {
        for (auto const& [key, list] : *group) {
            for (DocNodePtr const& child : list) {
                collectIds(*child, out);
            }
        }
    }
}
}  // namespace

// NOLINTNEXTLINE(misc-no-recursion): lowering a node lowers its children, which come back through here.
Node ScreenCore::lowerAll(std::span<DocNodePtr const> nodes, LowerCtx const& ctx, int gap) {
    LowerCtx listCtx = ctx;
    listCtx.listGap = gap;
    if (nodes.size() == 1) {
        return lower(*nodes.front(), listCtx);
    }
    std::vector<Node> children;
    children.reserve(nodes.size());
    for (DocNodePtr const& child : nodes) {
        children.push_back(lower(*child, listCtx));
    }
    // Several nodes share a holder, and a share of a row is not a share of a holder: each is reported and takes its
    // content size.
    for (auto&& [child, written] : std::views::zip(children, nodes)) {
        if (layoutHintsOf(child).width.kind == Sizing::Kind::Percent) {
            report(written->path,
                   "a percent among several nodes is a share of their holder; " + std::string{kPercentPlaces});
            child = withWidth(child, Sizing::content());
        }
    }
    Column holder = listColumn(std::move(children), gap);
    // The column is the mount's, not the document's: a child that bleeds reaches past the padding of the container
    // the document names, so the column passes that padding on instead of hiding it.
    holder.holder = true;
    return ui::column(std::move(holder));
}

void ScreenCore::fillHolder(std::vector<Node>& children, std::string const& path) {
    for (Node& child : children) {
        if (layoutHintsOf(child).width.kind == Sizing::Kind::Percent) {
            report(path, kPercentInHolder);
            child = withWidth(child, Sizing::stretch(1));
        }
    }
}

LayoutHints ScreenCore::nodeHints(DocNode const& node, std::set<std::string, std::less<>> const& own,
                                  LowerCtx const& ctx) {
    LayoutHints hints;
    if (ExprPtr const sizing = node.prop("sizing")) {
        if (sizing->kind != ExprKind::Literal && readsAny(sizing, own)) {
            report(sizing->path,
                   "this sizing reads a name the case declares, so the case is held at its "
                   "content size");
        } else {
            Value const value = once(ctx, sizing);
            Value const* const width = value.field("width");
            Value const* const height = value.field("height");
            if (width != nullptr || height != nullptr) {
                hints.width = width != nullptr && isSizing(*width) ? sizingOf(*width) : Sizing::content();
                hints.height = height != nullptr && isSizing(*height) ? sizingOf(*height) : Sizing::content();
            } else if (isSizing(value)) {
                hints.width = sizingOf(value);
            }
        }
    }
    return hints;
}

LayoutHints ScreenCore::holderHints(std::span<DocNode const* const> content, ScopeDecl const* decl,
                                    LowerCtx const& ctx) {
    std::set<std::string, std::less<>> own;
    for (DocNode const* const node : content) {
        collectIds(*node, own);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            own.insert(model.name);
        }
        for (StateDecl const& state : decl->state) {
            own.insert(state.name);
        }
        for (LetDecl const& let : decl->lets) {
            own.insert(let.name);
        }
        for (QueryDecl const& query : decl->queries) {
            own.insert(query.name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            own.insert(mutation.name);
        }
    }
    std::vector<Sizing> widths;
    std::vector<Sizing> heights;
    for (DocNode const* const node : content) {
        LayoutHints const hints = nodeHints(*node, own, ctx);
        widths.push_back(hints.width);
        heights.push_back(hints.height);
    }
    LayoutHints out;
    out.width = holderSizing(widths);
    out.height = holderSizing(heights);
    return out;
}

// NOLINTBEGIN(misc-no-recursion): a menu nests, and lowering descends the document.
Node ScreenCore::lower(DocNode const& node, LowerCtx const& ctx) {
    if (recording) {
        if (loweredNodes.size() >= kPruneLoweredAt && loweredNodes.size() % kPruneLoweredAt == 0) {
            std::erase_if(loweredNodes, [](LoweredNode const& entry) { return entry.scope.expired(); });
        }
        loweredNodes.push_back(LoweredNode{.node = &node, .scope = ctx.scope, .bindings = ctx.bindings});
    }
    // Only a switch reads the gap of the list around it; every other kind's children are held by a holder that sets
    // their gap, so none sees its grandparent's.
    if (ctx.listGap != 0 && node.kind != "switch") {
        LowerCtx plain = ctx;
        plain.listGap = 0;
        return lowerKind(node, plain, elementFor(*ctx.scope, node));
    }
    return lowerKind(node, ctx, elementFor(*ctx.scope, node));
}

Prop<TextRole> ScreenCore::roleProp(LowerCtx const& ctx, ExprPtr const& expr) {
    if (!expr || expr->kind == ExprKind::Literal) {
        return bindProp<TextRole>(ctx, expr, TextRole::Normal, roleOf);
    }
    return slotProp<TextRole>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        Value const value = evalIn(*scope, bindings.get(), expr);
        std::optional<TextRole> const role = roleNamed(textOf(value, scope->formatting()));
        if (!role) {
            report(expr->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
        }
        return role.value_or(TextRole::Normal);
    });
}

Prop<std::string> ScreenCore::surfaceOf(DocNode const& node, LowerCtx const& ctx) {
    ExprPtr const expr = node.prop("surface");
    auto nameOf = [this, path = pointerAppend(node.path, "surface")](Value const& value) -> std::string {
        if (std::string const* const name = value.asString()) {
            return *name;
        }
        if (!value.isNull()) {
            report(path, "a surface is the name of one; " + canonicalJson(value) + " is not a string");
        }
        return {};
    };
    if (!expr || expr->kind == ExprKind::Literal) {
        return expr ? Prop<std::string>{nameOf(expr->literal)} : Prop<std::string>{std::string{}};
    }
    return slotProp<std::string>([scope = ctx.scope, bindings = ctx.bindings, expr, nameOf = std::move(nameOf)] {
        return nameOf(evalIn(*scope, bindings.get(), expr));
    });
}

KeyValueItem ScreenCore::keyValueItemOf(Value const& item, std::string const& path, RtScope const& scope) {
    FormatContext const& format = scope.formatting();
    Value const* const label = item.field("label");
    Value const* const value = item.field("value");
    KeyValueItem out{
        .label = label == nullptr ? std::string{} : textOf(*label, format),
        .value = value == nullptr ? std::string{} : textOf(*value, format),
    };
    if (Value const* const spans = item.field("spans")) {
        bool dropped = false;
        bool badTone = false;
        out.spans = spansOf(*spans, format, dropped, &badTone);
        if (dropped) {
            report(path, kSpansReason);
        }
        if (badTone) {
            report(path, kSpanToneReason);
        }
    }
    if (Value const* const tone = item.field("tone")) {
        std::optional<Tone> const named = toneNamed(*tone);
        if (!named) {
            report(path, "an item's tone is " + canonicalJson(*tone) + "; " + std::string{kToneNames});
        }
        out.tone = named.value_or(Tone::Neutral);
    }
    return out;
}

// A group needs no widget of its own: it lowers to kinds every backend has, so the view tree and the contract do not
// change. Its description is a line read in order after its children, since `Common` carries no accessible
// description.
Node ScreenCore::lowerGroup(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                            std::vector<Node> children) {
    Text legend{
        .text = bindProp<std::string>(ctx, node.prop("label"), std::string{}, stringOf),
        .role = TextRole::Label,
    };
    legend.field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    std::vector<Node> column;
    column.reserve(children.size() + 3);
    column.push_back(ui::text(std::move(legend)));
    std::ranges::move(children, std::back_inserter(column));
    if (ExprPtr const description = node.prop("description")) {
        Text line{.text = bindProp<std::string>(ctx, description, std::string{}, stringOf), .role = TextRole::Muted};
        line.common.visible = slotProp<bool>([scope = ctx.scope, bindings = ctx.bindings, description] {
            return !textOf(evalIn(*scope, bindings.get(), description), scope->formatting()).empty();
        });
        column.push_back(ui::text(std::move(line)));
    }
    // The shown errors, one line each: a check that targets the group shows its message here.
    auto const shown = [this, element, scope = ctx.scope, bindings = ctx.bindings, errors = node.prop("errors")] {
        return stringsOf(element != nullptr ? elementValue(*element, "shownErrors")
                                            : evalIn(*scope, bindings.get(), errors));
    };
    Text messages{
        .text = slotProp<std::string>([shown] {
            std::string joined;
            for (std::string const& message : shown()) {
                joined += (joined.empty() ? "" : "\n") + message;
            }
            return joined;
        }),
        .role = TextRole::Error,
    };
    messages.common.visible = slotProp<bool>([shown] { return !shown().empty(); });
    column.push_back(ui::text(std::move(messages)));
    Column spec{
        .children = std::move(column),
        .gap = static_cast<int>(intOf(node.prop("gap") ? once(ctx, node.prop("gap")) : Value{}, 0)),
    };
    commonOf(node, ctx, element, spec.common);
    if (spec.common.a11y.role.empty()) {
        spec.common.a11y.role = "group";
    }
    bool const named = std::ranges::any_of(node.partList("a11y"),
                                           [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
    if (!named) {
        spec.common.a11y.name = bindProp<std::string>(ctx, node.prop("label"), std::string{}, stringOf);
    }
    return ui::column(std::move(spec));
}

Prop<std::vector<TextSpan>> ScreenCore::spansProp(LowerCtx const& ctx, ExprPtr const& expr) {
    return slotProp<std::vector<TextSpan>>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        bool dropped = false;
        bool badTone = false;
        std::vector<TextSpan> spans =
            spansOf(evalIn(*scope, bindings.get(), expr), scope->formatting(), dropped, &badTone);
        if (dropped) {
            report(expr->path, kSpansReason);
        }
        if (badTone) {
            report(expr->path, kSpanToneReason);
        }
        return spans;
    });
}

// The size request: `sizing` (the width, or both dimensions), `alignSelf`, and the bounds.
void ScreenCore::layoutOf(DocNode const& node, LowerCtx const& ctx, LayoutHints& layout) {
    if (ExprPtr const sizing = node.prop("sizing")) {
        // The width, or both dimensions from a `{width, height}` record, one left out being content. The loader
        // refuses a literal of another form; a computed one that is none is reported and lowers to content.
        Value const value = once(ctx, sizing);
        Value const* const width = value.field("width");
        Value const* const height = value.field("height");
        bool const record = width != nullptr || height != nullptr;
        bool const valid = record ? (width == nullptr || isSizing(*width)) && (height == nullptr || isSizing(*height))
                                  : isSizing(value);
        if (!valid) {
            report(sizing->path, "sizing is " + canonicalJson(value) +
                                     "; a sizing is \"content\", {\"fixed\": n}, {\"stretch\": w}, {\"percent\": n} "
                                     "with n from 1 to 100, or a record of width and height of them");
        } else if (record) {
            layout.width = width != nullptr ? sizingOf(*width) : Sizing::content();
            layout.height = height != nullptr ? sizingOf(*height) : Sizing::content();
            if (layout.height.kind == Sizing::Kind::Percent) {
                report(sizing->path, kPercentPlaces);
                layout.height = Sizing::content();
            }
        } else {
            layout.width = sizingOf(value);
        }
    }
    if (std::string_view const placed = node.name("alignSelf"); !placed.empty()) {
        layout.alignSelf = alignOf(placed);
    }
    layout.bleed = node.flag("bleed");
    // Literal integers, checked at load.
    for (auto const& [key, bound] : {
             std::pair{"minWidth", &LayoutHints::minWidth},
             std::pair{"maxWidth", &LayoutHints::maxWidth},
             std::pair{"minHeight", &LayoutHints::minHeight},
             std::pair{"maxHeight", &LayoutHints::maxHeight},
         }) {
        if (ExprPtr const expr = node.prop(key); expr != nullptr && expr->literal.asInt() != nullptr) {
            layout.*bound = static_cast<int>(*expr->literal.asInt());
        }
    }
}

namespace {

/// @brief Where a declaration is lowered, as a focus key reads it.
struct FocusPlace {
    /// @brief The focus key of the component use it is lowered for, and `>`; empty outside a component.
    std::string prefix;
    /// @brief The keys of the rows that enclose it, outermost first.
    std::vector<std::string> rows;
};

/// @brief Where @p scope is: the row keys of its chain and the component use it is lowered for.
/// @param scope The scope.
/// @return It.
[[nodiscard]] FocusPlace focusPlace(RtScope const& scope) {
    FocusPlace place;
    for (RtScope const* current = &scope; current != nullptr; current = current->parent.get()) {
        if (current->row != nullptr) {
            place.rows.push_back(current->rowKey);
        }
        if (current->parent == nullptr) {
            place.prefix = current->usePrefix;
        }
    }
    std::ranges::reverse(place.rows);
    return place;
}

/// @brief A focus key: a declaration's pointer under its component use's prefix, with one key per enclosing row.
/// @param place Where the declaration is lowered.
/// @param path  The declaring node's JSON Pointer.
/// @return The key, unique per declaration, row and component use.
[[nodiscard]] std::string focusKey(FocusPlace const& place, std::string_view path) {
    std::string key = place.prefix;
    key += path;
    for (std::string const& row : place.rows) {
        key += '@';
        key += row;
    }
    return key;
}

/// @brief The focus key of the element @p path declares, lowered in @p scope.
/// @param path  The declaring node's JSON Pointer.
/// @param scope The scope it is lowered in.
/// @return The key.
[[nodiscard]] std::string focusKeyOf(std::string_view path, RtScope const& scope) {
    return focusKey(focusPlace(scope), path);
}
}  // namespace

void ScreenCore::commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common) {
    static_cast<void>(element);
    common.visible = bindProp<bool>(ctx, node.prop("visible"), true, boolOf);
    common.enabled = bindProp<bool>(ctx, node.prop("enabled"), true, boolOf);
    common.tooltip = bindProp<std::string>(ctx, node.prop("tooltip"), std::string{}, stringOf);
    layoutOf(node, ctx, common.layout);
    common.testId = std::string{node.name("testId")};
    if (!node.id.empty()) {
        common.focusId = focusKeyOf(node.path, *ctx.scope);
    }
    common.surface = surfaceOf(node, ctx);
    if (ExprPtr const autofocus = node.prop("autofocus")) {
        common.autofocus = boolOf(once(ctx, autofocus), ctx.scope->formatting());
    }
    for (DocNodePtr const& a11y : node.partList("a11y")) {
        common.a11y.name = bindProp<std::string>(ctx, a11y->prop("name"), std::string{}, stringOf);
        common.a11y.role = std::string{a11y->name("role")};
    }
    for (DocNodePtr const& drag : node.partList("drag")) {
        ExprPtr const payload = drag->prop("payload");
        common.dragKey =
            slotProp<std::optional<Key>>([scope = ctx.scope, bindings = ctx.bindings, payload] -> std::optional<Key> {
                Value const value = evalIn(*scope, bindings.get(), payload);
                return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
            });
    }
    for (DocNodePtr const& drop : node.partList("drop")) {
        ExprPtr const accepts = drop->prop("accepts");
        common.accepts = [this, scope = ctx.scope, bindings = ctx.bindings, accepts](Key const& key) {
            if (!accepts) {
                return true;
            }
            Value const accepted = runtime().untracked([&] { return evalIn(*scope, bindings.get(), accepts); });
            if (ValueList const* const items = accepted.asList()) {
                return std::ranges::any_of(*items, [&](Value const& item) { return keyOf(item) == key; });
            }
            return accepted.isNull() || keyOf(accepted) == key;
        };
        common.onDrop = [this, ctx, commands = drop->event("onDrop")](Key const& key) {
            runEvent(commands, ctx, valueOfKey(key));
        };
    }
    for (auto const& [chord, commands] : node.keys) {
        common.keys.push_back(
            KeyBinding{.chord = chord, .onPress = [this, ctx, list = &commands] { runEvent(list, ctx, Value{}); }});
    }
}

void ScreenCore::fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field) {
    field.readonly = bindProp<bool>(ctx, node.prop("readonly"), false, boolOf);
    field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    field.stale = bindProp<bool>(ctx, node.prop("stale"), false, boolOf);
    if (element != nullptr) {
        field.errors = slotProp<std::vector<std::string>>(
            [this, scope = ctx.scope, element] { return stringsOf(elementValue(*element, "shownErrors")); });
    } else if (ExprPtr const errors = node.prop("errors")) {
        field.errors = slotProp<std::vector<std::string>>([scope = ctx.scope, bindings = ctx.bindings, errors] {
            return stringsOf(evalIn(*scope, bindings.get(), errors));
        });
    }
    // A tone evaluates the value and is not an error: it changes the field's look and nothing of `valid`.
    if (node.prop("description")) {
        field.description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf);
    }
    if (node.prop("labelHint")) {
        field.labelHint = bindProp<std::string>(ctx, node.prop("labelHint"), std::string{}, stringOf);
    }
    if (ExprPtr const tone = node.prop("tone")) {
        field.tone = slotProp<Tone>([this, scope = ctx.scope, bindings = ctx.bindings, tone] {
            Value const value = evalIn(*scope, bindings.get(), tone);
            std::optional<Tone> const named = toneNamed(value);
            if (!named) {
                report(tone->path, "tone is " + canonicalJson(value) + "; " + std::string{kToneNames});
            }
            return named.value_or(Tone::Neutral);
        });
    }
}

MenuItem ScreenCore::menuItemOf(DocNode const& part, LowerCtx const& ctx) {
    MenuItem item;
    if (part.flag("separator")) {
        item.separator = true;
        return item;
    }
    item.label = bindProp<std::string>(ctx, part.prop("label"), std::string{}, stringOf);
    item.description = bindProp<std::string>(ctx, part.prop("description"), std::string{}, stringOf);
    item.visible = bindProp<bool>(ctx, part.prop("visible"), true, boolOf);
    if (Commands const* const commands = part.event("onClick")) {
        item.onSelect = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
    }
    if (ExprPtr const icon = part.prop("icon")) {
        item.icon = textOf(once(ctx, icon), ctx.scope->formatting());
    }
    item.keys = std::string{part.name("keys")};
    if (ExprPtr const checked = part.prop("checked")) {
        item.checked = slotProp<std::optional<bool>>(
            [scope = ctx.scope, bindings = ctx.bindings, checked] -> std::optional<bool> {
                Value const value = evalIn(*scope, bindings.get(), checked);
                bool const* const flag = value.asBool();
                return flag == nullptr ? std::nullopt : std::optional<bool>{*flag};
            });
    }
    item.enabled = bindProp<bool>(ctx, part.prop("enabled"), true, boolOf);
    return item;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<MenuItem> ScreenCore::menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<MenuItem> items;
    for (DocNodePtr const& part : parts) {
        MenuItem item = menuItemOf(*part, ctx);
        if (!item.separator) {
            item.items = menuItems(part->partList("items"), ctx);
        }
        items.push_back(std::move(item));
    }
    return items;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<HeadMenuModel> ScreenCore::lowerHeadMenu(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<HeadMenuModel> models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        models.push_back(HeadMenuModel{
            .id = part->id,
            .path = part->path,
            .item = menuItemOf(*part, ctx),
            .items = lowerHeadMenu(part->partList("items"), ctx),
        });
    }
    return models;
}

std::vector<HeadToolModel> ScreenCore::lowerHeadTools(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    static constexpr int kSearchWidth = 30;
    std::vector<HeadToolModel> models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        bool const search = part->name("kind") == "search";
        HeadToolModel model{
            .id = part->id,
            .path = part->path,
            .kind = search ? HeadToolKind::Search : HeadToolKind::Select,
            .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
            .value = bindProp<std::string>(ctx, part->prop("value"), std::string{}, stringOf),
            .placeholder = bindProp<std::string>(ctx, part->prop("placeholder"), std::string{}, stringOf),
            .options = {},
            .selected = {},
            .width = search ? kSearchWidth : 0,
            .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
            .edit = {},
            .submit = {},
        };
        if (ExprPtr const width = part->prop("width")) {
            if (std::int64_t const* const units = width->literal.asInt()) {
                model.width = static_cast<int>(*units);
            }
        }
        if (!search) {
            model.options = slotProp<std::vector<SelectOption>>(
                [scope = ctx.scope, bindings = ctx.bindings, options = part->prop("options")] {
                    return selectOptions(evalIn(*scope, bindings.get(), options), scope->formatting());
                });
            model.selected = slotProp<std::optional<Key>>(
                [scope = ctx.scope, bindings = ctx.bindings, selected = part->prop("selected")] -> std::optional<Key> {
                    Value const value = selected ? evalIn(*scope, bindings.get(), selected) : Value{};
                    return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
                });
        }
        Commands const* const edited = part->event(search ? "onChange" : "onChoose");
        model.edit = [this, ctx, edited](Value event) { runEvent(edited, ctx, std::move(event)); };
        if (search) {
            model.submit = [this, ctx, onSubmit = part->event("onSubmit")](Value event) {
                runEvent(onSubmit, ctx, std::move(event));
            };
        }
        models.push_back(std::move(model));
    }
    return models;
}

ActionModels ScreenCore::lowerActionParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    ActionModels models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        ActionModel model{
            .id = part->id,
            .path = part->path,
            .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
            .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
            .iconOnly = bindProp<bool>(ctx, part->prop("iconOnly"), false, boolOf),
            .emphasis = ButtonEmphasis::Normal,
            .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
            .run = {},
        };
        model.emphasis = emphasisNamed(part->name("emphasis")).value_or(ButtonEmphasis::Normal);
        if (Commands const* const commands = part->event("onClick")) {
            model.run = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
        }
        models.push_back(std::move(model));
    }
    return models;
}

Node ScreenCore::actionBarOf(ActionModels actions) {
    ActionBar bar;
    for (ActionModel& action : actions) {
        Button button{
            .label = action.label,
            .emphasis = action.emphasis,
            .onClick = std::move(action.run),
            .common = {},  // NOLINT(readability-trailing-comma): the comma ends the designator, not the empty braces.
            .icon = action.icon.evaluate(),
        };
        if (action.iconOnly.isBound() || action.iconOnly.constant()) {
            // The label is the button's name and tooltip; the caption is empty while the icon stands alone.
            button.label = Prop<std::string>{[label = action.label, iconOnly = action.iconOnly] {
                return iconOnly.evaluate() ? std::string{} : label.evaluate();
            }};
            button.common.a11y.name = action.label;
            button.common.tooltip = action.label;
        }
        button.common.visible = std::move(action.visible);
        button.common.enabled = std::move(action.enabled);
        button.common.testId = "action_" + action.id;
        bar.children.push_back(ui::button(std::move(button)));
    }
    return ui::actionBar(std::move(bar));
}

std::vector<BadgeModel> ScreenCore::lowerBadgeParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<BadgeModel> badges;
    badges.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        badges.push_back(BadgeModel{
            .text = bindProp<std::string>(ctx, part->prop("text"), std::string{}, stringOf),
            .tone = bindProp<Tone>(ctx, part->prop("tone"), Tone::Neutral, toneOf),
            .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
        });
    }
    return badges;
}

// The readers every kind's lowering shares, each bound to the node being lowered.
class ScreenCore::KindKit {
public:
    KindKit(ScreenCore& core, DocNode const& node, LowerCtx const& ctx, ElementState* element)
        : _core{core}, _node{node}, _ctx{ctx}, _element{element}, _format{ctx.scope->formatting()} {}

    // The children under @p key, each lowered with @p gap as the gap of the list around it.
    [[nodiscard]] std::vector<Node> children(std::string_view key, int gap = 0) const {
        LowerCtx listCtx = _ctx;
        listCtx.listGap = gap;
        std::vector<Node> out;
        for (DocNodePtr const& child : _node.nodes(key)) {
            out.push_back(_core.lower(*child, listCtx));
        }
        return out;
    }

    // The children under @p key as one node: none is empty, several share a holder that sets @p gap.
    [[nodiscard]] Node child(std::string_view key, int gap = 0) const {
        auto const nodes = _node.nodes(key);
        return nodes.empty() ? Node{} : _core.lowerAll(nodes, _ctx, gap);
    }

    [[nodiscard]] std::int64_t onceInt(std::string_view key, std::int64_t fallback) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? intOf(_core.once(_ctx, expr), fallback) : fallback;
    }

    [[nodiscard]] bool onceBool(std::string_view key, bool fallback) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? boolOf(_core.once(_ctx, expr), _format) : fallback;
    }

    // A stack's padding: one step, or the loader-checked constant list of the vertical and the horizontal step. A
    // computed padding that yields a list is not a step: it is reported once and taken as 0.
    [[nodiscard]] Padding stackPadding() const {
        ExprPtr const expr = _node.prop("padding");
        if (!expr) {
            return {};
        }
        Value const value = _core.once(_ctx, expr);
        if (ValueList const* const steps = value.asList()) {
            if (expr->kind == ExprKind::Literal && steps->size() == 2) {
                return Padding{static_cast<int>(intOf(steps->at(0), 0)), static_cast<int>(intOf(steps->at(1), 0))};
            }
            _core.report(_node.path + "/padding", "a computed `padding` is one spacing step, not a list; taken as 0");
            return {};
        }
        return Padding{static_cast<int>(intOf(value, 0))};
    }

    [[nodiscard]] std::string onceText(std::string_view key) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? textOf(_core.once(_ctx, expr), _format) : std::string{};
    }

    [[nodiscard]] Prop<std::string> textProp(std::string_view key) const {
        return _core.bindProp<std::string>(_ctx, _node.prop(key), std::string{}, stringOf);
    }

    // A field shows no caption of its own, so its label is what names it to assistive technology. An `a11y` part
    // that gives a name was written for that purpose and wins; one that gives only a role leaves the label.
    void nameByLabel(Common& common) const {
        bool const named = std::ranges::any_of(_node.partList("a11y"),
                                               [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
        if (_node.prop("label") && !named) {
            common.a11y.name = textProp("label");
        }
    }

    // A standalone input draws its label as a caption only when the document asks for it (`showLabel`).
    template <typename Spec>
    void captionOf(Spec& spec) const {
        if (_node.prop("label")) {
            spec.label = textProp("label");
        }
        spec.showLabel = _node.flag("showLabel");
    }

    [[nodiscard]] Action action(std::string_view key) const {
        Commands const* const commands = _node.event(key);
        if (commands == nullptr) {
            return {};
        }
        return [core = &_core, ctx = _ctx, commands] { core->runEvent(commands, ctx, Value{}); };
    }

    [[nodiscard]] ActionButton actionPart() const {
        ActionButton button;
        for (DocNodePtr const& part : _node.partList("action")) {
            button.label = _core.bindProp<std::string>(_ctx, part->prop("label"), std::string{}, stringOf);
            if (Commands const* const commands = part->event("onClick")) {
                button.onClick = [core = &_core, ctx = _ctx, commands] { core->runEvent(commands, ctx, Value{}); };
            }
        }
        return button;
    }

    // A reader of the element's value under @p name.
    [[nodiscard]] auto read(std::string_view name) const {
        return [element = _element, core = &_core, name = std::string{name}] {
            return core->elementValue(*element, name);
        };
    }

private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): the kit lives inside one lowering call, in
    // which all four outlive it.
    ScreenCore& _core;
    DocNode const& _node;
    LowerCtx const& _ctx;
    ElementState* _element;
    FormatContext const& _format;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
std::optional<Node> ScreenCore::lowerWidget(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    FormatContext const& format = ctx.scope->formatting();
    KindKit const kit{*this, node, ctx, element};
    if (kind == "text") {
        Text spec{.role = roleProp(ctx, node.prop("role"))};
        // A text with spans shows them, so its `text`, an older client's fallback, is not lowered.
        if (ExprPtr const spans = node.prop("spans")) {
            spec.spans = spansProp(ctx, spans);
        } else {
            spec.text = kit.textProp("text");
        }
        if (!node.name("align").empty()) {
            spec.align = textAlignNamed(node.name("align"));
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::text(std::move(spec));
    }
    if (kind == "button") {
        Button spec{.label = kit.textProp("label")};
        spec.icon = kit.onceText("icon");
        spec.emphasis = emphasisNamed(node.name("emphasis")).value_or(ButtonEmphasis::Normal);
        if (node.name("style") == "item") {
            spec.style = ButtonStyle::Item;
        }
        spec.shape = shapeNamed(node.name("shape")).value_or(ControlShape::Default);
        spec.size = sizeNamed(node.name("size")).value_or(ButtonSize::Normal);
        spec.mono = node.flag("mono");
        spec.weight = weightNamed(node.name("weight")).value_or(0);
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
            // An entry's content, drawn in place of its label: one node as it is, several in a column.
            if (std::vector<Node> content = kit.children("children"); content.size() == 1) {
                spec.child = std::move(content.front());
            } else if (!content.empty()) {
                spec.child = ui::column(Column{.children = std::move(content)});
            }
        }
        Commands const* const onClick = node.event("onClick");
        Commands const* const onActivate = node.event("onActivate");
        if (onClick != nullptr || onActivate != nullptr) {
            spec.onClick = [this, ctx, onClick, onActivate] {
                runEvent(onClick, ctx, Value{});
                runEvent(onActivate, ctx, Value{});
            };
        }
        commonOf(node, ctx, element, spec.common);
        return ui::button(std::move(spec));
    }
    if (kind == "textInput") {
        std::string const commitOn = node.name("commitOn").empty() ? "change" : std::string{node.name("commitOn")};
        auto const commit = [this, ctx, element, commitOn, onChange = node.event("onChange")](
                                std::string const& when, std::string const& text) {
            if (commitOn != when) {
                return;
            }
            if (element->bound == nullptr) {
                session.noteWrite();
                element->committed.set(Value{text});
            }
            runEvent(onChange, ctx, Value{text});
        };
        TextInput spec;
        spec.value =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onChange = [this, element, commit](std::string text) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{text});
                element->edited.set(true);
            });
            commit("change", text);
        };
        spec.onCommit = [this, ctx, element, commit, onCommit = node.event("onCommit")](std::string const& text) {
            element->touched.set(true);
            commit("blur", text);
            runEvent(onCommit, ctx, Value{text});
        };
        // Only a field that does something on Enter submits: one without `onSubmit` that does not commit on Enter
        // lets Enter go on to its ancestors' chords, a dialog's `default` among them.
        if (Commands const* const onSubmit = node.event("onSubmit"); onSubmit != nullptr || commitOn == "enter") {
            spec.onSubmit = [this, ctx, commit, onSubmit](std::string const& text) {
                commit("enter", text);
                runEvent(onSubmit, ctx, Value{text});
            };
        }
        spec.placeholder = kit.textProp("placeholder");
        std::string const mode = kit.onceText("mode");
        spec.mode = TextInputMode::SingleLine;
        if (mode == "multiline") {
            spec.mode = TextInputMode::Multiline;
        } else if (mode == "password") {
            spec.mode = TextInputMode::Password;
        }
        // A multiline input shows no unit and no icon; a computed mode that turns out multiline is reported and
        // its unit and icon left out, so the mount never meets them there. The loader refuses a literal one.
        for (std::string_view const key : {
                 std::string_view{"unit"},
                 std::string_view{"icon"},
                 std::string_view{"prefix"},
                 std::string_view{"decimals"},
             }) {
            if (ExprPtr const given = node.prop(key); given != nullptr && spec.mode == TextInputMode::Multiline) {
                report(given->path, "a multiline input shows no " + std::string{key});
            }
        }
        spec.mono = node.flag("mono");
        if (spec.mode != TextInputMode::Multiline) {
            spec.emphasis = node.name("emphasis") == "prominent" ? InputEmphasis::Prominent : InputEmphasis::Normal;
            spec.icon = kit.onceText("icon");
            spec.align = alignOf(node.name("align"));
            if (spec.align == Align::Stretch) {
                spec.align = Align::Start;
            }
            if (node.prop("unit")) {
                spec.unit = kit.textProp("unit");
            }
            if (node.prop("prefix")) {
                spec.prefix = kit.textProp("prefix");
            }
            if (ExprPtr const places = node.prop("decimals")) {
                spec.decimals = bindProp<int>(ctx, places, -1, placesOf);
            }
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::textInput(std::move(spec));
    }
    if (kind == "checkbox") {
        Checkbox spec{
            .label = kit.textProp("label"),
            .checked = slotProp<bool>([read = kit.read("checked")] {
                Value const value = read();
                bool const* const flag = value.asBool();
                return flag != nullptr && *flag;
            }),
        };
        spec.onToggle = [this, ctx, element, onToggle = node.event("onToggle"),
                         onChange = node.event("onChange")](bool checked) {
            // A toggle is a check box's commit, so it touches the box.
            session.asUser([&] {
                if (element->bound == nullptr) {
                    session.noteWrite();
                    element->draft.set(checked);
                }
                element->touched.set(true);
            });
            runEvent(onToggle, ctx, Value{checked});
            runEvent(onChange, ctx, Value{checked});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::checkbox(std::move(spec));
    }
    if (kind == "select") {
        Select spec;
        spec.options = slotProp<std::vector<SelectOption>>(
            [scope = ctx.scope, element] { return selectOptions(element->rows->get(), scope->formatting()); });
        spec.selected = slotProp<std::optional<Key>>([read = kit.read("selected")] -> std::optional<Key> {
            Value const selected = read();
            return selected.isNull() ? std::nullopt : std::optional<Key>{keyOf(selected)};
        });
        spec.onSelect = [this, ctx, element, onChoose = node.event("onChoose"),
                         onChange = node.event("onChange")](Key const& key) {
            Value const chosen = valueOfKey(key);
            // A choice is a select's commit, so it touches the select.
            session.asUser([&] {
                if (element->bound == nullptr) {
                    session.noteWrite();
                    element->draft.set(chosen);
                }
                element->touched.set(true);
            });
            runEvent(onChoose, ctx, chosen);
            runEvent(onChange, ctx, chosen);
        };
        // A computed style of no known name is reported once and drawn as a dropdown; the loader refuses a constant
        // one.
        if (ExprPtr const style = node.prop("style")) {
            std::string const name = textOf(once(ctx, style), ctx.scope->formatting());
            if (name == "radio") {
                spec.style = SelectStyle::Radio;
            } else if (name == "segmented") {
                spec.style = SelectStyle::Segmented;
            } else if (name != "dropdown") {
                report(style->path, "style is `" + name + "`; a select's style is dropdown, radio or segmented");
            }
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::select(std::move(spec));
    }
    if (kind == "slider") {
        Slider spec;
        spec.value = slotProp<std::int64_t>([read = kit.read("value"), element] {
            return intOf(element->bound != nullptr ? read() : element->draft.get(), 0);
        });
        spec.minimum = kit.onceInt("min", 0);
        spec.maximum = kit.onceInt("max", 100);
        spec.step = kit.onceInt("step", 1);
        spec.onChange = [this, ctx, element, onChange = node.event("onChange")](std::int64_t value) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(value);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(value);
                }
            });
            runEvent(onChange, ctx, Value{value});
        };
        spec.onCommit = [this, ctx, element, onCommit = node.event("onCommit")](std::int64_t value) {
            element->touched.set(true);
            runEvent(onCommit, ctx, Value{value});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        return ui::slider(std::move(spec));
    }
    if (kind == "dateTimeInput") {
        DateTimeInput spec;
        DateMode const mode = kit.onceText("mode") == "date" ? DateMode::Date : DateMode::DateTime;
        std::optional<TypeKind> heldAtLoad;
        if (auto const found = _doc->dateValueKinds.find(&node); found != _doc->dateValueKinds.end()) {
            heldAtLoad = found->second;
        }
        std::string const valuePath = pointerAppend(node.path, "value");
        std::int32_t const offsetMinutes = format.utcOffsetMinutes;
        spec.value = slotProp<std::optional<time::Timestamp>>([this, element, offsetMinutes,
                                                               valuePath] -> std::optional<time::Timestamp> {
            DateReading const reading = dateInstantOf(element->draft.get(), offsetMinutes);
            if (!reading.readable) {
                report(valuePath, "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time string");
            }
            return reading.instant ? std::optional<time::Timestamp>{time::Timestamp{*reading.instant}} : std::nullopt;
        });
        spec.onChange = [this, ctx, element, heldAtLoad, mode, offsetMinutes, onChange = node.event("onChange"),
                         onCommit = node.event("onCommit")](std::optional<time::Timestamp> value) {
            bool asString = element->bound != nullptr && element->bound->get().asString() != nullptr;
            if (heldAtLoad) {
                asString = *heldAtLoad == TypeKind::String;
            }
            std::optional<time::DateTime> const instant = value ? value->value : std::nullopt;
            Value entered;
            if (!asString) {
                if (instant) {
                    entered = Value{*instant};
                }
            } else if (!instant) {
                entered = Value{std::string{}};
            } else if (mode == DateMode::Date) {
                entered = Value{dateTextOf(*instant, offsetMinutes)};
            } else {
                entered = Value{instant->toIso8601()};
            }
            element->touched.set(true);
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(entered);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(entered);
                }
            });
            runEvent(onChange, ctx, entered);
            runEvent(onCommit, ctx, entered);
        };
        spec.mode = mode;
        spec.offsetMinutes = offsetMinutes;
        // Tracked: a locale switch changes the pattern the screen's dates are written in, and the input follows.
        spec.pattern = slotProp<std::string>([this, mode] {
            FormatContext const& now = session.formatting();
            return mode == DateMode::Date ? now.datePattern : now.dateTimePattern;
        });
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::dateTimeInput(std::move(spec));
    }
    if (kind == "filePicker") {
        FilePicker spec;
        spec.path =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onPicked = [this, ctx, element, onPick = node.event("onPick")](std::string path) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{path});
                element->committed.set(Value{path});
            });
            runEvent(onPick, ctx, Value{path});
        };
        spec.mode = kit.onceText("mode") == "save" ? FilePickerMode::Save : FilePickerMode::Open;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        return ui::filePicker(std::move(spec));
    }
    if (kind == "menu") {
        Menu spec{.items = menuItems(node.partList("items"), ctx)};
        if (node.name("style") == "button") {
            spec.style = MenuStyle::Button;
            spec.label = kit.textProp("label");
            spec.icon = kit.onceText("icon");
            if (node.name("emphasis") == "primary") {
                spec.emphasis = ButtonEmphasis::Primary;
            } else if (node.name("emphasis") == "danger") {
                spec.emphasis = ButtonEmphasis::Danger;
            } else if (node.name("emphasis") == "ghost") {
                spec.emphasis = ButtonEmphasis::Ghost;
            }
        }
        commonOf(node, ctx, element, spec.common);
        return ui::menu(std::move(spec));
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
std::optional<Node> ScreenCore::lowerDisplay(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    if (kind == "banner") {
        Banner spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Info, toneOf)};
        if (ExprPtr const spans = node.prop("spans")) {
            spec.spans = spansProp(ctx, spans);
        } else {
            spec.text = kit.textProp("text");
        }
        spec.action = kit.actionPart();
        spec.dismissible = kit.onceBool("dismissible", false);
        spec.onDismiss = kit.action("onDismiss");
        spec.actionStyle = bannerActionStyleNamed(node.name("actionStyle")).value_or(BannerActionStyle::Ghost);
        spec.actionStretch = node.flag("actionStretch");
        commonOf(node, ctx, element, spec.common);
        return ui::banner(std::move(spec));
    }
    if (kind == "badge") {
        Badge spec{
            .tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Neutral, toneOf),
            .text = kit.textProp("text"),
        };
        spec.icon = kit.onceText("icon");
        std::string_view const style = node.name("style");
        spec.style = BadgeStyle::Filled;
        if (style == "outline") {
            spec.style = BadgeStyle::Outline;
        } else if (style == "quiet") {
            spec.style = BadgeStyle::Quiet;
        }
        spec.shape = shapeNamed(node.name("shape")).value_or(ControlShape::Default);
        spec.mono = node.flag("mono");
        if (Commands const* const onRemove = node.event("onRemove")) {
            spec.onRemove = [this, ctx, onRemove] { runEvent(onRemove, ctx, Value{}); };
            spec.removeLabel = kit.textProp("removeLabel");
        }
        commonOf(node, ctx, element, spec.common);
        return ui::badge(std::move(spec));
    }
    if (kind == "progress") {
        Progress spec;
        spec.value = slotProp<std::optional<double>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("value")] -> std::optional<double> {
                Value const value = evalIn(*scope, bindings.get(), expr);
                if (std::int64_t const* const number = value.asInt()) {
                    return static_cast<double>(*number);
                }
                if (Decimal const* const number = value.asDecimal()) {
                    return realOf(number->value);
                }
                return std::nullopt;
            });
        spec.label = kit.textProp("label");
        commonOf(node, ctx, element, spec.common);
        return ui::progress(std::move(spec));
    }
    if (kind == "steps") {
        Steps spec;
        spec.items =
            slotProp<std::vector<Step>>([scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<Step> steps;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        Value const* const label = item.field("label");
                        Value const* const state = item.field("state");
                        Value const* const detail = item.field("detail");
                        std::string const name = state == nullptr ? std::string{} : textOf(*state, {});
                        steps.push_back(Step{
                            .label = label == nullptr ? std::string{} : textOf(*label, scope->formatting()),
                            .state = stepStateOf(name),
                            .detail = detail == nullptr ? std::string{} : textOf(*detail, scope->formatting()),
                        });
                    }
                }
                return steps;
            });
        spec.current = slotProp<std::optional<std::size_t>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("current")] -> std::optional<std::size_t> {
                std::int64_t const index = intOf(evalIn(*scope, bindings.get(), expr), -1);
                return index < 0 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(index)};
            });
        spec.labelled = bindProp<bool>(ctx, node.prop("labelled"), true, boolOf);
        commonOf(node, ctx, element, spec.common);
        return ui::steps(std::move(spec));
    }
    if (kind == "chart") {
        return lowerChart(node, ctx, element);
    }
    if (kind == "keyValue") {
        KeyValue spec;
        spec.captions = node.name("fieldLayout") == "stacked" ? CaptionPlacement::Above : CaptionPlacement::Beside;
        // The loader has checked `captionWidth` is a literal integer.
        if (ExprPtr const width = node.prop("captionWidth")) {
            spec.captionWidth = static_cast<int>(*width->literal.asInt());
        }
        spec.items = slotProp<std::vector<KeyValueItem>>(
            [this, scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<KeyValueItem> out;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        out.push_back(keyValueItemOf(item, expr->path, *scope));
                    }
                }
                return out;
            });
        commonOf(node, ctx, element, spec.common);
        return ui::keyValue(std::move(spec));
    }
    if (kind == "emptyState") {
        EmptyState spec{.title = kit.textProp("title"), .text = kit.textProp("text")};
        spec.icon = kit.onceText("icon");
        spec.action = kit.actionPart();
        commonOf(node, ctx, element, spec.common);
        return ui::emptyState(std::move(spec));
    }
    return std::nullopt;
}

Node ScreenCore::lowerStack(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    int const gap = static_cast<int>(kit.onceInt("gap", 0));
    if (std::string const arrangement = arrangedBy(node.id); !arrangement.empty()) {
        return lowerArranged(node, ctx, element, arrangement, kind == "column" ? Axis::Vertical : Axis::Horizontal,
                             gap);
    }
    if (kind == "column") {
        Column spec{
            .children = kit.children("children", gap),
            .gap = gap,
            .align = alignOf(node.name("align")),
            .justify = justifyOf(node.name("justify")),
            .divided = node.flag("divided"),
            .padding = kit.stackPadding(),
        };
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
        }
        commonOf(node, ctx, element, spec.common);
        return ui::column(std::move(spec));
    }
    Row spec{
        .children = kit.children("children"),
        .gap = gap,
        .align = alignOf(node.name("align")),
        .justify = justifyOf(node.name("justify")),
        .wrap = node.flag("wrap"),
        .divided = node.flag("divided"),
        .padding = kit.stackPadding(),
    };
    if (node.prop("selected")) {
        spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
    }
    commonOf(node, ctx, element, spec.common);
    return ui::row(std::move(spec));
}

Node ScreenCore::lowerGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Grid spec{
        .columns = static_cast<int>(kit.onceInt("columns", 1)),
        .gap = static_cast<int>(kit.onceInt("gap", 0)),
        .align = alignOf(node.name("align")),
        .minColumnWidth = static_cast<int>(kit.onceInt("minColumnWidth", 0)),
    };
    for (DocNodePtr const& track : node.partList("tracks")) {
        ExprPtr const width = track->prop("width");
        spec.tracks.push_back(GridTrack{
            .width = width ? sizingOf(once(ctx, width)) : Sizing::stretch(),
            .align = cellPlaceNamed(track->name("align")).value_or(Align::Start),
        });
    }
    for (DocNodePtr const& cell : node.nodes("children")) {
        ExprPtr const span = cell->prop("span");
        spec.cells.push_back(
            GridCell{.node = lower(*cell, ctx), .span = static_cast<int>(span ? intOf(once(ctx, span), 1) : 1)});
    }
    commonOf(node, ctx, element, spec.common);
    return ui::grid(std::move(spec));
}

Node ScreenCore::lowerPanel(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Panel spec{.title = kit.textProp("title"), .padding = static_cast<int>(kit.onceInt("padding", 0))};
    spec.header = kit.child("header");
    spec.child = kit.child("children", static_cast<int>(kit.onceInt("gap", kPanelGap)));
    spec.collapsible = kit.onceBool("collapsible", false);
    spec.collapsed = bindProp<bool>(ctx, node.prop("collapsed"), false, boolOf);
    if (Commands const* const commands = node.event("onToggle")) {
        spec.onToggle = [this, ctx, commands](bool collapsed) { runEvent(commands, ctx, Value{collapsed}); };
    }
    commonOf(node, ctx, element, spec.common);
    return ui::panel(std::move(spec));
}

Node ScreenCore::lowerSplitter(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    ExprPtr const orientation = node.prop("orientation");
    Splitter spec{
        .children = kit.children("children"),
        .orientation =
            orientation && axisOf(once(ctx, orientation)) == Axis::Vertical ? Axis::Vertical : Axis::Horizontal,
    };
    // No `onResize`: a user's drag stays the splitter's own, and the document's sizes are where it starts.
    if (ExprPtr const sizes = node.prop("sizes")) {
        spec.sizes = bindProp<std::vector<int>>(
            ctx, sizes, {}, [this, path = sizes->path](Value const& value, FormatContext const& /*format*/) {
                std::optional<std::vector<int>> parsed = splitterSizesOf(value);
                if (!parsed) {
                    report(path, "sizes is " + canonicalJson(value) + "; sizes is a list of numbers, one per pane");
                }
                return std::move(parsed).value_or(std::vector<int>{});
            });
    }
    commonOf(node, ctx, element, spec.common);
    // A literal `sizes` that matches the panes places them, so a pane's own width is not used.
    if (ExprPtr const sizes = node.prop("sizes"); sizes && sizes->kind == ExprKind::Literal) {
        std::optional<std::vector<int>> const given = splitterSizesOf(sizes->literal);
        if (given && given->size() == spec.children.size()) {
            for (std::size_t at = 0; at < spec.children.size(); ++at) {
                // A stretch pane asks only to fill, which `sizes` leaves it to do; a fixed or percent pane names a
                // size.
                Sizing::Kind const kind = layoutHintsOf(spec.children.at(at)).width.kind;
                if (spec.orientation == Axis::Horizontal &&
                    (kind == Sizing::Kind::Fixed || kind == Sizing::Kind::Percent)) {
                    report(sizes->path, "this splitter's `sizes` places its panes; pane " + std::to_string(at) +
                                            "'s sizing is not used");
                }
            }
        }
    }
    return ui::splitter(std::move(spec));
}

void ScreenCore::reportActionBarGaps(DocNode const& bar, LowerCtx const& ctx) {
    for (DocNodePtr const& child : bar.nodes("children")) {
        if (child->kind != "forEach" || child->prop("gap") == nullptr) {
            continue;
        }
        ExprPtr const axis = child->prop("axis");
        if (axis != nullptr && axis->kind != ExprKind::Literal && axisOf(once(ctx, axis)) == Axis::Horizontal) {
            report(child->path + "/gap", kActionBarGapRefusal);
        }
    }
}

Node ScreenCore::lowerCollapsible(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Collapsible spec{.title = kit.textProp("title"), .open = bindProp<bool>(ctx, node.prop("open"), true, boolOf)};
    if (Commands const* const commands = node.event("onToggle")) {
        spec.onToggle = [this, ctx, commands](bool open) { runEvent(commands, ctx, Value{open}); };
    }
    spec.header = kit.child("header");
    spec.child = kit.child("children", static_cast<int>(kit.onceInt("gap", kPanelGap)));
    commonOf(node, ctx, element, spec.common);
    return ui::collapsible(std::move(spec));
}

Node ScreenCore::lowerDropZone(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    DropZone spec;
    spec.accept = stringsOf(node.prop("accept") ? once(ctx, node.prop("accept")) : Value{});
    spec.multiple = kit.onceBool("multiple", false);
    if (Commands const* const commands = node.event("onDrop")) {
        spec.onDrop = [this, ctx, commands](std::vector<std::string> const& paths) {
            runEvent(commands, ctx, listOfStrings(paths));
        };
    }
    spec.child = kit.child("children");
    commonOf(node, ctx, element, spec.common);
    return ui::dropZone(std::move(spec));
}

Node ScreenCore::lowerPage(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Page spec;
    spec.geometry = pageGeometryOf(node);
    std::optional<PageGeometry> const outer = std::exchange(pageBeingLowered, spec.geometry);
    spec.children = kit.children("children");
    pageBeingLowered = outer;
    commonOf(node, ctx, element, spec.common);
    return ui::page(std::move(spec));
}

Node ScreenCore::lowerPageRegion(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    if (!pageBeingLowered) {
        report(node.path, "a page region belongs in a page");
        return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
    }
    KindKit const kit{*this, node, ctx, element};
    PageRegion spec;
    spec.geometry = regionGeometryOf(node, *pageBeingLowered);
    std::optional<PageGeometry> const outer = std::exchange(pageBeingLowered, std::nullopt);
    spec.children = kit.children("children");
    pageBeingLowered = outer;
    commonOf(node, ctx, element, spec.common);
    return ui::pageRegion(std::move(spec));
}

Node ScreenCore::lowerLocaleScope(DocNode const& node, LowerCtx const& ctx) {
    auto const holder = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
    holder->transparent = true;
    ExprPtr const locale = node.prop("locale");
    holder->localeFormat = &holder->nodes.computed([this, scope = ctx.scope, bindings = ctx.bindings,
                                                    locale] -> std::shared_ptr<FormatContext const> {
        Value const value = evalIn(*scope, bindings.get(), locale);
        std::string const* const tag = value.asString();
        if (tag == nullptr && !value.isNull()) {
            report(locale->path, "the locale is " + canonicalJson(value) + "; a locale is a BCP 47 tag, a string");
        }
        if (tag == nullptr || tag->empty()) {
            return session.sharedFormatting();
        }
        if (session.firstUnlistedLocale(*tag)) {
            report(locale->path, "no formatting facts for locale `" + *tag + "`; numbers, money and dates use en's");
        }
        return session.scopedFormatting(*tag);
    });
    // The children are mounted again whenever the tag changes: a value a node reads once, as it is lowered (a
    // table's column labels, a `once` property, a literal), is written in the facts at that moment, and only a new
    // lowering writes it in the new ones. The key is the tag as the scope resolves it; a tag that is not known yet
    // is the key `""`, apart from every tag.
    std::vector<DocNode const*> content;
    for (DocNodePtr const& child : node.nodes("children")) {
        content.push_back(child.get());
    }
    LayoutHints const hints = holderHints(content, nullptr, ctx);
    Switch spec;
    spec.selector = slotProp<Key>([scope = ctx.scope, bindings = ctx.bindings, locale] {
        Value const value = evalIn(*scope, bindings.get(), locale);
        std::string const* const tag = value.asString();
        return Key{tag == nullptr ? std::string{} : *tag};
    });
    spec.fallback = lazy(
        [this, ctx, holder, content, path = node.path] -> Node {
            auto const inner = std::make_shared<RtScope>(*this, holder, nullptr, holder->nodes.depth() + 2);
            inner->transparent = true;
            LowerCtx const innerCtx{.scope = inner, .bindings = ctx.bindings, .boundary = ctx.boundary, .listGap = 0};
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, innerCtx));
            }
            fillHolder(children, path);
            return holding(std::move(children), inner, {}, 0);
        },
        hints);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
Node ScreenCore::lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    if (std::optional<Node> widget = lowerWidget(node, ctx, element)) {
        return std::move(*widget);
    }
    if (kind == "group") {
        return lowerGroup(node, ctx, element, kit.children("children"));
    }
    if (kind == "column" || kind == "row") {
        return lowerStack(node, ctx, element);
    }
    if (kind == "grid") {
        return lowerGrid(node, ctx, element);
    }
    if (kind == "spacer") {
        Spacer spec;
        commonOf(node, ctx, element, spec.common);
        return ui::spacer(std::move(spec));
    }
    if (kind == "panel") {
        return lowerPanel(node, ctx, element);
    }
    if (kind == "scroll") {
        ExprPtr const axis = node.prop("axis");
        Scroll spec{.child = kit.child("children"), .axis = axis ? axisOf(once(ctx, axis)) : Axis::Vertical};
        commonOf(node, ctx, element, spec.common);
        return ui::scroll(std::move(spec));
    }
    if (kind == "switch") {
        return lowerSwitch(node, ctx);
    }
    if (kind == "tabs") {
        return lowerTabs(node, ctx, element);
    }
    if (kind == "dialog" || kind == "drawer") {
        return lowerDialog(node, ctx, element);
    }
    if (kind == "busy") {
        Busy spec{.active = bindProp<bool>(ctx, node.prop("active"), false, boolOf), .label = kit.textProp("label")};
        commonOf(node, ctx, element, spec.common);
        return ui::busy(std::move(spec));
    }
    if (kind == "forEach" || kind == "table") {
        return lowerRows(node, ctx, element);
    }
    if (std::optional<Node> display = lowerDisplay(node, ctx, element)) {
        return std::move(*display);
    }
    if (kind == "splitter") {
        return lowerSplitter(node, ctx, element);
    }
    if (kind == "collapsible") {
        return lowerCollapsible(node, ctx, element);
    }
    if (kind == "dropZone") {
        return lowerDropZone(node, ctx, element);
    }
    if (kind == "actionBar") {
        KindKit const bar{*this, node, ctx, element};
        ActionBar spec{
            .children = bar.children("children"),
            .align = alignOf(node.name("align")),
            .justify = justifyOf(node.name("justify")),
            .wrap = node.flag("wrap"),
        };
        reportActionBarGaps(node, ctx);
        commonOf(node, ctx, element, spec.common);
        return ui::actionBar(std::move(spec));
    }
    if (kind == "page") {
        return lowerPage(node, ctx, element);
    }
    if (kind == "pageRegion") {
        return lowerPageRegion(node, ctx, element);
    }
    if (kind == "localeScope") {
        return lowerLocaleScope(node, ctx);
    }
    if (kind == "boundary") {
        return lowerBoundary(node, ctx);
    }
    if (kind == "custom") {
        return lowerCustom(node, ctx);
    }
    if (kind == "customize") {
        return lowerCustomize(node, ctx);
    }
    if (kind == "form") {
        return lowerForm(node, ctx, element);
    }
    if (kind == "collection" || kind == "wizard") {
        return lowerComposite(node, ctx, element);
    }
    report(node.path, "`" + node.kind + "` is a node kind this interpreter does not lower");
    return ui::text(Text{.text = std::string{node.kind}, .role = TextRole::Muted});
}

namespace {

/// @brief A document field overlay's expressions, evaluated once, as the engine's overlay.
/// @param scope The scope the form is declared in.
/// @param part  The `fields` entry.
/// @param format How values read as text and numbers.
/// @return The overlay.
[[nodiscard]] forms::FieldOverlay overlayOf(RtScope const& scope, DocNode const& part, FormatContext const& format) {
    auto const read = [&](std::string_view key) -> std::optional<Value> {
        ExprPtr const expr = part.prop(key);
        if (!expr) {
            return std::nullopt;
        }
        return scope.core.runtime().untracked([&] { return evalIn(scope, nullptr, expr); });
    };
    auto const text = [&](std::string_view key) -> std::optional<std::string> {
        auto const value = read(key);
        return value.has_value() && !value->isNull() ? std::optional<std::string>{textOf(*value, format)}
                                                     : std::nullopt;
    };
    auto const flag = [&](std::string_view key) {
        auto const value = read(key);
        bool const* const set = value.has_value() ? value->asBool() : nullptr;
        return set != nullptr && *set;
    };
    forms::FieldOverlay overlay{.omit = flag("omit"), .hidden = flag("hidden")};
    overlay.label = text("label");
    overlay.unit = text("unit");
    overlay.widget = text("widget");
    if (auto const decimals = read("decimals");
        decimals.has_value() && decimals->asInt() != nullptr && *decimals->asInt() >= 0) {
        overlay.decimals = static_cast<std::uint32_t>(std::min<std::int64_t>(*decimals->asInt(), 18));
    }
    if (auto const blankAs = text("blankAs")) {
        overlay.blankAs = *blankAs == "empty" ? forms::BlankAs::Empty : forms::BlankAs::Omit;
    }
    return overlay;
}

/// @brief A lowering context whose scope is held weakly: what an Effect the scope itself owns may keep, so the scope
///        does not keep itself alive.
struct DetachedLower {
    /// @brief The scope, weakly.
    std::weak_ptr<RtScope> scope;
    /// @brief The names bound there.
    std::shared_ptr<Bindings const> bindings;
    /// @brief The innermost boundary.
    std::shared_ptr<BoundaryRuntime> boundary;
};

/// @brief Detaches a lowering context's scope.
/// @param ctx The context.
/// @return The context, its scope held weakly.
[[nodiscard]] DetachedLower detachedLower(LowerCtx const& ctx) {
    return DetachedLower{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
}

/// @brief Puts a detached context's scope back.
/// @param held The detached context.
/// @return The context; its scope is null once the scope is gone.
[[nodiscard]] LowerCtx attached(DetachedLower const& held) {
    return LowerCtx{.scope = held.scope.lock(), .bindings = held.bindings, .boundary = held.boundary};
}
}  // namespace

// Builds the form's session when its element is declared, so its values can be read before the form is drawn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of form element and its sessions.
void ScreenCore::wireForm(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    std::optional<std::string> const schema = catalog.schema ? catalog.schema(action) : std::nullopt;
    if (!schema.has_value()) {
        report(node.path, "no schema for `" + action + "`: the catalog has none for a `form` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::FieldOverlays overlays;
    // `fields` is one record part holding a `field` part per overlaid member.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            forms::FieldOverlay overlay = overlayOf(scope, *part, session.formatting());
            // A computed `decimals` is carried by the effect below alone: baking the value read now would make it
            // the fallback when the expression later gives no places, where the schema's places belong.
            if (ExprPtr const places = part->prop("decimals"); places && places->kind != ExprKind::Literal) {
                overlay.decimals.reset();
            }
            overlays.insert_or_assign(part->id, std::move(overlay));
        }
    }
    auto model = forms::FormModel::fromSchema(action, *schema, overlays);
    if (!model) {
        report(node.path,
               "the form of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (node.name("submit") == "explicit") {
        model = model->withSubmitMode(forms::SubmitMode::Explicit);
    } else if (node.name("submit") == "auto") {
        model = model->withSubmitMode(forms::SubmitMode::Automatic);
    }
    element.form = std::make_unique<forms::FormSession>(runtime(), *std::move(model), aliasSubmits(alias->model),
                                                        aliasCalls(alias->model), engineOptions());
    followFormatting(element, scope);
    // An overlay's `decimals` follows its expression: the field keeps the draft as typed and checks it again against
    // the places the expression gives now.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            ExprPtr const places = part->prop("decimals");
            if (!places) {
                continue;
            }
            forms::FieldState* state = nullptr;
            try {
                state = &element.form->field(part->id);
            } catch (std::out_of_range const&) {
                continue;  // The overlay names a field the form does not have (an omitted one).
            }
            scope.nodes.effect([this, &scope, places, state] {
                int const now = placesOf(evalIn(scope, nullptr, places), session.formatting());
                runtime().untracked([&] {
                    state->places().set(now >= 0 ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(now)}
                                                 : std::nullopt);
                });
            });
        }
    }
    if (ExprPtr const prefill = node.prop("prefill")) {
        // A prefill follows its expression. A value of the record being edited (the first value, or one under the
        // same `key`) keeps the fields the user changed; another record, a new `key`, or any later value of a form
        // without one, loads every field.
        forms::FormSession* const form = element.form.get();
        struct Followed {
            bool loaded = false;
            Value key;
        };
        auto const followed = std::make_shared<Followed>();
        scope.nodes.effect([this, &scope, prefill, key = node.prop("key"), form, followed] {
            Value const value = evalIn(scope, nullptr, prefill);
            if (value.isNull()) {
                return;
            }
            runtime().untracked([&] {
                Value const current = key ? evalIn(scope, nullptr, key) : Value{};
                bool const same = !followed->loaded || (key && current == followed->key);
                followed->loaded = true;
                followed->key = current;
                std::string const json = glz::write_json(valueToJson(value)).value_or(std::string{});
                if (same) {
                    form->refill(json);
                } else {
                    form->prefill(json);
                }
            });
        });
    }
}

Node ScreenCore::lowerForm(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    forms::FormSession* const form = element == nullptr ? nullptr : element->form.get();
    if (form == nullptr) {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    // The form's events, each run once per change of the count it watches.
    auto const watch = [this, &ctx](Commands const* commands, auto count, auto payload) {
        if (commands == nullptr || commands->empty()) {
            return;
        }
        auto const seen = std::make_shared<std::uint64_t>(runtime().untracked(count));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), commands, count, payload, seen] {
            std::uint64_t const now = count();
            if (now == *seen) {
                return;
            }
            *seen = now;
            runtime().untracked([&] { runEvent(commands, attached(held), payload()); });
        });
    };
    watch(
        node.event("onSubmit"), [form] { return form->submitAttempts(); },
        [form] {
            std::optional<std::string> const body = form->body();
            return body.has_value() ? jsonTextValue(*body) : Value{};
        });
    watch(
        node.event("onSuccess"), [form] { return form->successCount(); },
        [form] {
            std::optional<std::string> const& reply = form->lastReply();
            return reply.has_value() ? jsonTextValue(*reply) : Value{};
        });
    if (Commands const* const onError = node.event("onError"); onError != nullptr && !onError->empty()) {
        // Each failure is a new exception: run once per failure, with its text.
        auto const seen =
            std::make_shared<std::exception_ptr>(runtime().untracked([form] { return form->lastError(); }));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), onError, form, seen] {
            std::exception_ptr const error = form->lastError();
            if (error == nullptr || error == *seen) {
                return;
            }
            *seen = error;
            runtime().untracked([&] { runEvent(onError, attached(held), Value{failureText(error)}); });
        });
    }
    forms::FormViewOptions options{.showTitle = false};
    presentForm(node, ctx, options);
    options.title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    // A field whose overlay names a component is drawn by it; only top-level fields take one, as every overlay.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& overlay : record->partList("fields")) {
            if (!overlay->name("component").empty()) {
                options.overrides.byField(overlay->id,
                                          [this, ctx, field = overlay.get()](forms::FieldView const& view) {
                                              return fieldComponent(view, *field, ctx);
                                          });
            }
        }
    }
    Column column{.children = {forms::formView(*form, std::move(options))}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

[[nodiscard]] std::expected<forms::detail::engine::Json, std::string> changeWire(std::string const& payload,
                                                                                 std::string_view type) {
    std::string_view text{payload};
    if (type == "string") {
        if (text.starts_with("s:")) {
            text.remove_prefix(2);
        }
        // Parentheses: braces would pick the initializer-list constructor and make a one-element array.
        return forms::detail::engine::Json(std::string{text});
    }
    if (text.starts_with("i:")) {
        text.remove_prefix(2);
    }
    std::optional<forms::detail::engine::Json> json = forms::detail::engine::parseJson(text);
    if (!json) {
        return std::unexpected(std::string{"it is not JSON"});
    }
    bool const fits = type == "json" || json->is_null() || (type == "bool" && json->is_boolean()) ||
                      ((type == "int" || type == "number") && json->is_number()) ||
                      (type == "exact" && (json->is_number() || json->is_string()));
    if (!fits) {
        bool const vowel = type == "int" || type == "exact";
        return std::unexpected("it is not " + std::string{vowel ? "an " : "a "} + std::string{type});
    }
    return *std::move(json);
}

namespace {

/// @brief What a form fills of a field component's props: the field's value, errors, read-only and required state,
///        and label, each a tracked read of the field.
/// @param prop  The prop's name.
/// @param state The field.
/// @return The read, or empty for a prop the form does not fill.
[[nodiscard]] std::function<Value()> formFilled(std::string const& prop, forms::FieldState const* state) {
    if (prop == "value") {
        return [state] {
            forms::Encoding const& encoding = state->encoded().get();
            if (!encoding.has_value() || !encoding->has_value()) {
                return Value{};
            }
            return (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
        };
    }
    if (prop == "errors") {
        return [state] {
            std::optional<forms::FieldError> const& error = state->error().get();
            return error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}};
        };
    }
    if (prop == "readonly") {
        return [state] { return Value{state->readonly().get()}; };
    }
    if (prop == "required") {
        return [state] { return Value{state->required().get()}; };
    }
    if (prop == "label") {
        return [state] { return Value{state->label()}; };
    }
    return {};
}
}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a prop's name, then its declared type.
CustomProp ScreenCore::componentProp(std::string const& prop, std::string const& type, std::function<Value()> read) {
    if (type == "bool") {
        return CustomProp{
            .name = prop,
            .value = slotProp<bool>([this, read = std::move(read)] { return boolOf(read(), session.formatting()); }),
        };
    }
    if (type == "number") {
        return CustomProp{
            .name = prop,
            .value = slotProp<double>([read = std::move(read)] { return componentNumber(read()); }),
        };
    }
    return CustomProp{
        .name = prop,
        .value = slotProp<std::string>(
            [this, read = std::move(read), type] { return componentText(read(), type, session.formatting()); }),
    };
}

void ScreenCore::changeField(forms::FieldState& state, forms::FieldSpec const& spec, std::string const& path,
                             std::string_view valueType, std::string const& payload) {
    auto wire = changeWire(payload, valueType);
    // A base64 member holds a file's bytes, which a document moves inline only under the session's cap.
    if (std::size_t const cap = session.options().maxInlineFileBytes;
        wire && spec.bytes && wire->is_string() && base64Size(wire->get_string()) > cap) {
        report(path, "field " + spec.name + ": " + std::to_string(base64Size(wire->get_string())) +
                         " bytes is over the " + std::to_string(cap) + " inline cap");
        return;
    }
    if (wire) {
        if (auto edited = state.edit(*wire); !edited) {
            wire = std::unexpected(edited.error());
        }
    }
    if (!wire) {
        report(path, "field " + spec.name + ": the component's change does not decode: " + wire.error());
    }
}

Node ScreenCore::fieldComponent(forms::FieldView const& view, DocNode const& overlay, LowerCtx const& ctx) {
    std::string const name{overlay.name("component")};
    // A loaded document's field component is declared, with a `value` prop and a `change` event.
    Component const& component = _doc->components.at(name);
    forms::FieldState* const state = &view.state();
    Custom spec{
        .name = name,
        .fallback = view.defaultControl(),
        .props = {},
        .minVersion =
            static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
        .onEvent = {},
    };
    for (auto const& [prop, type] : component.props) {
        if (prop == "value" && type == "json") {
            // The wire JSON itself, so an object's or a collection's every digit reaches the component.
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = slotProp<std::string>([state] {
                    forms::Encoding const& encoding = state->encoded().get();
                    return encoding.has_value() && encoding->has_value() ? (*encoding)->json : std::string{"null"};
                }),
            });
            continue;
        }
        std::function<Value()> read = formFilled(prop, state);
        if (!read) {
            ExprPtr given;
            for (DocNodePtr const& record : overlay.partList("props")) {
                if (ExprPtr const expr = record->prop(prop)) {
                    given = expr;
                }
            }
            read = [scope = ctx.scope, bindings = ctx.bindings, given] {
                return given ? evalIn(*scope, bindings.get(), given) : Value{};
            };
        }
        spec.props.push_back(componentProp(prop, type, std::move(read)));
    }
    spec.onEvent = [this, state, path = overlay.path, fieldSpec = &view.spec(),
                    valueType = component.props.at("value")](std::string const& event, std::string const& payload) {
        if (event == "change") {
            changeField(*state, *fieldSpec, path, valueType, payload);
        }
    };
    return ui::custom(std::move(spec));
}

// Every call goes through the alias's channel, bound for its current instance when the call is made.
forms::Submitter ScreenCore::aliasCalls(ModelAlias* alias) {
    return [this, alias](std::string_view actionId, std::string const& body) -> async::Completion<std::string> {
        std::shared_ptr<ModelChannel> const channel = channelOf(*alias, *this);
        if (channel == nullptr) {
            auto [failed, promise] = async::Completion<std::string>::makeSettleable(&runtime().owner());
            promise.reject(std::make_exception_ptr(std::runtime_error{"the node's model alias is not bound"}));
            return std::move(failed);
        }
        return channel->execute(std::string{actionId}, body, CallOptions{});
    };
}

// The calls of a form, collection or wizard: a success other than of @p read, the collection's own query, tells the
// session, which applies the action's effects, as a mutation's success does.
forms::Submitter ScreenCore::aliasSubmits(ModelAlias* alias, std::string read) {
    return [this, alias, call = aliasCalls(alias), read = std::move(read)](
               std::string_view actionId, std::string body) -> async::Completion<std::string> {
        async::Completion<std::string> completion = call(actionId, std::move(body));
        if (actionId != read) {
            completion.then([this, alive = std::weak_ptr<bool>{_alive}, alias, typeId = alias->decl->type,
                             action = std::string{actionId}](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                std::string const token = runtime().untracked([&] { return alias->token->get(); });
                session.actionSucceeded(typeId, action, value ? *value : Value{}, ReplyUse::Applied, token);
            });
        }
        return completion;
    };
}

// The session's locale, translations, display zone and date patterns, as every form the engine builds here reads them.
forms::FormSessionOptions ScreenCore::engineOptions() const {
    FormatContext const& format = session.formatting();
    forms::FormSessionOptions options{};
    options.locale = withoutGrouping(format.numeric);
    options.bcp47 = format.locale;
    options.translations = format.translations;
    options.displayOffsetMinutes = format.utcOffsetMinutes;
    options.datePattern = format.datePattern;
    options.dateTimePattern = format.dateTimePattern;
    return options;
}

// Keeps a form's, collection's or wizard's number locale and date patterns at the session's current formatting. The
// engine's number locale holds views into the facts it was given, so the effect also holds the facts it last applied.
void ScreenCore::followFormatting(ElementState& element, RtScope& scope) {
    auto const held = std::make_shared<std::shared_ptr<FormatContext const>>();
    forms::FormSession* const form = element.form.get();
    forms::CollectionSession* const collection = element.collection.get();
    forms::WizardSession* const wizard = element.wizard.get();
    scope.nodes.effect([this, held, form, collection, wizard] {
        std::shared_ptr<FormatContext const> const format = session.formattingHandle();
        runtime().untracked([&] {
            render::NumericLocale const locale = withoutGrouping(format->numeric);
            if (form != nullptr) {
                form->setFormat(locale, format->datePattern, format->dateTimePattern);
            }
            if (collection != nullptr) {
                collection->setFormat(locale, format->datePattern, format->dateTimePattern);
            }
            if (wizard != nullptr) {
                wizard->setFormat(locale, format->datePattern, format->dateTimePattern);
            }
            *held = format;
        });
    });
}

// Builds a collection's or wizard's session when its element is declared. The node's `action` is the collection's
// query or the wizard's first step; the catalog answers the document for it, and every call (the query, the actions,
// every step's submission) goes through the node's model alias.
void ScreenCore::wireComposite(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    bool const collection = node.kind == "collection";
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    auto const& lookup = collection ? catalog.view : catalog.wizard;
    std::optional<std::string> const document = lookup ? lookup(action) : std::nullopt;
    if (!document.has_value()) {
        report(node.path, std::string{"no "} + (collection ? "view" : "wizard") + " document for `" + action +
                              "`: the catalog has none for a `" + node.kind + "` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::SchemaLookup const schemas = catalog.schema ? forms::SchemaLookup{catalog.schema} : forms::SchemaLookup{};
    forms::Submitter const call = aliasCalls(alias->model);
    if (collection) {
        auto model = forms::CollectionModel::fromSchema(*document);
        if (!model) {
            report(node.path, "the collection of `" + action + "` does not build: " + model.error().path + ": " +
                                  model.error().message);
            return;
        }
        if (model->query() != action) {
            report(node.path, "the view document of `" + action + "` lists `" + std::string{model->query()} + "`");
            return;
        }
        std::string query{model->query()};
        element.collection = std::make_unique<forms::CollectionSession>(runtime(), *std::move(model), schemas,
                                                                        aliasSubmits(alias->model, std::move(query)),
                                                                        call, engineOptions());
        followFormatting(element, scope);
        return;
    }
    // The wizard's id, the stem of its translation keys, is the action it starts with.
    auto model = forms::WizardModel::fromSchema(action, *document);
    if (!model) {
        report(node.path,
               "the wizard of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (model->steps().front().action != action) {
        report(node.path,
               "the wizard document of `" + action + "` starts with `" + model->steps().front().action + "`");
        return;
    }
    try {
        element.wizard = std::make_unique<forms::WizardSession>(runtime(), *std::move(model), schemas,
                                                                aliasSubmits(alias->model), call, engineOptions());
        followFormatting(element, scope);
    } catch (std::invalid_argument const& error) {
        report(node.path, error.what());
    }
}

// The presentation keys a form, a collection and a wizard share, but the title: the label of the explicit-mode button
// and the status line naming it, bound in the node's scope; whether the status line shows, the grid's columns and the
// field layout and the inline caption column's width, as read at load.
void ScreenCore::presentForm(DocNode const& node, LowerCtx const& ctx, forms::FormViewOptions& options) {
    if (ExprPtr const label = node.prop("submitLabel")) {
        options.submitLabel = bindProp<std::string>(ctx, label, std::string{}, stringOf);
    }
    if (auto const shown = node.flags.find("showStatus"); shown != node.flags.end()) {
        options.showStatus = shown->second;
    }
    if (ExprPtr const columns = node.prop("columns"); columns != nullptr && columns->literal.asInt() != nullptr) {
        options.gridColumns = static_cast<int>(*columns->literal.asInt());
        options.flatGridColumns = options.gridColumns;
    }
    if (node.name("fieldLayout") == "inline") {
        options.fieldLayout = forms::FieldLayout::Inline;
    }
    // The loader has checked `captionWidth` is a literal integer.
    if (ExprPtr const width = node.prop("captionWidth")) {
        options.inlineLabelWidth = static_cast<int>(*width->literal.asInt());
    }
}

Node ScreenCore::lowerComposite(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    // The editor's and the steps' titles are the dialog's and the header's; the forms do not repeat them. A `title`
    // replaces the view's own heading.
    forms::FormViewOptions form{};
    form.showTitle = false;
    presentForm(node, ctx, form);
    Prop<std::string> title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    Node view;
    if (element != nullptr && element->collection != nullptr) {
        forms::CollectionViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        view = forms::collectionView(*element->collection, std::move(options));
    } else if (element != nullptr && element->wizard != nullptr) {
        forms::WizardViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        view = forms::wizardView(*element->wizard, options);
    } else {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    Column column{.children = {std::move(view)}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
Node ScreenCore::lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Prop<bool> open = node.prop("open") ? bindProp<bool>(ctx, node.prop("open"), false, boolOf)
                                        : slotProp<bool>([element] { return element->open.get(); });
    std::vector<DocNode const*> bodyContent;
    for (DocNodePtr const& child : node.nodes("children")) {
        bodyContent.push_back(child.get());
    }
    int const bodyGap =
        node.prop("gap") ? static_cast<int>(intOf(once(ctx, node.prop("gap")), kDialogGap)) : kDialogGap;
    LayoutHints const bodyHints = holderHints(bodyContent, node.scope ? &*node.scope : nullptr, ctx);
    Node body = lazy(
        [this, ctx, &node, element, bodyGap] -> Node {
            auto const inner = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
            inner->owner = element;
            addPending(1);
            inner->submitted = &element->submitted;
            inner->showErrors = std::string{node.name("showErrors")};
            std::vector<DocNode const*> content;
            for (DocNodePtr const& child : node.nodes("children")) {
                content.push_back(child.get());
            }
            std::vector<Commands const*> lists;
            for (std::string_view const key : {"default", "cancel"}) {
                if (Commands const* const commands = node.event(key)) {
                    lists.push_back(commands);
                }
            }
            for (DocNodePtr const& action : node.partList("actions")) {
                if (Commands const* const commands = action->event("onClick")) {
                    lists.push_back(commands);
                }
            }
            declare(*inner, node.scope ? &*node.scope : nullptr, content, lists);
            element->inner.set(inner.get());
            LowerCtx const innerCtx{
                .scope = inner,
                .bindings = ctx.bindings,
                .boundary = ctx.boundary,
                .listGap = bodyGap,
            };
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, innerCtx));
            }
            // The overlay's actions end its scoped content, so they bind in its scope; the mount puts them in its
            // footer.
            if (ActionModels actions = lowerActionParts(node.partList("actions"), innerCtx); !actions.empty()) {
                children.push_back(actionBarOf(std::move(actions)));
            }
            std::vector<KeyBinding> keys;
            if (Commands const* const commands = node.event("default")) {
                keys.push_back(KeyBinding{
                    .chord = "Enter",
                    .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); },
                });
            }
            if (Commands const* const commands = node.event("cancel")) {
                keys.push_back(KeyBinding{
                    .chord = "Esc",
                    .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); },
                });
            }
            fillHolder(children, node.path);
            return holding(std::move(children), inner, std::move(keys), bodyGap);
        },
        bodyHints);
    Action onDismiss = [this, ctx, element, commands = node.event("onDismiss")] {
        session.asUser([&] {
            session.noteWrite();
            element->open.set(false);
        });
        runEvent(commands, ctx, Value{});
    };
    if (node.kind == "drawer") {
        ExprPtr const side = node.prop("side");
        Drawer spec{
            .open = std::move(open),
            .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
            .description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf),
            .child = std::move(body),
            .onDismiss = std::move(onDismiss),
            .side = side && textOf(once(ctx, side), ctx.scope->formatting()) == "start" ? Side::Start : Side::End,
            .headNote = bindProp<std::string>(ctx, node.prop("headNote"), std::string{}, stringOf),
            .footerNote = bindProp<std::string>(ctx, node.prop("footerNote"), std::string{}, stringOf),
            .closeButton = bindProp<bool>(ctx, node.prop("closeButton"), false, boolOf),
            .dismissLabel = bindProp<std::string>(ctx, node.prop("dismissLabel"), std::string{"Close"}, stringOf),
        };
        if (ExprPtr const spans = node.prop("descriptionSpans")) {
            spec.descriptionSpans = spansProp(ctx, spans);
        }
        commonOf(node, ctx, element, spec.common);
        return ui::drawer(std::move(spec));
    }
    Dialog spec{
        .open = std::move(open),
        .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
        .description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf),
        .child = std::move(body),
        .onDismiss = std::move(onDismiss),
        .common = {},  // NOLINT(readability-trailing-comma): the comma ends the designator, not the empty braces.
        .sheet = bindProp<bool>(ctx, node.prop("sheet"), false, boolOf),
        .dismissLabel = bindProp<std::string>(ctx, node.prop("dismissLabel"), std::string{"Close"}, stringOf),
        .headNote = bindProp<std::string>(ctx, node.prop("headNote"), std::string{}, stringOf),
        .footerNote = bindProp<std::string>(ctx, node.prop("footerNote"), std::string{}, stringOf),
        .closeButton = bindProp<bool>(ctx, node.prop("closeButton"), false, boolOf),
    };
    if (ExprPtr const spans = node.prop("descriptionSpans")) {
        spec.descriptionSpans = spansProp(ctx, spans);
    }
    commonOf(node, ctx, element, spec.common);
    return ui::dialog(std::move(spec));
}

Node ScreenCore::lowerSwitch(DocNode const& node, LowerCtx const& ctx) {
    std::vector<ExprPtr> whens;
    Switch spec;
    std::int64_t index = 0;
    for (DocNodePtr const& part : node.partList("cases")) {
        whens.push_back(part->prop("when"));
        DocNode const* const casePart = part.get();
        std::vector<DocNode const*> caseContent;
        for (DocNodePtr const& child : casePart->nodes("children")) {
            caseContent.push_back(child.get());
        }
        LayoutHints const caseHints = holderHints(caseContent, casePart->scope ? &*casePart->scope : nullptr, ctx);
        spec.cases.push_back(SwitchCase{
            .key = Key{index++},
            .node = lazy(
                [this, ctx, casePart, caseGap = ctx.listGap] -> Node {
                    auto const inner =
                        std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
                    std::vector<DocNode const*> content;
                    for (DocNodePtr const& child : casePart->nodes("children")) {
                        content.push_back(child.get());
                    }
                    declare(*inner, casePart->scope ? &*casePart->scope : nullptr, content, {});
                    LowerCtx const innerCtx{
                        .scope = inner,
                        .bindings = ctx.bindings,
                        .boundary = ctx.boundary,
                        .listGap = caseGap,
                    };
                    std::vector<Node> children;
                    children.reserve(content.size());
                    for (DocNode const* const child : content) {
                        children.push_back(lower(*child, innerCtx));
                    }
                    fillHolder(children, casePart->path);
                    return holding(std::move(children), inner, {}, caseGap);
                },
                caseHints),
        });
    }
    spec.selector = slotProp<Key>(
        [scope = ctx.scope, bindings = ctx.bindings, selected = node.prop("on"), whens = std::move(whens)] {
            Value const value = evalIn(*scope, bindings.get(), selected);
            for (std::size_t at = 0; at < whens.size(); ++at) {
                if (evalIn(*scope, bindings.get(), whens.at(at)) == value) {
                    return Key{static_cast<std::int64_t>(at)};
                }
            }
            return Key{std::int64_t{-1}};
        });
    auto const fallback = node.nodes("default");
    spec.fallback = fallback.empty() ? Node{} : lowerAll(fallback, ctx, ctx.listGap);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// The tab a bound `selected` names. A value that names no tab shows the first, and is reported once.
Prop<std::size_t> ScreenCore::controlledTabSelection(DocNode const& node, LowerCtx const& ctx, ExprPtr const& selected,
                                                     std::shared_ptr<std::vector<std::string>> const& ids) {
    auto const unknownReported = std::make_shared<bool>(false);
    return Prop<std::size_t>{[this, scope = ctx.scope, bindings = ctx.bindings, selected, ids, unknownReported,
                              path = pointerAppend(node.path, "selected")] -> std::size_t {
        Value const value = evalIn(*scope, bindings.get(), selected);
        std::string const* const named = value.asString();
        auto const found = named == nullptr ? ids->end() : std::ranges::find(*ids, *named);
        if (found != ids->end()) {
            return static_cast<std::size_t>(std::distance(ids->begin(), found));
        }
        if (!*unknownReported) {
            *unknownReported = true;
            report(path, canonicalJson(value) + " names no tab; the first is shown");
        }
        return 0;
    }};
}

// A document names tabs by id; the view tree by index, so the ids map to indices once. A bound `selected` is the
// controlled index: a pick runs `onChoose` and the page follows `selected` alone. Without it the node keeps the
// user's pick itself, as a bound index too, whose page follows the pick: a constant would move only the highlight.
Node ScreenCore::lowerTabs(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Tabs spec;
    auto const ids = std::make_shared<std::vector<std::string>>();
    for (DocNodePtr const& tab : node.partList("tabs")) {
        auto const nodes = tab->nodes("children");
        ids->push_back(tab->id);
        spec.tabs.push_back(Tab{
            .label = bindProp<std::string>(ctx, tab->prop("label"), std::string{}, stringOf),
            .node = nodes.empty() ? Node{} : lowerAll(nodes, ctx),
        });
    }
    spec.moreLabel = bindProp<std::string>(ctx, node.prop("moreLabel"), std::string{"More"}, stringOf);
    if (ExprPtr const inset = node.prop("inset")) {
        spec.inset = static_cast<int>(intOf(once(ctx, inset), 0));
    }
    if (ExprPtr const pageInset = node.prop("pageInset")) {
        spec.pageInset = static_cast<int>(intOf(once(ctx, pageInset), 0));
    }
    spec.stripSurface = std::string{node.name("stripSurface")};
    ExprPtr const selected = node.prop("selected");
    Commands const* const onChoose = node.event("onChoose");
    bool const controlled = selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull());
    auto const picked = std::make_shared<reactive::Signal<std::size_t>>(runtime(), std::size_t{0});
    spec.selected = controlled ? controlledTabSelection(node, ctx, selected, ids)
                               : Prop<std::size_t>{[picked] { return picked->get(); }};
    spec.onSelect = [this, ctx, onChoose, ids, picked, controlled, element](std::size_t index) {
        if (!controlled) {
            picked->set(index);
            if (element != nullptr && index < ids->size()) {
                element->draft.set(Value{ids->at(index)});
            }
        }
        if (onChoose != nullptr && index < ids->size()) {
            runEvent(onChoose, ctx, Value{ids->at(index)});
        }
    };
    commonOf(node, ctx, element, spec.common);
    return ui::tabs(std::move(spec));
}

// Focuses the declaration the command's target resolved to: of several candidates (cases of one switch), the one
// that is mounted. Each candidate is keyed by the command's own enclosing row keys, as many as enclose the
// candidate; with `row`, the innermost of them is the row's key instead.
bool ScreenCore::focusTarget(Command const& command, RtScope const& scope, Bindings const* bindings) {
    if (!focusElement) {
        return false;
    }
    auto const found = _doc->focusTargets.find(command.path);
    if (found == _doc->focusTargets.end()) {
        return focusElement(command.target);
    }
    FocusPlace const place = focusPlace(scope);
    std::optional<std::string> row;
    if (found->second.row) {
        Value const value = evalIn(scope, bindings, command.extra);
        if (value.isNull()) {
            return false;
        }
        row = canonicalJson(valueOfKey(keyOf(value)));
    }
    for (FocusCandidate const& candidate : found->second.candidates) {
        std::size_t const kept = row ? candidate.rows - 1 : candidate.rows;
        if (candidate.rows == 0 && row) {
            continue;
        }
        if (kept > place.rows.size()) {
            continue;
        }
        FocusPlace candidatePlace{
            .prefix = place.prefix,
            .rows = {place.rows.begin(), std::next(place.rows.begin(), static_cast<std::ptrdiff_t>(kept))},
        };
        if (row) {
            candidatePlace.rows.push_back(*row);
        }
        if (focusElement(focusKey(candidatePlace, candidate.path))) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<RtScope> ScreenCore::rowScope(DocNode const& node, LowerCtx const& ctx,
                                              reactive::Signal<Value> const& row,
                                              std::vector<DocNode const*> const& content, std::string key) {
    auto scope = std::make_shared<RtScope>(*this, ctx.scope, &row, ctx.scope->nodes.depth() + 2);
    scope->rowKey = std::move(key);
    declare(*scope, node.scope ? &*node.scope : nullptr, content, {});
    return scope;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a list of rows and a table.
Node ScreenCore::lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    ExprPtr const rowsExpr = node.prop("rows");
    if (!rowsExpr) {
        report(node.path, "a model-backed `table` needs the table engine, which this interpreter does not lower yet");
        return ui::text(Text{.text = std::string{"table"}, .role = TextRole::Muted});
    }
    std::function<std::vector<Value>()> rows = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        Value const value = evalIn(*scope, bindings.get(), rowsExpr);
        ValueList const* const items = value.asList();
        return items == nullptr ? std::vector<Value>{} : std::vector<Value>{items->begin(), items->end()};
    };
    auto keyFn = [scope = ctx.scope, bindings = ctx.bindings, keyExpr = node.prop("key")](Value const& row) -> Key {
        if (Value const* const heading = row.field(kGroupHeadingField); heading != nullptr && heading->asString()) {
            return Key{*heading->asString()};
        }
        if (!keyExpr) {
            Value const rowKey = rowId(row);
            return keyOf(rowKey.isNull() ? row : rowKey);
        }
        ScopeEnv const env{*scope, bindings.get(), &row};
        return keyOf(evaluate(*keyExpr, env));
    };
    if (node.kind == "forEach") {
        std::vector<DocNode const*> content;
        for (DocNodePtr const& child : node.nodes("children")) {
            content.push_back(child.get());
        }
        auto rowView = [this, ctx, &node, content, keyFn](reactive::Signal<Value> const& row) -> Node {
            std::string key = runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); });
            auto const scope = rowScope(node, ctx, row, content, std::move(key));
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, rowCtx));
            }
            return holding(std::move(children), scope);
        };
        ExprPtr const axis = node.prop("axis");
        ExprPtr const gap = node.prop("gap");
        Common common;
        commonOf(node, ctx, element, common);
        Axis const along = axis ? axisOf(once(ctx, axis)) : Axis::Vertical;
        bool wrap = node.flag("wrap");
        if (wrap && along != Axis::Horizontal) {
            // Only a computed axis gets here: the loader refuses a literal one that is not horizontal.
            report(node.path + "/wrap",
                   "`wrap` fills lines along a row; this `forEach`'s `axis` is not \"horizontal\"");
            wrap = false;
        }
        bool divided = node.flag("divided");
        if (divided && along != Axis::Vertical) {
            report(node.path + "/divided",
                   "`divided` draws rules between rows of a column; this `forEach`'s `axis` is \"horizontal\"");
            divided = false;
        }
        return ui::forEach<Value>(std::move(rows), std::move(keyFn), std::move(rowView), along,
                                  gap ? static_cast<int>(intOf(once(ctx, gap), 0)) : 0, common, wrap, divided);
    }
    // The table's rows, sorted: the element's own view, which its element values read too, unless names bound
    // where the table is lowered (a component's `props`) reach its rows, which the element cannot see.
    reactive::Computed<TableView> const* view = element == nullptr ? nullptr : element->view;
    if (view != nullptr && ctx.bindings != nullptr && !ctx.bindings->empty()) {
        view = &ctx.scope->nodes.computed(
            [this, element, bindings = ctx.bindings] { return tableView(*element, bindings.get()); });
    }
    if (view != nullptr) {
        // The page's rows, each group's heading entry before the first of its rows the page holds, so a group split
        // across pages gets its heading on both.
        rows = [view] {
            TableView const& shown = view->get();
            std::vector<Value> entries;
            entries.reserve(shown.last - shown.first);
            for (std::size_t index = shown.first; index < shown.last; ++index) {
                if (!shown.runOf.empty() &&
                    (index == shown.first || shown.runOf.at(index) != shown.runOf.at(index - 1))) {
                    entries.push_back(shown.runs.at(shown.runOf.at(index)));
                }
                entries.push_back(shown.rows.at(index));
            }
            return entries;
        };
    }
    std::vector<TableColumn> columns;
    std::vector<DocNode const*> cells;
    std::vector<std::string> fields;
    // One entry per column, in the same order as `cells`/`fields`. Captured by `cellsOf` below (not `columns`
    // itself, which is moved into `ui::table<Value>(...)` further down and so cannot be read from inside a lambda
    // that outlives this function) so the default-cell closure can apply each column's role to its generated cell.
    std::vector<TextRole> roles;
    FormatContext const& format = ctx.scope->formatting();
    // The table's `valign` is the default of every column that gives none; the loader checked both names.
    Align const tableValign = cellPlaceNamed(node.name("valign")).value_or(Align::Center);
    if (ExprPtr const derived = node.prop("columns")) {
        Value const list = once(ctx, derived);
        // A computed place that names none is reported once for the table, whichever column and key it was.
        bool placeReported = false;
        auto const placeOf = [&](Value const& item, std::string_view key, Align fallback) {
            Value const* const given = item.field(key);
            if (given == nullptr || given->isNull()) {
                return fallback;
            }
            std::string const* const name = given->asString();
            std::optional<Align> const placed = name == nullptr ? std::nullopt : cellPlaceNamed(*name);
            if (!placed && !placeReported) {
                placeReported = true;
                report(derived->path, "a column's `" + std::string{key} + "` is " + canonicalJson(*given) +
                                          "; a cell's place is start, center or end");
            }
            return placed.value_or(fallback);
        };
        if (ValueList const* const items = list.asList()) {
            for (Value const& item : *items) {
                Value const* const field = item.field("id");
                Value const* const label = item.field("label");
                fields.push_back(field == nullptr ? std::string{} : textOf(*field, format));
                columns.push_back(TableColumn{
                    .label = textOf(label != nullptr ? *label : Value{fields.back()}, format),
                    .align = placeOf(item, "align", Align::Start),
                    .valign = placeOf(item, "valign", tableValign),
                });
                cells.push_back(nullptr);
                roles.push_back(TextRole::Normal);
            }
        }
    }
    // A part's column index, which a header click reports, counts the derived columns before it.
    std::size_t const derivedCount = columns.size();
    for (DocNodePtr const& part : node.partList("columns")) {
        ExprPtr const label = part->prop("label");
        ExprPtr const width = part->prop("width");
        ExprPtr const role = part->prop("role");
        TextRole columnRole = TextRole::Normal;
        if (role) {
            // Read once, with the rest of the column; a value that names no role is reported, as a text's is.
            Value const value = once(ctx, role);
            std::optional<TextRole> const named = roleNamed(textOf(value, format));
            if (!named) {
                report(role->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
            }
            columnRole = named.value_or(TextRole::Normal);
        }
        bool const shrink = part->flag("shrink");
        Sizing const sized = width ? sizingOf(once(ctx, width)) : Sizing::content();
        if (shrink && sized.kind != Sizing::Kind::Stretch) {
            report(part->path + "/shrink",
                   "`shrink` lets a stretch column go narrower than its header; this column's width does not stretch");
        }
        columns.push_back(TableColumn{
            .label = label ? textOf(once(ctx, label), format) : std::string{},
            .width = sized,
            .role = columnRole,
            .align = cellPlaceNamed(part->name("align")).value_or(Align::Start),
            .valign = cellPlaceNamed(part->name("valign")).value_or(tableValign),
            .sortable = part->flag("sortable"),
            .wrap = part->flag("wrap", true),
            .shrink = shrink && sized.kind == Sizing::Kind::Stretch,
        });
        cells.push_back(part.get());
        fields.emplace_back(part->id);
        roles.push_back(columnRole);
    }
    auto const registry = std::make_shared<std::map<std::string, std::weak_ptr<RtScope>, std::less<>>>();
    // A value that names no tone is reported once for the table, whichever row and value it was.
    auto const toneReported = std::make_shared<bool>(false);
    // Likewise for a `rowSpans` value that is no list of spans.
    auto const spansReported = std::make_shared<bool>(false);
    auto cellsOf = [this, ctx, &node, cells, fields, roles, registry, keyFn, toneReported,
                    spansReported](reactive::Signal<Value> const& row) {
        if (row.peek().field(kGroupHeadingField) != nullptr) {
            return headingRow(node, ctx, row,
                              runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); }));
        }
        std::vector<DocNode const*> content;
        for (DocNode const* const column : cells) {
            if (column != nullptr) {
                for (DocNodePtr const& cell : column->nodes("cell")) {
                    content.push_back(cell.get());
                }
            }
        }
        std::string key = runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); });
        auto const scope = rowScope(node, ctx, row, content, key);
        (*registry)[std::move(key)] = scope;
        LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        std::vector<Node> out;
        for (std::size_t index = 0; index < cells.size(); ++index) {
            DocNode const* const column = cells.at(index);
            auto const nodes = column == nullptr ? std::span<DocNodePtr const>{} : column->nodes("cell");
            if (!nodes.empty()) {
                out.push_back(lowerAll(nodes, rowCtx));
                continue;
            }
            out.push_back(ui::text(Text{
                .text = slotProp<std::string>([scope = rowCtx.scope, &row, field = fields.at(index)] {
                    Value const* const value = row.get().field(field);
                    return value == nullptr ? std::string{} : textOf(*value, scope->formatting());
                }),
                .role = roles.at(index),
            }));
        }
        if (!out.empty()) {
            // The row's first cell owns the row's scope, which lives as long as the row's node: a cell is placed by
            // its column, so the scope is not held by a wrapping container, which the column would place instead.
            auto const owner = std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(out.front(), scope);
            out.front() = Node{owner, owner->first.get()};
        }
        TableRowView rowView{.cells = std::move(out), .tone = std::nullopt};
        // The row's name, in its own scope: what its select box and assistive technology read.
        if (ExprPtr const labelExpr = node.prop("rowLabel")) {
            rowView.label = bindProp<std::string>(rowCtx, labelExpr, std::string{}, stringOf);
        }
        if (ExprPtr const toneExpr = node.prop("rowTone")) {
            rowView.tone = slotProp<std::optional<Tone>>(
                [this, scope, bindings = ctx.bindings, toneExpr, toneReported] -> std::optional<Tone> {
                    return rowToneOf(evalIn(*scope, bindings.get(), toneExpr), toneExpr->path, *toneReported);
                });
        }
        if (ExprPtr const spansExpr = node.prop("rowSpans")) {
            rowView.spans = slotProp<std::vector<RowSpan>>([this, scope, bindings = ctx.bindings, spansExpr,
                                                            spansReported, fields] {
                return rowSpansOf(evalIn(*scope, bindings.get(), spansExpr), fields, spansExpr->path, *spansReported);
            });
        }
        return rowView;
    };
    TableOptions options;
    options.showHeader = node.flag("showHeader", true);
    options.selectColumn = node.flag("selectColumn");
    if (ExprPtr const selectAll = node.prop("selectAllLabel")) {
        options.selectAllLabel = bindProp<std::string>(ctx, selectAll, std::string{}, stringOf);
    }
    if (ExprPtr const threshold = node.prop("virtualizeThreshold")) {
        options.virtualize.threshold =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, threshold), 2000)));
    }
    if (ExprPtr const overscan = node.prop("virtualizeOverscan")) {
        options.virtualize.overscan =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, overscan), 10)));
    }
    std::string const mode =
        node.prop("selectionMode") ? textOf(once(ctx, node.prop("selectionMode")), format) : std::string{};
    options.selectionMode = SelectionMode::None;
    if (mode == "single") {
        options.selectionMode = SelectionMode::Single;
    } else if (mode == "multiple") {
        options.selectionMode = SelectionMode::Multiple;
    }
    // The selection the table shows: the bound `selection`, or the table's own. A slot even for a literal list, so
    // a bound selection is fixed until the document writes it, and a user's change the document leaves unwritten
    // snaps back. A bound value that is neither a list nor `null` is reported once.
    auto const selectionReported = std::make_shared<bool>(false);
    options.selection = slotProp<std::vector<Key>>([this, element, selectionReported, path = node.path] {
        std::vector<Key> keys;
        if (element->bound != nullptr) {
            Value const bound = element->bound->get();
            if (!bound.isNull() && bound.asList() == nullptr && !*selectionReported) {
                *selectionReported = true;
                report(pointerAppend(path, "selection"),
                       "selection is " + canonicalJson(bound) + "; a table's selection is a list of keys");
            }
        }
        Value const selection = tableSelection(*element);
        if (ValueList const* const items = selection.asList()) {
            for (Value const& item : *items) {
                keys.push_back(keyOf(item));
            }
        }
        return keys;
    });
    // A user's change: the table's own selection takes it when no key binds one, then `onSelectionChange` runs with
    // `event` the new selection, in the table's scope, in the same batch. The backend holds one page: a `Replace`
    // is the whole new selection, and an `Extend` keeps the selected keys of the rows on other pages, before the
    // reported ones. A key whose row `rows` no longer has drops on any user change.
    options.onSelectionChange = [this, ctx, element, view, keyFn, commands = node.event("onSelectionChange")](
                                    std::vector<Key> const& keys, SelectionChange change) {
        ValueList items;
        if (change == SelectionChange::Extend && view != nullptr) {
            items = runtime().untracked([&] { return selectedOffPage(view->get(), tableSelection(*element), keyFn); });
        }
        // A heading is never selected. A virtualized backend knows the keys of rows it has not realized but not
        // which of them are headings, so its Ctrl+A or range can name one.
        for (Key const& key : keys) {
            if (std::string const* const text = std::get_if<std::string>(&key);
                text != nullptr && text->starts_with("\x1Fgroup:")) {
                continue;
            }
            items.push_back(valueOfKey(key));
        }
        Value const selected{std::move(items)};
        runtime().batch([&] {
            if (element->bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->selection.set(selected);
                });
            }
            runEvent(commands, ctx, selected);
        });
    };
    // The header marks the sort's first key; a sort that names no sortable column marks none.
    auto const columnIndex = [&node, derivedCount](std::string_view columnId) -> std::optional<std::size_t> {
        auto const parts = node.partList("columns");
        auto const found =
            std::ranges::find_if(parts, [columnId](DocNodePtr const& part) { return part->id == columnId; });
        if (found == parts.end()) {
            return std::nullopt;
        }
        return derivedCount + static_cast<std::size_t>(std::distance(parts.begin(), found));
    };
    options.sort = slotProp<std::optional<TableSort>>([element, &node, columnIndex] -> std::optional<TableSort> {
        std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, tableSort(*element));
        if (!keys || keys->empty()) {
            return std::nullopt;
        }
        std::optional<std::size_t> const index = columnIndex(keys->front().column->id);
        if (!index) {
            return std::nullopt;
        }
        return TableSort{
            .column = *index,
            .direction = keys->front().descending ? SortDirection::Descending : SortDirection::Ascending,
        };
    });
    // A header click makes its column the single key: descending when it already is the first key ascending,
    // ascending otherwise. The table's own sort takes it when no key binds one, then `onSortChange` runs with
    // `event` the new sort.
    options.onSort = [this, ctx, element, &node, derivedCount,
                      commands = node.event("onSortChange")](std::size_t index) {
        auto const parts = node.partList("columns");
        if (index < derivedCount || index - derivedCount >= parts.size() ||
            !parts.subspan(index - derivedCount).front()->flag("sortable")) {
            return;
        }
        std::string const& columnId = parts.subspan(index - derivedCount).front()->id;
        Value const current = runtime().untracked([&] { return tableSort(*element); });
        std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, current);
        bool const descending =
            keys && !keys->empty() && keys->front().column->id == columnId && !keys->front().descending;
        ValueRecord key;
        key.insert_or_assign("column", Value{columnId});
        key.insert_or_assign("direction", Value{std::string{descending ? "desc" : "asc"}});
        Value const next{ValueList{Value{std::move(key)}}};
        runtime().batch([&] {
            if (element->sortBound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->sort.set(next);
                });
            }
            runEvent(commands, ctx, next);
        });
    };
    options.onActivate = [this, ctx, element, registry, commands = node.event("onActivate")](Key const& key) {
        Value const activated = valueOfKey(key);
        session.asUser([&] {
            session.noteWrite();
            element->activeKey.set(activated);
        });
        auto const found = registry->find(canonicalJson(activated));
        std::shared_ptr<RtScope> const scope = found == registry->end() ? nullptr : found->second.lock();
        LowerCtx const rowCtx{.scope = scope ? scope : ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        runEvent(commands, rowCtx, activated);
    };
    commonOf(node, ctx, element, options.common);
    std::span<DocNodePtr const> const empty = node.nodes("empty");
    if (empty.empty()) {
        return ui::table<Value>(std::move(columns), std::move(rows), std::move(keyFn), std::move(cellsOf),
                                std::move(options));
    }
    // With `empty`, a column holds the table and the content shown in its place while `rows` is an empty list; a
    // `null` (a query not answered yet) shows the table. The column is placed as the table would be: it takes the
    // node's visibility and layout, and every other common property stays on the table, so its test id and the
    // element id `focus` finds name the table.
    auto const listed = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        ValueList const* const items = evalIn(*scope, bindings.get(), rowsExpr).asList();
        return items == nullptr || !items->empty();
    };
    Column wrapper;
    wrapper.common.visible = std::move(options.common.visible);
    wrapper.common.layout = options.common.layout;
    options.common.visible = slotProp<bool>(listed);
    Column content{.children = {lowerAll(empty, ctx)}};
    content.common.visible = slotProp<bool>([listed] { return !listed(); });
    wrapper.children.push_back(ui::table<Value>(std::move(columns), std::move(rows), std::move(keyFn),
                                                std::move(cellsOf), std::move(options)));
    wrapper.children.push_back(ui::column(std::move(content)));
    return ui::column(std::move(wrapper));
}

TableRowView ScreenCore::headingRow(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                                    std::string key) {
    auto const heading = node.partList("group").front()->nodes("heading");
    std::vector<DocNode const*> content;
    content.reserve(heading.size());
    for (DocNodePtr const& child : heading) {
        content.push_back(child.get());
    }
    auto const scope = rowScope(node, ctx, row, content, std::move(key));
    LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
    // The cell owns the heading's scope, for as long as the row's node lives, as a data row's first cell does.
    auto const owner =
        std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(lowerAll(heading, rowCtx), scope);
    return TableRowView{.cells = {Node{owner, owner->first.get()}}, .tone = std::nullopt, .heading = true};
}

Node ScreenCore::lowerChart(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Chart spec;
    spec.data = slotProp<ChartData>(
        [this, &node, scope = ctx.scope, bindings = ctx.bindings] { return chartData(node, *scope, bindings.get()); });
    commonOf(node, ctx, element, spec.common);
    return ui::chart(std::move(spec));
}

std::optional<math::Rational> ScreenCore::chartMagnitude(Value const& value, std::string const& path,
                                                         std::optional<std::string>& unit) {
    std::optional<Number> const number = numeric(value);
    if (!number) {
        report(path, "a chart value is a number or a quantity; this one is left out");
        return std::nullopt;
    }
    std::string const own = number->unit != nullptr ? *number->unit : std::string{};
    if (!unit) {
        unit = own;
        return number->value;
    }
    if (*unit == own) {
        return number->value;
    }
    std::optional<math::Rational> converted;
    if (!own.empty() && !unit->empty()) {
        converted = convertUnit(number->value, own, *unit, session.formatting().units);
    }
    if (!converted) {
        report(path, "a chart value does not convert to the unit of the chart's first value; it is left out");
    }
    return converted;
}

std::optional<Tone> ScreenCore::chartToneOf(Value const& value, std::string const& path) {
    static constexpr std::array<std::pair<std::string_view, Tone>, 5> kTones = {
        {
            {"neutral", Tone::Neutral},
            {"info", Tone::Info},
            {"ok", Tone::Ok},
            {"warn", Tone::Warn},
            {"err", Tone::Err},
        },
    };
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::string const* const text = value.asString()) {
        for (auto const& [name, tone] : kTones) {
            if (name == *text) {
                return tone;
            }
        }
    }
    report(path, "a chart tone is neutral, info, ok, warn or err; this one marks nothing");
    return std::nullopt;
}

std::optional<math::Rational> ScreenCore::chartPosition(Value const& value, std::string const& path,
                                                        detail::ChartAxisKinds& kinds) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::optional<Number> const number = numeric(value)) {
        kinds.numbers = true;
        return number->value;
    }
    if (std::optional<time::DateTime> const instant =
            dateInstantOf(value, session.formatting().utcOffsetMinutes).instant) {
        kinds.instants = true;
        return math::Rational{instant->value.time_since_epoch().count(), math::DecimalPlaces{0}};
    }
    report(path, "a chart's `x` is a number, a date or an instant; this point is left out");
    return std::nullopt;
}

std::vector<detail::ChartRawPoint> ScreenCore::chartPoints(DocNode const& series, RtScope const& scope,
                                                           Bindings const* bindings, std::optional<std::string>& unit,
                                                           detail::ChartAxisKinds& kinds) {
    // A chart is a picture of a few hundred values; a larger set is a table's.
    static constexpr std::size_t kMaxPoints = 1000;
    FormatContext const& format = scope.formatting();
    ExprPtr const pointsExpr = series.prop("points");
    Value const listed = evalIn(scope, bindings, pointsExpr);
    ValueList const* const list = listed.asList();
    if (list == nullptr) {
        // `null` is a query still loading, and draws as empty without a word.
        if (!listed.isNull()) {
            report(pointsExpr->path, "`points` is a list; the series draws as empty");
        }
        return {};
    }
    std::size_t const first = list->size() > kMaxPoints ? list->size() - kMaxPoints : 0;
    if (first > 0) {
        report(pointsExpr->path, "a series draws its last 1000 points");
    }
    ExprPtr const yExpr = series.prop("y");
    ExprPtr const xExpr = series.prop("x");
    ExprPtr const labelExpr = series.prop("xLabel");
    ExprPtr const toneExpr = series.prop("tone");
    ExprPtr const noteExpr = series.prop("note");
    std::vector<detail::ChartRawPoint> points;
    for (std::size_t index = first; index < list->size(); ++index) {
        ScopeEnv const env{scope, bindings, &list->at(index)};
        detail::ChartRawPoint point{.index = index};
        if (xExpr) {
            point.x = chartPosition(evaluate(*xExpr, env), xExpr->path, kinds);
            if (!point.x) {
                continue;
            }
        }
        Value const value = evaluate(*yExpr, env);
        if (!value.isNull()) {
            point.y = chartMagnitude(value, yExpr->path, unit);
            if (!point.y) {
                continue;
            }
        }
        point.xLabel = labelExpr ? textOf(evaluate(*labelExpr, env), format) : std::string{};
        point.note = noteExpr ? textOf(evaluate(*noteExpr, env), format) : std::string{};
        point.tone = toneExpr ? chartToneOf(evaluate(*toneExpr, env), toneExpr->path) : std::nullopt;
        points.push_back(std::move(point));
    }
    return points;
}

namespace {

/// @brief A chart's exact value at @p places, as the active locale writes it.
/// @param value  The value.
/// @param places The places, at most 18.
/// @param format The formatting facts.
/// @return The text; the exact JSON when the rounded value does not fit.
[[nodiscard]] std::string chartNumberText(math::Rational const& value, std::uint32_t places,
                                          FormatContext const& format) {
    std::optional<math::Rational> rounded =
        math::checkedRoundToDecimalPlaces(value, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    if (!rounded) {
        return textOf(Value{Decimal{value}}, format);
    }
    rounded->decimalPlaces = math::DecimalPlaces{places};
    return textOf(Value{Decimal{*rounded}}, format);
}

/// @brief A reference line's stroke by its document name.
/// @param name `solid`, `dashed`, `dotted`, or empty for solid; the loader refuses any other.
/// @return The stroke.
[[nodiscard]] ChartLineStyle chartLineStyle(std::string_view name) {
    if (name == "dashed") {
        return ChartLineStyle::Dashed;
    }
    return name == "dotted" ? ChartLineStyle::Dotted : ChartLineStyle::Solid;
}

/// @brief Where a point sits along a value axis that a document may have fixed, and on which side it is clipped.
/// @param value The point's value.
/// @param range The axis.
/// @return The value drawn, the axis end for a value outside, and the side it lies beyond.
[[nodiscard]] std::pair<math::Rational, ChartClip> chartClipped(math::Rational const& value, ChartRange const& range) {
    if (value < range.low) {
        return {range.low, ChartClip::Below};
    }
    if (value > range.high) {
        return {range.high, ChartClip::Above};
    }
    return {value, ChartClip::None};
}
}  // namespace

std::vector<detail::ChartRawLine> ScreenCore::chartLines(DocNode const& node, RtScope const& scope,
                                                         Bindings const* bindings, std::optional<std::string>& unit) {
    FormatContext const& format = scope.formatting();
    std::vector<detail::ChartRawLine> lines;
    for (DocNodePtr const& part : node.partList("lines")) {
        ExprPtr const valueExpr = part->prop("value");
        Value const value = evalIn(scope, bindings, valueExpr);
        std::optional<math::Rational> const magnitude =
            value.isNull() ? std::nullopt : chartMagnitude(value, valueExpr->path, unit);
        if (!magnitude) {
            continue;
        }
        detail::ChartRawLine line{.value = *magnitude, .label = std::nullopt};
        if (ExprPtr const label = part->prop("label")) {
            line.label = textOf(evalIn(scope, bindings, label), format);
        }
        if (ExprPtr const tone = part->prop("tone")) {
            line.tone = chartToneOf(evalIn(scope, bindings, tone), tone->path).value_or(Tone::Neutral);
        }
        line.style = detail::chartLineStyle(part->name("style"));
        lines.push_back(std::move(line));
    }
    return lines;
}

ChartRange ScreenCore::chartFixedRange(DocNode const& node, RtScope const& scope, Bindings const* bindings,
                                       std::optional<std::string>& unit, ChartRange const& range) {
    ChartRange fixed = range;
    for (auto const& [key, end] : {std::pair{"yMin", &fixed.low}, std::pair{"yMax", &fixed.high}}) {
        if (ExprPtr const expr = node.prop(key)) {
            Value const value = evalIn(scope, bindings, expr);
            if (!value.isNull()) {
                *end = chartMagnitude(value, expr->path, unit).value_or(*end);
            }
        }
    }
    if (fixed.high > fixed.low) {
        return fixed;
    }
    report(pointerAppend(node.path, "yMin"), "`yMin` is not below `yMax`; the chart takes its range from its values");
    return range;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per way a series can be drawn or clipped.
void ScreenCore::chartSeries(ChartData& data, DocNode const& node, RtScope const& scope, Bindings const* bindings,
                             std::vector<std::vector<detail::ChartRawPoint>>& series, bool positioned,
                             ChartRange const& range) {
    auto const positionOf = [positioned](detail::ChartRawPoint const& point) {
        return positioned && point.x ? *point.x
                                     : math::Rational{static_cast<std::int64_t>(point.index), math::DecimalPlaces{0}};
    };
    std::vector<math::Rational> positions;
    for (auto const& points : series) {
        std::ranges::transform(points, std::back_inserter(positions), positionOf);
    }
    // The room one position gives its bars: the least distance between two of their positions, gaps included, so a
    // bar never grows over the label of an empty position between two.
    std::vector<math::Rational> barPositions;
    {
        std::size_t seriesIndex = 0;
        for (DocNodePtr const& part : node.partList("series")) {
            if (part->name("style") == "bars") {
                std::ranges::transform(series.at(seriesIndex), std::back_inserter(barPositions), positionOf);
            }
            ++seriesIndex;
        }
    }
    std::ranges::sort(barPositions);
    std::optional<math::Rational> slot;
    for (std::size_t next = 1; next < barPositions.size(); ++next) {
        auto const distance = math::checkedSub(barPositions.at(next), barPositions.at(next - 1));
        if (distance && distance->numerator > 0 && (!slot || *distance < *slot)) {
            slot = *distance;
        }
    }
    if (!barPositions.empty()) {
        data.barSlot = realOf(slot.value_or(math::Rational{1, math::DecimalPlaces{0}}));
    }
    // A bar is centred on its position and half a slot wide each side, so the axis holds that much beyond the
    // outermost position.
    if (!barPositions.empty() && !positions.empty()) {
        math::Rational const half = math::checkedDiv(slot.value_or(math::Rational{1, math::DecimalPlaces{0}}),
                                                     math::Rational{2, math::DecimalPlaces{0}})
                                        .value_or(math::Rational{0, math::DecimalPlaces{0}});
        auto const [lowest, highest] = std::ranges::minmax(positions);
        auto const below = math::checkedSub(lowest, half);
        auto const above = math::checkedAdd(highest, half);
        if (below && above) {
            positions.push_back(*below);
            positions.push_back(*above);
        }
    }
    if (std::optional<ChartRange> const across = chartRange(positions, 0)) {
        data.xMin = realOf(across->low);
        data.xMax = realOf(across->high);
    }
    std::size_t index = 0;
    for (DocNodePtr const& part : node.partList("series")) {
        ChartSeries drawn;
        if (part->name("style") == "bars") {
            drawn.style = ChartStyle::Bars;
        }
        if (ExprPtr const label = part->prop("label")) {
            drawn.label = textOf(evalIn(scope, bindings, label), scope.formatting());
        }
        drawn.joined = drawn.style == ChartStyle::Line && (!part->flags.contains("joined") || part->flag("joined"));
        for (detail::ChartRawPoint& point : series.at(index)) {
            ChartPoint out{
                .x = realOf(positionOf(point)),
                .y = std::nullopt,
                .clip = ChartClip::None,
                .xLabel = std::move(point.xLabel),
                .tone = point.tone,
                .note = std::move(point.note),
            };
            if (point.y) {
                auto const [shown, clip] = detail::chartClipped(*point.y, range);
                out.y = realOf(shown);
                out.clip = clip;
            }
            drawn.points.push_back(std::move(out));
        }
        data.series.push_back(std::move(drawn));
        ++index;
    }
}

ChartData ScreenCore::chartData(DocNode const& node, RtScope const& scope, Bindings const* bindings) {
    FormatContext const& format = scope.formatting();
    ChartData data;
    data.summary = textOf(evalIn(scope, bindings, node.prop("summary")), format);
    if (ExprPtr const label = node.prop("yLabel")) {
        data.yLabel = textOf(evalIn(scope, bindings, label), format);
    }
    std::optional<std::string> unit;
    detail::ChartAxisKinds kinds;
    std::vector<std::vector<detail::ChartRawPoint>> series;
    for (DocNodePtr const& part : node.partList("series")) {
        series.push_back(chartPoints(*part, scope, bindings, unit, kinds));
    }
    std::vector<detail::ChartRawLine> const lines = chartLines(node, scope, bindings, unit);
    // The values the axis must hold, and the places they are shown at.
    std::vector<math::Rational> values;
    for (auto const& points : series) {
        for (detail::ChartRawPoint const& point : points) {
            if (point.y) {
                values.push_back(*point.y);
            }
        }
    }
    std::ranges::transform(lines, std::back_inserter(values), &detail::ChartRawLine::value);
    // A bar grows from 0, so the axis holds 0; the baseline's side is padded only when something lies beyond it, so
    // that the baseline is the plot's edge when nothing does.
    std::vector<math::Rational> axisValues = values;
    ChartPadding padding;
    bool barsHaveValues = false;
    {
        std::size_t seriesIndex = 0;
        for (DocNodePtr const& part : node.partList("series")) {
            barsHaveValues =
                barsHaveValues || (part->name("style") == "bars" &&
                                   std::ranges::any_of(series.at(seriesIndex), [](detail::ChartRawPoint const& point) {
                                       return point.y.has_value();
                                   }));
            ++seriesIndex;
        }
    }
    if (barsHaveValues) {
        axisValues.emplace_back(0, math::DecimalPlaces{0});
        padding.low = std::ranges::any_of(values, [](math::Rational const& value) { return value.numerator < 0; });
        padding.high = std::ranges::any_of(values, [](math::Rational const& value) { return value.numerator > 0; });
    }
    std::uint32_t widest = 0;
    for (math::Rational const& value : values) {
        widest = std::max(widest, value.decimalPlaces.value);
    }
    ExprPtr const placesExpr = node.prop("yPlaces");
    std::int64_t const* const fixedPlaces = placesExpr ? placesExpr->literal.asInt() : nullptr;
    std::uint32_t const places = fixedPlaces != nullptr ? static_cast<std::uint32_t>(*fixedPlaces) : widest;
    std::optional<ChartRange> const computed = chartRange(axisValues, places, padding);
    if (!computed) {
        report(node.path, "the chart's values overflow its arithmetic; it draws nothing");
        return data;
    }
    ChartRange const range = chartFixedRange(node, scope, bindings, unit, *computed);
    data.yMin = realOf(range.low);
    data.yMax = realOf(range.high);
    // Three or more lines are the scale a reader reads the chart against; fewer leave the values without one.
    std::optional<ChartTicks> const ticks = lines.size() < 3 ? chartTicks(range) : std::nullopt;
    if (ticks) {
        std::uint32_t const tickPlaces = std::max(places, ticks->places);
        for (math::Rational const& tick : ticks->values) {
            data.yTicks.push_back(
                ChartTick{.value = realOf(tick), .text = detail::chartNumberText(tick, tickPlaces, format)});
        }
    }
    for (detail::ChartRawLine const& line : lines) {
        data.lines.push_back(ChartLine{
            .value = realOf(line.value),
            .label = line.label.value_or(detail::chartNumberText(line.value, places, format)),
            .tone = line.tone,
            .style = line.style,
        });
    }
    if (kinds.numbers && kinds.instants) {
        report(pointerAppend(node.path, "series"),
               "a chart's `x` values are all numbers or all dates and instants; the points are spaced by index");
    }
    chartSeries(data, node, scope, bindings, series, kinds.numbers != kinds.instants, range);
    return data;
}

std::optional<Tone> ScreenCore::rowToneOf(Value const& value, std::string const& path, bool& noted) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::optional<Tone> const tone = toneNamed(value)) {
        return tone;
    }
    if (!noted) {
        noted = true;
        report(path, "rowTone is " + canonicalJson(value) + "; a tone is neutral, info, ok, warn or err");
    }
    return std::nullopt;
}

std::vector<RowSpan> ScreenCore::rowSpansOf(Value const& value, std::vector<std::string> const& columns,
                                            std::string const& path, bool& noted) {
    std::vector<RowSpan> spans;
    if (value.isNull()) {
        return spans;
    }
    ValueList const* const items = value.asList();
    bool bad = items == nullptr;
    for (Value const& item : items != nullptr ? *items : ValueList{}) {
        Value const* const column = item.field("column");
        Value const* const rows = item.field("rows");
        std::string const* const columnId = column != nullptr ? column->asString() : nullptr;
        std::int64_t const* const count = rows != nullptr ? rows->asInt() : nullptr;
        auto const found = columnId != nullptr ? std::ranges::find(columns, *columnId) : columns.end();
        if (found == columns.end() || count == nullptr || *count < 1) {
            bad = true;
            continue;
        }
        spans.push_back(RowSpan{
            .column = static_cast<std::size_t>(found - columns.begin()),
            .rows = static_cast<std::size_t>(*count),
        });
    }
    if (bad && !noted) {
        noted = true;
        report(path,
               "rowSpans is " + canonicalJson(value) + "; a span is {column: a column's id, rows: an integer from 1}");
    }
    return spans;
}

void ScreenCore::ownQueriesOf(ExprPtr const& expr, RtScope const& scope, BoundaryRuntime& boundary,
                              std::set<std::string, std::less<>>& seen) {
    if (!expr) {
        return;
    }
    bool const control = expr->kind == ExprKind::Query || expr->kind == ExprKind::Pending ||
                         expr->kind == ExprKind::Error || expr->kind == ExprKind::ErrorKind ||
                         expr->kind == ExprKind::Refusal;
    if (control || expr->kind == ExprKind::Ref) {
        NameRef const* const found = scope.find(expr->name);
        if (found != nullptr && found->category == NameCategory::Query && seen.insert(expr->name).second) {
            boundary.queries.push_back(found->query);
        }
        if (expr->kind == ExprKind::Ref && found != nullptr && found->category == NameCategory::Let &&
            seen.insert("let:" + expr->name).second) {
            ownQueriesOf(found->letExpr, scope, boundary, seen);
        }
    }
    for (ExprPtr const& arg : expr->args) {
        ownQueriesOf(arg, scope, boundary, seen);
    }
    for (auto const& [name, field] : expr->fields) {
        static_cast<void>(name);
        ownQueriesOf(field, scope, boundary, seen);
    }
}

void ScreenCore::ownQueries(DocNode const& node, RtScope const& scope, BoundaryRuntime& boundary,
                            std::set<std::string, std::less<>>& seen) {
    if (node.kind == "boundary") {
        return;
    }
    for (auto const& [key, expr] : node.props) {
        static_cast<void>(key);
        ownQueriesOf(expr, scope, boundary, seen);
    }
    for (auto const& [key, nodes] : node.children) {
        static_cast<void>(key);
        for (DocNodePtr const& child : nodes) {
            ownQueries(*child, scope, boundary, seen);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            ownQueries(*part, scope, boundary, seen);
        }
    }
}

Node ScreenCore::lowerBoundary(DocNode const& node, LowerCtx const& ctx) {
    auto const boundary = std::make_shared<BoundaryRuntime>();
    std::set<std::string, std::less<>> seen;
    for (DocNodePtr const& child : node.nodes("children")) {
        ownQueries(*child, *ctx.scope, *boundary, seen);
    }
    auto const bindings = std::make_shared<Bindings>(*ctx.bindings);
    bindings->push_back(Binding{
        .name = "boundary",
        .read =
            [boundary] {
                for (QueryRuntime const* const query : boundary->queries) {
                    Value const error = query->error();
                    if (!error.isNull() && query->value().isNull()) {
                        return Value{ValueRecord{{"error", error}}};
                    }
                }
                return Value{ValueRecord{{"error", Value{}}}};
            },
    });
    LowerCtx const errorCtx{.scope = ctx.scope, .bindings = bindings, .boundary = boundary};
    LowerCtx const childCtx{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = boundary};
    auto const group = [this](std::span<DocNodePtr const> nodes, LowerCtx const& where) -> Node {
        return nodes.empty() ? ui::spacer() : lowerAll(nodes, where);
    };
    Switch spec;
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{0}}, .node = group(node.nodes("loading"), childCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{1}}, .node = group(node.nodes("error"), errorCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{2}}, .node = group(node.nodes("children"), childCtx)});
    spec.selector = slotProp<Key>([boundary] {
        bool loading = false;
        for (QueryRuntime const* const query : boundary->queries) {
            if (!query->settled()) {
                loading = true;
            } else if (query->value().isNull() && !query->error().isNull()) {
                return Key{std::int64_t{1}};
            }
        }
        return Key{std::int64_t{loading ? 0 : 2}};
    });
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

Node ScreenCore::lowerCustom(DocNode const& node, LowerCtx const& ctx) {
    std::string const name{node.name("name")};
    // A loaded document's custom node names a declared component, and every component has a fallback.
    Component const& component = _doc->components.at(name);
    std::vector<std::pair<std::string, ExprPtr>> props;
    for (DocNodePtr const& record : node.partList("props")) {
        for (auto const& [key, expr] : record->props) {
            props.emplace_back(key, expr);
        }
    }
    auto const bindings = std::make_shared<Bindings>();
    bindings->push_back(Binding{
        .name = "props",
        .read =
            [scope = ctx.scope, outer = ctx.bindings, props = std::move(props)] {
                ValueRecord record;
                for (auto const& [key, expr] : props) {
                    record.insert_or_assign(key, evalIn(*scope, outer.get(), expr));
                }
                return Value{std::move(record)};
            },
    });
    auto const scope = std::make_shared<RtScope>(*this, nullptr, nullptr, ctx.scope->nodes.depth() + 1);
    scope->formatSource = ctx.scope;
    scope->usePrefix = focusKeyOf(node.path, *ctx.scope) + ">";
    DocNode const& fallback = *component.fallback;
    declare(*scope, nullptr, {&fallback}, {});
    Node lowered = lower(fallback, LowerCtx{.scope = scope, .bindings = bindings, .boundary = nullptr});
    Custom spec{
        .name = name,
        .fallback = std::move(lowered),
        .props = {},
        .minVersion =
            static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
        .onEvent = {},
    };
    // Every declared prop is given, a missing one as its type's empty value: a component lacking one is refused.
    for (auto const& [prop, type] : component.props) {
        ExprPtr expr;
        for (DocNodePtr const& record : node.partList("props")) {
            if (ExprPtr const given = record->prop(prop)) {
                expr = given;
            }
        }
        if (type == "bool") {
            spec.props.push_back(CustomProp{.name = prop, .value = bindProp<bool>(ctx, expr, false, boolOf)});
        } else if (type == "number") {
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = bindProp<double>(
                    ctx, expr, 0.0, [](Value const& value, FormatContext const&) { return componentNumber(value); }),
            });
        } else {
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = bindProp<std::string>(ctx, expr, std::string{},
                                               [type](Value const& value, FormatContext const& format) {
                                                   return componentText(value, type, format);
                                               }),
            });
        }
    }
    spec.onEvent = [this, ctx, site = &node, declared = &component](std::string const& event,
                                                                    std::string const& payload) {
        Commands const* const commands = site->event("on." + event);
        if (commands == nullptr) {
            return;
        }
        auto const type = declared->events.find(event);
        runEvent(commands, ctx, componentPayload(payload, type == declared->events.end() ? nullptr : &type->second));
    };
    return ui::custom(std::move(spec));
}

namespace {

/// @brief One child of an arranged container, as the user's choice places it.
struct ArrangedChild {
    /// @brief The child's id.
    std::string id;
    /// @brief Whether the choice hides it.
    bool hidden = false;
};

/// @brief A container's children in the order a stored `arrange` choice gives, each marked shown or hidden.
///
/// A child the choice does not name keeps its declared place after the named ones, shown, and an entry that names no
/// child, names one twice or is not a `{"id", "hidden"}` record is skipped, so a new document version survives an old
/// choice (spec 5 §12b). A choice that is not a list leaves the declared order. Only `"hidden": true` hides.
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return Every child, in order.
[[nodiscard]] std::vector<ArrangedChild> arrangedChildren(Value const& stored,
                                                          std::span<DocNode const* const> declared) {
    auto const isDeclared = [declared](std::string const& childId) {
        return std::ranges::find(declared, childId, &DocNode::id) != declared.end();
    };
    std::vector<ArrangedChild> out;
    std::set<std::string, std::less<>> placed;
    if (ValueList const* const entries = stored.asList()) {
        for (Value const& entry : *entries) {
            Value const* const named = entry.field("id");
            std::string const* const name = named == nullptr ? nullptr : named->asString();
            if (name == nullptr || !isDeclared(*name) || !placed.insert(*name).second) {
                continue;
            }
            Value const* const hidden = entry.field("hidden");
            bool const* const flag = hidden == nullptr ? nullptr : hidden->asBool();
            out.push_back(ArrangedChild{.id = *name, .hidden = flag != nullptr && *flag});
        }
    }
    for (DocNode const* const child : declared) {
        if (!placed.contains(child->id)) {
            out.push_back(ArrangedChild{.id = child->id});
        }
    }
    return out;
}

/// @brief The ids of the children an `arrange` choice shows, in its order (`arrangedChildren`, without the hidden).
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return The ids to show, in order.
[[nodiscard]] std::vector<Value> arrangedOrder(Value const& stored, std::span<DocNode const* const> declared) {
    std::vector<Value> order;
    for (ArrangedChild const& child : arrangedChildren(stored, declared)) {
        if (!child.hidden) {
            order.emplace_back(child.id);
        }
    }
    return order;
}

/// @brief An arrangement as the choice an `arrange` point stores.
/// @param children Every child, in order.
/// @return `[{"id", "hidden"}, …]`.
[[nodiscard]] Value arrangementValue(std::vector<ArrangedChild> const& children) {
    ValueList entries;
    entries.reserve(children.size());
    for (ArrangedChild const& child : children) {
        entries.emplace_back(ValueRecord{{"id", Value{child.id}}, {"hidden", Value{child.hidden}}});
    }
    return Value{std::move(entries)};
}

/// @brief The element of a document tree with @p elementId.
/// @param node The subtree.
/// @param elementId The element's id.
/// @return The element, or `nullptr` when the subtree has none with that id.
// NOLINTNEXTLINE(misc-no-recursion): one level per level of the document tree.
[[nodiscard]] DocNode const* elementNode(DocNode const& node, std::string_view elementId) {
    if (node.id == elementId) {
        return &node;
    }
    for (auto const* const groups : {&node.children, &node.parts}) {
        for (auto const& [slot, nodes] : *groups) {
            static_cast<void>(slot);
            for (DocNodePtr const& child : nodes) {
                if (DocNode const* const found = elementNode(*child, elementId)) {
                    return found;
                }
            }
        }
    }
    return nullptr;
}
}  // namespace

std::string ScreenCore::arrangedBy(std::string const& elementId) const {
    if (elementId.empty()) {
        return {};
    }
    for (auto const& [name, point] : _doc->customize) {
        if (point.kind == "arrange" && point.of == elementId) {
            return name;
        }
    }
    return {};
}

// A keyed list over the children's ids rather than a stack, so a new choice moves the shown widgets, keeping their
// state and focus, and mounts or unmounts only what it shows or hides. A child is lowered in the container's own
// scope: arranging changes where it is, not what it reads.
Node ScreenCore::lowerArranged(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                               std::string const& point, Axis axis, int gap) {
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : node.nodes("children")) {
        declared.push_back(child.get());
    }
    auto order = [this, point, declared] { return arrangedOrder(prefs.get().at(point), declared); };
    auto key = [](Value const& childId) { return keyOf(childId); };
    LowerCtx childCtx = ctx;
    childCtx.listGap = axis == Axis::Vertical ? gap : 0;
    auto view = [this, childCtx, declared](reactive::Signal<Value> const& row) -> Node {
        // The order lists only declared ids, so the lookup always finds the child.
        auto const found = std::ranges::find(declared, textOf(row.peek(), session.formatting()), &DocNode::id);
        return lower(**found, childCtx);
    };
    ForEach spec{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(order), std::move(key), std::move(view)),
        .axis = axis,
        .gap = gap,
    };
    commonOf(node, ctx, element, spec.common);
    return makeNode(std::move(spec));
}

// One row per child of the arranged container, hidden ones included: a checkbox shows or hides it, and two buttons
// move it a place. Each edit stores the whole arrangement, so the stored list always names every child.
Node ScreenCore::lowerArrangeEditor(DocNode const& node, LowerCtx const& ctx, std::string const& point,
                                    CustomizePoint const& spec, std::function<void(Value)> const& choose) {
    // The loader checked that `of` names a column or row of this screen.
    DocNode const* const container = elementNode(*_doc->view, spec.of);
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : container->nodes("children")) {
        declared.push_back(child.get());
    }
    auto const current = [this, point, declared] { return arrangedChildren(prefs.get().at(point), declared); };
    auto const place = [current](std::string const& childId) {
        auto const children = current();
        return static_cast<std::size_t>(std::ranges::find(children, childId, &ArrangedChild::id) - children.begin());
    };
    // Applies an edit to the arrangement as it stands, read untracked: the edit is a transition, not a binding.
    auto const edit = [this, current, choose](std::string const& childId, auto const& change) {
        auto children = runtime().untracked(current);
        auto const found = std::ranges::find(children, childId, &ArrangedChild::id);
        change(children, static_cast<std::size_t>(found - children.begin()));
        choose(arrangementValue(children));
    };
    auto rows = [current] {
        std::vector<Value> ids;
        for (ArrangedChild const& child : current()) {
            ids.emplace_back(child.id);
        }
        return ids;
    };
    auto key = [](Value const& childId) { return keyOf(childId); };
    auto view = [this, current, place, edit](reactive::Signal<Value> const& row) -> Node {
        std::string const childId = textOf(row.peek(), session.formatting());
        Checkbox shown{.label = childId};
        shown.checked = slotProp<bool>([current, childId] {
            auto const children = current();
            return !std::ranges::find(children, childId, &ArrangedChild::id)->hidden;
        });
        shown.onToggle = [edit, childId](bool checked) {
            edit(childId, [checked](std::vector<ArrangedChild>& children, std::size_t index) {
                children.at(index).hidden = !checked;
            });
        };
        Button moveUp{.label = std::string{"\xE2\x86\x91"}};
        moveUp.onClick = [edit, childId] {
            // Clamped rather than refused: at the edge the child swaps with itself, which stores the same order.
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::max(index, std::size_t{1}) - 1));
            });
        };
        moveUp.common.enabled = slotProp<bool>([place, childId] { return place(childId) > 0; });
        moveUp.common.a11y.name = "Move " + childId + " up";
        moveUp.common.testId = "arrange-up-" + childId;
        Button moveDown{.label = std::string{"\xE2\x86\x93"}};
        moveDown.onClick = [edit, childId] {
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::min(index + 1, children.size() - 1)));
            });
        };
        moveDown.common.enabled =
            slotProp<bool>([current, place, childId] { return place(childId) + 1 < current().size(); });
        moveDown.common.a11y.name = "Move " + childId + " down";
        moveDown.common.testId = "arrange-down-" + childId;
        shown.common.testId = "arrange-shown-" + childId;
        return ui::row(Row{
            .children =
                {
                    ui::checkbox(std::move(shown)),
                    ui::button(std::move(moveUp)),
                    ui::button(std::move(moveDown)),
                },
            .gap = 1,
        });
    };
    ForEach editor{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(rows), std::move(key), std::move(view)),
    };
    commonOf(node, ctx, nullptr, editor.common);
    return makeNode(std::move(editor));
}

Node ScreenCore::lowerCustomize(DocNode const& node, LowerCtx const& ctx) {
    std::string const point{node.name("point")};
    // A loaded document's customize node names a declared point, and every point has a choice in `prefs`.
    CustomizePoint const& spec = _doc->customize.at(point);
    auto const choose = [this, point](Value value) {
        session.asUser([&] {
            ValueRecord next = prefs.peek();
            next.insert_or_assign(point, value);
            prefs.set(std::move(next));
        });
        if (host != nullptr) {
            host->preferenceChanged(_doc->id, point, value);
        }
    };
    if (spec.kind == "arrange") {
        return lowerArrangeEditor(node, ctx, point, spec, choose);
    }
    if (spec.kind == "choice") {
        Select select;
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (std::string const& option : spec.options) {
            options.push_back(SelectOption{.key = Key{option}, .label = option});
        }
        select.options = std::move(options);
        select.selected = slotProp<std::optional<Key>>([this, point] -> std::optional<Key> {
            Value const& chosen = prefs.get().at(point);
            return chosen.isNull() ? std::nullopt : std::optional<Key>{keyOf(chosen)};
        });
        select.onSelect = [choose](Key const& key) { choose(valueOfKey(key)); };
        commonOf(node, ctx, nullptr, select.common);
        return ui::select(std::move(select));
    }
    if (spec.kind == "value" && (spec.type.kind == TypeKind::String || spec.type.isAny())) {
        TextInput input;
        input.value =
            slotProp<std::string>([this, point] { return textOf(prefs.get().at(point), session.formatting()); });
        input.onChange = [choose](std::string text) { choose(Value{std::move(text)}); };
        commonOf(node, ctx, nullptr, input.common);
        return ui::textInput(std::move(input));
    }
    Text placeholder{.text = point, .role = TextRole::Muted};
    commonOf(node, ctx, nullptr, placeholder.common);
    return ui::text(std::move(placeholder));
}
// NOLINTEND(misc-no-recursion)

}  // namespace detail

std::vector<TextSpan> Screen::headSpans(ExprPtr const& expr) const {
    if (expr == nullptr) {
        return {};
    }
    bool dropped = false;
    bool badTone = false;
    std::vector<TextSpan> spans =
        detail::spansOf(detail::evalIn(*_scope, nullptr, expr), _scope->formatting(), dropped, &badTone);
    if (dropped) {
        _core->report(expr->path, detail::kSpansReason);
    }
    if (badTone) {
        _core->report(expr->path, detail::kSpanToneReason);
    }
    return spans;
}

DocNode const* Screen::shownRoot() const {
    DocNode const* node = _core->doc().view.get();
    while (node != nullptr && node->kind == "switch") {
        Value const value = detail::evalIn(*_scope, nullptr, node->prop("on"));
        std::span<DocNodePtr const> shown = node->nodes("default");
        for (DocNodePtr const& part : node->partList("cases")) {
            if (!(detail::evalIn(*_scope, nullptr, part->prop("when")) == value)) {
                continue;
            }
            if (part->scope.has_value() && detail::declaresNames(*part->scope)) {
                return nullptr;
            }
            shown = part->nodes("children");
            break;
        }
        node = shown.size() == 1 ? shown.front().get() : nullptr;
    }
    return node;
}

bool Screen::pageEdge() const {
    DocNode const* const node = shownRoot();
    return node != nullptr && node->flag("pageEdge");
}

bool Screen::headJoined() const {
    DocNode const* const node = shownRoot();
    return node != nullptr && node->flag("headJoined");
}

LeadPlacement Screen::leadPlacement() const {
    ExprPtr const& placement = _core->doc().leadPlacement;
    if (placement == nullptr) {
        return LeadPlacement::Below;
    }
    Value const value = detail::evalIn(*_scope, nullptr, placement);
    if (std::string const* const name = value.asString()) {
        if (*name == "inline") {
            return LeadPlacement::Inline;
        }
        if (*name == "end") {
            return LeadPlacement::End;
        }
        if (*name == "below") {
            return LeadPlacement::Below;
        }
    }
    if (!value.isNull()) {
        _core->report(placement->path, "`leadPlacement` is `below`, `inline` or `end`");
    }
    return LeadPlacement::Below;
}

TextRole Screen::titleRole() const {
    ExprPtr const& role = _core->doc().titleRole;
    if (role == nullptr) {
        return TextRole::Heading;
    }
    Value const value = detail::evalIn(*_scope, nullptr, role);
    if (std::string const* const name = value.asString()) {
        if (*name == "headline") {
            return TextRole::Headline;
        }
        if (*name == "heading") {
            return TextRole::Heading;
        }
    }
    if (!value.isNull()) {
        _core->report(role->path, "`titleRole` is `heading` or `headline`");
    }
    return TextRole::Heading;
}

std::string Screen::headSurface() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().headSurface);
    std::string const* const name = value.asString();
    return name == nullptr ? std::string{} : *name;
}

std::string Screen::pageSurface() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().pageSurface);
    std::string const* const name = value.asString();
    return name == nullptr ? std::string{} : *name;
}

std::vector<TextSpan> Screen::subtitleSpans() const { return headSpans(_core->doc().subtitleSpans); }

std::vector<TextSpan> Screen::titleSpans() const { return headSpans(_core->doc().titleSpans); }

std::vector<TextSpan> Screen::eyebrowSpans() const { return headSpans(_core->doc().eyebrowSpans); }

BorrowedModel Screen::lend(std::string_view alias) const {
    detail::NameRef const* const found = _scope->find(alias);
    if (found == nullptr || found->category != detail::NameCategory::Model) {
        return {};
    }
    detail::ModelAlias& model = *found->model;
    return BorrowedModel{
        .channel = detail::channelOf(model, *_core),
        .token = _core->runtime().untracked([&] { return model.token->get(); }),
    };
}

std::expected<std::unique_ptr<Screen>, Refusal> Interpreter::mount(std::shared_ptr<Document const> doc,
                                                                   ValueRecord const& params, IViewBackend& backend,
                                                                   MountOptions const& options) {
    auto const refuse = [&](std::string path, std::string reason) {
        return std::unexpected(Refusal{.document = doc->id, .path = std::move(path), .reason = std::move(reason)});
    };
    if (!doc->view) {
        return refuse("", "the document has no view");
    }
    ValueRecord given;
    for (auto const& [name, type] : doc->params) {
        auto const found = params.find(name);
        Value value = found == params.end() ? Value{} : found->second;
        if (!type.admits(value)) {
            return refuse(pointerAppend("/params", name), value.isNull()
                                                              ? "param `" + name + "` is missing"
                                                              : "param `" + name + "` is not a " + type.name());
        }
        given.insert_or_assign(name, std::move(value));
    }
    for (ModelDecl const& model : doc->scope.models) {
        if (model.instance == InstanceMode::Borrowed && !options.borrowed.contains(model.name)) {
            return refuse(model.path, "alias `" + model.name + "` borrows a model its opener did not lend");
        }
    }
    reactive::Runtime& runtime = _session->runtime();
    std::unique_ptr<Screen> screen{new Screen{}};
    screen->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, options);
    detail::ScreenCore& core = *screen->_core;
    if (options.keepStateOf != nullptr) {
        core.keptStates = options.keepStateOf->ownState();
    }
    screen->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, options.depth);
    core.rootScope = screen->_scope;
    detail::RtScope& scope = *screen->_scope;
    scope.submitted = &core.submitted;
    scope.showErrors = doc->showErrors;
    for (auto const& [name, value] : given) {
        detail::NameRef ref;
        ref.category = detail::NameCategory::Param;
        ref.param = value;
        scope.names.insert_or_assign(name, std::move(ref));
    }
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    for (auto const& [chord, commands] : doc->keys) {
        static_cast<void>(chord);
        lists.push_back(&commands);
    }
    detail::headCommandLists(*doc, lists);
    Node root = runtime.untracked([&] {
        core.declare(scope, &doc->scope, {doc->view.get()}, lists);
        detail::LowerCtx const ctx{
            .scope = screen->_scope,
            .bindings = std::make_shared<detail::Bindings const>(),
            .boundary = nullptr,
        };
        Node lowered = core.lower(*doc->view, ctx);
        screen->_headStatus = core.lowerBadgeParts(doc->status, ctx);
        screen->_headActions = core.lowerActionParts(doc->actions, ctx);
        screen->_headMenu = core.lowerHeadMenu(doc->menu, ctx);
        screen->_headTools = core.lowerHeadTools(doc->tools, ctx);
        if (!doc->keys.empty()) {
            std::vector<KeyBinding> keys;
            for (auto const& [chord, commands] : doc->keys) {
                keys.push_back(KeyBinding{
                    .chord = chord,
                    .onPress = [&core, ctx, list = &commands] { core.runEvent(list, ctx, Value{}); },
                });
            }
            Column column{.children = {std::move(lowered)}};
            column.common.keys = std::move(keys);
            lowered = ui::column(std::move(column));
        }
        return lowered;
    });
    screen->_mounted = std::make_unique<Mounted>(runtime, backend, std::move(root), options.parent, options.depth + 1);
    screen->_core->focusElement = [mounted = screen->_mounted.get()](std::string_view element) {
        return mounted->focus(element);
    };
    screen->runLifecycle("onMount");
    return screen;
}

std::expected<std::unique_ptr<AppRoot>, Refusal> Interpreter::mountApp(std::shared_ptr<Document const> doc) {
    if (doc->id != "app") {
        return std::unexpected(
            Refusal{.document = doc->id, .path = "/id", .reason = "the app document's id is `app`"});
    }
    if (_session->appScope() != nullptr) {
        return std::unexpected(
            Refusal{.document = doc->id, .path = {}, .reason = "an app document is already mounted on the session"});
    }
    std::unique_ptr<AppRoot> root{new AppRoot{}};
    root->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, MountOptions{});
    detail::ScreenCore& core = *root->_core;
    root->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, 0);
    core.rootScope = root->_scope;
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    _session->runtime().untracked([&] {
        core.declare(*root->_scope, &doc->scope, {}, lists);
        detail::LowerCtx const ctx{
            .scope = root->_scope,
            .bindings = std::make_shared<detail::Bindings const>(),
            .boundary = nullptr,
        };
        std::vector<detail::BadgeModel> badges = core.lowerBadgeParts(doc->navigation, ctx);
        for (std::size_t index = 0; index < badges.size(); ++index) {
            root->_navigation.push_back(AppRoot::PlacedBadge{
                .screen = std::string{doc->navigation.at(index)->name("screen")},
                .badge = std::move(badges.at(index)),
            });
        }
    });
    root->_navigationBadges = std::make_unique<reactive::Computed<std::vector<NavigationBadge>>>(
        _session->runtime(), [placed = &root->_navigation] {
            std::vector<NavigationBadge> shown;
            for (AppRoot::PlacedBadge const& entry : *placed) {
                if (entry.badge.visible.evaluate()) {
                    shown.push_back(NavigationBadge{
                        .screen = entry.screen,
                        .text = entry.badge.text.evaluate(),
                        .tone = entry.badge.tone.evaluate(),
                    });
                }
            }
            return shown;
        });
    // The app document's date patterns are the application's: every screen of the bundle shows and reads dates so.
    if (!doc->datePattern.empty() || !doc->dateTimePattern.empty()) {
        _session->setDatePatterns(doc->datePattern, doc->dateTimePattern);
    }
    _session->setAppScope(root->_scope.get());
    if (auto const found = doc->lifecycle.find("onMount"); found != doc->lifecycle.end()) {
        root->runCommands(found->second);
    }
    return root;
}

}  // namespace morph::ui
