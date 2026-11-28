// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/form_session.hpp
/// @brief `FormSession`: the reactive state of one form over a `FormModel` — drafts, encoded
///        values, readiness, presentation rules, Choice options and submission. A controller in
///        the sense of `docs/spec/reactive/control.md`: no toolkit, testable headless.
///
/// Specified in `docs/spec/forms/engine.md`, "FormSession".

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../reactive/control.hpp"
#include "../i18n.hpp"
#include "values.hpp"

namespace morph::forms {

/// @brief Executes a form's action: the action type and its JSON body in, the JSON reply out.
using Submitter = std::function<async::Completion<std::string>(std::string_view actionType, std::string bodyJson)>;

/// @brief Executes a Choice's options action: the action and its JSON request in, the reply out.
using ChoiceFetcher =
    std::function<async::Completion<std::string>(std::string_view optionsAction, std::string bodyJson)>;

/// @brief How a session reads numbers, dates and text.
struct FormSessionOptions {
    /// @brief How numbers are typed and shown. It holds views: the strings it names must outlive the session.
    render::NumericLocale locale{};
    /// @brief The locale translations are resolved in.
    std::string bcp47 = "en";
    /// @brief The host's catalog; empty means every label is the schema's literal.
    render::TranslationProvider translations;
    /// @brief The display zone for date-times, in minutes east of UTC.
    int displayOffsetMinutes = 0;
    /// @brief The pattern a date field shows and reads (`ui::checkDatePattern`), such as `dd.MM.yyyy`; empty keeps
    /// ISO.
    ///        Only the input follows it: the draft text, and so the stored value, stays ISO.
    std::string datePattern;
    /// @brief The pattern a date-time field shows and reads, such as `dd.MM.yyyy HH:mm`; empty keeps ISO.
    std::string dateTimePattern;
    /// @brief An extra readiness gate over the assembled body; empty means none. `Form<A>` puts
    ///        the action's own `validate()` here.
    std::function<bool(std::string_view bodyJson)> accepts;
};

class FieldState;
class RowState;

namespace detail::engine {

/// @brief What every field of one session shares: the runtime, the model, the options, the
///        Choice fetcher, and the hook a programmatic change ends with.
class MORPH_API SessionCore {
public:
    /// @param runtime The runtime. Borrowed.
    /// @param model   The form model.
    /// @param choices The Choice fetcher (may be empty when no field is a Choice).
    /// @param options The session options.
    SessionCore(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, FormModel model, ChoiceFetcher choices,
                FormSessionOptions options)
        : _rt{&runtime},
          _model{std::move(model)},
          _choices{std::move(choices)},
          _options{std::move(options)},
          _values{
              .locale = _options.locale,
              .displayOffsetMinutes = _options.displayOffsetMinutes,
              .translations = _options.translations,
              .bcp47 = _options.bcp47,
          },
          _format{runtime, std::uint64_t{0}},
          _texts{runtime, std::uint64_t{0}} {}

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return *_rt; }

    /// @brief The form model.
    /// @return The model.
    [[nodiscard]] FormModel const& model() const noexcept { return _model; }

    /// @brief The session options.
    /// @return The options.
    [[nodiscard]] FormSessionOptions const& options() const noexcept { return _options; }

    /// @brief The options a value conversion reads. Tracked: a change of the number format (`setFormat`) or of the
    ///        translations (`setTranslations`) reruns what read them.
    /// @return The value context.
    [[nodiscard]] ValueContext const& values() const;

    /// @brief The pattern a date or date-time input shows and reads. Tracked.
    /// @param date `true` for the date pattern, `false` for the date-time pattern.
    /// @return The pattern; empty keeps ISO.
    [[nodiscard]] std::string const& pattern(bool date) const;

    /// @brief Replaces the number locale and the date patterns every field reads. Drafts keep their text; the
    ///        fields check it again against the new format.
    /// @param locale          The number format. Its views must outlive the session or the next call.
    /// @param datePattern     The date pattern.
    /// @param dateTimePattern The date-time pattern.
    void setFormat(render::NumericLocale locale, std::string datePattern, std::string dateTimePattern);

    /// @brief Replaces the translations and the locale every field text is resolved in, and reruns what read one.
    /// @param translations The catalog; empty shows every schema literal.
    /// @param bcp47        The locale.
    void setTranslations(render::TranslationProvider translations, std::string bcp47);

    /// @brief Records the translations and the locale as a dependency of the running computation, so a text it
    ///        resolves follows `setTranslations`.
    void trackTexts() const { static_cast<void>(_texts.get()); }

    /// @brief The Choice fetcher.
    /// @return The fetcher; may be empty.
    [[nodiscard]] ChoiceFetcher const& choices() const noexcept { return _choices; }

    /// @brief Installs what `markProgrammatic` runs (the session records its current body as seen).
    /// @param hook The hook.
    void onProgrammatic(std::function<void()> hook) { _programmatic = std::move(hook); }

    /// @brief Ends a programmatic change: automatic submission does not submit the body it produced.
    void markProgrammatic() const;

    /// @brief Installs the top-level fields rules look up.
    /// @param fields The session's fields. Borrowed: they must outlive this core's use.
    void adopt(std::vector<std::unique_ptr<FieldState>> const* fields) noexcept { _fields = fields; }

    /// @brief A top-level field by wire name.
    /// @param name The wire name.
    /// @return The field, or null.
    [[nodiscard]] FieldState* topLevel(std::string_view name) const;

    /// @brief The tracked lookup rules evaluate over.
    /// @return A lookup reading each named field's encoding.
    [[nodiscard]] RuleLookup lookup() const;

    /// @brief Fetches a Choice's options through the fetcher, once per `(optionsAction, request)` for the session's
    ///        life: a second ask for a pair joins the fetch in flight or takes its reply. A failed fetch is not kept,
    ///        so the next ask fetches again.
    /// @param action  The options action.
    /// @param request The request body.
    /// @return The reply, delivered on the runtime's owner.
    [[nodiscard]] async::Completion<std::string> sharedChoices(std::string const& action, std::string request);

private:
    // One shared options fetch: its reply once it arrived, and who waits for it until then.
    struct SharedFetch {
        std::optional<std::string> reply;
        std::vector<async::Completion<std::string>::Promise> waiting;
    };
    using Fetches = std::map<std::string, std::shared_ptr<SharedFetch>, std::less<>>;

    reactive::Runtime* _rt;
    FormModel _model;
    ChoiceFetcher _choices;
    FormSessionOptions _options;
    ValueContext _values;
    reactive::Signal<std::uint64_t> _format;
    // Bumped by setTranslations: what resolved a text through the translations reads it.
    reactive::Signal<std::uint64_t> _texts;
    std::function<void()> _programmatic;
    std::vector<std::unique_ptr<FieldState>> const* _fields = nullptr;
    std::shared_ptr<Fetches> _fetches = std::make_shared<Fetches>();
};

}  // namespace detail::engine

/// @brief One collection row: the element's member fields.
class RowState {
public:
    /// @param core      The session core. Borrowed.
    /// @param arraySpec The collection field's spec (its `children` are the members).
    /// @param arrayPath The collection field's path.
    /// @param key       The row's stable key.
    /// @param parent    The collection field.
    RowState(detail::engine::SessionCore& core, FieldSpec const& arraySpec, std::string const& arrayPath,
             std::uint64_t key, FieldState const* parent);
    ~RowState();
    RowState(RowState const&) = delete;
    RowState& operator=(RowState const&) = delete;
    RowState(RowState&&) = delete;
    RowState& operator=(RowState&&) = delete;

    /// @brief The row's stable key (not its position).
    /// @return The key.
    [[nodiscard]] std::uint64_t key() const noexcept { return _key; }

    /// @brief The member fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> fields() const noexcept { return _fields; }

    /// @brief A member by wire name.
    /// @param name The wire name.
    /// @return The member, or null.
    [[nodiscard]] FieldState* field(std::string_view name) const noexcept;

    /// @brief Whether every member is blank. Tracked.
    /// @return `true` when nothing is entered in the row.
    [[nodiscard]] bool blank() const;

    /// @brief Starts every member; called once the row is built and loaded.
    void start();

private:
    std::uint64_t _key;
    std::vector<std::unique_ptr<FieldState>> _fields;
};

/// @brief A collection row `FieldState::removeRows` took out, with the position it stood at.
struct RemovedRow {
    /// @brief The row's position in the collection when it was removed.
    std::size_t position = 0;
    /// @brief The row, kept whole: its key and every member's draft and unit.
    std::shared_ptr<RowState> row;
};

/// @brief The reactive state of one field: its draft and everything derived from it.
class MORPH_API FieldState {
public:
    /// @param core     The session core. Borrowed: it must outlive the field.
    /// @param spec     The field's spec. Borrowed from the core's model (or a parent's children).
    /// @param path     The instance path (`name`, `parent.member`, `rows#key.member`).
    /// @param siblings The fields `x-optionsDependsOn` names are looked up among. Borrowed.
    /// @param parent   The containing Object or ObjectArray field, or null at the top level.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's or ObjectArray row's members are fields, to the model's depth.
    FieldState(detail::engine::SessionCore& core MORPH_LIFETIMEBOUND, FieldSpec const& spec MORPH_LIFETIMEBOUND,
               std::string path, std::vector<std::unique_ptr<FieldState>> const* siblings, FieldState const* parent);

    ~FieldState() = default;
    FieldState(FieldState const&) = delete;
    FieldState& operator=(FieldState const&) = delete;
    FieldState(FieldState&&) = delete;
    FieldState& operator=(FieldState&&) = delete;

    /// @brief The field's spec.
    /// @return The spec.
    [[nodiscard]] FieldSpec const& spec() const noexcept { return *_spec; }

    /// @brief The instance path.
    /// @return `name`, `parent.member`, or `rows#key.member` inside a collection row.
    [[nodiscard]] std::string const& path() const noexcept { return _path; }

    /// @brief The label: the explicit key's translation, else the derived key's (top level
    ///        only), else the schema's title. Tracked: it follows the session's `setTranslations`.
    /// @return The label.
    [[nodiscard]] std::string label() const;

    /// @brief The help text, resolved like the label. Tracked.
    /// @return The help text; empty when neither the schema nor the translations state one.
    [[nodiscard]] std::string help() const;

    /// @brief Whether the field has a help text now, read without tracking: what a view decides, when it is built,
    ///        whether to give the help a line of its own. The line's text is `help()`, which follows the translations.
    /// @return Whether `help()` is not empty.
    [[nodiscard]] bool hasHelp() const {
        return !_core->runtime().untracked([this] { return help(); }).empty();
    }

    /// @brief The placeholder: the one `overridePlaceholder` set, else `x-placeholder` resolved like the label, else
    ///        `defaultPlaceholder`. Tracked.
    /// @return The placeholder.
    [[nodiscard]] std::string placeholder() const;

    /// @brief The draft: what the control holds. Setting it is a user edit.
    /// @return The draft signal.
    [[nodiscard]] reactive::Signal<std::string>& text() noexcept { return _text; }

    /// @brief The draft, read-only. Tracked.
    /// @return What the control holds.
    [[nodiscard]] std::string const& draft() const { return _text.get(); }

    /// @brief A Decimal's or Quantity's entry unit, an index into `spec().units`.
    /// @return The unit signal.
    [[nodiscard]] reactive::Signal<std::size_t>& unit() noexcept { return _unit; }

    /// @brief A Decimal's or Quantity's entry unit, read-only.
    /// @return The unit signal.
    [[nodiscard]] reactive::Signal<std::size_t> const& unit() const noexcept { return _unit; }

    /// @brief The field's places as a record gives them after the form was built, with the meaning of
    ///        `x-decimalPlaces`: the canonical unit's entry places and the wire `dp`. Another unit of a Quantity keeps
    ///        its own. Unset keeps the schema's. A change checks the draft again and leaves its text as typed.
    /// @return The places signal.
    [[nodiscard]] reactive::Signal<std::optional<std::uint32_t>>& places() noexcept { return _places; }

    /// @brief Whether a blank-capable field was filled once (an `x-blankAs: empty` text, or a
    ///        collection that had rows).
    /// @return The engagement signal.
    [[nodiscard]] reactive::Signal<bool>& engaged() noexcept { return _engaged; }

    /// @brief Whether the control was engaged, read-only.
    /// @return The engaged signal.
    [[nodiscard]] reactive::Signal<bool> const& engaged() const noexcept { return _engaged; }

    /// @brief The draft's encoding. Tracked.
    /// @return The encoding computed.
    [[nodiscard]] reactive::Computed<Encoding> const& encoded() const noexcept { return _encoded; }

    /// @brief Why the draft does not encode, if it does not. Tracked.
    /// @return The error computed.
    [[nodiscard]] reactive::Computed<std::optional<FieldError>> const& error() const noexcept { return _error; }

    /// @brief Whether the field is shown: not `x-hidden`, and no `visibleWhen` naming it is False.
    /// @return The visibility computed.
    [[nodiscard]] reactive::Computed<bool> const& visible() const noexcept { return _visible; }

    /// @brief Whether the field is frozen: `x-readonly`, `x-computed`, `x-derived`, a frozen parent, or a
    /// `readonlyWhen`
    ///        naming it that is True.
    /// @return The read-only computed.
    [[nodiscard]] reactive::Computed<bool> const& readonly() const noexcept { return _readonly; }

    /// @brief Whether the field must be filled: `required`, or a `requiredWhen` naming it that is True.
    /// @return The required computed.
    [[nodiscard]] reactive::Computed<bool> const& required() const noexcept { return _required; }

    /// @brief Whether nothing is entered: a blank draft, an Object whose members are all blank, a
    ///        collection with no rows that was never engaged. Tracked.
    /// @return `true` when blank.
    // NOLINTNEXTLINE(misc-no-recursion): an Object asks its members.
    [[nodiscard]] bool blank() const;

    /// @brief What a rule sees of this field. Tracked.
    /// @return Blank, Invalid, or Valid with the exact value.
    [[nodiscard]] RuleOperand ruleOperand() const;

    /// @brief The selectable options: an Enum's from the schema, a Choice's as last fetched for
    ///        its current parents, less those `narrowOptions` leaves out. Tracked.
    /// @return The options; empty for every other kind, and for a Choice with nothing fetched.
    [[nodiscard]] std::vector<ChoiceOption> const& options() const { return _offered.get(); }

    /// @brief Narrows the options `options()` offers to those whose value is listed: a grid column's `options` overlay
    ///        for the field's row.
    /// @param keep The option values kept, as their JSON literals; nothing offers every option.
    void narrowOptions(std::optional<std::vector<std::string>> keep);

    /// @brief Replaces the placeholder: a grid column's `placeholder` overlay for the field's row.
    /// @param text The placeholder; empty restores the schema's.
    void overridePlaceholder(std::string text) { _placeholderOverride = std::move(text); }

    /// @brief What an editor of the field runs as it opens: a Choice member of a collection row fetches its options
    ///        from now on (a column of rows fetches only for the rows a user opens), and a failed fetch is retried
    ///        (`retryOptions`).
    void openOptions();

    /// @brief Fetches a Choice's options again when the last fetch failed; does nothing otherwise. What opening its
    ///        select runs, so options that could not be fetched when the form was built are fetched once the user
    ///        asks for them.
    void retryOptions();

    /// @brief Whether a Choice's options request is in flight. Tracked.
    /// @return `true` while fetching.
    [[nodiscard]] bool optionsPending() const { return _optionsQuery != nullptr && _optionsQuery->pending(); }

    /// @brief Switches a Quantity's entry unit, converting the draft exactly (half away from
    ///        zero to the new unit's decimals). A user action.
    /// @param index The new unit, an index into `spec().units`.
    void switchUnit(std::size_t index);

    /// @brief An Object's members, in `x-order`.
    /// @return The members; empty for every other kind.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> members() const noexcept { return _members; }

    /// @brief An Object's member by wire name.
    /// @param name The wire name.
    /// @return The member, or null.
    [[nodiscard]] FieldState* member(std::string_view name) const noexcept;

    /// @brief A collection's rows, in order. Tracked.
    /// @return The rows; empty for every other kind.
    [[nodiscard]] std::vector<std::shared_ptr<RowState>> const& rows() const { return _rows.get(); }

    /// @brief Appends a blank row to a collection and engages it. A user action.
    /// @return The new row's key; 0 when the field is not a collection, or a truncated one.
    std::uint64_t addRow();

    /// @brief Inserts a blank row into a collection before the row at @p position and engages it. A user action.
    /// @param position The new row's position; one past the end appends.
    /// @return The new row's key; 0 when the field is not a collection, or a truncated one.
    std::uint64_t insertRow(std::size_t position);

    /// @brief Removes a collection row by key. A user action. A holder of the row's `shared_ptr` (a mounted view)
    ///        keeps its state alive until it lets go.
    /// @param key The row's key.
    void removeRow(std::uint64_t key) {
        _rows.mutate([&](auto& rows) { std::erase_if(rows, [&](auto const& row) { return row->key() == key; }); });
    }

    /// @brief Removes collection rows by key, in one change. A user action.
    /// @param keys The rows' keys; a key the collection does not hold is skipped.
    /// @return The rows removed, each with the position it stood at, ascending: what `restoreRows` puts back.
    std::vector<RemovedRow> removeRows(std::span<std::uint64_t const> keys);

    /// @brief Puts rows `removeRows` took out back, each at the position it stood at, in one change. A user action.
    /// @param rows The rows, ascending by position, as `removeRows` gave them; a position past the end appends.
    void restoreRows(std::vector<RemovedRow> rows);

    /// @brief What the field holds, as one string: two snapshots are equal exactly when the field holds the same
    ///        entry (draft, unit and engagement, and every member's and row's). A computed or derived member, which
    ///        only a round of derived values writes, is no entry and is left out, so a round never makes a field look
    ///        edited or unsaved. Untracked.
    /// @return The snapshot.
    [[nodiscard]] std::string snapshot() const;

    /// @brief Whether loading @p value would leave what the field encodes unchanged: a scalar whose draft decodes
    ///        from the wire value to the same body entry as the current draft, such as `1` and the record's `1.000`.
    ///        An Object, a collection and a truncated field never match (their rows and members would be rebuilt
    ///        rather than compared). Untracked.
    /// @param value The wire value, or null for blank.
    /// @return `true` when the current draft already encodes as @p value does.
    [[nodiscard]] bool encodesLike(detail::engine::Json const* value) const;

    /// @brief Replaces the draft with a wire value as the user's own edit, in one batch: a `load` the session does
    ///        not mark programmatic, so the field counts as changed and is validated as an edit is.
    /// @param value The wire value; null is blank.
    /// @return Nothing, or why the value does not fit the field: an Object takes a JSON object, a collection a JSON
    ///         array. Then nothing changes.
    std::expected<void, std::string> edit(detail::engine::Json const& value);

    /// @brief Replaces the draft with the decoded wire value; null or absent is blank. Does not
    ///        mark the change programmatic — the caller does, in the same batch.
    /// @param value The wire value, or null.
    // An Object loads its members, a collection its rows; one case per kind.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
    void load(detail::engine::Json const* value);

    /// @brief Blanks the draft, the unit, the engagement, the members and the rows.
    // NOLINTNEXTLINE(misc-no-recursion): an Object clears its members.
    void clear();

    /// @brief Starts what needs every sibling to exist: an `x-blankAs` field's engagement, a
    ///        Choice's options query, and every member's own. Called once, after the owner built
    ///        all its fields.
    // NOLINTNEXTLINE(misc-no-recursion): an Object starts its members.
    void start();

private:
    // Each draft is written with its length first, so no draft can be read as the end of another. A computed or
    // derived member writes a fixed mark in its place.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's cells are fields.
    void appendSnapshot(std::string& out) const;

    [[nodiscard]] std::string resolveSlot(i18n::FieldSlot slot, std::string const& literal) const;

    // A member of a collection row.
    [[nodiscard]] bool inRow() const noexcept {
        return _parent != nullptr && _parent->spec().kind == FieldKind::ObjectArray;
    }

    // A field `x-optionsDependsOn` names: a sibling first (a row's member, an object's member), then a top-level
    // field.
    [[nodiscard]] FieldState* sibling(std::string_view name) const;

    /// The options request for the current parents, or nullopt while one does not encode, or while a row's member was
    /// never opened.
    [[nodiscard]] std::optional<std::string> optionsRequest() const;

    /// Clears a selection the options no longer back — once the query has answered for the
    /// current parents, so a value set before its options arrive survives the fetch. A failed
    /// fetch judges nothing, so it keeps the selection.
    void clearStaleSelection();

    // NOLINTNEXTLINE(misc-no-recursion): an Object or a row encodes its members.
    [[nodiscard]] std::expected<std::string, FieldError> encodeMembers(
        std::span<std::unique_ptr<FieldState> const> members) const;

    // An Object or a row encodes its members; one case per kind.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
    [[nodiscard]] Encoding computeEncoding() const;

    // Declaration order is destruction order reversed: the signals precede the computeds that
    // read them, and the effects are last, so nothing is torn down while something reads it.
    detail::engine::SessionCore* _core;
    FieldSpec const* _spec;
    std::string _path;
    std::vector<std::unique_ptr<FieldState>> const* _siblings;
    FieldState const* _parent;
    std::uint64_t _nextRowKey = 1;
    reactive::Signal<std::string> _text;
    reactive::Signal<std::size_t> _unit;
    reactive::Signal<std::optional<std::uint32_t>> _places;
    reactive::Signal<bool> _engaged;
    reactive::Signal<bool> _opened;
    reactive::Signal<std::optional<std::vector<std::string>>> _narrow;
    /// The placeholder a grid column's overlay gives the field's row; empty for the schema's.
    std::string _placeholderOverride;
    reactive::Signal<std::vector<std::shared_ptr<RowState>>> _rows;
    std::vector<std::unique_ptr<FieldState>> _members;
    reactive::Computed<Encoding> _encoded;
    reactive::Computed<std::optional<FieldError>> _error;
    reactive::Computed<bool> _visible;
    reactive::Computed<bool> _readonly;
    reactive::Computed<bool> _required;
    std::unique_ptr<reactive::Effect> _engage;
    std::unique_ptr<reactive::Query<std::string, std::string>> _optionsQuery;
    std::unique_ptr<reactive::Computed<std::vector<ChoiceOption>>> _fetched;
    std::unique_ptr<reactive::Effect> _clearStale;
    // Last: it reads `_fetched`, which is a pointer and not a reactive source, so nothing may read `options()` before
    // `start()` made it: the Computed would keep the options it read then. A row is started before it is published and
    // `load` decodes without options, so no path does.
    reactive::Computed<std::vector<ChoiceOption>> _offered;
};

inline RowState::RowState(detail::engine::SessionCore& core, FieldSpec const& arraySpec, std::string const& arrayPath,
                          std::uint64_t key, FieldState const* parent)
    : _key{key} {
    for (FieldSpec const& member : arraySpec.children) {
        _fields.push_back(std::make_unique<FieldState>(
            core, member, arrayPath + "#" + std::to_string(key) + "." + member.name, &_fields, parent));
    }
}

inline RowState::~RowState() = default;

inline FieldState* RowState::field(std::string_view name) const noexcept {
    auto const found = std::ranges::find_if(_fields, [&](auto const& entry) { return entry->spec().name == name; });
    return found == _fields.end() ? nullptr : found->get();
}

inline bool RowState::blank() const {
    return std::ranges::all_of(_fields, [](auto const& entry) { return entry->blank(); });
}

inline void RowState::start() {
    for (auto const& entry : _fields) {
        entry->start();
    }
}

namespace detail::engine {

inline FieldState* SessionCore::topLevel(std::string_view name) const {
    if (_fields == nullptr) {
        return nullptr;
    }
    auto const found = std::ranges::find_if(*_fields, [&](auto const& entry) { return entry->spec().name == name; });
    return found == _fields->end() ? nullptr : found->get();
}

inline RuleLookup SessionCore::lookup() const {
    return [this](std::string_view name) {
        FieldState const* const field = topLevel(name);
        return field == nullptr ? RuleOperand{} : field->ruleOperand();
    };
}

}  // namespace detail::engine

/// @brief The reactive state of one form: a controller over a `FormModel`.
///
/// Non-copyable and non-movable: fields, bindings and in-flight replies point into it. Destroy a
/// mounted view of it before the session.
class MORPH_API FormSession {
public:
    /// @param runtime The runtime. Borrowed: it must outlive the session.
    /// @param model   The form model.
    /// @param submit  Executes the form's action. Required.
    /// @param choices Fetches Choice options; may be empty when no field is a Choice.
    /// @param options Locale, translations, display zone, extra readiness gate.
    /// @throws std::invalid_argument when @p submit is empty.
    FormSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, FormModel model, Submitter submit,
                ChoiceFetcher choices, FormSessionOptions options = {});

    ~FormSession() = default;
    FormSession(FormSession const&) = delete;
    FormSession& operator=(FormSession const&) = delete;
    FormSession(FormSession&&) = delete;
    FormSession& operator=(FormSession&&) = delete;

    /// @brief The form model.
    /// @return The model.
    [[nodiscard]] FormModel const& model() const noexcept { return _core->model(); }

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return _core->runtime(); }

    /// @brief When the form submits.
    /// @return The model's submit mode.
    [[nodiscard]] SubmitMode submitMode() const noexcept { return _core->model().submitMode(); }

    /// @brief The session options.
    /// @return The options.
    [[nodiscard]] FormSessionOptions const& options() const noexcept { return _core->options(); }

    /// @brief Replaces the number locale and the date patterns the form reads, so a mounted form follows a change
    ///        of the session's formatting. Drafts keep their text and are checked again against the new format.
    /// @param locale          The number format. Its views must outlive the session or the next call.
    /// @param datePattern     The date pattern; empty keeps ISO.
    /// @param dateTimePattern The date-time pattern; empty keeps ISO.
    void setFormat(render::NumericLocale locale, std::string datePattern, std::string dateTimePattern) {
        _core->setFormat(locale, std::move(datePattern), std::move(dateTimePattern));
    }

    /// @brief The pattern the form's date or date-time inputs show. Tracked.
    /// @param date `true` for the date pattern, `false` for the date-time pattern.
    /// @return The pattern; empty keeps ISO.
    [[nodiscard]] std::string const& pattern(bool date) const { return _core->pattern(date); }

    /// @brief Replaces the translations and the locale the form's texts are resolved in, so a mounted form follows a
    ///        change of the session's catalogue or locale: every label, help text, placeholder, group title and error
    ///        message read through a tracked accessor is resolved again. Drafts and values are untouched.
    /// @param translations The catalog; empty shows every schema literal.
    /// @param bcp47        The locale.
    void setTranslations(render::TranslationProvider translations, std::string bcp47) {
        _core->setTranslations(std::move(translations), std::move(bcp47));
    }

    /// @brief The session options, read as a dependency on the translations and the locale: a computation that
    ///        resolves a text through them reruns on `setTranslations`.
    /// @return The options.
    [[nodiscard]] FormSessionOptions const& trackedOptions() const;

    /// @brief The top-level fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> fields() const noexcept { return _fields; }

    /// @brief A row position written in a path's brackets.
    /// @param digits The text between the brackets.
    /// @return The position, or `std::nullopt` unless the text is all decimal digits.
    [[nodiscard]] static std::optional<std::size_t> rowIndexOf(std::string_view digits);

    /// @brief A field by path: `name`, `parent.member`, `rows[i].member` (a row by position) or `rows#key.member` (a
    ///        row by its key, as `FieldState::path` writes it).
    /// @param path The path.
    /// @return The field.
    /// @throws std::out_of_range when no field has that path.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
    [[nodiscard]] FieldState& field(std::string_view path) const;

    /// @brief Whether the form can submit. Tracked.
    /// @return `true` when every required field encodes, no field has an error, no gating rule
    ///         is False, and `options().accepts` (when set) accepts the body.
    [[nodiscard]] bool ready() const { return _ready.get(); }

    /// @brief The body the form would submit. Tracked.
    /// @return The JSON body, fields in `x-order`, while ready; `std::nullopt` otherwise.
    [[nodiscard]] std::optional<std::string> body() const { return _body.get(); }

    /// @brief Submits the current body; does nothing while not ready, or while a submission is in
    ///        flight (the submission is `Exclusive`). The explicit-mode trigger; in automatic mode
    ///        it forces a resubmission of an unchanged body.
    void submit();

    /// @brief Loads a stored payload for editing. Every field is replaced (an absent member starts
    ///        blank), every unit returns to the canonical one, and nothing is submitted. Text that
    ///        is not a JSON object changes nothing.
    /// @param bodyJson The payload.
    void prefill(std::string_view bodyJson);

    /// @brief Loads a stored payload like `prefill`, except that a top-level field the user changed since the form
    ///        was last loaded keeps what they entered: a later value of the record being edited, such as its first
    ///        value arriving after the user started typing, or a refetch, does not discard the edit. A successful
    ///        submission ends the edit of every field still holding what it sent, so the record's later values reach
    ///        those. An automatic form submits while the person may still be typing, so there a saved scalar takes a
    ///        record value only when it encodes differently from the draft: the record's form of the same value
    ///        (`1.000` for a typed `1`) leaves the text alone. A saved object or collection of an automatic form
    ///        stays an edit. Nothing is submitted. Text that is not a JSON object changes nothing.
    /// @param bodyJson The payload.
    void refill(std::string_view bodyJson);

    /// @brief Sets one field from a JSON value, programmatically (nothing is submitted).
    /// @param path      The field's path (see `field`).
    /// @param jsonValue The value as JSON; text that is not JSON changes nothing.
    /// @throws std::out_of_range when no field has that path.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a path and a JSON value; swapped, the path is not found.
    void assign(std::string_view path, std::string_view jsonValue);

    /// @brief Blanks every field, programmatically (nothing is submitted).
    void reset();

    /// @brief A token that expires when the session is destroyed: what an object that may outlive it (a grid's source,
    ///        whose node a mount holds) checks before it calls back into the session.
    /// @return The token.
    [[nodiscard]] std::weak_ptr<void const> alive() const noexcept { return _alive; }

    /// @brief Registers what `submit` runs first, before it assembles the body: what ends an edit not committed yet,
    ///        such as a grid's open cell editor. Hooks run in the order they were added.
    /// @param hook The hook.
    /// @return The hook's id, for `removeBeforeSubmit`.
    [[nodiscard]] std::size_t addBeforeSubmit(std::function<void()> hook);

    /// @brief Removes a hook `addBeforeSubmit` registered.
    /// @param hookId The hook's id; one not registered is ignored.
    void removeBeforeSubmit(std::size_t hookId) {
        std::erase_if(_beforeSubmit, [hookId](auto const& entry) { return entry.first == hookId; });
    }

    /// @brief Whether a scalar field holds another entry than when the form was last loaded: its draft or its unit
    ///        differs, or it is a member of a row added since. Tracked. A computed or derived member is never
    ///        modified: only a round writes it. Any other member stays modified whatever freezes it after the user
    ///        typed in it.
    /// @param path The field's path (see `field`), such as `specimens#3.mass`.
    /// @return `true` when modified; `false` for a path that names no field, and for an Object or a collection.
    [[nodiscard]] bool modified(std::string_view path) const;

    /// @brief A count that moves with each load: each write through `FieldState::load`, which sets the fields it
    ///        writes afresh, their units included. Construction, `prefill`, `reset`, a `refill` that loads a field, a
    ///        submission that ends a field's edit and a round of derived values that writes a member are loads; a
    ///        switch of units as loaded alone is not, and nor is a revert. Tracked.
    /// @return The count.
    [[nodiscard]] std::uint64_t loadGeneration() const { return _loads.get(); }

    /// @brief Whether a row of a collection holds another entry than when the form was last loaded: one of its members
    ///        is modified, or the row was added since. Tracked.
    /// @param arrayPath The collection's path.
    /// @param key       The row's key.
    /// @return `true` when modified; `false` for a row the collection does not hold.
    [[nodiscard]] bool rowModified(std::string_view arrayPath, std::uint64_t key) const;

    /// @brief Why the last submission's reply refused a field: the reply's `errors` member, an object of field paths
    ///        (`field`) to a message or a list of messages, names it. Held while the field keeps the draft it had
    ///        then. Tracked.
    /// @param path The field's path.
    /// @return The message; empty while none holds.
    [[nodiscard]] std::optional<std::string> refusal(std::string_view path) const;

    /// @brief Restores what a field held when the form was last loaded, programmatically (nothing is submitted): a
    ///        scalar's draft and unit, or each member of a row (`rows#key`). A row added since the load is removed.
    /// @param path The field's or the row's path.
    void revert(std::string_view path);

    /// @brief Asks for derived and computed values in rounds from now on: each commit (`noteCommit`) starts a round
    /// once
    ///        `options.debounce` passed with no further commit, which evaluates the body and calls the derive action.
    /// @param options The evaluation, the derive call, the timers and the debounce.
    void deriveWith(DeriveOptions options);

    /// @brief An edit was committed (a grid's cell): with a round in flight its replies are marked stale, so one more
    ///        round follows them; otherwise a round starts once the debounce passed with no further commit, at once
    ///        when there are no timers. Does nothing until `deriveWith` gave an evaluation or a derive call.
    void noteCommit();

    /// @brief Makes one more round at once, unless one is in flight: what "Recalculate" runs after a failed round.
    void recalculate();

    /// @brief Where the derived values stand. Tracked.
    /// @return `Running` while a round is in flight, `Failed` after a round whose evaluation or derive call failed,
    ///         else `Current`.
    [[nodiscard]] DeriveState deriveState() const { return _deriveState.get(); }

    /// @brief Whether the derived values are the last ones loaded before a failure: true from a round that failed
    /// until
    ///        a round succeeds, so it holds through a retry in flight (what "Recalculate" offers stays until it
    ///        worked). Tracked.
    /// @return `true` while the last round to settle failed.
    [[nodiscard]] bool deriveFailing() const { return _deriveFailing.get(); }

    /// @brief The body as far as it encodes: every top-level field that encodes; an Object or a collection member by
    ///        member, leaving out a member that is blank or does not encode; every row of a collection written, in
    ///        order, `{}` for a row with nothing. Untracked.
    /// @return The JSON body.
    [[nodiscard]] std::string partialBody() const;

    /// @brief Starts a round's record: the next sequence, `partialBody()`, and the row keys of every collection by its
    ///        `FieldState::path()`, in the order the body writes the rows. Untracked.
    /// @return The round.
    [[nodiscard]] DeriveRound captureRound();

    /// @brief Loads a derive reply's values: each path names a derived member (`x-derived`) of the body @p round sent,
    ///        `rows[i]` resolved through the row keys at sending to the row's current position, so a value lands on
    ///        the row it was computed for; a value for a row removed since is dropped. Programmatic, and no edit:
    ///        nothing is submitted, and a member that held its loaded value holds the new one as loaded. A derived
    ///        member is in no field's snapshot, so a top-level field that held its loaded value, or what the
    ///        submission in flight sent, keeps holding it. A round older than the last loaded derive reply loads
    ///        nothing.
    /// @param round The round the reply answers.
    /// @param reply The reply.
    /// @return How many members were written, and the paths refused: those naming no derived member.
    DeriveLoad loadDerived(DeriveRound const& round, Derived const& reply);

    /// @brief Loads an evaluation's recomputed body: every computed member (`x-computed`) of the body @p round sent,
    ///        rows resolved as `loadDerived(round, Derived)` resolves them; a computed member the body leaves out of
    ///        an object it holds is blank. The reply's `errors` and `ready` are not read. Programmatic, and no edit,
    ///        as `loadDerived(round, Derived)` writes. A round older than the last loaded evaluation loads nothing.
    /// @param round         The round the reply answers.
    /// @param evaluatedBody The recomputed body.
    /// @return How many members were written; nothing is refused, since only computed members are read.
    DeriveLoad loadDerived(DeriveRound const& round, std::string_view evaluatedBody);

    /// @brief Switches units as part of how the fields were loaded: what the first unit a grid's per-row `unit`
    /// overlay
    ///        gives each loaded row is. Each switch converts the draft as `FieldState::switchUnit` does. A cell that
    ///        held its loaded value is then recorded as loaded with what it holds after the switch, so it draws no
    ///        modified mark and a revert returns to it; a modified cell, and a member of a row added since the load,
    ///        stay as they are, the switch an ordinary programmatic change. A top-level field that held its loaded
    ///        value, or what the submission in flight sent, keeps holding it. Nothing is submitted. All the switches
    ///        are one batch and one pass over the top-level fields, so a load's rows cost no more than one switch.
    /// @param switches Each field, a scalar of this session, and the index into its `spec().units` to switch it to.
    void switchUnitsAsLoaded(std::span<std::pair<FieldState*, std::size_t> const> switches);

    /// @brief Whether a submission is in flight. Tracked.
    /// @return `true` while one is.
    [[nodiscard]] bool pending() const { return _mutation.pending(); }

    /// @brief The last successful reply. Tracked.
    /// @return The reply JSON, or `std::nullopt` before the first success.
    [[nodiscard]] std::optional<std::string> const& lastReply() const { return _mutation.lastResult(); }

    /// @brief The last failure. Tracked.
    /// @return The exception, or null; cleared by the next success.
    [[nodiscard]] std::exception_ptr lastError() const { return _mutation.error(); }

    /// @brief How many submissions have succeeded. Tracked; each success ticks it, so an observer sees every
    ///        submission even when two replies are equal.
    /// @return The count.
    [[nodiscard]] std::uint64_t successCount() const { return _mutation.successCount(); }

    /// @brief How many submissions have been sent, automatic and explicit. Tracked; a submission the `Exclusive`
    ///        mutation refused while another was in flight does not count.
    /// @return The count.
    [[nodiscard]] std::uint64_t submitAttempts() const { return _attempts.get(); }

    /// @brief An accordion group's collapsed state.
    /// @param group Index into `model().groups()`.
    /// @return The signal.
    /// @throws std::out_of_range for an index past the groups.
    [[nodiscard]] reactive::Signal<bool>& groupCollapsed(std::size_t group) const { return *_collapsed.at(group); }

    /// @brief A tab run's selected tab, keyed by the run's first group index.
    /// @param group Index into `model().groups()` of the run's first tab group.
    /// @return The signal holding the selected tab's position in the run.
    /// @throws std::out_of_range for an index past the groups.
    [[nodiscard]] reactive::Signal<std::size_t>& tabSelection(std::size_t group) const { return *_tabs.at(group); }

    /// @brief A group's title, resolved through the translations (key `<action>.group.<index>`).
    /// @param group Index into `model().groups()`.
    /// @return The title; empty for the implicit trailing group. Tracked: it follows `setTranslations`.
    /// @throws std::out_of_range for an index past the groups.
    [[nodiscard]] std::string groupTitle(std::size_t group) const;

private:
    // Only while nothing is in flight: the submission is Exclusive, so a run made during one is refused and what it
    // would have sent was never saved.
    void rememberSent();

    // An explicit form ends the edit of what it saved. An automatic one does so only for a scalar: its refill compares
    // the record's value with the draft, which a collection's rows and an object's members do not allow, and
    // rebuilding them would replace the row the person is typing in.
    [[nodiscard]] bool endsEditOnSave(FieldState const& field) const;

    // Ends the edit of what the last success sent. Runs untracked: the accept effect and the next send both call it.
    void acceptSent();

    void markLoaded();

    // A field the last submission refused: the message, and the draft the field held then.
    struct Refused {
        std::string message;
        std::string draft;
    };

    // The field errors a reply names in its `errors` member, by the field's path: the first message of each entry. An
    // entry that names no field of the form is left out. A failure's exception names none: a refusal's arguments are
    // its sentence's.
    [[nodiscard]] std::map<std::string, Refused, std::less<>> fieldErrorsOf(
        std::optional<std::string> const& reply) const;

    // Takes the field errors of each submission as it settles: a reply's `errors`, or none for a failure.
    void takeFieldErrors(std::uint64_t successes, std::exception_ptr const& error);

    // What one scalar held when the form was last loaded; a row's own entry marks the row as loaded.
    struct LoadedCell {
        std::string text;
        std::size_t unit = 0;
    };

    // Records what a field holds now as loaded: each scalar's draft and unit, each row of a collection, each member.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    void recordLoaded(FieldState const& state);

    // A round of derived values loads its targets (`FieldState::load` sets each afresh, its unit included): the load
    // count moves when it wrote any.
    void noteLoads(std::size_t written);

    // Records a top-level field as loaded again, forgetting what it held before.
    void reloaded(FieldState const& state);

    // NOLINTNEXTLINE(misc-no-recursion): an Object's members are fields.
    [[nodiscard]] bool modified(FieldState const& state) const;

    // A collection's row by key, tracked; null when there is none.
    [[nodiscard]] RowState const* rowOf(std::string_view arrayPath, std::uint64_t key) const;

    [[nodiscard]] bool addedSinceLoad(RowState const& row) const;

    // Puts back what a scalar held when the form was last loaded, or blanks a member of a row added since.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members are fields.
    void restoreLoaded(FieldState& state);

    [[nodiscard]] std::optional<std::string> assemble() const;

    void autoSubmit();

    [[nodiscard]] static bool isComposite(FieldSpec const& spec) noexcept {
        return spec.truncated || spec.kind == FieldKind::Object || spec.kind == FieldKind::ObjectArray;
    }

    // What one field adds to the partial body: a scalar's encoding, an object's or a collection's members that encode;
    // nothing for a blank field or one that does not encode.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    [[nodiscard]] static std::optional<std::string> partialOf(FieldState const& field);

    // Every collection's row keys, by its path, in order, its rows' own collections included.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    static void collectRowKeys(FieldState const& field,
                               std::map<std::string, std::vector<std::uint64_t>, std::less<>>& out);

    // Where a reply's path lands: a field, a row removed since the round was sent, or nothing the body held.
    struct Resolved {
        enum class Kind : std::uint8_t { Found, Dropped, Missing } kind = Kind::Missing;
        FieldState* field = nullptr;
    };

    // Resolves `name`, `parent.member` and `rows[i].member` against the body @p round sent: `rows[i]` is the row whose
    // key was i-th when it was sent, wherever it is now.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per segment's form.
    [[nodiscard]] Resolved resolve(DeriveRound const& round, std::string_view path) const;

    // The computed members under one field and their values in the recomputed body: a scalar's when computed, an
    // object's members, a collection's rows by the keys they had when the round was sent. A computed member the body
    // leaves out of an object it holds is written blank (a null value).
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    static void collectComputed(DeriveRound const& round, FieldState& field, detail::engine::Json const& object,
                                std::vector<std::pair<FieldState*, detail::engine::Json const*>>& out);

    // Whether a scalar holds what the form recorded for it at the last load. A member of a row added since holds no
    // loaded value.
    [[nodiscard]] bool holdsLoaded(FieldState const& field) const;

    // A programmatic write into @p targets that is no edit: a target that held its loaded value is recorded as loaded
    // with its new one, a modified target stays modified, and each top-level field that held its loaded value, or what
    // the submission in flight sent, keeps holding it. Nothing is submitted.
    // A round writes only computed and derived members, which no snapshot holds, so it passes `keepTop` false: the
    // top-level fields' loaded and sent snapshots are unchanged by it already.
    template <typename Write>
    void writeKeepingLoaded(std::span<FieldState* const> targets, Write const& write, bool keepTop = true) {
        std::vector<bool> heldLoaded;
        std::vector<bool> topLoaded(_fields.size(), false);
        std::vector<bool> topSent(_fields.size(), false);
        runtime().untracked([&] {
            for (FieldState const* const target : targets) {
                heldLoaded.push_back(holdsLoaded(*target));
            }
            for (std::size_t index = 0; keepTop && index < _fields.size(); ++index) {
                std::string const snap = _fields.at(index)->snapshot();
                topLoaded.at(index) = snap == _loaded.at(index);
                topSent.at(index) = index < _sent.size() && snap == _sent.at(index);
            }
        });
        runtime().batch([&] {
            write();
            _core->markProgrammatic();
        });
        runtime().untracked([&] {
            for (std::size_t at = 0; at < targets.size(); ++at) {
                if (heldLoaded.at(at)) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < targets.size()
                    FieldState const& target = *targets[at];
                    _loadedCells.insert_or_assign(target.path(),
                                                  LoadedCell{.text = target.draft(), .unit = target.unit().peek()});
                }
            }
            for (std::size_t index = 0; index < _fields.size(); ++index) {
                if (!topLoaded.at(index) && !topSent.at(index)) {
                    continue;
                }
                std::string const snap = _fields.at(index)->snapshot();
                if (topLoaded.at(index)) {
                    _loaded.at(index) = snap;
                }
                if (topSent.at(index)) {
                    _sent.at(index) = snap;
                }
            }
        });
        _loadGeneration.set(_loadGeneration.peek() + 1);
    }

    // One round in flight: what it sent, which replies settled, whether one failed, and whether a commit came since.
    struct InFlight {
        DeriveRound round;
        bool evaluated = false;
        bool derived = false;
        bool failed = false;
        bool stale = false;
    };

    // Sends a round: the evaluation and the derive call, each with the partial body.
    // NOLINTNEXTLINE(misc-no-recursion): a stale round's settling starts the next round, which settles through its replies in turn.
    void startRound();

    // One reply of the round in flight settled; once both did, the state follows, and a stale round makes one more.
    // NOLINTNEXTLINE(misc-no-recursion): a stale round's settling starts the next round, which settles through its replies in turn.
    void settled(std::uint64_t sequence, bool evaluation, bool failed);

    std::unique_ptr<detail::engine::SessionCore> _core;
    Submitter _submit;
    std::vector<std::unique_ptr<FieldState>> _fields;
    std::vector<std::unique_ptr<reactive::Signal<bool>>> _collapsed;
    std::vector<std::unique_ptr<reactive::Signal<std::size_t>>> _tabs;
    reactive::Computed<std::optional<std::string>> _assembled;
    reactive::Computed<bool> _ready;
    reactive::Computed<std::optional<std::string>> _body;
    reactive::Mutation<std::string, std::string> _mutation;
    reactive::Signal<std::uint64_t> _attempts;
    std::optional<std::string> _lastSubmitted;
    /// @brief Each top-level field's snapshot as last loaded; a field whose snapshot differs was edited since.
    std::vector<std::string> _loaded;
    /// @brief Each top-level field's snapshot when the explicit submission in flight was sent.
    std::vector<std::string> _sent;
    std::uint64_t _acceptedSuccesses = 0;
    /// @brief What each scalar held when the form was last loaded, by its path, and each row of a collection then.
    std::map<std::string, LoadedCell, std::less<>> _loadedCells;
    /// @brief Moves on every change of `_loadedCells`, so what compares against it is evaluated again.
    reactive::Signal<std::uint64_t> _loadGeneration;
    /// @brief Moves with each load (`loadGeneration`); a switch of units as loaded leaves it.
    reactive::Signal<std::uint64_t> _loads;
    /// @brief What `submit` runs first, by id, in the order added.
    std::vector<std::pair<std::size_t, std::function<void()>>> _beforeSubmit;
    std::size_t _nextHook = 0;
    std::shared_ptr<void const> _alive = std::make_shared<int const>(0);
    /// @brief The fields the last failed submission refused, by path, and the failure they were read from.
    reactive::Signal<std::map<std::string, Refused, std::less<>>> _refusals;
    std::exception_ptr _refusalSeen;
    std::uint64_t _errorsSeenSuccesses = 0;
    /// @brief How the session asks for derived values, and the round in flight.
    DeriveOptions _derive;
    std::optional<InFlight> _round;
    std::uint64_t _sequence = 0;
    std::uint64_t _derivedLoaded = 0;
    std::uint64_t _evaluationLoaded = 0;
    reactive::Signal<DeriveState> _deriveState;
    reactive::Signal<bool> _deriveFailing;
    reactive::TimerHandle _debounce;
    async::CallbackScope _deriveScope;
    std::unique_ptr<reactive::Effect> _autoSubmit;
    std::unique_ptr<reactive::Effect> _acceptSent;
    std::unique_ptr<reactive::Effect> _refused;
};

}  // namespace morph::forms
