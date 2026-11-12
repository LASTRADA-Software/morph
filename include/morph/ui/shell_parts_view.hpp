// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief What a `ShellFrame` draws of an application's declared shell parts, and what it reports back: plain data,
///        so the frame needs no interpreter.
///
/// Specified in `docs/spec/ui/shell_frame.md` (Declared parts).

namespace morph::ui {

/// @brief One option of the scope switcher.
struct ShellOptionView {
    /// @brief The caption.
    std::string label;
    /// @brief A second line; empty for none.
    std::string detail;
    /// @brief The heading the option sits under; empty for none.
    std::string group;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellOptionView const&) const = default;
};

/// @brief The scope switcher.
struct ShellContextView {
    /// @brief Whether the part is declared.
    bool shown = false;
    /// @brief The options, in order.
    std::vector<ShellOptionView> options;
    /// @brief The chosen option.
    std::optional<std::size_t> chosen;
    /// @brief Whether an option can be chosen; false while there are none.
    bool enabled = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellContextView const&) const = default;
};

/// @brief One search hit.
struct ShellHitView {
    /// @brief The hit's type, drawn before the title; empty for none.
    std::string kind;
    /// @brief The title.
    std::string title;
    /// @brief The muted line after the title; empty for none.
    std::string detail;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellHitView const&) const = default;
};

/// @brief One source's hits under its heading.
struct ShellSourceView {
    /// @brief The heading.
    std::string title;
    /// @brief Whether the source could not be searched.
    bool failed = false;
    /// @brief Whether its request is in flight.
    bool pending = false;
    /// @brief The hits, at most `kSearchHitsPerSource`.
    std::vector<ShellHitView> hits;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellSourceView const&) const = default;
};

/// @brief The search part.
struct ShellSearchView {
    /// @brief Whether the part is declared.
    bool shown = false;
    /// @brief The field's placeholder.
    std::string hint;
    /// @brief The sources, in order.
    std::vector<ShellSourceView> sources;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellSearchView const&) const = default;
};

/// @brief The inbox button.
struct ShellInboxView {
    /// @brief Whether the button is drawn.
    bool shown = false;
    /// @brief The screen's title, the button's caption.
    std::string title;
    /// @brief The sum of the sources that answered; empty while none has.
    std::optional<long long> count;
    /// @brief Whether a source failed.
    bool failed = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellInboxView const&) const = default;
};

/// @brief A role, with the line that says what it holds.
struct ShellRoleView {
    /// @brief The role.
    std::string label;
    /// @brief Its scope or limit; empty for none.
    std::string detail;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellRoleView const&) const = default;
};

/// @brief A language or format the person may choose.
struct ShellChoiceView {
    /// @brief The tag, such as `de`.
    std::string tag;
    /// @brief The name drawn.
    std::string label;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellChoiceView const&) const = default;
};

/// @brief The signed-in person's menu.
struct ShellPrincipalView {
    /// @brief Whether the menu is drawn.
    bool shown = false;
    /// @brief The name.
    std::string name;
    /// @brief The line under the name in the trigger.
    std::string subtitle;
    /// @brief The menu's detail line.
    std::string detail;
    /// @brief The roles.
    std::vector<ShellRoleView> roles;
    /// @brief The languages offered.
    std::vector<ShellChoiceView> locales;
    /// @brief The chosen language.
    std::optional<std::size_t> chosenLocale;
    /// @brief The formats offered; empty hides the choice.
    std::vector<ShellChoiceView> formats;
    /// @brief The chosen format; none follows the language.
    std::optional<std::size_t> chosenFormat;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellPrincipalView const&) const = default;
};

/// @brief The texts the frame draws that are not data.
struct ShellLabels {
    /// @brief The switcher's accessible-name prefix, "Scope".
    std::string scope = "Scope";
    /// @brief The search field's name.
    std::string search = "Search";
    /// @brief The results list's name.
    std::string searchResults = "Search results";
    /// @brief "No results for" followed by the text.
    std::string searchEmpty = "No results for";
    /// @brief The line under a failed source's heading, after its title.
    std::string searchFailed = "could not be searched";
    /// @brief The busy row.
    std::string searching = "Searching";
    /// @brief The inbox's accessible-name suffix after the count, "waiting".
    std::string waiting = "waiting";
    /// @brief The principal menu's accessible-name prefix.
    std::string signedInAs = "Signed in as";
    /// @brief The heading over the roles.
    std::string roles = "Your roles";
    /// @brief The heading over the languages.
    std::string language = "Language";
    /// @brief The heading over the formats.
    std::string formats = "Number and date format";
    /// @brief The name of a count that could not be read, after the label.
    std::string countFailed = "count unavailable";
    /// @brief The close button of the compact search overlay.
    std::string close = "Close";

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellLabels const&) const = default;
};

/// @brief Everything a frame draws of the declared parts.
struct ShellPartsView {
    /// @brief The scope switcher.
    ShellContextView context;
    /// @brief The search.
    ShellSearchView search;
    /// @brief The inbox.
    ShellInboxView inbox;
    /// @brief The person's menu.
    ShellPrincipalView principal;
    /// @brief The texts.
    ShellLabels labels;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ShellPartsView const&) const = default;
};

/// @brief What a frame reports of the declared parts. An empty handler does nothing.
struct ShellPartsHandlers {
    /// @brief An option of the switcher was chosen, by its place.
    std::function<void(std::size_t)> onContext;
    /// @brief The search field's text changed.
    std::function<void(std::string const&)> onSearchText;
    /// @brief A hit was chosen: its source's place and its place in the source.
    std::function<void(std::size_t, std::size_t)> onSearchOpen;
    /// @brief Every drawn hit was asked to open.
    std::function<void()> onSearchOpenAll;
    /// @brief The inbox button was pressed.
    std::function<void()> onInbox;
    /// @brief A language was chosen, by its place.
    std::function<void(std::size_t)> onLocale;
    /// @brief A format was chosen, by its place.
    std::function<void(std::size_t)> onFormat;
};

}  // namespace morph::ui
