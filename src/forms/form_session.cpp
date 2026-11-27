// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/forms/engine/form_session.hpp>.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/forms/derive.hpp>
#include <morph/forms/engine/detail/text.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/rules.hpp>
#include <morph/forms/engine/values.hpp>
#include <morph/forms/i18n.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/render/i18n.hpp>
#include <morph/render/locale_format.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace morph::forms::detail::engine {

ValueContext const& SessionCore::values() const {
    static_cast<void>(_format.get());
    trackTexts();
    return _values;
}

std::string const& SessionCore::pattern(bool date) const {
    static_cast<void>(_format.get());
    return date ? _options.datePattern : _options.dateTimePattern;
}

void SessionCore::setFormat(render::NumericLocale locale, std::string datePattern, std::string dateTimePattern) {
    _options.locale = locale;
    _options.datePattern = std::move(datePattern);
    _options.dateTimePattern = std::move(dateTimePattern);
    _values.locale = locale;
    _format.set(_format.peek() + 1);
}

void SessionCore::setTranslations(render::TranslationProvider translations, std::string bcp47) {
    _options.translations = std::move(translations);
    _options.bcp47 = std::move(bcp47);
    _values.translations = _options.translations;
    _values.bcp47 = _options.bcp47;
    _texts.set(_texts.peek() + 1);
}

void SessionCore::markProgrammatic() const {
    if (_programmatic) {
        _programmatic();
    }
}

async::Completion<std::string> SessionCore::sharedChoices(std::string const& action, std::string request) {
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

}  // namespace morph::forms::detail::engine

namespace morph::forms {

// NOLINTNEXTLINE(misc-no-recursion): an Object's or ObjectArray row's members are fields, to the model's depth.
FieldState::FieldState(detail::engine::SessionCore& core MORPH_LIFETIMEBOUND,
                       FieldSpec const& spec MORPH_LIFETIMEBOUND, std::string path,
                       std::vector<std::unique_ptr<FieldState>> const* siblings, FieldState const* parent)
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
      _required{core.runtime(),
                [this] {
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
            _members.push_back(std::make_unique<FieldState>(core, child, _path + "." + child.name, &_members, this));
        }
    }
}

std::string FieldState::label() const {
    _core->trackTexts();
    return resolveSlot(i18n::FieldSlot::Label, _spec->title);
}

std::string FieldState::help() const {
    _core->trackTexts();
    return resolveSlot(i18n::FieldSlot::Help, _spec->help);
}

std::string FieldState::placeholder() const {
    if (!_placeholderOverride.empty()) {
        return _placeholderOverride;
    }
    if (_spec->placeholder.empty()) {
        return defaultPlaceholder(*_spec);
    }
    _core->trackTexts();
    return resolveSlot(i18n::FieldSlot::Placeholder, _spec->placeholder);
}

// NOLINTNEXTLINE(misc-no-recursion): an Object asks its members.
bool FieldState::blank() const {
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

RuleOperand FieldState::ruleOperand() const {
    Encoding const& encoding = _encoded.get();
    if (!encoding.has_value()) {
        return RuleOperand{};
    }
    if (!encoding->has_value()) {
        return RuleOperand{.state = RuleOperand::State::Invalid, .value = {}};
    }
    return RuleOperand{.state = RuleOperand::State::Valid, .value = (*encoding)->value};
}

void FieldState::narrowOptions(std::optional<std::vector<std::string>> keep) {
    if (_narrow.peek() != keep) {
        _narrow.set(std::move(keep));
    }
}

void FieldState::openOptions() {
    if (!_opened.peek()) {
        _opened.set(true);
    }
    retryOptions();
}

void FieldState::retryOptions() {
    if (_optionsQuery != nullptr && !_optionsQuery->pending() && _optionsQuery->error() != nullptr) {
        _optionsQuery->refetch();
    }
}

void FieldState::switchUnit(std::size_t index) {
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

FieldState* FieldState::member(std::string_view name) const noexcept {
    auto const found = std::ranges::find_if(_members, [&](auto const& entry) { return entry->spec().name == name; });
    return found == _members.end() ? nullptr : found->get();
}

std::uint64_t FieldState::addRow() {
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

std::uint64_t FieldState::insertRow(std::size_t position) {
    if (_spec->kind != FieldKind::ObjectArray || _spec->truncated) {
        return 0;
    }
    auto row = std::make_shared<RowState>(*_core, *_spec, _path, _nextRowKey++, this);
    row->start();
    std::uint64_t const key = row->key();
    _core->runtime().batch([&] {
        _rows.mutate([&](auto& rows) {
            auto const offset = static_cast<std::ptrdiff_t>(std::min(position, rows.size()));
            rows.insert(rows.begin() + offset, std::move(row));
        });
        _engaged.set(true);
    });
    return key;
}

std::vector<RemovedRow> FieldState::removeRows(std::span<std::uint64_t const> keys) {
    std::vector<RemovedRow> removed;
    std::vector<std::shared_ptr<RowState>> const& rows = _rows.peek();
    for (std::size_t at = 0; at < rows.size(); ++at) {
        if (std::ranges::contains(keys, rows.at(at)->key())) {
            removed.push_back(RemovedRow{.position = at, .row = rows.at(at)});
        }
    }
    if (!removed.empty()) {
        _rows.mutate([&](auto& held) {
            std::erase_if(held, [&](auto const& row) { return std::ranges::contains(keys, row->key()); });
        });
    }
    return removed;
}

void FieldState::restoreRows(std::vector<RemovedRow> rows) {
    if (rows.empty()) {
        return;
    }
    _core->runtime().batch([&] {
        _rows.mutate([&](auto& held) {
            for (RemovedRow& removed : rows) {
                auto const offset = static_cast<std::ptrdiff_t>(std::min(removed.position, held.size()));
                held.insert(held.begin() + offset, std::move(removed.row));
            }
        });
        _engaged.set(true);
    });
}

std::string FieldState::snapshot() const {
    std::string out;
    appendSnapshot(out);
    return out;
}

bool FieldState::encodesLike(detail::engine::Json const* value) const {
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

std::expected<void, std::string> FieldState::edit(detail::engine::Json const& value) {
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

// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
void FieldState::load(detail::engine::Json const* value) {
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

// NOLINTNEXTLINE(misc-no-recursion): an Object clears its members.
void FieldState::clear() {
    _text.set(std::string{});
    _unit.set(_spec->entryUnit);
    _engaged.set(false);
    for (auto const& entry : _members) {
        entry->clear();
    }
    _rows.set({});
}

// NOLINTNEXTLINE(misc-no-recursion): an Object starts its members.
void FieldState::start() {
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

// NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's cells are fields.
void FieldState::appendSnapshot(std::string& out) const {
    if (_spec->computed || _spec->derived) {
        out += "=;";
        return;
    }
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

std::string FieldState::resolveSlot(i18n::FieldSlot slot, std::string const& literal) const {
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

FieldState* FieldState::sibling(std::string_view name) const {
    if (_siblings != nullptr) {
        auto const found =
            std::ranges::find_if(*_siblings, [&](auto const& entry) { return entry->spec().name == name; });
        if (found != _siblings->end()) {
            return found->get();
        }
    }
    return _parent != nullptr ? _core->topLevel(name) : nullptr;
}

std::optional<std::string> FieldState::optionsRequest() const {
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

void FieldState::clearStaleSelection() {
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
std::expected<std::string, FieldError> FieldState::encodeMembers(
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

// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
Encoding FieldState::computeEncoding() const {
    if (_spec->truncated) {
        std::string_view const draft = detail::engine::trim(_text.get());
        bool const array = _spec->kind == FieldKind::ObjectArray;
        if (draft.empty() && !_engaged.get()) {
            return (array && _spec->required) ? Encoding{WireValue{.json = "[]", .value = ui::Value{ui::ValueList{}}}}
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

FormSession::FormSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, FormModel model, Submitter submit,
                         ChoiceFetcher choices, FormSessionOptions options)
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
      _mutation{runtime, [this](std::string body) { return _submit(_core->model().actionType(), std::move(body)); }},
      _attempts{runtime, std::uint64_t{0}},
      _loadGeneration{runtime, std::uint64_t{0}},
      _loads{runtime, std::uint64_t{0}},
      _refusals{runtime, std::map<std::string, Refused, std::less<>>{}},
      _deriveState{runtime, DeriveState::Current},
      _deriveFailing{runtime, false} {
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

FormSessionOptions const& FormSession::trackedOptions() const {
    _core->trackTexts();
    return _core->options();
}

std::optional<std::size_t> FormSession::rowIndexOf(std::string_view digits) {
    std::size_t parsed = 0;
    char const* const end = std::to_address(digits.end());
    auto const result = std::from_chars(std::to_address(digits.begin()), end, parsed);
    if (digits.empty() || result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
FieldState& FormSession::field(std::string_view path) const {
    FieldState* current = nullptr;
    std::span<std::unique_ptr<FieldState> const> scope = _fields;
    std::string_view rest = path;
    auto const missing = [&] { return std::out_of_range{"FormSession::field: no field '" + std::string{path} + "'"}; };
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

void FormSession::submit() {
    // A hook ends an edit not yet committed (a grid's open cell editor), so the body sent holds it.
    auto const hooks = _beforeSubmit;
    for (auto const& [hookId, hook] : hooks) {
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

void FormSession::prefill(std::string_view bodyJson) {
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

void FormSession::refill(std::string_view bodyJson) {
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a path and a JSON value; swapped, the path is not found.
void FormSession::assign(std::string_view path, std::string_view jsonValue) {
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
            reloaded(*_fields.at(index));
        }
    }
}

void FormSession::reset() {
    runtime().batch([&] {
        for (auto const& field : _fields) {
            field->clear();
        }
        _core->markProgrammatic();
    });
    markLoaded();
}

std::size_t FormSession::addBeforeSubmit(std::function<void()> hook) {
    std::size_t const hookId = ++_nextHook;
    _beforeSubmit.emplace_back(hookId, std::move(hook));
    return hookId;
}

bool FormSession::modified(std::string_view path) const {
    FieldState const* state = nullptr;
    try {
        state = &field(path);
    } catch (std::out_of_range const&) {
        return false;
    }
    return modified(*state);
}

bool FormSession::rowModified(std::string_view arrayPath, std::uint64_t key) const {
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

std::optional<std::string> FormSession::refusal(std::string_view path) const {
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

void FormSession::revert(std::string_view path) {
    if (auto const hash = path.rfind('#');
        hash != std::string_view::npos && path.find('.', hash) == std::string_view::npos) {
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

void FormSession::deriveWith(DeriveOptions options) {
    _debounce.cancel();
    _derive = std::move(options);
}

void FormSession::noteCommit() {
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

void FormSession::recalculate() {
    if (!_round && (_derive.evaluate || _derive.derive)) {
        _debounce.cancel();
        startRound();
    }
}

std::string FormSession::partialBody() const {
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

DeriveRound FormSession::captureRound() {
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

DeriveLoad FormSession::loadDerived(DeriveRound const& round, Derived const& reply) {
    DeriveLoad load;
    if (round.sequence < _derivedLoaded) {
        return load;
    }
    _derivedLoaded = round.sequence;
    std::vector<FieldState*> targets;
    std::vector<detail::engine::Json> values;
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
        auto dom = detail::engine::parseJson(value.value.str);
        if (!dom) {
            load.refused.push_back(value.path);
            continue;
        }
        targets.push_back(target.field);
        values.push_back(*std::move(dom));
    }
    writeKeepingLoaded(
        targets,
        [&] {
            for (std::size_t at = 0; at < targets.size(); ++at) {
                targets.at(at)->load(&values.at(at));
            }
        },
        false);
    noteLoads(targets.size());
    load.loaded = targets.size();
    return load;
}

DeriveLoad FormSession::loadDerived(DeriveRound const& round, std::string_view evaluatedBody) {
    DeriveLoad load;
    if (round.sequence < _evaluationLoaded) {
        return load;
    }
    auto const dom = detail::engine::parseJson(evaluatedBody);
    if (!dom || !dom->is_object()) {
        return load;
    }
    _evaluationLoaded = round.sequence;
    std::vector<std::pair<FieldState*, detail::engine::Json const*>> writes;
    runtime().untracked([&] {
        for (auto const& field : _fields) {
            collectComputed(round, *field, *dom, writes);
        }
    });
    std::vector<FieldState*> targets;
    targets.reserve(writes.size());
    for (auto const& write : writes) {
        targets.push_back(write.first);
    }
    writeKeepingLoaded(
        targets,
        [&] {
            for (auto const& [field, value] : writes) {
                field->load(value);
            }
        },
        false);
    noteLoads(writes.size());
    load.loaded = writes.size();
    return load;
}

void FormSession::switchUnitsAsLoaded(std::span<std::pair<FieldState*, std::size_t> const> switches) {
    if (switches.empty()) {
        return;
    }
    std::vector<FieldState*> targets;
    for (auto const& entry : switches) {
        targets.push_back(entry.first);
    }
    writeKeepingLoaded(targets, [&] {
        for (auto const& [field, index] : switches) {
            field->switchUnit(index);
        }
    });
}

std::string FormSession::groupTitle(std::size_t group) const {
    FieldGroupSpec const& spec = detail::engine::elementAt(_core->model().groups(), group);
    if (spec.implicit) {
        return {};
    }
    _core->trackTexts();
    return render::resolveText(_core->options().translations, _core->options().bcp47, std::nullopt,
                               i18n::groupKey(_core->model().actionType(), group), spec.title);
}

void FormSession::rememberSent() {
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

bool FormSession::endsEditOnSave(FieldState const& field) const {
    if (_core->model().submitMode() == SubmitMode::Explicit) {
        return true;
    }
    FieldSpec const& spec = field.spec();
    return !spec.truncated && spec.kind != FieldKind::Object && spec.kind != FieldKind::ObjectArray;
}

void FormSession::acceptSent() {
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

void FormSession::markLoaded() {
    _loaded.clear();
    for (auto const& field : _fields) {
        _loaded.push_back(field->snapshot());
    }
    _loadedCells.clear();
    for (auto const& field : _fields) {
        recordLoaded(*field);
    }
    _loadGeneration.set(_loadGeneration.peek() + 1);
    _loads.set(_loads.peek() + 1);
}

std::map<std::string, FormSession::Refused, std::less<>> FormSession::fieldErrorsOf(
    std::optional<std::string> const& reply) const {
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

void FormSession::takeFieldErrors(std::uint64_t successes, std::exception_ptr const& error) {
    if (successes == _errorsSeenSuccesses && error == _refusalSeen) {
        return;
    }
    bool const succeeded = successes != _errorsSeenSuccesses;
    _errorsSeenSuccesses = successes;
    _refusalSeen = error;
    _refusals.set(succeeded ? fieldErrorsOf(_mutation.lastResult()) : std::map<std::string, Refused, std::less<>>{});
}

// NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
void FormSession::recordLoaded(FieldState const& state) {
    FieldSpec const& spec = state.spec();
    if (spec.truncated) {
        return;
    }
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields, which the walk visits in turn.
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
            _loadedCells.insert_or_assign(state.path(), LoadedCell{.text = state.draft(), .unit = state.unit().get()});
        }
    });
}

void FormSession::noteLoads(std::size_t written) {
    if (written > 0) {
        _loads.set(_loads.peek() + 1);
    }
}

void FormSession::reloaded(FieldState const& state) {
    std::string const& name = state.path();
    std::erase_if(_loadedCells, [&name](auto const& entry) {
        std::string const& path = entry.first;
        return path == name || (path.size() > name.size() && path.starts_with(name) &&
                                (path.at(name.size()) == '.' || path.at(name.size()) == '#'));
    });
    recordLoaded(state);
    _loadGeneration.set(_loadGeneration.peek() + 1);
    _loads.set(_loads.peek() + 1);
}

// NOLINTNEXTLINE(misc-no-recursion): an Object's members are fields.
bool FormSession::modified(FieldState const& state) const {
    FieldSpec const& spec = state.spec();
    if (spec.truncated || spec.kind == FieldKind::Object || spec.kind == FieldKind::ObjectArray || spec.computed ||
        spec.derived) {
        return false;
    }
    static_cast<void>(_loadGeneration.get());
    auto const found = _loadedCells.find(state.path());
    // A member of a row added since the load.
    if (found == _loadedCells.end()) {
        return true;
    }
    return state.draft() != found->second.text || state.unit().get() != found->second.unit;
}

RowState const* FormSession::rowOf(std::string_view arrayPath, std::uint64_t key) const {
    FieldState const* array = nullptr;
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

bool FormSession::addedSinceLoad(RowState const& row) const {
    std::string const& first = row.fields().empty() ? std::string{} : row.fields().front()->path();
    // A member's path is `<array>#<key>.<member>`: the row's own entry is the part before the member.
    std::string const rowPath = first.substr(0, first.rfind('.'));
    return !_loadedCells.contains(rowPath);
}

// NOLINTNEXTLINE(misc-no-recursion): an Object's members are fields.
void FormSession::restoreLoaded(FieldState& state) {
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

std::optional<std::string> FormSession::assemble() const {
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

void FormSession::autoSubmit() {
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

// NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
std::optional<std::string> FormSession::partialOf(FieldState const& field) {
    FieldSpec const& spec = field.spec();
    // NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields, which the walk visits in turn.
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
    if (!encoding.has_value() && spec.blankAs == BlankAs::Empty && field.engaged().get()) {
        return std::string{R"("")"};
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
void FormSession::collectRowKeys(FieldState const& field,
                                 std::map<std::string, std::vector<std::uint64_t>, std::less<>>& out) {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per segment's form.
FormSession::Resolved FormSession::resolve(DeriveRound const& round, std::string_view path) const {
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
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

// NOLINTNEXTLINE(misc-no-recursion): an Object's members and a row's are fields.
void FormSession::collectComputed(DeriveRound const& round, FieldState& field, detail::engine::Json const& object,
                                  std::vector<std::pair<FieldState*, detail::engine::Json const*>>& out) {
    FieldSpec const& spec = field.spec();
    if (spec.truncated) {
        return;
    }
    detail::engine::Json const* const value = detail::engine::member(object, spec.name);
    if (spec.kind == FieldKind::Object) {
        if (value != nullptr && value->is_object()) {
            for (auto const& member : field.members()) {
                collectComputed(round, *member, *value, out);
            }
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
                for (auto const& member : (*row)->fields()) {
                    collectComputed(round, *member, elements.at(at), out);
                }
            }
        }
        return;
    }
    if (spec.computed) {
        out.emplace_back(&field, value);
    }
}

bool FormSession::holdsLoaded(FieldState const& field) const {
    auto const found = _loadedCells.find(field.path());
    return found != _loadedCells.end() && field.draft() == found->second.text &&
           field.unit().peek() == found->second.unit;
}

// NOLINTNEXTLINE(misc-no-recursion): a stale round's settling starts the next round, which settles through its replies in turn.
void FormSession::startRound() {
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

// NOLINTNEXTLINE(misc-no-recursion): a stale round's settling starts the next round, which settles through its replies in turn.
void FormSession::settled(std::uint64_t sequence, bool evaluation, bool failed) {
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
    _deriveFailing.set(anyFailed);
    if (stale) {
        startRound();
    }
}

}  // namespace morph::forms
