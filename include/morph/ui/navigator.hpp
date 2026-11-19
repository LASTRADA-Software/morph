// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backend.hpp"
#include "bundle.hpp"
#include "catalogue.hpp"
#include "view.hpp"

/// @file
/// @brief The workspace's navigator and pins (spec 6 §7): the catalog listed by module and group, searched by title,
///        with the person's pinned shortcuts.
///
/// Specified in `docs/spec/ui/workspace.md`.

namespace morph::ui {

/// @brief A count on a navigator entry, such as the items waiting on its screen.
struct NavigatorBadge {
    /// @brief The badge's text.
    std::string text;
    /// @brief Its tone.
    Tone tone = Tone::Neutral;

    /// @brief Memberwise equality.
    /// @return Whether both members match.
    bool operator==(NavigatorBadge const&) const = default;
};

/// @brief A person's custom screen as the navigator lists it, in its Custom section.
struct NavigatorCustomEntry {
    /// @brief The screen's id, `custom:` and its digits.
    std::string id;
    /// @brief The name its store gave it.
    std::string title;
    /// @brief Who saved it.
    std::string savedBy;
    /// @brief Whether the person saved it; an entry they did not save names who did.
    bool mine = true;
    /// @brief Why it may not be mounted today; empty when it may.
    std::string broken;
    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(NavigatorCustomEntry const&) const = default;
};

/// @brief How a `Navigator` lists the catalog and what it reports.
struct NavigatorOptions {
    /// @brief The catalog's entries, in the order the navigator lists them within a group.
    std::vector<CatalogEntry> entries;
    /// @brief An entry's titles, each one a search can match: one per locale (spec 6 §19). Empty uses the entry's
    ///        title, a string or a message reference's `fallback`. The first title is the one shown.
    std::function<std::vector<std::string>(CatalogEntry const&)> titles;
    /// @brief Maps a group heading, or a module's when its entries have no group, to the text shown: a message key
    ///        becomes its message. Empty shows the text as written.
    std::function<std::string(std::string_view text)> label;
    /// @brief The person's pins as stored, in order; unknown ids and those past `maxPins` are dropped.
    std::vector<std::string> pins;
    /// @brief The most pins (`app-max-pins`).
    std::size_t maxPins = 12;
    /// @brief Called with an available entry's screen id when it, or its pin, is chosen.
    std::function<void(std::string const&)> onOpen;
    /// @brief Called with an available entry's screen id when the pointer rests on it or its pin: the time to start
    ///        the screen's prefetch queries.
    std::function<void(std::string const&)> onPrefetch;
    /// @brief Called with the whole new list each time the pins change, for the application to store.
    std::function<void(std::vector<std::string> const&)> onPins;
    /// @brief The heading of the Custom section, and the label of its submenu in `menuEntries`.
    std::string customHeading = "Custom";
    /// @brief The mark every custom entry carries, so none passes for an application screen's entry.
    std::string customMark = "Custom";
};

/// @brief The catalog as a list a person opens screens from, with a title search and pinned shortcuts.
///
/// Entries are grouped by module, then group, in the order each first appears; each group is a column of its own
/// holding its heading and its entries, and the groups are a spacing step apart. Each entry is a row as wide as the
/// navigator holding a list item (`ButtonStyle::Item`) whose content is the title and, while `setBadge` gives it one,
/// a badge at its end, so the item's selection spans the badge; `setCurrent` marks the open screen's. An unavailable
/// entry is listed disabled, with its reason as its tooltip. The search shows the entries one of whose titles contains
/// the text, ignoring ASCII case, and the headings of the groups that still show one. `menuEntries` and `choose` give
/// the same list as a menu, for a frame too narrow for the navigator. Neither copyable nor movable; owned by the owner
/// thread of its backend.
///
/// Test ids: `navigator`, `navigator_search`, `navigator_pins`, `nav_row_<screen id>`, `nav_<screen id>`,
/// `nav_content_<screen id>`, `nav_badge_<screen id>`, `pin_<screen id>`, `navigator_group_<module>/<group>` for a
/// heading and `navigator_group_<module>/<group>_section` for its column.
class MORPH_API Navigator {
public:
    /// @brief The navigator, appended to @p parent.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param parent  The container it is appended to; null makes it a backend root.
    /// @param options The entries, the pins and the handlers.
    Navigator(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, NavigatorOptions options);

    Navigator(Navigator const&) = delete;
    Navigator& operator=(Navigator const&) = delete;
    Navigator(Navigator&&) = delete;
    Navigator& operator=(Navigator&&) = delete;
    ~Navigator() = default;

    /// @brief The navigator's root widget, which the host places.
    /// @return It.
    [[nodiscard]] StackWidget& root() noexcept { return *_root; }

    /// @brief Moves keyboard focus to the search, as the navigator's shortcut does.
    void focusSearch() { _search->focus(); }

    /// @brief Pins a screen at the end of the pins.
    /// @param screenId The screen id.
    /// @return Whether it was pinned: `false` for an unknown or already pinned screen, or when the pins are full.
    bool pin(std::string const& screenId);

    /// @brief Removes a pin.
    /// @param screenId The screen id.
    /// @return Whether it was pinned.
    bool unpin(std::string const& screenId);

    /// @brief The pins, in order.
    /// @return Their screen ids.
    [[nodiscard]] std::vector<std::string> const& pins() const noexcept { return _pins; }

    /// @brief Marks the entry of the screen that is open, and clears the mark of the one marked before. A pin is not
    ///        marked.
    /// @param screenId The open screen's id; one the navigator does not list, or empty, leaves no entry marked.
    void setCurrent(std::string_view screenId);

    /// @brief The screen whose entry is marked.
    /// @return Its id; empty when none is.
    [[nodiscard]] std::string const& current() const noexcept { return _current; }

    /// @brief Puts a count at the end of a screen's entry, or takes it away. The entry stays one button to assistive
    ///        technology, and its accessible name carries the count: "Review, 4".
    /// @param screenId The screen whose entry it goes on.
    /// @param badge    The count, or `std::nullopt` to take it away.
    /// @return Whether the navigator lists the screen: `false`, changing nothing, for one it does not list.
    bool setBadge(std::string_view screenId, std::optional<NavigatorBadge> const& badge);

    /// @brief The count on a screen's entry.
    /// @param screenId The screen.
    /// @return It, or `std::nullopt` for an entry without one or a screen the navigator does not list.
    [[nodiscard]] std::optional<NavigatorBadge> badgeOf(std::string_view screenId) const;

    /// @brief The navigator as the entries of a menu: the pins first, then a separator, then each group as a
    ///        submenu labelled by its heading (the entries of a group with no heading at the top level instead).
    ///        An entry's label is its title with its count after it ("Review · 4"), the current screen's entry is
    ///        checked, and an unavailable one is disabled.
    /// @return The entries; `choose` takes a path into them.
    [[nodiscard]] std::vector<MenuEntry> menuEntries() const { return menu().entries; }

    /// @brief Opens the entry at @p path of `menuEntries()`, as choosing it in the navigator does.
    /// @param path The entry's index at each level from the top, separators counted.
    /// @return Whether it named an available entry, which was opened.
    bool choose(std::vector<std::size_t> const& path);

    /// @brief Collapses the navigator to one row per module: the row holds the module's label, a chevron and, while
    ///        the module is closed, its count; only the open module lists its entries, under the headings of its
    ///        later groups (the first group's heading is the module's label). A module with one entry is that
    ///        entry's row. A search text shows the matching entries of every module and hides the module rows.
    /// @param grouped Whether to collapse.
    void setModuleGroups(bool grouped);

    /// @brief Whether modules are collapsed.
    /// @return `true` after `setModuleGroups(true)`.
    [[nodiscard]] bool moduleGroups() const noexcept { return _moduleGroups; }

    /// @brief Opens a module's entries and closes the one open before.
    /// @param module The module.
    /// @return Whether the navigator lists the module.
    bool setOpenModule(std::string_view module);

    /// @brief The module whose entries are listed.
    /// @return It; empty when none is open.
    [[nodiscard]] std::string const& openModule() const noexcept { return _openModule; }

    /// @brief The words a module row's accessible name carries after its label and count.
    /// @param closed      Said of a closed module.
    /// @param open        Said of the open module.
    /// @param countFailed Said when the count could not be read.
    void setModuleTexts(std::string closed, std::string open, std::string countFailed);

    /// @brief Puts a count on a module's row while the module is closed, or takes it away.
    /// @param module The module.
    /// @param badge  The count, or `std::nullopt` for none.
    /// @param failed Whether the count could not be read, which draws an error marker beside the label.
    /// @return Whether the navigator lists the module.
    bool setModuleBadge(std::string_view module, std::optional<NavigatorBadge> const& badge, bool failed = false);

    /// @brief The count on a module's row.
    /// @param module The module.
    /// @return It; empty for none or for a module the navigator does not list.
    [[nodiscard]] std::optional<NavigatorBadge> moduleBadgeOf(std::string_view module) const;

    /// @brief Shows or hides the navigator's own search field, which a shell with a global search replaces.
    /// @param shown Whether it shows; hiding it clears its text.
    void setSearchShown(bool shown);

    /// @brief Reads every entry's titles and every heading again, as after a change of locale: the entry buttons,
    ///        their accessible names, the pins, the headings and the search filter follow. The pins, the current
    ///        entry, the badges and the search text stay. Calls the handler `setOnChanged` set.
    void relabel();

    /// @brief Sets what is called after anything `menuEntries()` lists changed: a count, the current entry, a pin.
    /// @param onChanged The handler; empty calls nothing.
    void setOnChanged(std::function<void()> onChanged) { _onChanged = std::move(onChanged); }

    /// @brief Lists the person's custom screens in the Custom section, after the application's groups, replacing what
    ///        it listed before.
    ///
    /// Each entry is a row as the catalog's are (`nav_row_<id>`, `nav_<id>`), whose content holds the custom mark
    /// (`nav_mark_<id>`), the title (`nav_title_<id>`), who saved it when it is not the person's own
    /// (`nav_author_<id>`, "by <savedBy>") and, for a broken screen, a badge (`nav_broken_<id>`) with the reason as
    /// the entry's tooltip. Choosing an entry calls `onOpen` with its id, a broken one included: what opening it does
    /// is the application's. Titles and names are shown as text, never read as markup or as a message key. The search
    /// filters the section as it filters the groups, `setCurrent` marks its entries, and `menuEntries` ends with it as
    /// a submenu. An empty list hides the section.
    /// @param entries The screens, in order.
    void setCustom(std::vector<NavigatorCustomEntry> entries);

    /// @brief The custom screens the Custom section lists.
    /// @return They, in order.
    [[nodiscard]] std::vector<NavigatorCustomEntry> const& custom() const noexcept { return _custom; }

private:
    struct Item {
        CatalogEntry const* entry = nullptr;
        std::vector<std::string> titles;
        // Declared parent before child: the button is the row's only child, and the title and the badge are the
        // button's content, which the button owns.
        std::unique_ptr<StackWidget> row;
        std::unique_ptr<ButtonWidget> button;
        std::unique_ptr<StackWidget> content;
        std::unique_ptr<TextWidget> label;
        std::unique_ptr<BadgeWidget> badge;
        std::optional<NavigatorBadge> shown;
    };

    struct Group {
        std::string module;
        std::string group;
        std::unique_ptr<StackWidget> section;
        std::unique_ptr<TextWidget> heading;
        std::vector<Item> items;
    };

    // A module's row in the collapsed form: a button holding a chevron, the label, the count and an error marker.
    struct Module {
        std::string module;
        std::size_t entries = 0;
        std::unique_ptr<StackWidget> row;
        std::unique_ptr<ButtonWidget> button;
        std::unique_ptr<StackWidget> content;
        std::unique_ptr<TextWidget> chevron;
        std::unique_ptr<TextWidget> label;
        std::unique_ptr<BadgeWidget> badge;
        std::unique_ptr<TextWidget> error;
        std::optional<NavigatorBadge> shown;
        bool failed = false;
    };

    // A custom screen's row: the button holding the mark, the title, the author and the broken badge.
    struct CustomItem {
        NavigatorCustomEntry entry;
        std::unique_ptr<StackWidget> row;
        std::unique_ptr<ButtonWidget> button;
        std::unique_ptr<StackWidget> content;
        std::unique_ptr<BadgeWidget> mark;
        std::unique_ptr<TextWidget> label;
        std::unique_ptr<TextWidget> author;
        std::unique_ptr<BadgeWidget> broken;
    };

    [[nodiscard]] CustomItem const* customItemOf(std::string_view screenId) const;

    // The Custom section, made empty and hidden at construction, after every group.
    void makeCustomSection();

    // What one entry of `menuEntries()` opens: a screen, or a submenu's entries; neither for a separator.
    struct MenuSlot {
        std::string screenId;
        std::vector<MenuSlot> items;
    };

    struct Menu {
        std::vector<MenuEntry> entries;
        std::vector<MenuSlot> targets;
    };

    // One entry of the menu: its title, then its count.
    [[nodiscard]] MenuEntry menuEntry(CatalogEntry const& entry) const;

    [[nodiscard]] Menu menu() const;

    // The collapsed form as a menu: one submenu per module, its count after the label, the module's later groups as
    // heading entries inside it; a module with one entry is that entry at the top level.
    void moduleMenu(Menu& out) const;

    // The group, or the module when the entry has no group, through the label.
    [[nodiscard]] std::string headingOf(Group const& group) const;

    void changed() const;

    [[nodiscard]] static std::string lower(std::string_view text);

    [[nodiscard]] std::vector<std::string> titlesOf(CatalogEntry const& entry) const;

    [[nodiscard]] CatalogEntry const* entryOf(std::string_view screenId) const;

    [[nodiscard]] bool known(std::string_view screenId) const { return entryOf(screenId) != nullptr; }

    [[nodiscard]] Item const* itemOf(std::string_view screenId) const { return itemIn(_groups, screenId); }

    [[nodiscard]] Item* itemOf(std::string_view screenId) { return itemIn(_groups, screenId); }

    // One search for both constnesses of itemOf: `Groups` is the const or the mutable group list, and the item
    // pointer it returns carries the same constness.
    template <typename Groups>
    [[nodiscard]] static auto itemIn(Groups& groups, std::string_view screenId)
        -> decltype(&groups.front().items.front()) {
        for (auto& group : groups) {
            auto const found = std::ranges::find_if(
                group.items, [screenId](Item const& item) { return !screenId.empty() && item.entry->id == screenId; });
            if (found != group.items.end()) {
                return &*found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool pinned(std::string_view screenId) const {
        return std::ranges::find(_pins, screenId) != _pins.end();
    }

    // Wires a button to open, and prefetch on hover, an available entry.
    void wire(ButtonWidget& button, CatalogEntry const& entry, std::string const& title);

    void listEntries();

    // The module's row of the collapsed form, hidden until module groups are on.
    void makeModuleRow(std::string const& moduleName);

    void makeGroup(Group& group);

    // The groups in the order the catalog first names each (module, group) pair: the order of the sidebar and the menu
    // while modules are not grouped.
    [[nodiscard]] std::vector<Group const*> catalogOrder() const;

    // Places the groups' sections: module by module, each under its module's row, while modules are grouped; in
    // catalog order otherwise. A catalog that never interleaves its modules has one order, and nothing moves.
    void arrange();

    void filter(std::string const& text);

    [[nodiscard]] Module const* moduleOf(std::string_view module) const;

    [[nodiscard]] Module* moduleOf(std::string_view module);

    [[nodiscard]] bool listsModule(std::string_view module) const { return moduleOf(module) != nullptr; }

    // The heading of the module's first group, through the label: what its row says.
    [[nodiscard]] std::string moduleLabel(Module const& module) const;

    // The row's accessible name: the label, the count and whether the module is open.
    void nameModule(Module const& module) const;

    // Shows what the search text, the module form and the open module leave: see `setModuleGroups`.
    void refreshVisibility();

    void showPins();

    void pinsChanged();

    IViewBackend* _backend;
    NavigatorOptions _options;
    std::unique_ptr<StackWidget> _root;
    std::unique_ptr<TextInputWidget> _search;
    std::unique_ptr<StackWidget> _pinRow;
    std::vector<std::unique_ptr<ButtonWidget>> _pinButtons;
    std::vector<Group> _groups;
    std::vector<Module> _modules;
    std::unique_ptr<StackWidget> _customSection;
    std::unique_ptr<TextWidget> _customHeading;
    // After the section, so the rows go before the column that holds them.
    std::vector<CustomItem> _customItems;
    std::vector<NavigatorCustomEntry> _custom;
    bool _moduleGroups = false;
    // Whether the sections are placed module by module, which is how they are made.
    bool _moduleMajor = true;
    std::string _openModule;
    std::string _closedWord = "collapsed";
    std::string _openWord = "expanded";
    std::string _failedWord = "count unavailable";
    std::vector<std::string> _pins;
    std::string _current;
    std::string _searchText;
    std::function<void()> _onChanged;
};

}  // namespace morph::ui
