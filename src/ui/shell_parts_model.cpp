// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/shell_parts_model.hpp>
#include <morph/ui/shell_parts_view.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {
namespace {

/// @brief The texts the shell draws that no application declares, in English and German, by catalogue key.
/// @param key    A `morph.shell.*` key.
/// @param locale The session's locale; only its language matters.
/// @return The text; the key when it names none.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a message key, then a locale tag.
std::string builtinShellText(std::string_view key, std::string_view locale) {
    struct Entry {
        std::string_view key;
        std::string_view en;
        std::string_view de;
    };
    static constexpr std::array<Entry, 27> kRows = {{
        // NOLINTBEGIN(modernize-use-designated-initializers): the columns are key, English, German.
        {"morph.shell.context", "Scope", "Bereich"},
        {"morph.shell.search", "Search", "Suche"},
        {"morph.shell.searchResults", "Search results", "Suchergebnisse"},
        {"morph.shell.searchEmpty", "No results for", "Keine Ergebnisse für"},
        {"morph.shell.searchFailed", "could not be searched", "konnte nicht durchsucht werden"},
        {"morph.shell.searching", "Searching…", "Suche läuft…"},
        {"morph.shell.inbox", "waiting", "offen"},
        {"morph.shell.principal", "Signed in as", "Angemeldet als"},
        {"morph.shell.roles", "Your roles", "Ihre Rollen"},
        {"morph.shell.language", "Language", "Sprache"},
        {"morph.shell.formats", "Number and date format", "Zahlen- und Datumsformat"},
        {"morph.shell.countFailed", "count unavailable", "Anzahl nicht verfügbar"},
        {"morph.shell.close", "Close", "Schließen"},
        {"morph.shell.moduleClosed", "collapsed", "eingeklappt"},
        {"morph.shell.moduleOpen", "expanded", "ausgeklappt"},
        {"morph.shell.contextDirty", "Switching scope discards unsaved changes in {n} tabs",
         "Der Wechsel des Bereichs verwirft ungespeicherte Änderungen in {n} Tabs"},
        {"morph.shell.contextDiscard", "Discard and switch", "Verwerfen und wechseln"},
        {"morph.shell.contextCancel", "Cancel", "Abbrechen"},
        {"morph.shell.locale.en", "English", "Englisch"},
        {"morph.shell.locale.de", "German", "Deutsch"},
        {"morph.shell.navigation", "Navigation", "Navigation"},
        {"morph.shell.signOut", "Sign out", "Abmelden"},
        {"morph.shell.help", "Help", "Hilfe"},
        {"morph.shell.loading", "Loading…", "Lädt…"},
        {"morph.shell.connected", "Connected", "Verbunden"},
        {"morph.shell.reconnecting", "Reconnecting…", "Verbindung wird wiederhergestellt…"},
        {"morph.shell.offline", "Offline", "Offline"},
        // NOLINTEND(modernize-use-designated-initializers)
    }};
    bool const german = locale.starts_with("de");
    // NOLINTNEXTLINE(readability-qualified-auto): an array iterator is a pointer on some standard libraries only.
    auto const found = std::ranges::find_if(kRows, [key](Entry const& row) { return row.key == key; });
    if (found == kRows.end()) {
        return std::string{key};
    }
    return std::string{german ? found->de : found->en};
}

}  // namespace
}  // namespace morph::ui

namespace morph::ui {

std::string shellText(std::string_view key, FormatContext const& format) {
    if (format.translations) {
        if (auto const pattern = format.translations(key, format.locale)) {
            return *pattern;
        }
    }
    return builtinShellText(key, format.locale);
}

ShellLabels shellLabels(FormatContext const& format) {
    return ShellLabels{.scope = shellText("morph.shell.context", format),
                       .search = shellText("morph.shell.search", format),
                       .searchResults = shellText("morph.shell.searchResults", format),
                       .searchEmpty = shellText("morph.shell.searchEmpty", format),
                       .searchFailed = shellText("morph.shell.searchFailed", format),
                       .searching = shellText("morph.shell.searching", format),
                       .waiting = shellText("morph.shell.inbox", format),
                       .signedInAs = shellText("morph.shell.principal", format),
                       .roles = shellText("morph.shell.roles", format),
                       .language = shellText("morph.shell.language", format),
                       .formats = shellText("morph.shell.formats", format),
                       .countFailed = shellText("morph.shell.countFailed", format),
                       .close = shellText("morph.shell.close", format),
                       .signOut = shellText("morph.shell.signOut", format),
                       .help = shellText("morph.shell.help", format),
                       .connected = shellText("morph.shell.connected", format),
                       .reconnecting = shellText("morph.shell.reconnecting", format),
                       .offline = shellText("morph.shell.offline", format)};
}

ShellPartsModel::ShellPartsModel(Session& session MORPH_LIFETIMEBOUND, AppRoot& root MORPH_LIFETIMEBOUND,
                                 ShellParts const& parts MORPH_LIFETIMEBOUND, ShellModelOptions options)
    : _session{&session},
      _root{&root},
      _parts{&parts},
      _options{std::move(options)},
      _chosen{session.runtime(), Value{}},
      _answered{session.runtime(), !parts.context.has_value()},
      _searchText{session.runtime(), std::string{}},
      _optionList{session.runtime(), std::vector<Option>{}},
      _connection{session.runtime(), std::nullopt} {
    _session->setContextHandler([this](Value const& value) { return requestContext(value); });
    checkDeclaration();
    if (_parts->context) {
        _follow = std::make_unique<reactive::Effect>(_session->runtime(), [this] { followOptions(); });
    }
}

ShellPartsModel::~ShellPartsModel() {
    _follow.reset();
    _session->setContextHandler({});
    _session->setContextOffered(false);
    _session->setContext(Value{});
}

ShellPartsView ShellPartsModel::view() const {
    FormatContext const& format = _session->formatting();
    ShellPartsView out;
    out.labels = shellLabels(format);
    out.context = contextView(format);
    out.search = searchView(format);
    out.inbox = inboxView(format);
    out.principal = principalView(format);
    out.brand = brandView(format);
    out.help = helpView();
    out.connection = _connection.get();
    return out;
}

std::vector<ShellPartsModel::ModuleState> ShellPartsModel::moduleCounts() const {
    std::vector<ModuleState> out;
    for (std::size_t index = 0; index < _parts->modules.size(); ++index) {
        ModuleCount const& declared = _parts->modules.at(index);
        AppRoot::QueryRead const read = _root->query(shell_names::count(index));
        if (read.denied || moduleHidden(declared.module)) {
            continue;
        }
        ModuleState state{.module = declared.module, .count = std::nullopt, .failed = read.failed};
        if (read.failed) {
            problem("shell count " + shell_names::count(index) + ": the read failed");
        }
        if (!read.failed && !read.pending) {
            state.count = countOf(read.value, declared.countField, shell_names::count(index));
            state.failed = !state.count.has_value() && !read.value.isNull();
        }
        out.push_back(std::move(state));
    }
    return out;
}

void ShellPartsModel::chooseContext(std::size_t index) {
    std::vector<Option> const& options = _optionList.peek();
    if (index < options.size()) {
        static_cast<void>(requestContext(options.at(index).value));
    }
}

bool ShellPartsModel::requestContext(Value const& value) {
    std::vector<Option> const& options = _optionList.peek();
    auto const found = std::ranges::find_if(options, [&value](Option const& option) { return option.value == value; });
    if (found == options.end()) {
        return false;
    }
    if (value == _chosen.peek()) {
        return true;
    }
    std::size_t const dirty = _options.dirtyTabs ? _options.dirtyTabs() : 0;
    if (dirty == 0 || !_options.confirmDiscard) {
        apply(value);
        return true;
    }
    _options.confirmDiscard(dirty, [this, value, alive = std::weak_ptr<bool>{_alive}](bool discard) {
        if (alive.expired() || !discard) {
            return;
        }
        if (_options.closeDirtyTabs) {
            _options.closeDirtyTabs();
        }
        apply(value);
    });
    return true;
}

void ShellPartsModel::setSearchText(std::string const& text) {
    _searchText.set(text);
    static_cast<void>(_root->setState(shell_names::kSearchText, Value{text}));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the source index, then the hit index within it.
std::optional<ShellPartsModel::Target> ShellPartsModel::hit(std::size_t source, std::size_t hit) const {
    std::vector<Target> const hits = sourceHits(source);
    if (hit >= hits.size()) {
        return std::nullopt;
    }
    return hits.at(hit);
}

std::vector<ShellPartsModel::Target> ShellPartsModel::allHits() const {
    std::vector<Target> out;
    for (std::size_t source = 0; source < _parts->search.size(); ++source) {
        for (Target& target : sourceHits(source)) {
            if (out.size() < kSearchOpenAllLimit) {
                out.push_back(std::move(target));
            }
        }
    }
    return out;
}

std::optional<ShellPartsModel::Target> ShellPartsModel::helpScreen() const {
    if (!_parts->help || _parts->help->screen.empty()) {
        return std::nullopt;
    }
    return Target{.screen = _parts->help->screen, .params = helpParams()};
}

bool ShellPartsModel::runHelp() {
    if (!_parts->help || _parts->help->action.empty()) {
        return false;
    }
    return _root->runMutation(shell_names::kHelp, Value{helpParams()}, [this](bool succeeded) {
        if (!succeeded) {
            problem("shell help: the action " + _parts->help->action + " failed");
        }
    });
}

void ShellPartsModel::chooseLocale(std::size_t index) {
    std::vector<std::string> const tags = localeTags();
    if (index < tags.size()) {
        _session->setLocale(tags.at(index));
    }
}

void ShellPartsModel::chooseFormat(std::size_t index) {
    if (_parts->principal && index < _parts->principal->formats.size()) {
        _session->setFormatLocale(_parts->principal->formats.at(index));
    }
}

void ShellPartsModel::checkDeclaration() const {
    auto const record = [this](ModuleCount const& count, std::string const& part) {
        if (_options.replyIsRecord && !_options.replyIsRecord(count.action)) {
            problem("shell " + part + ": the reply of " + count.action +
                    " is not a registered record, so the count is read on mount and on a context change only");
        }
    };
    for (std::size_t index = 0; index < _parts->modules.size(); ++index) {
        if (!moduleHidden(_parts->modules.at(index).module)) {
            record(_parts->modules.at(index), "count " + shell_names::count(index));
        }
    }
    if (_parts->inbox && !screenHidden(_parts->inbox->screen)) {
        for (std::size_t index = 0; index < _parts->inbox->sources.size(); ++index) {
            record(_parts->inbox->sources.at(index), "inbox " + shell_names::inbox(index));
        }
    }
}

void ShellPartsModel::problem(std::string const& text) const {
    if (_options.onProblem && _reported.insert(text).second) {
        _options.onProblem(text);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per value shape.
std::string ShellPartsModel::describe(Value const& value, FormatContext const& format) {
    if (ValueRecord const* const record = value.asRecord()) {
        auto const key = record->find("t");
        if (key != record->end() && key->second.asString() != nullptr) {
            std::string pattern = *key->second.asString();
            bool translated = false;
            if (format.translations) {
                if (auto const text = format.translations(pattern, format.locale)) {
                    pattern = *text;
                    translated = true;
                }
            }
            if (auto const fallback = record->find("fallback");
                !translated && fallback != record->end() && fallback->second.asString() != nullptr) {
                pattern = *fallback->second.asString();
            }
            if (auto const args = record->find("args"); args != record->end()) {
                if (ValueRecord const* const named = args->second.asRecord()) {
                    for (auto const& [name, argument] : *named) {
                        std::string const token = "{$" + name + "}";
                        for (std::size_t at = pattern.find(token); at != std::string::npos;
                             at = pattern.find(token, at)) {
                            std::string const shown = detail::textOf(argument, format);
                            pattern.replace(at, token.size(), shown);
                            at += shown.size();
                        }
                    }
                }
            }
            return pattern;
        }
    }
    return detail::textOf(value, format);
}

std::string ShellPartsModel::translate(std::string const& textOrKey, FormatContext const& format) {
    if (!textOrKey.empty() && format.translations) {
        if (auto const text = format.translations(textOrKey, format.locale)) {
            return *text;
        }
    }
    return textOrKey;
}

std::string ShellPartsModel::memberText(Value const& record, std::string const& name, FormatContext const& format) {
    if (name.empty()) {
        return {};
    }
    Value const* const found = record.field(name);
    return found == nullptr ? std::string{} : describe(*found, format);
}

std::optional<long long> ShellPartsModel::countOf(Value const& reply, std::string const& field,
                                                  std::string const& query) const {
    Value const* const found = reply.field(field);
    std::optional<std::int64_t> number;
    if (found != nullptr) {
        if (std::int64_t const* const plain = found->asInt()) {
            number = *plain;
        } else if (Decimal const* const exact = found->asDecimal();
                   exact != nullptr && exact->value.denominator == 1) {
            number = exact->value.numerator;
        } else if (Quantity const* const quantity = found->asQuantity();
                   quantity != nullptr && quantity->value.denominator == 1) {
            number = quantity->value.numerator;
        }
    }
    if (!number || *number < 0) {
        if (!reply.isNull()) {
            problem("shell count " + query + ": the reply has no non-negative integer `" + field + "`");
        }
        return std::nullopt;
    }
    return static_cast<long long>(*number);
}

std::vector<ShellPartsModel::Option> ShellPartsModel::parseOptions(AppRoot::QueryRead const& read,
                                                                   FormatContext const& format) {
    std::vector<Option> out;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): parseOptions runs only once a context switch is declared.
    ContextSwitch const& declared = *_parts->context;
    Value const* const list = read.value.field(declared.optionsField);
    ValueList const* const items = list == nullptr ? nullptr : list->asList();
    if (items == nullptr) {
        if (!read.value.isNull()) {
            problem("shell context: the reply has no list `" + declared.optionsField + "`");
        }
        return out;
    }
    for (Value const& item : *items) {
        Value const* const value = item.field(declared.valueField);
        if (value == nullptr || (value->asString() == nullptr && value->asInt() == nullptr)) {
            problem("shell context: an option has no string or integer `" + declared.valueField + "`");
            continue;
        }
        out.push_back(Option{.value = *value,
                             .shown = ShellOptionView{.label = memberText(item, declared.labelField, format),
                                                      .detail = memberText(item, declared.detailField, format),
                                                      .group = memberText(item, declared.groupField, format)}});
    }
    return out;
}

void ShellPartsModel::followOptions() {
    AppRoot::QueryRead const read = _root->query(shell_names::kContext);
    FormatContext const& format = _session->formatting();
    std::vector<Option> options = parseOptions(read, format);
    bool const settled = (!read.pending && !read.value.isNull()) || read.failed || read.denied;
    if (read.failed && !read.denied) {
        problem("shell context: the scope switcher's read failed");
    }
    _session->runtime().untracked([&] {
        _optionList.set(std::move(options));
        std::vector<Option> const& held = _optionList.peek();
        _session->setContextOffered(!held.empty());
        if (held.empty()) {
            if (!_chosen.peek().isNull()) {
                apply(Value{});
            }
        } else {
            bool const present =
                std::ranges::any_of(held, [this](Option const& option) { return option.value == _chosen.peek(); });
            if (!present) {
                apply(held.front().value);
            }
        }
        if (settled && !_answered.peek()) {
            _answered.set(true);
        }
    });
}

void ShellPartsModel::apply(Value const& value) {
    _chosen.set(value);
    _session->setContext(value);
}

ShellContextView ShellPartsModel::contextView(FormatContext const& format) const {
    ShellContextView out;
    if (!_parts->context) {
        return out;
    }
    // A switcher the server refuses is the authoriser working: it is left out.
    if (_root->query(shell_names::kContext).denied) {
        return out;
    }
    Value const chosen = _chosen.get();
    std::vector<Option> const& options = _optionList.get();
    out.shown = true;
    for (std::size_t index = 0; index < options.size(); ++index) {
        out.options.push_back(options.at(index).shown);
        if (options.at(index).value == chosen) {
            out.chosen = index;
        }
    }
    out.enabled = !out.options.empty();
    static_cast<void>(format);
    return out;
}

bool ShellPartsModel::isHit(Value const& item) {
    Value const* const screen = item.field("screen");
    return screen != nullptr && screen->asString() != nullptr && item.field("title") != nullptr;
}

std::vector<ShellPartsModel::Target> ShellPartsModel::sourceHits(std::size_t source) const {
    std::vector<Target> out;
    if (source >= _parts->search.size() || _searchText.peek().empty()) {
        return out;
    }
    SearchSource const& declared = _parts->search.at(source);
    AppRoot::QueryRead const read = _root->query(shell_names::search(source));
    // The hits of a source the view does not draw are not the person's to open.
    if (read.denied || read.failed || moduleHidden(declared.module)) {
        return out;
    }
    Value const* const list = read.value.field(declared.resultsField);
    ValueList const* const items = list == nullptr ? nullptr : list->asList();
    if (items == nullptr) {
        return out;
    }
    for (Value const& item : *items) {
        if (!isHit(item) || out.size() >= kSearchHitsPerSource) {
            continue;
        }
        Value const* const screen = item.field("screen");
        Target target{.screen = *screen->asString(), .params = {}};
        if (Value const* const params = item.field("params"); params != nullptr && params->asRecord() != nullptr) {
            target.params = *params->asRecord();
        }
        out.push_back(std::move(target));
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per declared search source.
ShellSearchView ShellPartsModel::searchView(FormatContext const& format) const {
    ShellSearchView out;
    if (_parts->search.empty()) {
        return out;
    }
    out.shown = true;
    out.hint = translate(_parts->searchHint, format);
    if (_searchText.get().empty()) {
        return out;
    }
    for (std::size_t source = 0; source < _parts->search.size(); ++source) {
        SearchSource const& declared = _parts->search.at(source);
        AppRoot::QueryRead const read = _root->query(shell_names::search(source));
        if (read.denied || moduleHidden(declared.module)) {
            continue;
        }
        ShellSourceView shown;
        shown.source = source;
        std::string title = translate(declared.title, format);
        if (title.empty() && _options.moduleLabel) {
            title = _options.moduleLabel(declared.module);
        }
        shown.title = title.empty() ? declared.module : title;
        shown.pending = read.pending;
        shown.failed = read.failed;
        std::string const query = shell_names::search(source);
        if (read.failed) {
            problem("shell search " + query + ": the read failed");
        } else {
            Value const* const list = read.value.field(declared.resultsField);
            ValueList const* const items = list == nullptr ? nullptr : list->asList();
            if (items == nullptr && !read.value.isNull()) {
                problem("shell search " + query + ": the reply has no list `" + declared.resultsField + "`");
            }
            for (Value const& item : items == nullptr ? ValueList{} : *items) {
                if (!isHit(item)) {
                    problem("shell search " + query + ": a hit has no string `screen` or no `title` and is dropped");
                    continue;
                }
                if (shown.hits.size() < kSearchHitsPerSource) {
                    shown.hits.push_back(ShellHitView{.kind = memberText(item, "kind", format),
                                                      .title = describe(*item.field("title"), format),
                                                      .detail = memberText(item, "detail", format)});
                }
            }
        }
        out.sources.push_back(std::move(shown));
    }
    return out;
}

ShellInboxView ShellPartsModel::inboxView(FormatContext const& format) const {
    ShellInboxView out;
    if (!_parts->inbox || screenHidden(_parts->inbox->screen)) {
        return out;
    }
    static_cast<void>(format);
    long long sum = 0;
    bool answered = false;
    bool permitted = false;
    for (std::size_t index = 0; index < _parts->inbox->sources.size(); ++index) {
        ModuleCount const& declared = _parts->inbox->sources.at(index);
        AppRoot::QueryRead const read = _root->query(shell_names::inbox(index));
        if (read.denied) {
            continue;
        }
        permitted = true;
        if (read.failed) {
            out.failed = true;
            problem("shell inbox " + shell_names::inbox(index) + ": the read failed");
        } else if (!read.pending) {
            if (auto const count = countOf(read.value, declared.countField, shell_names::inbox(index))) {
                sum += *count;
                answered = true;
            } else if (!read.value.isNull()) {
                out.failed = true;
            }
        }
    }
    out.shown = permitted;
    out.title = _options.screenTitle ? _options.screenTitle(_parts->inbox->screen) : _parts->inbox->screen;
    if (answered) {
        out.count = sum;
    }
    return out;
}

std::vector<std::string> ShellPartsModel::localeTags() const {
    if (!_parts->principal) {
        return {};
    }
    if (!_parts->principal->locales.empty() || !_options.catalogueLocales) {
        return _parts->principal->locales;
    }
    return _options.catalogueLocales();
}

ShellBrandView ShellPartsModel::brandView(FormatContext const& format) const {
    ShellBrandView out;
    if (!_parts->brand) {
        return out;
    }
    out.shown = true;
    out.title = translate(_parts->brand->title, format);
    out.icon = _parts->brand->icon;
    out.version = translate(_parts->brand->version, format);
    return out;
}

ShellHelpView ShellPartsModel::helpView() const {
    ShellHelpView out;
    out.shown = _parts->help && (_parts->help->screen.empty() || !screenHidden(_parts->help->screen));
    return out;
}

ValueRecord ShellPartsModel::helpParams() const {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): helpParams runs only while a help view is declared.
    auto const text = glz::write<::morph::wire::detail::EscapingWriteOpts{}>(_parts->help->params);
    auto const json = text ? parseJson(*text) : std::unexpected(std::string{"unwritable"});
    auto const value = json ? valueFromJson(*json) : std::unexpected(json.error());
    ValueRecord const* const record = value ? value->asRecord() : nullptr;
    return record == nullptr ? ValueRecord{} : *record;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per principal facet.
ShellPrincipalView ShellPartsModel::principalView(FormatContext const& format) const {
    ShellPrincipalView out;
    if (!_parts->principal) {
        return out;
    }
    PrincipalMenu const& declared = *_parts->principal;
    AppRoot::QueryRead const read = _root->query(shell_names::kPrincipal);
    if (read.denied) {
        return out;
    }
    out.shown = true;
    if (read.failed) {
        problem("shell principal: the read failed");
    } else if (!read.value.isNull() && read.value.field(declared.nameField) == nullptr) {
        problem("shell principal: the reply has no `" + declared.nameField + "`");
    }
    out.name = memberText(read.value, declared.nameField, format);
    out.initials = shellInitials(out.name);
    out.subtitle = memberText(read.value, declared.subtitleField, format);
    out.detail = memberText(read.value, declared.detailField, format);
    if (Value const* const roles = declared.rolesField.empty() ? nullptr : read.value.field(declared.rolesField)) {
        if (ValueList const* const list = roles->asList()) {
            for (Value const& role : *list) {
                if (role.asRecord() != nullptr) {
                    out.roles.push_back(ShellRoleView{.label = memberText(role, "label", format),
                                                      .detail = memberText(role, "detail", format)});
                } else {
                    out.roles.push_back(ShellRoleView{.label = describe(role, format), .detail = {}});
                }
            }
        }
    }
    auto const choices = [&format](std::vector<std::string> const& tags) {
        std::vector<ShellChoiceView> list;
        for (std::string const& tag : tags) {
            list.push_back(ShellChoiceView{.tag = tag, .label = shellText("morph.shell.locale." + tag, format)});
            if (list.back().label == "morph.shell.locale." + tag) {
                list.back().label = tag;
            }
        }
        return list;
    };
    out.locales = choices(localeTags());
    for (std::size_t index = 0; index < out.locales.size(); ++index) {
        if (out.locales.at(index).tag == format.locale) {
            out.chosenLocale = index;
        }
    }
    out.formats = choices(declared.formats);
    std::string const chosenFormat = _session->formatLocale();
    for (std::size_t index = 0; index < out.formats.size(); ++index) {
        if (out.formats.at(index).tag == chosenFormat) {
            out.chosenFormat = index;
        }
    }
    return out;
}
}  // namespace morph::ui
