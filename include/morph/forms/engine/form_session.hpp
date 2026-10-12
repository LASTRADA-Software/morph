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
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/completion.hpp"
#include "../../reactive/control.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../../render/i18n.hpp"
#include "../../render/locale_format.hpp"
#include "../i18n.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "rules.hpp"
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
    render::TranslationProvider translations{};
    /// @brief The display zone for date-times, in minutes east of UTC.
    int displayOffsetMinutes = 0;
    /// @brief An extra readiness gate over the assembled body; empty means none. `Form<A>` puts
    ///        the action's own `validate()` here.
    std::function<bool(std::string_view bodyJson)> accepts{};
};

class FieldState;
class RowState;

namespace detail::engine {

/// @brief What every field of one session shares: the runtime, the model, the options, the
///        Choice fetcher, and the hook a programmatic change ends with.
class SessionCore {
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
          _values{.locale = _options.locale,
                  .displayOffsetMinutes = _options.displayOffsetMinutes,
                  .translations = _options.translations,
                  .bcp47 = _options.bcp47} {}

    /// @brief The runtime.
    /// @return The runtime passed at construction.
    [[nodiscard]] reactive::Runtime& runtime() const noexcept { return *_rt; }

    /// @brief The form model.
    /// @return The model.
    [[nodiscard]] FormModel const& model() const noexcept { return _model; }

    /// @brief The session options.
    /// @return The options.
    [[nodiscard]] FormSessionOptions const& options() const noexcept { return _options; }

    /// @brief The options a value conversion reads.
    /// @return The value context.
    [[nodiscard]] ValueContext const& values() const noexcept { return _values; }

    /// @brief The Choice fetcher.
    /// @return The fetcher; may be empty.
    [[nodiscard]] ChoiceFetcher const& choices() const noexcept { return _choices; }

    /// @brief Installs what `markProgrammatic` runs (the session records its current body as seen).
    /// @param hook The hook.
    void onProgrammatic(std::function<void()> hook) { _programmatic = std::move(hook); }

    /// @brief Ends a programmatic change: automatic submission does not submit the body it produced.
    void markProgrammatic() const {
        if (_programmatic) {
            _programmatic();
        }
    }

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

private:
    reactive::Runtime* _rt;
    FormModel _model;
    ChoiceFetcher _choices;
    FormSessionOptions _options;
    ValueContext _values;
    std::function<void()> _programmatic;
    std::vector<std::unique_ptr<FieldState>> const* _fields = nullptr;
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

/// @brief The reactive state of one field: its draft and everything derived from it.
class FieldState {
public:
    /// @param core     The session core. Borrowed: it must outlive the field.
    /// @param spec     The field's spec. Borrowed from the core's model (or a parent's children).
    /// @param path     The instance path (`name`, `parent.member`, `rows#key.member`).
    /// @param siblings The fields `x-optionsDependsOn` names are looked up among. Borrowed.
    /// @param parent   The containing Object or ObjectArray field, or null at the top level.
    FieldState(detail::engine::SessionCore& core MORPH_LIFETIMEBOUND, FieldSpec const& spec MORPH_LIFETIMEBOUND,
               std::string path, std::vector<std::unique_ptr<FieldState>> const* siblings, FieldState const* parent)
        : _core{&core},
          _spec{&spec},
          _path{std::move(path)},
          _siblings{siblings},
          _parent{parent},
          _text{core.runtime(), std::string{}},
          _unit{core.runtime(), std::size_t{0}},
          _engaged{core.runtime(), false},
          _rows{core.runtime(), std::vector<std::shared_ptr<RowState>>{}},
          _encoded{core.runtime(), [this] { return computeEncoding(); }},
          _error{core.runtime(),
                 [this]() -> std::optional<FieldError> {
                     Encoding const& encoding = _encoded.get();
                     if (encoding.has_value() && !encoding->has_value()) {
                         return encoding->error();
                     }
                     return std::nullopt;
                 }},
          _visible{core.runtime(),
                   [this] {
                       if (_spec->hidden) {
                           return false;
                       }
                       return _parent != nullptr ||
                              visibleVerdict(_core->model().rules(), _spec->name, _core->lookup()) != Tri::False;
                   }},
          _readonly{core.runtime(),
                    [this] {
                        if (_spec->readOnly || (_parent != nullptr && _parent->readonly().get())) {
                            return true;
                        }
                        return _parent == nullptr &&
                               readonlyVerdict(_core->model().rules(), _spec->name, _core->lookup()) == Tri::True;
                    }},
          _required{core.runtime(), [this] {
                        return _spec->required ||
                               (_parent == nullptr &&
                                requiredVerdict(_core->model().rules(), _spec->name, _core->lookup()) == Tri::True);
                    }} {
        if (_spec->kind == FieldKind::Object) {
            for (FieldSpec const& child : _spec->children) {
                _members.push_back(
                    std::make_unique<FieldState>(core, child, _path + "." + child.name, &_members, this));
            }
        }
        _label = resolveSlot(i18n::FieldSlot::Label, _spec->title);
        _help = resolveSlot(i18n::FieldSlot::Help, _spec->help);
        _placeholder = _spec->placeholder.empty() ? defaultPlaceholder(*_spec)
                                                  : resolveSlot(i18n::FieldSlot::Placeholder, _spec->placeholder);
    }

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
    ///        only), else the schema's title.
    /// @return The label.
    [[nodiscard]] std::string const& label() const noexcept { return _label; }

    /// @brief The help text, resolved like the label.
    /// @return The help text; empty when the schema states none.
    [[nodiscard]] std::string const& help() const noexcept { return _help; }

    /// @brief The placeholder: `x-placeholder` resolved like the label, else `defaultPlaceholder`.
    /// @return The placeholder.
    [[nodiscard]] std::string const& placeholder() const noexcept { return _placeholder; }

    /// @brief The draft: what the control holds. Setting it is a user edit.
    /// @return The draft signal.
    [[nodiscard]] reactive::Signal<std::string>& text() noexcept { return _text; }

    /// @brief A Decimal's or Quantity's entry unit, an index into `spec().units`.
    /// @return The unit signal.
    [[nodiscard]] reactive::Signal<std::size_t>& unit() noexcept { return _unit; }

    /// @brief Whether a blank-capable field was filled once (an `x-blankAs: empty` text, or a
    ///        collection that had rows).
    /// @return The engagement signal.
    [[nodiscard]] reactive::Signal<bool>& engaged() noexcept { return _engaged; }

    /// @brief The draft's encoding. Tracked.
    /// @return The encoding computed.
    [[nodiscard]] reactive::Computed<Encoding> const& encoded() const noexcept { return _encoded; }

    /// @brief Why the draft does not encode, if it does not. Tracked.
    /// @return The error computed.
    [[nodiscard]] reactive::Computed<std::optional<FieldError>> const& error() const noexcept { return _error; }

    /// @brief Whether the field is shown: not `x-hidden`, and no `visibleWhen` naming it is False.
    /// @return The visibility computed.
    [[nodiscard]] reactive::Computed<bool> const& visible() const noexcept { return _visible; }

    /// @brief Whether the field is frozen: `x-readonly`, a frozen parent, or a `readonlyWhen` naming it that is
    ///        True.
    /// @return The read-only computed.
    [[nodiscard]] reactive::Computed<bool> const& readonly() const noexcept { return _readonly; }

    /// @brief Whether the field must be filled: `required`, or a `requiredWhen` naming it that is True.
    /// @return The required computed.
    [[nodiscard]] reactive::Computed<bool> const& required() const noexcept { return _required; }

    /// @brief Whether nothing is entered: a blank draft, an Object whose members are all blank, a
    ///        collection with no rows that was never engaged. Tracked.
    /// @return `true` when blank.
    // NOLINTNEXTLINE(misc-no-recursion): an Object asks its members.
    [[nodiscard]] bool blank() const {
        if (_spec->kind == FieldKind::Object) {
            if (_spec->truncated) {
                return detail::engine::trim(_text.get()).empty();
            }
            return std::ranges::all_of(_members, [](auto const& entry) { return entry->blank(); });
        }
        if (_spec->kind == FieldKind::ObjectArray) {
            if (_spec->truncated) {
                return detail::engine::trim(_text.get()).empty() && !_engaged.get();
            }
            return _rows.get().empty() && !_engaged.get();
        }
        return detail::engine::trim(_text.get()).empty();
    }

    /// @brief What a rule sees of this field. Tracked.
    /// @return Blank, Invalid, or Valid with the exact value.
    [[nodiscard]] RuleOperand ruleOperand() const {
        Encoding const& encoding = _encoded.get();
        if (!encoding.has_value()) {
            return RuleOperand{};
        }
        if (!encoding->has_value()) {
            return RuleOperand{.state = RuleOperand::State::Invalid, .value = {}};
        }
        return RuleOperand{.state = RuleOperand::State::Valid, .value = (*encoding)->value};
    }

    /// @brief The selectable options: an Enum's from the schema, a Choice's as last fetched for
    ///        its current parents. Tracked.
    /// @return The options; empty for every other kind, and for a Choice with nothing fetched.
    [[nodiscard]] std::vector<ChoiceOption> const& options() const {
        if (_fetched) {
            return _fetched->get();
        }
        return _spec->enumOptions;
    }

    /// @brief Whether a Choice's options request is in flight. Tracked.
    /// @return `true` while fetching.
    [[nodiscard]] bool optionsPending() const { return _optionsQuery != nullptr && _optionsQuery->pending(); }

    /// @brief Switches a Quantity's entry unit, converting the draft exactly (half away from
    ///        zero to the new unit's decimals). A user action.
    /// @param index The new unit, an index into `spec().units`.
    void switchUnit(std::size_t index) {
        std::size_t const current = _unit.peek();
        if (index >= _spec->units.size() || index == current || current >= _spec->units.size()) {
            return;
        }
        auto converted =
            convertDraft(_text.peek(), _spec->units.at(current), _spec->units.at(index), _core->values().locale);
        _core->runtime().batch([&] {
            _unit.set(index);
            if (!detail::engine::trim(_text.peek()).empty()) {
                _text.set(converted.value_or(std::string{}));
            }
        });
    }

    /// @brief An Object's members, in `x-order`.
    /// @return The members; empty for every other kind.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> members() const noexcept { return _members; }

    /// @brief An Object's member by wire name.
    /// @param name The wire name.
    /// @return The member, or null.
    [[nodiscard]] FieldState* member(std::string_view name) const noexcept {
        auto const found =
            std::ranges::find_if(_members, [&](auto const& entry) { return entry->spec().name == name; });
        return found == _members.end() ? nullptr : found->get();
    }

    /// @brief A collection's rows, in order. Tracked.
    /// @return The rows; empty for every other kind.
    [[nodiscard]] std::vector<std::shared_ptr<RowState>> const& rows() const { return _rows.get(); }

    /// @brief Appends a blank row to a collection and engages it. A user action.
    /// @return The new row's key; 0 when the field is not a collection, or a truncated one.
    std::uint64_t addRow() {
        if (_spec->kind != FieldKind::ObjectArray || _spec->truncated) {
            return 0;
        }
        auto row = std::make_shared<RowState>(*_core, *_spec, _path, _nextRowKey++, this);
        row->start();
        std::uint64_t const key = row->key();
        _core->runtime().batch([&] {
            _rows.mutate([&](auto& rows) { rows.push_back(std::move(row)); });
            _engaged.set(true);
        });
        return key;
    }

    /// @brief Removes a collection row by key. A user action. A holder of the row's `shared_ptr` (a mounted view)
    ///        keeps its state alive until it lets go.
    /// @param key The row's key.
    void removeRow(std::uint64_t key) {
        _rows.mutate([&](auto& rows) { std::erase_if(rows, [&](auto const& row) { return row->key() == key; }); });
    }

    /// @brief Replaces the draft with the decoded wire value; null or absent is blank. Does not
    ///        mark the change programmatic — the caller does, in the same batch.
    /// @param value The wire value, or null.
    // NOLINTNEXTLINE(misc-no-recursion): an Object loads its members.
    void load(detail::engine::Json const* value) {
        using detail::engine::Json;
        bool const present = value != nullptr && !value->is_null();
        if (_spec->truncated) {
            _text.set(present ? detail::engine::toJson(*value) : std::string{});
            _engaged.set(present && value->is_array());
            return;
        }
        if (_spec->kind == FieldKind::Object) {
            for (auto const& entry : _members) {
                entry->load(present ? detail::engine::member(*value, entry->spec().name) : nullptr);
            }
            return;
        }
        if (_spec->kind == FieldKind::ObjectArray) {
            std::vector<std::shared_ptr<RowState>> loaded;
            if (present && value->is_array()) {
                for (Json const& element : value->get_array()) {
                    auto row = std::make_shared<RowState>(*_core, *_spec, _path, _nextRowKey++, this);
                    for (auto const& cell : row->fields()) {
                        cell->load(detail::engine::member(element, cell->spec().name));
                    }
                    row->start();
                    loaded.push_back(std::move(row));
                }
            }
            _rows.set(std::move(loaded));
            _engaged.set(present && value->is_array());
            return;
        }
        _text.set(present ? decodeScalar(*_spec, *value, _core->values()) : std::string{});
        _unit.set(0);
        _engaged.set(_spec->blankAs == BlankAs::Empty && present && value->is_string());
    }

    /// @brief Blanks the draft, the unit, the engagement, the members and the rows.
    // NOLINTNEXTLINE(misc-no-recursion): an Object clears its members.
    void clear() {
        _text.set(std::string{});
        _unit.set(0);
        _engaged.set(false);
        for (auto const& entry : _members) {
            entry->clear();
        }
        _rows.set({});
    }

    /// @brief Starts what needs every sibling to exist: an `x-blankAs` field's engagement, a
    ///        Choice's options query, and every member's own. Called once, after the owner built
    ///        all its fields.
    // NOLINTNEXTLINE(misc-no-recursion): an Object starts its members.
    void start() {
        for (auto const& entry : _members) {
            entry->start();
        }
        if (_spec->blankAs == BlankAs::Empty) {
            _engage = std::make_unique<reactive::Effect>(_core->runtime(), [this] {
                if (!detail::engine::trim(_text.get()).empty() && !_engaged.peek()) {
                    _engaged.set(true);
                }
            });
        }
        if (_spec->kind == FieldKind::Choice && _spec->choice.has_value() && _core->choices()) {
            reactive::Runtime& runtime = _core->runtime();
            _optionsQuery = std::make_unique<reactive::Query<std::string, std::string>>(
                runtime,
                [this](std::string const& request) {
                    _issued = request;
                    return _core->choices()(_spec->choice->optionsAction, request);
                },
                [this] { return optionsRequest(); });
            _fetched = std::make_unique<reactive::Computed<std::vector<ChoiceOption>>>(runtime, [this] {
                std::optional<std::string> const& reply = _optionsQuery->value();
                return reply.has_value() ? parseOptions(*reply, *_spec->choice) : std::vector<ChoiceOption>{};
            });
            _clearStale = std::make_unique<reactive::Effect>(runtime, [this] { clearStaleSelection(); });
        }
    }

private:
    [[nodiscard]] std::string resolveSlot(i18n::FieldSlot slot, std::string const& literal) const {
        auto const& translations = _core->options().translations;
        std::string_view const locale = _core->options().bcp47;
        auto const explicitKey = i18n::explicitFieldKey(_spec->i18nKey, slot);
        if (_parent == nullptr) {
            return render::resolveText(translations, locale, explicitKey,
                                       i18n::fieldKey(_core->model().actionType(), _spec->name, slot), literal);
        }
        if (translations && explicitKey) {
            if (auto hit = translations(*explicitKey, locale)) {
                return *std::move(hit);
            }
        }
        return literal;
    }

    [[nodiscard]] FieldState* sibling(std::string_view name) const {
        if (_siblings == nullptr) {
            return nullptr;
        }
        auto const found =
            std::ranges::find_if(*_siblings, [&](auto const& entry) { return entry->spec().name == name; });
        return found == _siblings->end() ? nullptr : found->get();
    }

    /// The options request for the current parents, or nullopt while one does not encode.
    [[nodiscard]] std::optional<std::string> optionsRequest() const {
        std::string body = "{";
        bool first = true;
        for (std::string const& parent : _spec->choice->dependsOn) {
            FieldState const* const source = sibling(parent);
            if (source == nullptr) {
                return std::nullopt;
            }
            Encoding const& encoding = source->encoded().get();
            if (!encoding.has_value() || !encoding->has_value()) {
                return std::nullopt;
            }
            body += (first ? "" : ",") + detail::engine::quote(parent) + ":" + (*encoding)->json;
            first = false;
        }
        return body + "}";
    }

    /// Clears a selection the options no longer back — once the query has answered for the
    /// current parents, so a value set before its options arrive survives the fetch. A failed
    /// fetch judges nothing, so it keeps the selection.
    void clearStaleSelection() {
        if (_optionsQuery->pending() || _optionsQuery->error() != nullptr) {
            return;
        }
        std::optional<std::string> const request = optionsRequest();
        if (request.has_value() && request != _issued) {
            return;
        }
        std::vector<ChoiceOption> const& current = _fetched->get();
        std::string_view const selected = detail::engine::trim(_text.get());
        if (selected.empty() ||
            std::ranges::any_of(current, [&](ChoiceOption const& option) { return option.valueJson == selected; })) {
            return;
        }
        _core->runtime().batch([&] {
            _text.set(std::string{});
            _core->markProgrammatic();
        });
    }

    // NOLINTNEXTLINE(misc-no-recursion): an Object or a row encodes its members.
    [[nodiscard]] std::expected<std::string, FieldError> encodeMembers(
        std::span<std::unique_ptr<FieldState> const> members) const {
        std::string json = "{";
        bool first = true;
        for (auto const& entry : members) {
            Encoding const& encoding = entry->encoded().get();
            if (!encoding.has_value()) {
                if (entry->spec().required) {
                    return std::unexpected(fieldError("incomplete", _core->values()));
                }
                continue;
            }
            if (!encoding->has_value()) {
                return std::unexpected(encoding->error());
            }
            json += (first ? "" : ",") + detail::engine::quote(entry->spec().name) + ":" + (*encoding)->json;
            first = false;
        }
        return json + "}";
    }

    // NOLINTNEXTLINE(misc-no-recursion): an Object or a row encodes its members.
    [[nodiscard]] Encoding computeEncoding() const {
        if (_spec->truncated) {
            std::string_view const draft = detail::engine::trim(_text.get());
            bool const array = _spec->kind == FieldKind::ObjectArray;
            if (draft.empty() && !_engaged.get()) {
                return (array && _spec->required) ? Encoding{WireValue{"[]", ui::Value{ui::ValueList{}}}} : Encoding{};
            }
            if (array && (draft == "[]" || draft.empty())) {
                return WireValue{"[]", ui::Value{ui::ValueList{}}};
            }
            return std::unexpected(fieldError("unrepresentable", _core->values()));
        }
        if (_spec->kind == FieldKind::Object) {
            if (blank()) {
                return std::nullopt;
            }
            auto json = encodeMembers(_members);
            if (!json) {
                return std::unexpected(json.error());
            }
            return WireValue{*std::move(json), ui::Value{}};
        }
        if (_spec->kind == FieldKind::ObjectArray) {
            auto const& rows = _rows.get();
            if (rows.empty() && !_engaged.get() && !_spec->required) {
                return std::nullopt;
            }
            std::string json = "[";
            for (std::size_t i = 0; i < rows.size(); ++i) {
                auto row = encodeMembers(rows.at(i)->fields());
                if (!row) {
                    return std::unexpected(row.error());
                }
                json += (i == 0 ? "" : ",") + *row;
            }
            return WireValue{json + "]", ui::Value{}};
        }
        return encodeScalar(*_spec, _text.get(), _unit.get(), _core->values());
    }

    // Declaration order is destruction order reversed: the signals precede the computeds that
    // read them, and the effects are last, so nothing is torn down while something reads it.
    detail::engine::SessionCore* _core;
    FieldSpec const* _spec;
    std::string _path;
    std::vector<std::unique_ptr<FieldState>> const* _siblings;
    FieldState const* _parent;
    std::string _label;
    std::string _help;
    std::string _placeholder;
    std::uint64_t _nextRowKey = 1;
    std::optional<std::string> _issued;
    reactive::Signal<std::string> _text;
    reactive::Signal<std::size_t> _unit;
    reactive::Signal<bool> _engaged;
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
class FormSession {
public:
    /// @param runtime The runtime. Borrowed: it must outlive the session.
    /// @param model   The form model.
    /// @param submit  Executes the form's action. Required.
    /// @param choices Fetches Choice options; may be empty when no field is a Choice.
    /// @param options Locale, translations, display zone, extra readiness gate.
    /// @throws std::invalid_argument when @p submit is empty.
    FormSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, FormModel model, Submitter submit,
                ChoiceFetcher choices, FormSessionOptions options = {})
        : _core{std::make_unique<detail::engine::SessionCore>(runtime, std::move(model), std::move(choices),
                                                              std::move(options))},
          _submit{std::move(submit)},
          _assembled{runtime, [this] { return assemble(); }},
          _ready{runtime,
                 [this] {
                     std::optional<std::string> const& assembled = _assembled.get();
                     if (!assembled.has_value()) {
                         return false;
                     }
                     auto const& accepts = _core->options().accepts;
                     return !accepts || accepts(*assembled);
                 }},
          _body{runtime, [this] { return _ready.get() ? _assembled.get() : std::optional<std::string>{}; }},
          _mutation{runtime,
                    [this](std::string body) { return _submit(_core->model().actionType(), std::move(body)); }} {
        if (!_submit) {
            throw std::invalid_argument{"FormSession: a Submitter is required"};
        }
        for (FieldSpec const& spec : _core->model().fields()) {
            _fields.push_back(std::make_unique<FieldState>(*_core, spec, spec.name, &_fields, nullptr));
        }
        _core->adopt(&_fields);
        _core->onProgrammatic([this] { _lastSubmitted = _body.peek(); });
        for (std::size_t i = 0; i < _core->model().groups().size(); ++i) {
            _collapsed.push_back(std::make_unique<reactive::Signal<bool>>(runtime, false));
            _tabs.push_back(std::make_unique<reactive::Signal<std::size_t>>(runtime, std::size_t{0}));
        }
        for (auto const& field : _fields) {
            field->start();
        }
        _autoSubmit = std::make_unique<reactive::Effect>(runtime, [this] { autoSubmit(); });
    }

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

    /// @brief The top-level fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> fields() const noexcept { return _fields; }

    /// @brief A field by path: `name`, `parent.member`, or `rows[i].member` (a row by position).
    /// @param path The path.
    /// @return The field.
    /// @throws std::out_of_range when no field has that path.
    [[nodiscard]] FieldState& field(std::string_view path) const {
        FieldState* current = nullptr;
        std::span<std::unique_ptr<FieldState> const> scope = _fields;
        std::string_view rest = path;
        auto const missing = [&] {
            return std::out_of_range{"FormSession::field: no field '" + std::string{path} + "'"};
        };
        while (!rest.empty()) {
            auto const dot = rest.find('.');
            std::string_view segment = rest.substr(0, dot);
            rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
            std::optional<std::size_t> index;
            if (auto const bracket = segment.find('['); bracket != std::string_view::npos) {
                if (!segment.ends_with(']')) {
                    throw missing();
                }
                std::string_view const digits = segment.substr(bracket + 1, segment.size() - bracket - 2);
                std::size_t parsed = 0;
                auto const result = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
                if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
                    throw missing();
                }
                index = parsed;
                segment = segment.substr(0, bracket);
            }
            auto const found =
                std::ranges::find_if(scope, [&](auto const& entry) { return entry->spec().name == segment; });
            if (found == scope.end()) {
                throw missing();
            }
            current = found->get();
            if (index.has_value()) {
                auto const& rows = current->rows();
                if (*index >= rows.size() || rest.empty()) {
                    throw missing();
                }
                scope = rows.at(*index)->fields();
            } else {
                scope = current->members();
                if (!rest.empty() && scope.empty()) {
                    throw missing();
                }
            }
        }
        if (current == nullptr) {
            throw missing();
        }
        return *current;
    }

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
    void submit() {
        std::optional<std::string> const body = _body.peek();
        if (!body.has_value()) {
            return;
        }
        _lastSubmitted = body;
        _mutation.run(*body);
    }

    /// @brief Loads a stored payload for editing. Every field is replaced (an absent member starts
    ///        blank), every unit returns to the canonical one, and nothing is submitted. Text that
    ///        is not a JSON object changes nothing.
    /// @param bodyJson The payload.
    void prefill(std::string_view bodyJson) {
        auto const dom = detail::engine::parseJson(bodyJson);
        if (!dom || !dom->is_object()) {
            return;
        }
        runtime().batch([&] {
            for (auto const& field : _fields) {
                field->load(detail::engine::member(*dom, field->spec().name));
            }
            _core->markProgrammatic();
        });
    }

    /// @brief Sets one field from a JSON value, programmatically (nothing is submitted).
    /// @param path      The field's path (see `field`).
    /// @param jsonValue The value as JSON; text that is not JSON changes nothing.
    /// @throws std::out_of_range when no field has that path.
    void assign(std::string_view path, std::string_view jsonValue) {
        FieldState& target = field(path);
        auto const dom = detail::engine::parseJson(jsonValue);
        if (!dom) {
            return;
        }
        runtime().batch([&] {
            target.load(&*dom);
            _core->markProgrammatic();
        });
    }

    /// @brief Blanks every field, programmatically (nothing is submitted).
    void reset() {
        runtime().batch([&] {
            for (auto const& field : _fields) {
                field->clear();
            }
            _core->markProgrammatic();
        });
    }

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
    /// @return The title; empty for the implicit trailing group.
    /// @throws std::out_of_range for an index past the groups.
    [[nodiscard]] std::string groupTitle(std::size_t group) const {
        FieldGroupSpec const& spec = detail::engine::elementAt(_core->model().groups(), group);
        if (spec.implicit) {
            return {};
        }
        return render::resolveText(_core->options().translations, _core->options().bcp47, std::nullopt,
                                   i18n::groupKey(_core->model().actionType(), group), spec.title);
    }

private:
    [[nodiscard]] std::optional<std::string> assemble() const {
        std::string json = "{";
        bool first = true;
        for (auto const& field : _fields) {
            Encoding const& encoding = field->encoded().get();
            bool const required = field->required().get();
            std::string literal;
            if (!encoding.has_value()) {
                if (field->spec().blankAs == BlankAs::Empty && field->engaged().get() && !required) {
                    literal = R"("")";
                } else if (required) {
                    return std::nullopt;
                } else {
                    continue;
                }
            } else if (!encoding->has_value()) {
                return std::nullopt;
            } else {
                literal = (*encoding)->json;
            }
            json += (first ? "" : ",") + detail::engine::quote(field->spec().name) + ":" + literal;
            first = false;
        }
        if (gatingVerdict(_core->model().rules(), _core->lookup()) == Tri::False) {
            return std::nullopt;
        }
        return json + "}";
    }

    void autoSubmit() {
        if (_core->model().submitMode() != SubmitMode::Automatic) {
            return;
        }
        // While a Choice's options are in flight its selection is unjudged: a parent just changed, and the selection
        // may be one the new options no longer back. Waiting lets the stale selection be cleared first; the settled
        // query wakes this Effect again.
        if (std::ranges::any_of(_fields, [](auto const& field) { return field->optionsPending(); })) {
            return;
        }
        std::optional<std::string> const& body = _body.get();
        if (!body.has_value() || body == _lastSubmitted) {
            return;
        }
        _lastSubmitted = body;
        std::string const toSend = *body;
        runtime().untracked([&] { _mutation.run(toSend); });
    }

    std::unique_ptr<detail::engine::SessionCore> _core;
    Submitter _submit;
    std::vector<std::unique_ptr<FieldState>> _fields;
    std::vector<std::unique_ptr<reactive::Signal<bool>>> _collapsed;
    std::vector<std::unique_ptr<reactive::Signal<std::size_t>>> _tabs;
    reactive::Computed<std::optional<std::string>> _assembled;
    reactive::Computed<bool> _ready;
    reactive::Computed<std::optional<std::string>> _body;
    reactive::Mutation<std::string, std::string> _mutation;
    std::optional<std::string> _lastSubmitted;
    std::unique_ptr<reactive::Effect> _autoSubmit;
};

}  // namespace morph::forms
