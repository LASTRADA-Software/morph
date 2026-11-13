// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <glaze/glaze.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// @file
/// @brief `morph::ui::ShellParts`: the parts of an application's shell that no screen can draw (a scope switcher, a
///        count per module, one search, an inbox button and the signed-in person's menu), declared as data in the app
///        document and lowered to queries of its scope.
///
/// Specified in `docs/spec/ui/shell_parts.md`.

namespace morph::ui {

/// @brief The pause, in milliseconds, between the last edit of the search field and its request.
inline constexpr int kSearchPause = 200;
/// @brief The most hits of one source the search panel draws.
inline constexpr std::size_t kSearchHitsPerSource = 8;
/// @brief The most tabs a Shift Enter in the search opens.
inline constexpr std::size_t kSearchOpenAllLimit = 10;
/// @brief The largest count drawn as a number; a larger one is drawn `99+`.
inline constexpr long long kCountCap = 99;
/// @brief The widest the top bar's search field stretches to, in units.
inline constexpr int kShellSearchWidth = 48;

/// @brief The scope switcher: the model action that lists the scopes a person may choose.
struct ContextSwitch {
    /// @brief The model's registered type id.
    std::string model;
    /// @brief The read action's id.
    std::string action;
    /// @brief The request member that receives the session's locale tag; empty sends none.
    std::string localeField;
    /// @brief The reply member holding the list of options.
    std::string optionsField;
    /// @brief The option member holding its label.
    std::string labelField;
    /// @brief The option member holding its value: a string or an integer.
    std::string valueField;
    /// @brief The option member holding a second line; empty for none.
    std::string detailField;
    /// @brief The option member holding the heading the option sits under; empty for none.
    std::string groupField;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ContextSwitch const&) const = default;
};

/// @brief A count of a module's open work: one read action of the module's model.
struct ModuleCount {
    /// @brief The `CatalogEntry::module` the count belongs to.
    std::string module;
    /// @brief The model's registered type id.
    std::string model;
    /// @brief The read action's id.
    std::string action;
    /// @brief The reply member holding the count, a non-negative integer.
    std::string countField;
    /// @brief The request member that receives the chosen context; empty sends none.
    std::string contextField;
    /// @brief The request member that receives the session's locale tag; empty sends none.
    std::string localeField;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ModuleCount const&) const = default;
};

/// @brief A module the global search reads.
struct SearchSource {
    /// @brief The `CatalogEntry::module` whose records the source finds.
    std::string module;
    /// @brief The heading over the source's hits: a catalogue key or text; empty uses the module's label.
    std::string title;
    /// @brief The model's registered type id.
    std::string model;
    /// @brief The read action's id.
    std::string action;
    /// @brief The request member that receives the typed text.
    std::string queryField;
    /// @brief The reply member holding the list of hits.
    std::string resultsField;
    /// @brief The request member that receives the chosen context; empty sends none.
    std::string contextField;
    /// @brief The request member that receives the session's locale tag; empty sends none.
    std::string localeField;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(SearchSource const&) const = default;
};

/// @brief The inbox button: the screen it opens and the counts it sums.
struct InboxEntry {
    /// @brief The screen the button opens.
    std::string screen;
    /// @brief The counts whose sum the button shows.
    std::vector<ModuleCount> sources;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(InboxEntry const&) const = default;
};

/// @brief The signed-in person's menu.
struct PrincipalMenu {
    /// @brief The model's registered type id.
    std::string model;
    /// @brief The read action's id.
    std::string action;
    /// @brief The request member that receives the chosen context; empty sends none.
    std::string contextField;
    /// @brief The request member that receives the session's locale tag; empty sends none.
    std::string localeField;
    /// @brief The reply member holding the person's name.
    std::string nameField;
    /// @brief The reply member holding a detail line for the menu; empty for none.
    std::string detailField;
    /// @brief The reply member holding the line under the name in the trigger; empty for none.
    std::string subtitleField;
    /// @brief The reply member holding the list of roles, each a string or `{label, detail}`.
    std::string rolesField;
    /// @brief The language tags the menu offers; empty derives them from the bundle's message catalogues.
    std::vector<std::string> locales;
    /// @brief The tags whose number, money and date facts the person may choose apart from the language; empty hides
    ///        the choice.
    std::vector<std::string> formats;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(PrincipalMenu const&) const = default;
};

/// @brief The application's product mark, at the top of the sidebar, and its version in the sidebar's foot.
struct BrandMark {
    /// @brief The product's name: a catalogue key or text.
    std::string title;
    /// @brief The name of an icon in the theme's icon set, drawn in the mark's square; empty draws the title's first
    ///        letter there.
    std::string icon;
    /// @brief The version string drawn beside the connection state in the sidebar's foot: a catalogue key or text;
    ///        empty draws none.
    std::string version;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(BrandMark const&) const = default;
};

/// @brief A parameter of the help entry: a string, a number or a flag.
using HelpValue = std::variant<std::string, double, bool>;

/// @brief The help button: what it opens, which is a screen or an action of a model.
///
/// Exactly one of `screen` and `action` is given. The screen opens as a tab, with `params` as its params; the action
/// runs with `params` as its request, and an action must be a stateless model's, as every part's read is.
struct HelpEntry {
    /// @brief The screen the button opens; empty when the button runs an action.
    std::string screen;
    /// @brief The model's registered type id, for an action.
    std::string model;
    /// @brief The action's registered id; empty when the button opens a screen.
    std::string action;
    /// @brief The screen's params, or the action's request members, by name.
    std::map<std::string, HelpValue> params;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(HelpEntry const&) const = default;
};

/// @brief The parts of a shell an application declares.
struct ShellParts {
    /// @brief The scope switcher.
    std::optional<ContextSwitch> context;
    /// @brief A count per module.
    std::vector<ModuleCount> modules;
    /// @brief The modules the global search reads.
    std::vector<SearchSource> search;
    /// @brief The inbox button.
    std::optional<InboxEntry> inbox;
    /// @brief The signed-in person's menu.
    std::optional<PrincipalMenu> principal;
    /// @brief The search field's placeholder: a catalogue key or text.
    std::string searchHint;
    /// @brief The product mark and version.
    std::optional<BrandMark> brand;
    /// @brief The help button.
    std::optional<HelpEntry> help;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellParts const&) const = default;

    /// @brief Whether any part is declared.
    /// @return `false` for a default `ShellParts`.
    [[nodiscard]] bool any() const noexcept {
        return context || !modules.empty() || !search.empty() || inbox || principal || brand || help;
    }
};

/// @brief The names a lowered part takes in the app document's scope; a document that declares one is refused.
namespace shell_names {
/// @brief The state holding the search field's text.
inline constexpr std::string_view kSearchText = "shellSearchText";
/// @brief The scope switcher's query.
inline constexpr std::string_view kContext = "shellContext";
/// @brief The principal's query.
inline constexpr std::string_view kPrincipal = "shellPrincipal";
/// @brief The mutation the help button runs, when it names an action.
inline constexpr std::string_view kHelp = "shellHelp";

/// @brief The query of the module count at @p index of `ShellParts::modules`.
/// @param index The place.
/// @return `shellCount<index>`.
[[nodiscard]] inline std::string count(std::size_t index) { return "shellCount" + std::to_string(index); }
/// @brief The query of the search source at @p index of `ShellParts::search`.
/// @param index The place.
/// @return `shellSearch<index>`.
[[nodiscard]] inline std::string search(std::size_t index) { return "shellSearch" + std::to_string(index); }
/// @brief The query of the inbox source at @p index of `InboxEntry::sources`.
/// @param index The place.
/// @return `shellInbox<index>`.
[[nodiscard]] inline std::string inbox(std::size_t index) { return "shellInbox" + std::to_string(index); }

/// @brief Whether a name is one the shell's parts declare: the search text, the switcher and the person's queries, and
///        a model alias, count, search or inbox query followed by its number.
/// @param name A name declared in the app document's scope.
/// @return True when the lowering of the parts would declare it.
[[nodiscard]] inline bool reserved(std::string_view name) {
    if (name == kSearchText || name == kContext || name == kPrincipal || name == kHelp) {
        return true;
    }
    for (std::string_view const prefix : {"shellModel", "shellCount", "shellSearch", "shellInbox"}) {
        if (name.starts_with(prefix)) {
            std::string_view const digits = name.substr(prefix.size());
            if (!digits.empty() && std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace shell_names

/// @brief A count as a badge shows it: the number, or `99+` above `kCountCap`.
/// @param count The count, not negative.
/// @return Its text.
[[nodiscard]] inline std::string shellCountText(long long count) {
    return count > kCountCap ? std::to_string(kCountCap) + "+" : std::to_string(count);
}

namespace detail {

/// @brief A JSON string literal.
/// @param text The text.
/// @return It, quoted and escaped.
[[nodiscard]] inline std::string jsonString(std::string_view text) {
    return glz::write_json(std::string{text}).value_or("\"\"");
}

/// @brief A request body expression: an object of the declared request fields.
/// @param members The `(member, expression)` pairs; a pair with an empty member is left out.
/// @return `{"obj": {...}}`, or an empty string when no member remains.
[[nodiscard]] inline std::string bodyOf(std::vector<std::pair<std::string, std::string>> const& members) {
    std::string out;
    for (auto const& [name, expression] : members) {
        if (name.empty()) {
            continue;
        }
        out += (out.empty() ? "" : ",") + jsonString(name) + ":" + expression;
    }
    return out.empty() ? out : R"({"obj":{)" + out + "}}";
}

}  // namespace detail

/// @brief Lowers the parts to the declarations of an app document's scope: one shared model alias per distinct model,
///        one query per read, and the search text as state.
/// @param parts The parts.
/// @return A JSON object with `models`, `state` and `queries`, in the document's own shape.
[[nodiscard]] inline std::string lowerShellParts(ShellParts const& parts) {
    std::map<std::string, std::string, std::less<>> aliases;
    std::string models;
    auto alias = [&](std::string const& model) {
        auto const found = aliases.find(model);
        if (found != aliases.end()) {
            return found->second;
        }
        std::string const name = "shellModel" + std::to_string(aliases.size());
        aliases.emplace(model, name);
        models += (models.empty() ? "" : ",") + detail::jsonString(name) + ":{\"type\":" + detail::jsonString(model) +
                  ",\"instance\":\"shared\"}";
        return name;
    };
    std::string queries;
    auto add = [&](std::string const& name, std::string const& model, std::string const& action,
                   std::string const& body, std::string const& extra) {
        std::string const owner = alias(model);
        queries += (queries.empty() ? "" : ",") + detail::jsonString(name) +
                   ":{\"model\":" + detail::jsonString(owner) + ",\"action\":" + detail::jsonString(action) +
                   (body.empty() ? "" : ",\"body\":" + body) + extra + "}";
    };
    auto env = [](std::string_view name) { return std::string{R"({"env":")"} + std::string{name} + "\"}"; };
    std::string const engagedContext = R"({"fn":"engaged","args":[{"env":"context"}]})";
    auto read = [&](std::string const& name, std::string const& model, std::string const& action,
                    std::string const& contextField, std::string const& localeField) {
        // A read that sends the scope waits until there is one, so nothing is asked unscoped.
        std::string const when =
            parts.context && !contextField.empty() ? ",\"when\":" + engagedContext : std::string{};
        add(name, model, action, detail::bodyOf({{contextField, env("context")}, {localeField, env("locale")}}), when);
    };
    if (parts.context) {
        add(std::string{shell_names::kContext}, parts.context->model, parts.context->action,
            detail::bodyOf({{parts.context->localeField, env("locale")}}), std::string{});
    }
    for (std::size_t index = 0; index < parts.modules.size(); ++index) {
        ModuleCount const& count = parts.modules.at(index);
        read(shell_names::count(index), count.model, count.action, count.contextField, count.localeField);
    }
    for (std::size_t index = 0; index < parts.search.size(); ++index) {
        SearchSource const& source = parts.search.at(index);
        std::string const text = R"({"ref":")" + std::string{shell_names::kSearchText} + "\"}";
        std::string const typed = R"({"fn":"ne","args":[)" + text + R"(,""]})";
        std::string const searchWhen = parts.context && !source.contextField.empty()
                                           ? R"({"fn":"and","args":[)" + typed + "," + engagedContext + "]}"
                                           : typed;
        add(shell_names::search(index), source.model, source.action,
            detail::bodyOf({{source.queryField, text},
                            {source.contextField, env("context")},
                            {source.localeField, env("locale")}}),
            ",\"when\":" + searchWhen + ",\"debounce\":" + std::to_string(kSearchPause));
    }
    if (parts.inbox) {
        for (std::size_t index = 0; index < parts.inbox->sources.size(); ++index) {
            ModuleCount const& count = parts.inbox->sources.at(index);
            read(shell_names::inbox(index), count.model, count.action, count.contextField, count.localeField);
        }
    }
    if (parts.principal) {
        PrincipalMenu const& menu = *parts.principal;
        read(std::string{shell_names::kPrincipal}, menu.model, menu.action, menu.contextField, menu.localeField);
    }
    // The button runs the action with its params as the request, so the mutation declares only where it goes.
    std::string mutations;
    if (parts.help && !parts.help->action.empty()) {
        mutations = ",\"mutations\":{" + detail::jsonString(shell_names::kHelp) +
                    ":{\"model\":" + detail::jsonString(alias(parts.help->model)) +
                    ",\"action\":" + detail::jsonString(parts.help->action) + "}}";
    }
    std::string const state =
        parts.search.empty() ? std::string{} : "\"" + std::string{shell_names::kSearchText} + "\":{\"init\":\"\"}";
    return "{\"models\":{" + models + "},\"state\":{" + state + "},\"queries\":{" + queries + "}" + mutations + "}";
}

}  // namespace morph::ui
