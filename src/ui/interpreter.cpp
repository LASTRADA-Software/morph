// SPDX-License-Identifier: Apache-2.0

// The interpreter's core: sessions, scopes, queries and mutations, and mounting.

#include "interpreter_detail.hpp"

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

namespace {

// The shortest text that reads back as @p number, in fixed notation: `0.0001`, never `1e-04`, so that every value
// a decimal can hold is read as one. The buffer holds the longest fixed form of a double (about 330 characters).
Value decimalOrText(double number) {
    std::array<char, 512> buffer{};
    auto const [end, error] =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): `buffer.data() + buffer.size()` is the end pointer `to_chars` takes
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), number, std::chars_format::fixed);
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

[[nodiscard]] Key keyOf(Value const& value) {
    if (std::int64_t const* const number = value.asInt()) {
        return Key{*number};
    }
    if (std::string const* const text = value.asString()) {
        return Key{*text};
    }
    return Key{canonicalJson(value)};
}

[[nodiscard]] Value valueOfKey(Key const& key) {
    if (std::int64_t const* const number = std::get_if<std::int64_t>(&key)) {
        return Value{*number};
    }
    return Value{std::get<std::string>(key)};
}

[[nodiscard]] Value cellRangeValue(std::optional<CellRange> const& range) {
    if (!range) {
        return Value{};
    }
    auto const corner = [](CellRef const& cell) {
        return Value{ValueRecord{{"key", valueOfKey(cell.key)}, {"column", Value{cell.column}}}};
    };
    return Value{ValueRecord{{"anchor", corner(range->anchor)}, {"focus", corner(range->focus)}}};
}

[[nodiscard]] std::optional<CellRange> cellRangeOf(Value const& value) {
    auto const corner = [&value](std::string_view name) -> std::optional<CellRef> {
        Value const* const part = value.field(name);
        Value const* const key = part == nullptr ? nullptr : part->field("key");
        Value const* const column = part == nullptr ? nullptr : part->field("column");
        if (key == nullptr || key->isNull() || column == nullptr || column->asString() == nullptr) {
            return std::nullopt;
        }
        return CellRef{.key = keyOf(*key), .column = *column->asString()};
    };
    std::optional<CellRef> anchor = corner("anchor");
    std::optional<CellRef> focus = corner("focus");
    if (!anchor || !focus) {
        return std::nullopt;
    }
    return CellRange{.anchor = std::move(*anchor), .focus = std::move(*focus)};
}

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

[[nodiscard]] Value listOfStrings(std::vector<std::string> const& texts) {
    ValueList items;
    items.reserve(texts.size());
    for (std::string const& text : texts) {
        items.emplace_back(text);
    }
    return Value{std::move(items)};
}

[[nodiscard]] CommandCtx detached(CommandCtx const& ctx) {
    CommandCtx kept = ctx;
    kept.scope = nullptr;
    return kept;
}

namespace {

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

}  // namespace

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

[[nodiscard]] KeySpec const* keySpec(KindSpec const& kind, std::string_view key) {
    for (KeySpec const& spec : kind.keys) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] bool statefulKind(std::string_view kind) {
    static constexpr std::array<std::string_view, 12> kStateful = {
        "textInput", "checkbox", "select", "dateTimeInput", "slider",     "filePicker",
        "dialog",    "drawer",   "table",  "form",          "collection", "wizard",
    };
    return listed(kStateful, kind);
}

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

[[nodiscard]] Value rowId(Value const& row) {
    Value const* const found = row.field("id");
    return found == nullptr ? Value{} : *found;
}

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
            // NOLINTNEXTLINE(misc-no-recursion): recurses into the nested items of a value, one level per nesting; the depth is the data's
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
    return std::ranges::any_of(paths, [&](std::string const& path) { return memberSetAt(value, pathMembers(path)); });
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

bool Session::hostNameIn(std::string_view name,  // NOLINT(bugprone-easily-swappable-parameters)
                         std::string_view document) const {
    static_cast<void>(_hostScopesRevision.get());
    auto const found = _hostScopes.find(name);
    return found == _hostScopes.end() || found->second.contains(document);
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
    auto value =
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a three-way pick: the opaque rule, the strict rule or the parse error
        json ? (opaque ? detail::tableValueFromJson(*json) : valueFromJson(*json)) : std::unexpected(json.error());
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
    // A point that names a model-backed table edits the table's state, which then says what the point reads.
    if (std::optional<bool> const taken = _core->tablePoint(found->second, value)) {
        if (!*taken) {
            return false;
        }
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
    _tours.reset();
    _runtimeTours.reset();
    _navigationBadges.reset();
    _navigation.clear();
    _core->session.setAppScope(nullptr);
    _scope.reset();
}

reactive::Computed<std::vector<NavigationBadge>> const& AppRoot::navigationBadges() const {
    return *_navigationBadges;
}

reactive::Computed<std::vector<Tour>> const& AppRoot::tours() const { return *_tours; }

void AppRoot::setRuntimeTours(std::vector<TourDecl> tours) {
    _runtimeTours->set(std::make_shared<std::vector<TourDecl> const>(std::move(tours)));
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
    RtScope const* owner = _scope;
    while (owner != nullptr && owner->row == nullptr) {
        owner = owner->parent.get();
    }
    if (owner == nullptr) {
        return Value{};
    }
    Value const& current = owner->row->get();
    // A server-mode row holds the columns fetched: a field it lacks is named, to be fetched after this binding.
    if (owner->onMissingRowField != nullptr && !path.empty() && current.asRecord() != nullptr &&
        current.field(path.front()) == nullptr) {
        (*owner->onMissingRowField)(path.front());
    }
    return walk(current, path);
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
    // The cache's own topics need no answer: a refused one simply sends no events. An extra watch belongs to a
    // client that cannot work without it, so a refusal of one is reported.
    std::vector<std::string> extras;
    if (kind == wire::kWatchKind) {
        std::ranges::copy_if(body.records, std::back_inserter(extras),
                             [this](std::string const& record) { return _extraWatch.contains(record); });
    }
    auto completion = _options.watcher(std::string{kind}, glz::write_json(body).value_or(std::string{}));
    // A watch's records are in flight until its answer: a `watchRecord` for one of them resolves then.
    std::weak_ptr<detail::WatchRegistry> const weak = _watchRegistry;
    std::vector<std::string> records;
    if (kind == wire::kWatchKind) {
        records = body.records;
        if (_watchRegistry) {
            _watchRegistry->inFlight.insert(records.begin(), records.end());
        }
    }
    auto const answered = [weak, records = std::move(records)] {
        if (auto const registry = weak.lock(); registry && !records.empty()) {
            registry->answered(records);
        }
    };
    completion
        .then([extras, answered](std::string const& reply) {
            auto const read = wire::readWatchReply(reply);
            for (std::string const& record : extras) {
                if (!read || std::ranges::find(read->refusedRecords, record) != read->refusedRecords.end()) {
                    ::morph::log::logWarn("the server refused the watch on `{}`; events for it will not arrive",
                                          record);
                }
            }
            answered();
        })
        .onError([extras, answered](std::exception_ptr const& error) {
            for (std::string const& record : extras) {
                ::morph::log::logWarn("the watch on `{}` failed: {}", record, detail::failureText(error));
            }
            answered();
        });
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
    // An extra watch outlives the cache's last entry on the record.
    std::erase_if(_unwatchRecords, [this](std::string const& record) { return _extraWatch.contains(record); });
    if (!_unwatchRecords.empty() || !_unwatchInstances.empty()) {
        sendWatch(wire::kUnwatchKind, take(_unwatchRecords, _unwatchInstances));
    }
}

void Session::addExtraWatch(std::string const& record) {
    if (!_extraWatch.insert(record).second || !_options.watcher) {
        return;
    }
    sendWatch(wire::kWatchKind, wire::WatchBody{.records = {record}, .instances = {}, .replace = false});
}

void Session::removeExtraWatch(std::string const& record) {
    if (_extraWatch.erase(record) == 0 || !_options.watcher || _recordRefs.contains(record)) {
        return;
    }
    sendWatch(wire::kUnwatchKind, wire::WatchBody{.records = {record}, .instances = {}, .replace = false});
}

wire::WatchBody Session::watchSet() const {
    wire::WatchBody body;
    std::set<std::string, std::less<>> records = _extraWatch;
    for (auto const& [record, refs] : _recordRefs) {
        static_cast<void>(refs);
        records.insert(record);
    }
    body.records.assign(records.begin(), records.end());
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
        for (auto const& [watchId, watch] : _watchRegistry->watches) {
            static_cast<void>(watchId);
            records.insert(watch.record);
        }
        for (std::string const& record : records) {
            notifyWatchers(effects::RecordChange{.record = record, .keys = std::nullopt});
        }
    }
    std::erase_if(_reconnectListeners, [](auto const& listener) { return listener.expired(); });
    std::vector<std::shared_ptr<std::function<void()>>> listeners;
    for (auto const& listener : _reconnectListeners) {
        if (auto held = listener.lock()) {
            listeners.push_back(std::move(held));
        }
    }
    for (auto const& listener : listeners) {
        (*listener)();
    }
}

std::shared_ptr<void> Session::onReconnected(std::function<void()> handler) {
    auto held = std::make_shared<std::function<void()>>(std::move(handler));
    std::erase_if(_reconnectListeners, [](auto const& listener) { return listener.expired(); });
    _reconnectListeners.push_back(held);
    return held;
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
    for (auto const& [watchId, watch] : _watchRegistry->watches) {
        static_cast<void>(watchId);
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
    auto const watchId = ++registry.next;
    registry.watches.emplace(watchId, detail::WatchRegistry::Watch{.record = record, .handler = std::move(handler)});
    auto registration = std::make_shared<detail::WatchRegistration>();
    registration->registry = _watchRegistry;
    registration->id = watchId;
    registration->record = record;
    WatchHolder holder{std::move(registration)};
    countRecord(record, +1);
    flushWatches();
    auto [completion, promise] = async::Completion<WatchHolder>::makeSettleable(&_runtime->owner());
    if (registry.inFlight.contains(record)) {
        registry.waiters.push_back(detail::WatchRegistry::Waiter{
            .record = record, .promise = std::move(promise), .holder = std::move(holder)});
    } else {
        promise.resolve(std::move(holder));
    }
    return std::move(completion);
}

async::Completion<std::string> Session::read(std::shared_ptr<ModelChannel> const& channel, std::string const& actionId,
                                             Value const& body, ::core::async::StopToken const& stop) {
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
        onStop =
            std::make_shared<OnStop>(stop, std::function<void()>{[settle] {
                                         settle->reject(std::make_exception_ptr(::core::async::OperationCancelled{}));
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

RtScope::RtScope(ScreenCore& screen, std::shared_ptr<RtScope> enclosing, reactive::Signal<Value> const* current,
                 std::size_t depth)
    : core{screen}, parent{std::move(enclosing)}, row{current}, nodes{screen.runtime(), depth} {
    if (core.recording) {
        core.scopes.push_back(this);
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
        static constexpr std::string_view kHostPrefix = "host:";
        std::string_view action = name;
        if (found != nullptr && found->category == NameCategory::Query) {
            action = found->query->decl().action;
        } else if (found != nullptr && found->category == NameCategory::Mutation) {
            action = found->mutation->decl().action;
        }
        // A query or mutation whose action is a host command is held to the same document scope as the command.
        if (action.starts_with(kHostPrefix) &&
            !session.hostNameIn(action.substr(kHostPrefix.size()), _scope->core.doc().id)) {
            return Value{false};
        }
        return session.can(action);
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

Value ScopeEnv::client(ExprKind form, std::string_view name) const {
    Session const& session = _scope->core.session;
    // A host value limited to other documents reads as one nobody registered.
    if (form == ExprKind::Host && !session.hostNameIn(name, _scope->core.doc().id)) {
        return Value{};
    }
    return session.client(form, name);
}

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

}  // namespace detail

// --- Rows an action works on ----------------------------------------------------------------------------------

std::vector<TableInput> tableInputsOf(std::string_view schemaJson) {
    std::vector<TableInput> out;
    glz::generic_u64 schema;
    if (glz::read_json(schema, schemaJson) || !schema.is_object()) {
        return out;
    }
    auto const& members = schema.get_object();
    // An edit action is itself the marked type: its keyword sits at the root, and its row is its `key`.
    for (std::string_view const keyword : {std::string_view{"x-cellEditOf"}, std::string_view{"x-rowEditOf"}}) {
        const auto* const found = members.find(std::string{keyword});
        if (found != members.end() && found->second.is_string()) {
            out.push_back(
                TableInput{.member = "key", .record = found->second.get_string(), .kind = TableInputKind::CellEdit});
        }
    }
    const auto* const properties = members.find("properties");
    if (properties == members.end() || !properties->second.is_object()) {
        return out;
    }
    for (auto const& [name, property] : properties->second.get_object()) {
        if (!property.is_object()) {
            continue;
        }
        auto const& keywords = property.get_object();
        for (auto const& [keyword, kind] : {std::pair{std::string_view{"x-selectionOf"}, TableInputKind::Selection},
                                            std::pair{std::string_view{"x-rowOf"}, TableInputKind::Row}}) {
            const auto* const found = keywords.find(std::string{keyword});
            if (found != keywords.end() && found->second.is_string()) {
                out.push_back(TableInput{.member = name, .record = found->second.get_string(), .kind = kind});
            }
        }
    }
    return out;
}

namespace detail {

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
    run->sessionRun = _core->session.nextRunId();
    for (RunObserver* const observer : _core->session.runObservers()) {
        observer->started(RunStarted{.run = run->sessionRun, .action = _decl->action, .body = run->body});
    }
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
    if (auto const observers = session.runObservers(); !observers.empty()) {
        std::string code;
        if (!succeeded) {
            Value const refusal = detail::refusalValue(error);
            if (Value const* const named = refusal.field("code"); named != nullptr && named->asString() != nullptr) {
                code = *named->asString();
            }
        }
        // The rerun holds the mutation weakly: it may be chosen after the screen that ran it is gone.
        std::weak_ptr<bool> const alive = _alive;
        auto const rerun = [this, alive, bindings = run->bindings](Value body) {
            if (!alive.expired()) {
                this->run(std::move(body), bindings, [](bool, Value const&) {});
            }
        };
        Value const replied = reply ? *reply : Value{};
        for (RunObserver* const observer : observers) {
            observer->settled(RunSettled{.run = run->sessionRun,
                                         .action = _decl->action,
                                         .body = run->body,
                                         .succeeded = succeeded,
                                         .reply = replied,
                                         .refusalCode = code,
                                         .rerun = rerun});
        }
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

}  // namespace morph::ui
