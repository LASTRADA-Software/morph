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
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/callback_scope.hpp"
#include "../../core/completion.hpp"
#include "../../reactive/control.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../../render/i18n.hpp"
#include "../../render/locale_format.hpp"
#include "../derive.hpp"
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
    [[nodiscard]] ValueContext const& values() const {
        static_cast<void>(_format.get());
        trackTexts();
        return _values;
    }

    /// @brief The pattern a date or date-time input shows and reads. Tracked.
    /// @param date `true` for the date pattern, `false` for the date-time pattern.
    /// @return The pattern; empty keeps ISO.
    [[nodiscard]] std::string const& pattern(bool date) const {
        static_cast<void>(_format.get());
        return date ? _options.datePattern : _options.dateTimePattern;
    }

    /// @brief Replaces the number locale and the date patterns every field reads. Drafts keep their text; the
    ///        fields check it again against the new format.
    /// @param locale          The number format. Its views must outlive the session or the next call.
    /// @param datePattern     The date pattern.
    /// @param dateTimePattern The date-time pattern.
    void setFormat(render::NumericLocale locale, std::string datePattern, std::string dateTimePattern) {
        _options.locale = locale;
        _options.datePattern = std::move(datePattern);
        _options.dateTimePattern = std::move(dateTimePattern);
        _values.locale = locale;
        _format.set(_format.peek() + 1);
    }

    /// @brief Replaces the translations and the locale every field text is resolved in, and reruns what read one.
    /// @param translations The catalog; empty shows every schema literal.
    /// @param bcp47        The locale.
    void setTranslations(render::TranslationProvider translations, std::string bcp47) {
        _options.translations = std::move(translations);
        _options.bcp47 = std::move(bcp47);
        _values.translations = _options.translations;
        _values.bcp47 = _options.bcp47;
        _texts.set(_texts.peek() + 1);
    }

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

    /// @brief Fetches a Choice's options through the fetcher, once per `(optionsAction, request)` for the session's
    ///        life: a second ask for a pair joins the fetch in flight or takes its reply. A failed fetch is not kept,
    ///        so the next ask fetches again.
    /// @param action  The options action.
    /// @param request The request body.
    /// @return The reply, delivered on the runtime's owner.
    [[nodiscard]] async::Completion<std::string> sharedChoices(std::string const& action, std::string request) {
        std::string const key = action + '\n' + request;
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_rt->owner());
        auto& fetches = *_fetches;
        if (auto const found = fetches.find(key); found != fetches.end()) {
            if (found->second->reply) {
                promise.resolve(*found->second->reply);
            } else {
                found->second->waiting.push_back(std::move(promise));
            }
            return std::move(completion);
        }
        auto fetch = std::make_shared<SharedFetch>();
        fetch->waiting.push_back(std::move(promise));
        fetches.emplace(key, fetch);
        std::weak_ptr<Fetches> const weak = _fetches;
        _choices(action, std::move(request))
            .then([fetch](std::string const& reply) {
                fetch->reply = reply;
                for (auto& waiter : std::exchange(fetch->waiting, {})) {
                    waiter.resolve(reply);
                }
            })
            .onError([fetch, weak, key](std::exception_ptr const& error) {
                if (auto const held = weak.lock()) {
                    if (auto const found = held->find(key); found != held->end() && found->second == fetch) {
                        held->erase(found);
                    }
                }
                for (auto& waiter : std::exchange(fetch->waiting, {})) {
                    waiter.reject(error);
                }
            });
        return std::move(completion);
    }

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

/// @brief The reactive state of one field: its draft and everything derived from it.
class FieldState {
public:
    /// @param core     The session core. Borrowed: it must outlive the field.
    /// @param spec     The field's spec. Borrowed from the core's model (or a parent's children).
    /// @param path     The instance path (`name`, `parent.member`, `rows#key.member`).
    /// @param siblings The fields `x-optionsDependsOn` names are looked up among. Borrowed.
    /// @param parent   The containing Object or ObjectArray field, or null at the top level.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's or ObjectArray row's members are fields, to the model's depth.
    FieldState(detail::engine::SessionCore& core MORPH_LIFETIMEBOUND, FieldSpec const& spec MORPH_LIFETIMEBOUND,
               std::string path, std::vector<std::unique_ptr<FieldState>> const* siblings, FieldState const* parent)
        : _core{&core},
          _spec{&spec},
          _path{std::move(path)},
          _siblings{siblings},
          _parent{parent},
          _text{core.runtime(), std::string{}},
          _unit{core.runtime(), spec.entryUnit},
          _places{core.runtime(), std::optional<std::uint32_t>{}},
          _engaged{core.runtime(), false},
          _opened{core.runtime(), false},
          _narrow{core.runtime(), std::optional<std::vector<std::string>>{}},
          _rows{core.runtime(), std::vector<std::shared_ptr<RowState>>{}},
          _encoded{core.runtime(), [this] { return computeEncoding(); }},
          _error{core.runtime(),
                 [this] -> std::optional<FieldError> {
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
                        if (_spec->readOnly || _spec->computed || _spec->derived ||
                            (_parent != nullptr && _parent->readonly().get())) {
                            return true;
                        }
                        return _parent == nullptr &&
                               readonlyVerdict(_core->model().rules(), _spec->name, _core->lookup()) == Tri::True;
                    }},
          _required{core.runtime(), [this] {
                        return _spec->required ||
                               (_parent == nullptr &&
                                requiredVerdict(_core->model().rules(), _spec->name, _core->lookup()) == Tri::True);
                    }},
          _offered{core.runtime(), [this] {
                       std::vector<ChoiceOption> const& all = _fetched ? _fetched->get() : _spec->enumOptions;
                       std::optional<std::vector<std::string>> const& keep = _narrow.get();
                       if (!keep.has_value()) {
                           return all;
                       }
                       std::vector<ChoiceOption> kept;
                       for (ChoiceOption const& option : all) {
                           if (std::ranges::find(*keep, option.valueJson) != keep->end()) {
                               kept.push_back(option);
                           }
                       }
                       return kept;
                   }} {
        if (_spec->kind == FieldKind::Object) {
            for (FieldSpec const& child : _spec->children) {
                _members.push_back(
                    std::make_unique<FieldState>(core, child, _path + "." + child.name, &_members, this));
            }
        }
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
    ///        only), else the schema's title. Tracked: it follows the session's `setTranslations`.
    /// @return The label.
    [[nodiscard]] std::string label() const {
        _core->trackTexts();
        return resolveSlot(i18n::FieldSlot::Label, _spec->title);
    }

    /// @brief The help text, resolved like the label. Tracked.
    /// @return The help text; empty when neither the schema nor the translations state one.
    [[nodiscard]] std::string help() const {
        _core->trackTexts();
        return resolveSlot(i18n::FieldSlot::Help, _spec->help);
    }

    /// @brief Whether the field has a help text now, read without tracking: what a view decides, when it is built,
    ///        whether to give the help a line of its own. The line's text is `help()`, which follows the translations.
    /// @return Whether `help()` is not empty.
    [[nodiscard]] bool hasHelp() const {
        return !_core->runtime().untracked([this] { return help(); }).empty();
    }

    /// @brief The placeholder: `x-placeholder` resolved like the label, else `defaultPlaceholder`. Tracked.
    /// @return The placeholder.
    [[nodiscard]] std::string placeholder() const {
        if (_spec->placeholder.empty()) {
            return defaultPlaceholder(*_spec);
        }
        _core->trackTexts();
        return resolveSlot(i18n::FieldSlot::Placeholder, _spec->placeholder);
    }

    /// @brief The draft: what the control holds. Setting it is a user edit.
    /// @return The draft signal.
    [[nodiscard]] reactive::Signal<std::string>& text() noexcept { return _text; }

    /// @brief The draft, read-only. Tracked.
    /// @return What the control holds.
    [[nodiscard]] std::string const& draft() const { return _text.get(); }

    /// @brief A Decimal's or Quantity's entry unit, an index into `spec().units`.
    /// @return The unit signal.
    [[nodiscard]] reactive::Signal<std::size_t>& unit() noexcept { return _unit; }

    /// @brief The field's places as a record gives them after the form was built, with the meaning of
    ///        `x-decimalPlaces`: the canonical unit's entry places and the wire `dp`. Another unit of a Quantity keeps
    ///        its own. Unset keeps the schema's. A change checks the draft again and leaves its text as typed.
    /// @return The places signal.
    [[nodiscard]] reactive::Signal<std::optional<std::uint32_t>>& places() noexcept { return _places; }

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

    /// @brief Whether the field is frozen: `x-readonly`, `x-computed`, `x-derived`, a frozen parent, or a `readonlyWhen`
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
    [[nodiscard]] bool blank() const {
        if (_spec->kind == FieldKind::Object) {
            if (_spec->truncated) {
                return detail::engine::trim(_text.get()).empty();
            }
            // NOLINTNEXTLINE(misc-no-recursion): each member answers for itself.
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
    ///        its current parents, less those `narrowOptions` leaves out. Tracked.
    /// @return The options; empty for every other kind, and for a Choice with nothing fetched.
    [[nodiscard]] std::vector<ChoiceOption> const& options() const { return _offered.get(); }

    /// @brief Narrows the options `options()` offers to those whose value is listed: a grid column's `options` overlay
    ///        for the field's row.
    /// @param keep The option values kept, as their JSON literals; nothing offers every option.
    void narrowOptions(std::optional<std::vector<std::string>> keep) {
        if (_narrow.peek() != keep) {
            _narrow.set(std::move(keep));
        }
    }

    /// @brief Replaces the placeholder: a grid column's `placeholder` overlay for the field's row.
    /// @param text The placeholder; empty restores the schema's.
    void overridePlaceholder(std::string text) {
        _placeholder = text.empty() ? (_spec->placeholder.empty()
                                           ? defaultPlaceholder(*_spec)
                                           : resolveSlot(i18n::FieldSlot::Placeholder, _spec->placeholder))
                                    : std::move(text);
    }

    /// @brief What an editor of the field runs as it opens: a Choice member of a collection row fetches its options
    ///        from now on (a column of rows fetches only for the rows a user opens), and a failed fetch is retried
    ///        (`retryOptions`).
    void openOptions() {
        if (!_opened.peek()) {
            _opened.set(true);
        }
        retryOptions();
    }

    /// @brief Fetches a Choice's options again when the last fetch failed; does nothing otherwise. What opening its
    ///        select runs, so options that could not be fetched when the form was built are fetched once the user
    ///        asks for them.
    void retryOptions() {
        if (_optionsQuery != nullptr && !_optionsQuery->pending() && _optionsQuery->error() != nullptr) {
            _optionsQuery->refetch();
        }
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
        UnitOption target = _spec->units.at(index);
        if (index == 0 && _places.peek().has_value()) {
            target.decimals = std::min(*_places.peek(), math::kMaxDecimalPlaces);
        }
        auto converted = convertDraft(_text.peek(), _spec->units.at(current), target, _core->values().locale);
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

    /// @brief Inserts a blank row into a collection before the row at @p position and engages it. A user action.
    /// @param position The new row's position; one past the end appends.
    /// @return The new row's key; 0 when the field is not a collection, or a truncated one.
    std::uint64_t insertRow(std::size_t position) {
        if (_spec->kind != FieldKind::ObjectArray || _spec->truncated) {
            return 0;
        }
        auto row = std::make_shared<RowState>(*_core, *_spec, _path, _nextRowKey++, this);
        row->start();
        std::uint64_t const key = row->key();
        _core->runtime().batch([&] {
            _rows.mutate([&](auto& rows) {
                auto const at = static_cast<std::ptrdiff_t>(std::min(position, rows.size()));
                rows.insert(rows.begin() + at, std::move(row));
            });
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

    /// @brief What the field holds, as one string: two snapshots are equal exactly when the field holds the same
    ///        entry (draft, unit and engagement, and every member's and row's). Untracked.
    /// @return The snapshot.
    [[nodiscard]] std::string snapshot() const {
        std::string out;
        appendSnapshot(out);
        return out;
    }

    /// @brief Whether loading @p value would leave what the field encodes unchanged: a scalar whose draft decodes
    ///        from the wire value to the same body entry as the current draft, such as `1` and the record's `1.000`.
    ///        An Object, a collection and a truncated field never match (their rows and members would be rebuilt
    ///        rather than compared). Untracked.
    /// @param value The wire value, or null for blank.
    /// @return `true` when the current draft already encodes as @p value does.
    [[nodiscard]] bool encodesLike(detail::engine::Json const* value) const {
        if (_spec->truncated || _spec->kind == FieldKind::Object || _spec->kind == FieldKind::ObjectArray) {
            return false;
        }
        bool const present = value != nullptr && !value->is_null();
        std::string draft = present ? decodeScalar(*_spec, *value, _core->values(), _places.peek()) : std::string{};
        std::size_t const unit = _spec->entryUnit;
        if (unit != 0 && unit < _spec->units.size() && !draft.empty()) {
            draft = convertDraft(draft, _spec->units.front(), _spec->units.at(unit), _core->values().locale)
                        .value_or(std::string{});
        }
        Encoding const incoming = encodeScalar(*_spec, draft, unit, _core->values(), _places.peek());
        Encoding const& current = _encoded.peek();
        if (incoming.has_value() != current.has_value()) {
            return false;
        }
        if (!incoming.has_value()) {
            return true;
        }
        return incoming->has_value() && current->has_value() && (*incoming)->json == (*current)->json;
    }

    /// @brief Replaces the draft with a wire value as the user's own edit, in one batch: a `load` the session does
    ///        not mark programmatic, so the field counts as changed and is validated as an edit is.
    /// @param value The wire value; null is blank.
    /// @return Nothing, or why the value does not fit the field: an Object takes a JSON object, a collection a JSON
    ///         array. Then nothing changes.
    std::expected<void, std::string> edit(detail::engine::Json const& value) {
        if (!value.is_null() && !_spec->truncated) {
            if (_spec->kind == FieldKind::Object && !value.is_object()) {
                return std::unexpected(std::string{"an object field takes a JSON object"});
            }
            if (_spec->kind == FieldKind::ObjectArray && !value.is_array()) {
                return std::unexpected(std::string{"a list field takes a JSON array"});
            }
        }
        _core->runtime().batch([&] { load(&value); });
        return {};
    }

    /// @brief Replaces the draft with the decoded wire value; null or absent is blank. Does not
    ///        mark the change programmatic — the caller does, in the same batch.
    /// @param value The wire value, or null.
    // An Object loads its members, a collection its rows; one case per kind.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
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
        std::string draft = present ? decodeScalar(*_spec, *value, _core->values(), _places.peek()) : std::string{};
        std::size_t const unit = _spec->entryUnit;
        if (unit != 0 && unit < _spec->units.size() && !draft.empty()) {
            // The wire value is in the canonical unit; the draft is shown in the entry unit.
            draft = convertDraft(draft, _spec->units.front(), _spec->units.at(unit), _core->values().locale)
                        .value_or(std::string{});
        }
        _text.set(std::move(draft));
        _unit.set(unit);
        _engaged.set(_spec->blankAs == BlankAs::Empty && present && value->is_string());
    }

    /// @brief Blanks the draft, the unit, the engagement, the members and the rows.
    // NOLINTNEXTLINE(misc-no-recursion): an Object clears its members.
    void clear() {
        _text.set(std::string{});
        _unit.set(_spec->entryUnit);
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
            // A row's member shares its fetches with every row asking the same: a column of rows over five materials
            // fetches five times.
            _optionsQuery = std::make_unique<reactive::Query<std::string, std::string>>(
                runtime,
                [this](std::string const& request) {
                    return inRow() ? _core->sharedChoices(_spec->choice->optionsAction, request)
                                   : _core->choices()(_spec->choice->optionsAction, request);
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
    // Each draft is written with its length first, so no draft can be read as the end of another.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's cells are fields.
    void appendSnapshot(std::string& out) const {
        std::string const& text = _text.peek();
        out += std::to_string(text.size());
        out += ':';
        out += text;
        out += std::to_string(_unit.peek());
        out += _engaged.peek() ? '+' : '-';
        for (auto const& entry : _members) {
            entry->appendSnapshot(out);
        }
        for (auto const& row : _rows.peek()) {
            out += '#';
            for (auto const& cell : row->fields()) {
                cell->appendSnapshot(out);
            }
        }
        out += ';';
    }

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

    // A member of a collection row.
    [[nodiscard]] bool inRow() const noexcept {
        return _parent != nullptr && _parent->spec().kind == FieldKind::ObjectArray;
    }

    // A field `x-optionsDependsOn` names: a sibling first (a row's member, an object's member), then a top-level field.
    [[nodiscard]] FieldState* sibling(std::string_view name) const {
        if (_siblings != nullptr) {
            auto const found =
                std::ranges::find_if(*_siblings, [&](auto const& entry) { return entry->spec().name == name; });
            if (found != _siblings->end()) {
                return found->get();
            }
        }
        return _parent != nullptr ? _core->topLevel(name) : nullptr;
    }

    /// The options request for the current parents, or nullopt while one does not encode, or while a row's member was
    /// never opened.
    [[nodiscard]] std::optional<std::string> optionsRequest() const {
        if (inRow() && !_opened.get()) {
            return std::nullopt;
        }
        std::string body = "{";
        bool first = true;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): only a Choice asks, and a Choice has its spec.
        for (std::string const& parent : _spec->choice.value().dependsOn) {
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
        // An idle query (a parent is blank or does not encode) fetched nothing to judge the selection against.
        std::optional<std::string> const request = optionsRequest();
        if (!request.has_value()) {
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

    // An Object or a row encodes its members; one case per kind.
    // NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
    [[nodiscard]] Encoding computeEncoding() const {
        if (_spec->truncated) {
            std::string_view const draft = detail::engine::trim(_text.get());
            bool const array = _spec->kind == FieldKind::ObjectArray;
            if (draft.empty() && !_engaged.get()) {
                return (array && _spec->required)
                           ? Encoding{WireValue{.json = "[]", .value = ui::Value{ui::ValueList{}}}}
                           : Encoding{};
            }
            if (array && (draft == "[]" || draft.empty())) {
                return WireValue{.json = "[]", .value = ui::Value{ui::ValueList{}}};
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
            return WireValue{.json = *std::move(json), .value = ui::Value{}};
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
            return WireValue{.json = json + "]", .value = ui::Value{}};
        }
        return encodeScalar(*_spec, _text.get(), _unit.get(), _core->values(), _places.get());
    }

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
    // Last: it reads `_fetched`.
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
                    [this](std::string body) { return _submit(_core->model().actionType(), std::move(body)); }},
          _attempts{runtime, std::uint64_t{0}},
          _loadGeneration{runtime, std::uint64_t{0}},
          _refusals{runtime, std::map<std::string, Refused, std::less<>>{}},
          _deriveState{runtime, DeriveState::Current} {
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
        markLoaded();
        _autoSubmit = std::make_unique<reactive::Effect>(runtime, [this] { autoSubmit(); });
        _acceptSent = std::make_unique<reactive::Effect>(runtime, [this, &runtime] {
            static_cast<void>(_mutation.successCount());
            runtime.untracked([&] { acceptSent(); });
        });
        _refused = std::make_unique<reactive::Effect>(runtime, [this, &runtime] {
            std::uint64_t const successes = _mutation.successCount();
            std::exception_ptr const error = _mutation.error();
            runtime.untracked([&] { takeFieldErrors(successes, error); });
        });
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
    [[nodiscard]] FormSessionOptions const& trackedOptions() const {
        _core->trackTexts();
        return _core->options();
    }

    /// @brief The top-level fields, in `x-order`.
    /// @return The fields.
    [[nodiscard]] std::span<std::unique_ptr<FieldState> const> fields() const noexcept { return _fields; }

    /// @brief A row position written in a path's brackets.
    /// @param digits The text between the brackets.
    /// @return The position, or `std::nullopt` unless the text is all decimal digits.
    [[nodiscard]] static std::optional<std::size_t> rowIndexOf(std::string_view digits) {
        std::size_t parsed = 0;
        char const* const end = std::to_address(digits.end());
        auto const result = std::from_chars(std::to_address(digits.begin()), end, parsed);
        if (digits.empty() || result.ec != std::errc{} || result.ptr != end) {
            return std::nullopt;
        }
        return parsed;
    }

    /// @brief A field by path: `name`, `parent.member`, `rows[i].member` (a row by position) or `rows#key.member` (a
    ///        row by its key, as `FieldState::path` writes it).
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
                index = rowIndexOf(segment.substr(bracket + 1, segment.size() - bracket - 2));
                if (!index.has_value()) {
                    throw missing();
                }
                segment = segment.substr(0, bracket);
            }
            std::optional<std::uint64_t> key;
            if (auto const hash = segment.find('#'); hash != std::string_view::npos && !index.has_value()) {
                std::optional<std::size_t> const parsed = rowIndexOf(segment.substr(hash + 1));
                if (!parsed.has_value()) {
                    throw missing();
                }
                key = static_cast<std::uint64_t>(*parsed);
                segment = segment.substr(0, hash);
            }
            auto const found =
                std::ranges::find_if(scope, [&](auto const& entry) { return entry->spec().name == segment; });
            if (found == scope.end()) {
                throw missing();
            }
            current = found->get();
            if (key.has_value()) {
                auto const& rows = current->rows();
                auto const row = std::ranges::find(rows, *key, &RowState::key);
                if (row == rows.end() || rest.empty()) {
                    throw missing();
                }
                scope = (*row)->fields();
            } else if (index.has_value()) {
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
        // A hook ends an edit not yet committed (a grid's open cell editor), so the body sent holds it.
        auto const hooks = _beforeSubmit;
        for (auto const& [id, hook] : hooks) {
            if (hook) {
                hook();
            }
        }
        std::optional<std::string> const body = _body.peek();
        if (!body.has_value()) {
            return;
        }
        rememberSent();
        _lastSubmitted = body;
        if (_mutation.run(*body)) {
            _attempts.set(_attempts.peek() + 1);
        }
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
        markLoaded();
    }

    /// @brief Loads a stored payload like `prefill`, except that a top-level field the user changed since the form
    ///        was last loaded keeps what they entered: a later value of the record being edited, such as its first
    ///        value arriving after the user started typing, or a refetch, does not discard the edit. A successful
    ///        submission ends the edit of every field still holding what it sent, so the record's later values reach
    ///        those. An automatic form submits while the person may still be typing, so there a saved scalar takes a
    ///        record value only when it encodes differently from the draft: the record's form of the same value
    ///        (`1.000` for a typed `1`) leaves the text alone. A saved object or collection of an automatic form
    ///        stays an edit. Nothing is submitted. Text that is not a JSON object changes nothing.
    /// @param bodyJson The payload.
    void refill(std::string_view bodyJson) {
        auto const dom = detail::engine::parseJson(bodyJson);
        if (!dom || !dom->is_object()) {
            return;
        }
        std::vector<std::size_t> loaded;
        runtime().batch([&] {
            for (std::size_t index = 0; index < _fields.size(); ++index) {
                FieldState& field = *_fields.at(index);
                if (field.snapshot() != _loaded.at(index)) {
                    continue;
                }
                detail::engine::Json const* const incoming = detail::engine::member(*dom, field.spec().name);
                // An automatic form's saved field is a finished edit only while the record differs from what the
                // person typed: the record's form of the same value (`1.000` for `1`) is left out, so a refill
                // never rewrites text the person may still be typing.
                if (submitMode() == SubmitMode::Automatic && field.encodesLike(incoming)) {
                    continue;
                }
                field.load(incoming);
                loaded.push_back(index);
            }
            _core->markProgrammatic();
        });
        for (std::size_t const index : loaded) {
            _loaded.at(index) = _fields.at(index)->snapshot();
            reloaded(*_fields.at(index));
        }
    }

    /// @brief Sets one field from a JSON value, programmatically (nothing is submitted).
    /// @param path      The field's path (see `field`).
    /// @param jsonValue The value as JSON; text that is not JSON changes nothing.
    /// @throws std::out_of_range when no field has that path.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a path and a JSON value; swapped, the path is not found.
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
        // A programmatic value is not an edit: the top-level field holding the target counts as loaded.
        for (std::size_t index = 0; index < _fields.size(); ++index) {
            std::string const& name = _fields.at(index)->spec().name;
            if (path.starts_with(name) &&
                (path.size() == name.size() || path.at(name.size()) == '.' || path.at(name.size()) == '[')) {
                _loaded.at(index) = _fields.at(index)->snapshot();
            }
        }
    }

    /// @brief Blanks every field, programmatically (nothing is submitted).
    void reset() {
        runtime().batch([&] {
            for (auto const& field : _fields) {
                field->clear();
            }
            _core->markProgrammatic();
        });
        markLoaded();
    }

    /// @brief A token that expires when the session is destroyed: what an object that may outlive it (a grid's source,
    ///        whose node a mount holds) checks before it calls back into the session.
    /// @return The token.
    [[nodiscard]] std::weak_ptr<void const> alive() const noexcept { return _alive; }

    /// @brief Registers what `submit` runs first, before it assembles the body: what ends an edit not committed yet,
    ///        such as a grid's open cell editor. Hooks run in the order they were added.
    /// @param hook The hook.
    /// @return The hook's id, for `removeBeforeSubmit`.
    [[nodiscard]] std::size_t addBeforeSubmit(std::function<void()> hook) {
        std::size_t const id = ++_nextHook;
        _beforeSubmit.emplace_back(id, std::move(hook));
        return id;
    }

    /// @brief Removes a hook `addBeforeSubmit` registered.
    /// @param id The hook's id; one not registered is ignored.
    void removeBeforeSubmit(std::size_t id) {
        std::erase_if(_beforeSubmit, [id](auto const& entry) { return entry.first == id; });
    }

    /// @brief Whether a scalar field holds another entry than when the form was last loaded: its draft or its unit
    ///        differs, or it is a member of a row added since. Tracked. A computed or derived member is never modified:
    ///        only a round writes it. Any other member stays modified whatever freezes it after the user typed in it.
    /// @param path The field's path (see `field`), such as `specimens#3.mass`.
    /// @return `true` when modified; `false` for a path that names no field, and for an Object or a collection.
    [[nodiscard]] bool modified(std::string_view path) const {
        FieldState* state = nullptr;
        try {
            state = &field(path);
        } catch (std::out_of_range const&) {
            return false;
        }
        return modified(*state);
    }

    /// @brief Whether a row of a collection holds another entry than when the form was last loaded: one of its members
    ///        is modified, or the row was added since. Tracked.
    /// @param arrayPath The collection's path.
    /// @param key       The row's key.
    /// @return `true` when modified; `false` for a row the collection does not hold.
    [[nodiscard]] bool rowModified(std::string_view arrayPath, std::uint64_t key) const {
        RowState const* const row = rowOf(arrayPath, key);
        if (row == nullptr) {
            return false;
        }
        static_cast<void>(_loadGeneration.get());
        if (addedSinceLoad(*row)) {
            return true;
        }
        return std::ranges::any_of(row->fields(), [this](auto const& member) { return modified(*member); });
    }

    /// @brief Why the last submission's reply refused a field: the reply's `errors` member, an object of field paths
    ///        (`field`) to a message or a list of messages, names it. Held while the field keeps the draft it had then.
    ///        Tracked.
    /// @param path The field's path.
    /// @return The message; empty while none holds.
    [[nodiscard]] std::optional<std::string> refusal(std::string_view path) const {
        auto const& held = _refusals.get();
        FieldState const* state = nullptr;
        try {
            state = &field(path);
        } catch (std::out_of_range const&) {
            return std::nullopt;
        }
        auto const found = held.find(state->path());
        if (found == held.end() || state->draft() != found->second.draft) {
            return std::nullopt;
        }
        return found->second.message;
    }

    /// @brief Restores what a field held when the form was last loaded, programmatically (nothing is submitted): a
    ///        scalar's draft and unit, or each member of a row (`rows#key`). A row added since the load is removed.
    /// @param path The field's or the row's path.
    void revert(std::string_view path) {
        if (auto const hash = path.rfind('#'); hash != std::string_view::npos && path.find('.', hash) == std::string_view::npos) {
            std::optional<std::size_t> const key = rowIndexOf(path.substr(hash + 1));
            RowState const* const row = key ? rowOf(path.substr(0, hash), *key) : nullptr;
            if (row == nullptr) {
                return;
            }
            FieldState& array = field(path.substr(0, hash));
            runtime().batch([&] {
                if (addedSinceLoad(*row)) {
                    array.removeRow(row->key());
                } else {
                    for (auto const& member : row->fields()) {
                        restoreLoaded(*member);
                    }
                }
                _core->markProgrammatic();
            });
            return;
        }
        FieldState* state = nullptr;
        try {
            state = &field(path);
        } catch (std::out_of_range const&) {
            return;
        }
        runtime().batch([&] {
            restoreLoaded(*state);
            _core->markProgrammatic();
        });
    }

    /// @brief Asks for derived and computed values in rounds from now on: each commit (`noteCommit`) starts a round once
    ///        `options.debounce` passed with no further commit, which evaluates the body and calls the derive action.
    /// @param options The evaluation, the derive call, the timers and the debounce.
    void deriveWith(DeriveOptions options) {
        _debounce.cancel();
        _derive = std::move(options);
    }

    /// @brief An edit was committed (a grid's cell): with a round in flight its replies are marked stale, so one more
    ///        round follows them; otherwise a round starts once the debounce passed with no further commit, at once
    ///        when there are no timers. Does nothing until `deriveWith` gave an evaluation or a derive call.
    void noteCommit() {
        if (!_derive.evaluate && !_derive.derive) {
            return;
        }
        if (_round) {
            _round->stale = true;
            return;
        }
        if (_derive.scheduler == nullptr) {
            startRound();
            return;
        }
        _debounce = _derive.scheduler->after(_derive.debounce, _deriveScope.guard([this] { startRound(); }));
    }

    /// @brief Makes one more round at once, unless one is in flight: what "Recalculate" runs after a failed round.
    void recalculate() {
        if (!_round && (_derive.evaluate || _derive.derive)) {
            _debounce.cancel();
            startRound();
        }
    }

    /// @brief Where the derived values stand. Tracked.
    /// @return `Running` while a round is in flight, `Failed` after a round whose evaluation or derive call failed,
    ///         else `Current`.
    [[nodiscard]] DeriveState deriveState() const { return _deriveState.get(); }

    /// @brief The body as far as it encodes: every top-level field that encodes; an Object or a collection member by
    ///        member, leaving out a member that is blank or does not encode; every row of a collection written, in
    ///        order, `{}` for a row with nothing. Untracked.
    /// @return The JSON body.
    [[nodiscard]] std::string partialBody() const {
        return runtime().untracked([&] {
            std::string json = "{";
            bool first = true;
            for (auto const& field : _fields) {
                std::optional<std::string> literal = partialOf(*field);
                if (!literal) {
                    continue;
                }
                json += (first ? "" : ",") + detail::engine::quote(field->spec().name) + ":" + *literal;
                first = false;
            }
            return json + "}";
        });
    }

    /// @brief Starts a round's record: the next sequence, `partialBody()`, and the row keys of every collection by its
    ///        `FieldState::path()`, in the order the body writes the rows. Untracked.
    /// @return The round.
    [[nodiscard]] DeriveRound captureRound() {
        DeriveRound round;
        round.sequence = ++_sequence;
        round.body = partialBody();
        runtime().untracked([&] {
            for (auto const& field : _fields) {
                collectRowKeys(*field, round.rowKeys);
            }
        });
        return round;
    }

    /// @brief Loads a derive reply's values: each path names a derived member (`x-derived`) of the body @p round sent,
    ///        `rows[i]` resolved through the row keys at sending to the row's current position, so a value lands on the
    ///        row it was computed for; a value for a row removed since is dropped. Programmatic: nothing is submitted,
    ///        and no field's loaded snapshot moves. A round older than the last loaded derive reply loads nothing.
    /// @param round The round the reply answers.
    /// @param reply The reply.
    /// @return How many members were written, and the paths refused: those naming no derived member.
    DeriveLoad loadDerived(DeriveRound const& round, Derived const& reply) {
        DeriveLoad load;
        if (round.sequence < _derivedLoaded) {
            return load;
        }
        _derivedLoaded = round.sequence;
        runtime().batch([&] {
            for (Derived::Value const& value : reply.values) {
                Resolved const target = resolve(round, value.path);
                if (target.kind == Resolved::Kind::Dropped) {
                    continue;
                }
                if (target.kind == Resolved::Kind::Missing || !target.field->spec().derived ||
                    isComposite(target.field->spec())) {
                    load.refused.push_back(value.path);
                    continue;
                }
                auto const dom = detail::engine::parseJson(value.value.str);
                if (!dom) {
                    load.refused.push_back(value.path);
                    continue;
                }
                target.field->load(&*dom);
                ++load.loaded;
            }
            _core->markProgrammatic();
        });
        return load;
    }

    /// @brief Loads an evaluation's recomputed body: every computed member (`x-computed`) of the body @p round sent,
    ///        rows resolved as `loadDerived(round, Derived)` resolves them; a computed member the body leaves out of an
    ///        object it holds is blank. The reply's `errors` and `ready` are not read. Programmatic, and no loaded
    ///        snapshot moves. A round older than the last loaded evaluation loads nothing.
    /// @param round         The round the reply answers.
    /// @param evaluatedBody The recomputed body.
    /// @return How many members were written; nothing is refused, since only computed members are read.
    DeriveLoad loadDerived(DeriveRound const& round, std::string_view evaluatedBody) {
        DeriveLoad load;
        if (round.sequence < _evaluationLoaded) {
            return load;
        }
        auto const dom = detail::engine::parseJson(evaluatedBody);
        if (!dom || !dom->is_object()) {
            return load;
        }
        _evaluationLoaded = round.sequence;
        runtime().batch([&] {
            runtime().untracked([&] {
                for (auto const& field : _fields) {
                    loadComputed(round, *field, *dom, load);
                }
            });
            _core->markProgrammatic();
        });
        return load;
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
    [[nodiscard]] std::string groupTitle(std::size_t group) const {
        FieldGroupSpec const& spec = detail::engine::elementAt(_core->model().groups(), group);
        if (spec.implicit) {
            return {};
        }
        _core->trackTexts();
        return render::resolveText(_core->options().translations, _core->options().bcp47, std::nullopt,
                                   i18n::groupKey(_core->model().actionType(), group), spec.title);
    }

private:
    // Only while nothing is in flight: the submission is Exclusive, so a run made during one is refused and what it
    // would have sent was never saved.
    void rememberSent() {
        runtime().untracked([&] {
            if (_mutation.pending()) {
                return;
            }
            // A success the accept effect has not yet seen belongs to the snapshots about to be replaced.
            acceptSent();
            _sent.clear();
            for (auto const& field : _fields) {
                _sent.push_back(field->snapshot());
            }
        });
    }

    // An explicit form ends the edit of what it saved. An automatic one does so only for a scalar: its refill compares
    // the record's value with the draft, which a collection's rows and an object's members do not allow, and
    // rebuilding them would replace the row the person is typing in.
    [[nodiscard]] bool endsEditOnSave(FieldState const& field) const {
        if (_core->model().submitMode() == SubmitMode::Explicit) {
            return true;
        }
        FieldSpec const& spec = field.spec();
        return !spec.truncated && spec.kind != FieldKind::Object && spec.kind != FieldKind::ObjectArray;
    }

    // Ends the edit of what the last success sent. Runs untracked: the accept effect and the next send both call it.
    void acceptSent() {
        std::uint64_t const successes = _mutation.successCount();
        if (successes == _acceptedSuccesses) {
            return;
        }
        _acceptedSuccesses = successes;
        // A reply that names field errors did not save the record: what was sent stays an edit.
        if (!fieldErrorsOf(_mutation.lastResult()).empty()) {
            _sent.clear();
            return;
        }
        for (std::size_t index = 0; index < _sent.size(); ++index) {
            if (_fields.at(index)->snapshot() == _sent.at(index) && endsEditOnSave(*_fields.at(index))) {
                _loaded.at(index) = _sent.at(index);
                reloaded(*_fields.at(index));
            }
        }
        _sent.clear();
    }

    void markLoaded() {
        _loaded.clear();
        for (auto const& field : _fields) {
            _loaded.push_back(field->snapshot());
        }
        _loadedCells.clear();
        for (auto const& field : _fields) {
            recordLoaded(*field);
        }
        _loadGeneration.set(_loadGeneration.peek() + 1);
    }

    // A field the last submission refused: the message, and the draft the field held then.
    struct Refused {
        std::string message;
        std::string draft;
    };

    // The field errors a reply names in its `errors` member, by the field's path: the first message of each entry. An
    // entry that names no field of the form is left out. A failure's exception names none: a refusal's arguments are
    // its sentence's.
    [[nodiscard]] std::map<std::string, Refused, std::less<>> fieldErrorsOf(std::optional<std::string> const& reply) const {
        std::map<std::string, Refused, std::less<>> out;
        auto const dom = reply ? detail::engine::parseJson(*reply) : std::nullopt;
        detail::engine::Json const* const errors = dom ? detail::engine::member(*dom, "errors") : nullptr;
        if (errors == nullptr || !errors->is_object()) {
            return out;
        }
        for (auto const& [path, entry] : errors->get_object()) {
            std::string message;
            if (entry.is_string()) {
                message = entry.get_string();
            } else if (entry.is_array() && !entry.get_array().empty() && entry.get_array().front().is_string()) {
                message = entry.get_array().front().get_string();
            }
            if (message.empty()) {
                continue;
            }
            try {
                FieldState const& state = field(path);
                out.insert_or_assign(state.path(), Refused{.message = std::move(message), .draft = state.draft()});
            } catch (std::out_of_range const&) {  // NOLINT(bugprone-empty-catch)
                // A path the form has no field for is not shown at any field.
            }
        }
        return out;
    }

    // Takes the field errors of each submission as it settles: a reply's `errors`, or none for a failure.
    void takeFieldErrors(std::uint64_t successes, std::exception_ptr const& error) {
        if (successes == _errorsSeenSuccesses && error == _refusalSeen) {
            return;
        }
        bool const succeeded = successes != _errorsSeenSuccesses;
        _errorsSeenSuccesses = successes;
        _refusalSeen = error;
        _refusals.set(succeeded ? fieldErrorsOf(_mutation.lastResult()) : std::map<std::string, Refused, std::less<>>{});
    }

    // What one scalar held when the form was last loaded; a row's own entry marks the row as loaded.
    struct LoadedCell {
        std::string text;
        std::size_t unit = 0;
    };

    // Records what a field holds now as loaded: each scalar's draft and unit, each row of a collection, each member.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    void recordLoaded(FieldState const& state) {
        FieldSpec const& spec = state.spec();
        if (spec.truncated) {
            return;
        }
        runtime().untracked([&] {
            if (spec.kind == FieldKind::Object) {
                for (auto const& member : state.members()) {
                    recordLoaded(*member);
                }
            } else if (spec.kind == FieldKind::ObjectArray) {
                for (auto const& row : state.rows()) {
                    _loadedCells.insert_or_assign(state.path() + "#" + std::to_string(row->key()), LoadedCell{});
                    for (auto const& member : row->fields()) {
                        recordLoaded(*member);
                    }
                }
            } else {
                _loadedCells.insert_or_assign(
                    state.path(),
                    LoadedCell{.text = state.draft(), .unit = const_cast<FieldState&>(state).unit().get()});
            }
        });
    }

    // Records a top-level field as loaded again, forgetting what it held before.
    void reloaded(FieldState const& state) {
        std::string const& name = state.path();
        std::erase_if(_loadedCells, [&name](auto const& entry) {
            std::string const& path = entry.first;
            return path == name || (path.size() > name.size() && path.starts_with(name) &&
                                    (path.at(name.size()) == '.' || path.at(name.size()) == '#'));
        });
        recordLoaded(state);
        _loadGeneration.set(_loadGeneration.peek() + 1);
    }

    // NOLINTNEXTLINE(misc-no-recursion): an Object's members are fields.
    [[nodiscard]] bool modified(FieldState const& state) const {
        FieldSpec const& spec = state.spec();
        if (spec.truncated || spec.kind == FieldKind::Object || spec.kind == FieldKind::ObjectArray ||
            spec.computed || spec.derived) {
            return false;
        }
        static_cast<void>(_loadGeneration.get());
        auto const found = _loadedCells.find(state.path());
        // A member of a row added since the load.
        if (found == _loadedCells.end()) {
            return true;
        }
        return state.draft() != found->second.text || const_cast<FieldState&>(state).unit().get() != found->second.unit;
    }

    // A collection's row by key, tracked; null when there is none.
    [[nodiscard]] RowState const* rowOf(std::string_view arrayPath, std::uint64_t key) const {
        FieldState* array = nullptr;
        try {
            array = &field(arrayPath);
        } catch (std::out_of_range const&) {
            return nullptr;
        }
        if (array->spec().kind != FieldKind::ObjectArray) {
            return nullptr;
        }
        auto const& rows = array->rows();
        auto const found = std::ranges::find(rows, key, &RowState::key);
        return found == rows.end() ? nullptr : found->get();
    }

    [[nodiscard]] bool addedSinceLoad(RowState const& row) const {
        std::string const& first = row.fields().empty() ? std::string{} : row.fields().front()->path();
        // A member's path is `<array>#<key>.<member>`: the row's own entry is the part before the member.
        std::string const rowPath = first.substr(0, first.rfind('.'));
        return !_loadedCells.contains(rowPath);
    }

    // Puts back what a scalar held when the form was last loaded, or blanks a member of a row added since.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members are fields.
    void restoreLoaded(FieldState& state) {
        FieldSpec const& spec = state.spec();
        if (spec.truncated || spec.kind == FieldKind::ObjectArray) {
            return;
        }
        if (spec.kind == FieldKind::Object) {
            for (auto const& member : state.members()) {
                restoreLoaded(*member);
            }
            return;
        }
        auto const found = _loadedCells.find(state.path());
        state.text().set(found == _loadedCells.end() ? std::string{} : found->second.text);
        state.unit().set(found == _loadedCells.end() ? spec.entryUnit : found->second.unit);
    }

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
        // The submission is Exclusive: while one is in flight a new run would be refused and its body lost. Waiting
        // here sends the latest body once the one in flight settles, which wakes this Effect again.
        if (_mutation.pending()) {
            return;
        }
        std::optional<std::string> const& body = _body.get();
        if (!body.has_value() || body == _lastSubmitted) {
            return;
        }
        _lastSubmitted = body;
        rememberSent();
        std::string const toSend = *body;
        runtime().untracked([&] {
            if (_mutation.run(toSend)) {
                _attempts.set(_attempts.peek() + 1);
            }
        });
    }

    [[nodiscard]] static bool isComposite(FieldSpec const& spec) noexcept {
        return spec.truncated || spec.kind == FieldKind::Object || spec.kind == FieldKind::ObjectArray;
    }

    // What one field adds to the partial body: a scalar's encoding, an object's or a collection's members that encode;
    // nothing for a blank field or one that does not encode.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    [[nodiscard]] static std::optional<std::string> partialOf(FieldState const& field) {
        FieldSpec const& spec = field.spec();
        auto const membersOf = [](std::span<std::unique_ptr<FieldState> const> members) {
            std::string json = "{";
            bool first = true;
            for (auto const& member : members) {
                if (std::optional<std::string> literal = partialOf(*member)) {
                    json += (first ? "" : ",") + detail::engine::quote(member->spec().name) + ":" + *literal;
                    first = false;
                }
            }
            return json + "}";
        };
        if (!spec.truncated && spec.kind == FieldKind::Object) {
            std::string json = membersOf(field.members());
            return json == "{}" ? std::nullopt : std::optional{json};
        }
        if (!spec.truncated && spec.kind == FieldKind::ObjectArray && !field.rows().empty()) {
            std::string json = "[";
            bool first = true;
            for (auto const& row : field.rows()) {
                json += (first ? "" : ",") + membersOf(row->fields());
                first = false;
            }
            return json + "]";
        }
        Encoding const& encoding = field.encoded().get();
        if (encoding.has_value() && encoding->has_value()) {
            return (*encoding)->json;
        }
        if (!encoding.has_value() && spec.blankAs == BlankAs::Empty &&
            const_cast<FieldState&>(field).engaged().get()) {  // NOLINT(cppcoreguidelines-pro-type-const-cast)
            return std::string{R"("")"};
        }
        return std::nullopt;
    }

    // Every collection's row keys, by its path, in order, its rows' own collections included.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    static void collectRowKeys(FieldState const& field, std::map<std::string, std::vector<std::uint64_t>, std::less<>>& out) {
        FieldSpec const& spec = field.spec();
        if (spec.truncated) {
            return;
        }
        if (spec.kind == FieldKind::Object) {
            for (auto const& member : field.members()) {
                collectRowKeys(*member, out);
            }
        } else if (spec.kind == FieldKind::ObjectArray) {
            std::vector<std::uint64_t>& keys = out[field.path()];
            for (auto const& row : field.rows()) {
                keys.push_back(row->key());
                for (auto const& member : row->fields()) {
                    collectRowKeys(*member, out);
                }
            }
        }
    }

    // Where a reply's path lands: a field, a row removed since the round was sent, or nothing the body held.
    struct Resolved {
        enum class Kind : std::uint8_t { Found, Dropped, Missing } kind = Kind::Missing;
        FieldState* field = nullptr;
    };

    // Resolves `name`, `parent.member` and `rows[i].member` against the body @p round sent: `rows[i]` is the row whose
    // key was i-th when it was sent, wherever it is now.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per segment's form.
    [[nodiscard]] Resolved resolve(DeriveRound const& round, std::string_view path) const {
        return runtime().untracked([&] {
            std::span<std::unique_ptr<FieldState> const> scope = _fields;
            FieldState* current = nullptr;
            std::string_view rest = path;
            while (!rest.empty()) {
                auto const dot = rest.find('.');
                std::string_view segment = rest.substr(0, dot);
                rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
                std::optional<std::size_t> index;
                if (auto const bracket = segment.find('['); bracket != std::string_view::npos) {
                    if (!segment.ends_with(']')) {
                        return Resolved{};
                    }
                    index = rowIndexOf(segment.substr(bracket + 1, segment.size() - bracket - 2));
                    if (!index) {
                        return Resolved{};
                    }
                    segment = segment.substr(0, bracket);
                }
                auto const found =
                    std::ranges::find_if(scope, [&](auto const& entry) { return entry->spec().name == segment; });
                if (found == scope.end()) {
                    return Resolved{};
                }
                current = found->get();
                if (index) {
                    if (current->spec().kind != FieldKind::ObjectArray || rest.empty()) {
                        return Resolved{};
                    }
                    auto const sent = round.rowKeys.find(current->path());
                    if (sent == round.rowKeys.end() || *index >= sent->second.size()) {
                        return Resolved{};
                    }
                    auto const& rows = current->rows();
                    auto const row = std::ranges::find(rows, sent->second.at(*index), &RowState::key);
                    if (row == rows.end()) {
                        return Resolved{.kind = Resolved::Kind::Dropped, .field = nullptr};
                    }
                    scope = (*row)->fields();
                } else {
                    scope = current->members();
                    if (!rest.empty() && scope.empty()) {
                        return Resolved{};
                    }
                }
            }
            return current == nullptr ? Resolved{} : Resolved{.kind = Resolved::Kind::Found, .field = current};
        });
    }

    // Loads the computed members under one field from the recomputed body's value for it: a scalar's when computed, an
    // object's members, a collection's rows by the keys they had when the round was sent.
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    static void loadComputedIn(DeriveRound const& round, std::span<std::unique_ptr<FieldState> const> members,
                               detail::engine::Json const& object, DeriveLoad& load) {
        for (auto const& member : members) {
            loadComputed(round, *member, object, load);
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
    static void loadComputed(DeriveRound const& round, FieldState& field, detail::engine::Json const& object,
                             DeriveLoad& load) {
        FieldSpec const& spec = field.spec();
        if (spec.truncated) {
            return;
        }
        detail::engine::Json const* const value = detail::engine::member(object, spec.name);
        if (spec.kind == FieldKind::Object) {
            if (value != nullptr && value->is_object()) {
                loadComputedIn(round, field.members(), *value, load);
            }
            return;
        }
        if (spec.kind == FieldKind::ObjectArray) {
            auto const sent = round.rowKeys.find(field.path());
            if (value == nullptr || !value->is_array() || sent == round.rowKeys.end()) {
                return;
            }
            auto const& elements = value->get_array();
            auto const& rows = field.rows();
            for (std::size_t at = 0; at < elements.size() && at < sent->second.size(); ++at) {
                auto const row = std::ranges::find(rows, sent->second.at(at), &RowState::key);
                if (row != rows.end() && elements.at(at).is_object()) {
                    loadComputedIn(round, (*row)->fields(), elements.at(at), load);
                }
            }
            return;
        }
        if (spec.computed) {
            field.load(value);
            ++load.loaded;
        }
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
    void startRound() {
        _debounce.cancel();
        _round = InFlight{.round = captureRound()};
        _round->evaluated = !_derive.evaluate;
        _round->derived = !_derive.derive;
        _deriveState.set(DeriveState::Running);
        std::uint64_t const sequence = _round->round.sequence;
        std::string const body = _round->round.body;
        if (_derive.evaluate) {
            _derive.evaluate(body)
                .then(_deriveScope.token(),
                      [this, sequence](std::string const& reply) {
                          if (_round && _round->round.sequence == sequence) {
                              static_cast<void>(loadDerived(_round->round, std::string_view{reply}));
                          }
                          settled(sequence, true, false);
                      })
                .onError(_deriveScope.token(),
                         [this, sequence](std::exception_ptr const&) { settled(sequence, true, true); });
        }
        if (_derive.derive) {
            _derive.derive(R"({"body":)" + body + "}")
                .then(_deriveScope.token(),
                      [this, sequence](std::string const& reply) {
                          bool failed = false;
                          if (_round && _round->round.sequence == sequence) {
                              Derived values;
                              if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(values, reply)) {
                                  failed = true;
                              } else {
                                  DeriveLoad const load = loadDerived(_round->round, values);
                                  if (!load.refused.empty() && _derive.refused) {
                                      _derive.refused(load.refused);
                                  }
                              }
                          }
                          settled(sequence, false, failed);
                      })
                .onError(_deriveScope.token(),
                         [this, sequence](std::exception_ptr const&) { settled(sequence, false, true); });
        }
        if (_round->evaluated && _round->derived) {
            settled(sequence, true, false);
        }
    }

    // One reply of the round in flight settled; once both did, the state follows, and a stale round makes one more.
    void settled(std::uint64_t sequence, bool evaluation, bool failed) {
        if (!_round || _round->round.sequence != sequence) {
            return;
        }
        (evaluation ? _round->evaluated : _round->derived) = true;
        _round->failed = _round->failed || failed;
        if (!_round->evaluated || !_round->derived) {
            return;
        }
        bool const stale = _round->stale;
        bool const anyFailed = _round->failed;
        _round.reset();
        _deriveState.set(anyFailed ? DeriveState::Failed : DeriveState::Current);
        if (stale) {
            startRound();
        }
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
    reactive::TimerHandle _debounce;
    async::CallbackScope _deriveScope;
    std::unique_ptr<reactive::Effect> _autoSubmit;
    std::unique_ptr<reactive::Effect> _acceptSent;
    std::unique_ptr<reactive::Effect> _refused;
};

}  // namespace morph::forms
