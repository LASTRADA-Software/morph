// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
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
class Navigator {
public:
    /// @brief The navigator, appended to @p parent.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param parent  The container it is appended to; null makes it a backend root.
    /// @param options The entries, the pins and the handlers.
    Navigator(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, NavigatorOptions options)
        : _backend{&backend}, _options{std::move(options)}, _root{backend.createStack(parent, Axis::Vertical)} {
        _root->setTestId("navigator");
        // The search, the pins and the groups stand a spacing step apart; a hidden pin row or group takes no gap.
        _root->setGap(3);
        _search = backend.createTextInput(_root.get(), TextInputMode::SingleLine);
        _search->setTestId("navigator_search");
        _search->setPlaceholder("Search");
        _search->setAccessibleName("Search screens");
        _search->setOnChange([this](std::string const& text) { filter(text); });
        _pinRow = backend.createStack(_root.get(), Axis::Horizontal);
        _pinRow->setTestId("navigator_pins");
        listEntries();
        std::vector<std::string> stored = std::move(_options.pins);
        _options.pins.clear();
        for (std::string& screenId : stored) {
            if (_pins.size() < _options.maxPins && known(screenId) && !pinned(screenId)) {
                _pins.push_back(std::move(screenId));
            }
        }
        showPins();
    }

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
    bool pin(std::string const& screenId) {
        if (!known(screenId) || pinned(screenId) || _pins.size() >= _options.maxPins) {
            return false;
        }
        _pins.push_back(screenId);
        pinsChanged();
        return true;
    }

    /// @brief Removes a pin.
    /// @param screenId The screen id.
    /// @return Whether it was pinned.
    bool unpin(std::string const& screenId) {
        if (std::erase(_pins, screenId) == 0) {
            return false;
        }
        pinsChanged();
        return true;
    }

    /// @brief The pins, in order.
    /// @return Their screen ids.
    [[nodiscard]] std::vector<std::string> const& pins() const noexcept { return _pins; }

    /// @brief Marks the entry of the screen that is open, and clears the mark of the one marked before. A pin is not
    ///        marked.
    /// @param screenId The open screen's id; one the navigator does not list, or empty, leaves no entry marked.
    void setCurrent(std::string_view screenId) {
        if (screenId == _current) {
            return;
        }
        if (Item const* const previous = itemOf(_current)) {
            previous->button->setSelected(false);
        }
        _current = std::string{screenId};
        if (Item const* const next = itemOf(_current)) {
            next->button->setSelected(true);
            if (CatalogEntry const* const entry = entryOf(_current);
                entry != nullptr && _openModule != entry->module) {
                _openModule = entry->module;
                refreshVisibility();
            }
        } else {
            _current.clear();
        }
        changed();
    }

    /// @brief The screen whose entry is marked.
    /// @return Its id; empty when none is.
    [[nodiscard]] std::string const& current() const noexcept { return _current; }

    /// @brief Puts a count at the end of a screen's entry, or takes it away. The entry stays one button to assistive
    ///        technology, and its accessible name carries the count: "Review, 4".
    /// @param screenId The screen whose entry it goes on.
    /// @param badge    The count, or `std::nullopt` to take it away.
    /// @return Whether the navigator lists the screen: `false`, changing nothing, for one it does not list.
    bool setBadge(std::string_view screenId, std::optional<NavigatorBadge> const& badge) {
        Item* const item = itemOf(screenId);
        if (item == nullptr) {
            return false;
        }
        if (item->shown == badge) {
            return true;
        }
        item->shown = badge;
        std::string const& title = item->titles.front();
        if (badge) {
            item->badge->setText(badge->text);
            item->badge->setTone(badge->tone);
            item->button->setAccessibleName(title + ", " + badge->text);
        } else {
            item->button->setAccessibleName(title);
        }
        item->badge->setVisible(badge.has_value());
        changed();
        return true;
    }

    /// @brief The count on a screen's entry.
    /// @param screenId The screen.
    /// @return It, or `std::nullopt` for an entry without one or a screen the navigator does not list.
    [[nodiscard]] std::optional<NavigatorBadge> badgeOf(std::string_view screenId) const {
        Item const* const item = itemOf(screenId);
        return item == nullptr ? std::nullopt : item->shown;
    }

    /// @brief The navigator as the entries of a menu: the pins first, then a separator, then each group as a
    ///        submenu labelled by its heading (the entries of a group with no heading at the top level instead).
    ///        An entry's label is its title with its count after it ("Review · 4"), the current screen's entry is
    ///        checked, and an unavailable one is disabled.
    /// @return The entries; `choose` takes a path into them.
    [[nodiscard]] std::vector<MenuEntry> menuEntries() const { return menu().entries; }

    /// @brief Opens the entry at @p path of `menuEntries()`, as choosing it in the navigator does.
    /// @param path The entry's index at each level from the top, separators counted.
    /// @return Whether it named an available entry, which was opened.
    bool choose(std::vector<std::size_t> const& path) {
        Menu const shown = menu();
        std::vector<MenuSlot> const* level = &shown.targets;
        MenuSlot const* slot = nullptr;
        for (std::size_t const index : path) {
            if (index >= level->size()) {
                return false;
            }
            slot = &level->at(index);
            level = &slot->items;
        }
        if (slot == nullptr || slot->screenId.empty() || !slot->items.empty()) {
            return false;
        }
        CatalogEntry const* const entry = entryOf(slot->screenId);
        if (entry == nullptr || !entry->available) {
            return false;
        }
        if (_options.onOpen) {
            _options.onOpen(slot->screenId);
        }
        return true;
    }

    /// @brief Collapses the navigator to one row per module: the row holds the module's label, a chevron and, while
    ///        the module is closed, its count; only the open module lists its entries, under the headings of its
    ///        later groups (the first group's heading is the module's label). A module with one entry is that
    ///        entry's row. A search text shows the matching entries of every module and hides the module rows.
    /// @param grouped Whether to collapse.
    void setModuleGroups(bool grouped) {
        if (_moduleGroups == grouped) {
            return;
        }
        _moduleGroups = grouped;
        refreshVisibility();
        changed();
    }

    /// @brief Whether modules are collapsed.
    /// @return `true` after `setModuleGroups(true)`.
    [[nodiscard]] bool moduleGroups() const noexcept { return _moduleGroups; }

    /// @brief Opens a module's entries and closes the one open before.
    /// @param module The module.
    /// @return Whether the navigator lists the module.
    bool setOpenModule(std::string_view module) {
        if (!listsModule(module)) {
            return false;
        }
        if (_openModule != module) {
            _openModule = std::string{module};
            refreshVisibility();
            changed();
        }
        return true;
    }

    /// @brief The module whose entries are listed.
    /// @return It; empty when none is open.
    [[nodiscard]] std::string const& openModule() const noexcept { return _openModule; }

    /// @brief The words a module row's accessible name carries after its label and count.
    /// @param closed      Said of a closed module.
    /// @param open        Said of the open module.
    /// @param countFailed Said when the count could not be read.
    void setModuleTexts(std::string closed, std::string open, std::string countFailed) {
        _closedWord = std::move(closed);
        _openWord = std::move(open);
        _failedWord = std::move(countFailed);
        for (Module const& module : _modules) {
            nameModule(module);
        }
    }

    /// @brief Puts a count on a module's row while the module is closed, or takes it away.
    /// @param module The module.
    /// @param badge  The count, or `std::nullopt` for none.
    /// @param failed Whether the count could not be read, which draws an error marker beside the label.
    /// @return Whether the navigator lists the module.
    bool setModuleBadge(std::string_view module, std::optional<NavigatorBadge> const& badge, bool failed = false) {
        Module* const found = moduleOf(module);
        if (found == nullptr) {
            return false;
        }
        if (found->shown == badge && found->failed == failed) {
            return true;
        }
        found->shown = badge;
        found->failed = failed;
        if (badge) {
            found->badge->setText(badge->text);
            found->badge->setTone(badge->tone);
        }
        found->error->setVisible(failed);
        nameModule(*found);
        refreshVisibility();
        changed();
        return true;
    }

    /// @brief The count on a module's row.
    /// @param module The module.
    /// @return It; empty for none or for a module the navigator does not list.
    [[nodiscard]] std::optional<NavigatorBadge> moduleBadgeOf(std::string_view module) const {
        Module const* const found = moduleOf(module);
        return found == nullptr ? std::nullopt : found->shown;
    }

    /// @brief Shows or hides the navigator's own search field, which a shell with a global search replaces.
    /// @param shown Whether it shows; hiding it clears its text.
    void setSearchShown(bool shown) {
        if (!shown) {
            _search->setText({});
            filter({});
        }
        _search->setVisible(shown);
    }

    /// @brief Reads every entry's titles and every heading again, as after a change of locale: the entry buttons,
    ///        their accessible names, the pins, the headings and the search filter follow. The pins, the current
    ///        entry, the badges and the search text stay. Calls the handler `setOnChanged` set.
    void relabel() {
        for (Group& group : _groups) {
            group.heading->setText(headingOf(group));
            for (Item& item : group.items) {
                item.titles = titlesOf(*item.entry);
                item.button->setLabel(item.titles.front());
                item.label->setText(item.titles.front());
                if (item.shown) {
                    item.button->setAccessibleName(item.titles.front() + ", " + item.shown->text);
                }
            }
        }
        for (Module const& module : _modules) {
            module.label->setText(moduleLabel(module));
            nameModule(module);
        }
        showPins();
        filter(_searchText);
        changed();
    }

    /// @brief Sets what is called after anything `menuEntries()` lists changed: a count, the current entry, a pin.
    /// @param onChanged The handler; empty calls nothing.
    void setOnChanged(std::function<void()> onChanged) { _onChanged = std::move(onChanged); }

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
    [[nodiscard]] MenuEntry menuEntry(CatalogEntry const& entry) const {
        MenuEntry made;
        made.label = titlesOf(entry).front();
        if (Item const* const item = itemOf(entry.id); item != nullptr && item->shown) {
            made.label += " · " + item->shown->text;
        }
        if (entry.id == _current) {
            made.checked = true;
        }
        made.enabled = entry.available;
        return made;
    }

    [[nodiscard]] Menu menu() const {
        Menu out;
        for (std::string const& screenId : _pins) {
            out.entries.push_back(menuEntry(*entryOf(screenId)));
            out.targets.push_back(MenuSlot{.screenId = screenId, .items = {}});
        }
        if (!_pins.empty() && !_groups.empty()) {
            out.entries.push_back(MenuEntry{.label = {},
                                            .icon = {},
                                            .keys = {},
                                            .checked = std::nullopt,
                                            .enabled = true,
                                            .items = {},
                                            .separator = true,
                                            .description = {}});
            out.targets.push_back(MenuSlot{});
        }
        if (_moduleGroups) {
            moduleMenu(out);
            return out;
        }
        for (Group const& group : _groups) {
            std::string const heading = headingOf(group);
            MenuEntry submenu;
            MenuSlot slot;
            for (Item const& item : group.items) {
                (heading.empty() ? out.entries : submenu.items).push_back(menuEntry(*item.entry));
                (heading.empty() ? out.targets : slot.items)
                    .push_back(MenuSlot{.screenId = item.entry->id, .items = {}});
            }
            if (!heading.empty()) {
                submenu.label = heading;
                out.entries.push_back(std::move(submenu));
                out.targets.push_back(std::move(slot));
            }
        }
        return out;
    }

    // The collapsed form as a menu: one submenu per module, its count after the label, the module's later groups as
    // heading entries inside it; a module with one entry is that entry at the top level.
    void moduleMenu(Menu& out) const {
        for (Module const& module : _modules) {
            MenuEntry submenu;
            MenuSlot slot;
            bool first = true;
            for (Group const& group : _groups) {
                if (group.module != module.module) {
                    continue;
                }
                bool const lone = module.entries <= 1;
                if (!first && !lone && !headingOf(group).empty()) {
                    MenuEntry heading;
                    heading.label = headingOf(group);
                    heading.heading = true;
                    heading.enabled = false;
                    submenu.items.push_back(std::move(heading));
                    slot.items.push_back(MenuSlot{});
                }
                first = false;
                for (Item const& item : group.items) {
                    (lone ? out.entries : submenu.items).push_back(menuEntry(*item.entry));
                    (lone ? out.targets : slot.items).push_back(MenuSlot{.screenId = item.entry->id, .items = {}});
                }
            }
            if (module.entries > 1) {
                submenu.label = moduleLabel(module);
                if (module.shown) {
                    submenu.label += " · " + module.shown->text;
                }
                if (module.failed) {
                    submenu.label += " · !";
                }
                out.entries.push_back(std::move(submenu));
                out.targets.push_back(std::move(slot));
            }
        }
    }

    // The group, or the module when the entry has no group, through the label.
    [[nodiscard]] std::string headingOf(Group const& group) const {
        std::string const& text = group.group.empty() ? group.module : group.group;
        return _options.label ? _options.label(text) : text;
    }

    void changed() const {
        if (_onChanged) {
            _onChanged();
        }
    }

    [[nodiscard]] static std::string lower(std::string_view text) {
        std::string out{text};
        std::ranges::transform(out, out.begin(), [](char byte) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
        });
        return out;
    }

    [[nodiscard]] std::vector<std::string> titlesOf(CatalogEntry const& entry) const {
        if (_options.titles) {
            std::vector<std::string> titles = _options.titles(entry);
            if (!titles.empty()) {
                return titles;
            }
        }
        return catalogTitles(entry, nullptr);
    }

    [[nodiscard]] CatalogEntry const* entryOf(std::string_view screenId) const {
        auto const found = std::ranges::find(_options.entries, screenId, &CatalogEntry::id);
        return found == _options.entries.end() ? nullptr : &*found;
    }

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
    void wire(ButtonWidget& button, CatalogEntry const& entry, std::string const& title) {
        button.setLabel(title);
        button.setEnabled(entry.available);
        if (!entry.available) {
            button.setTooltip(entry.reason.value_or(std::string{}));
            return;
        }
        std::string const screenId = entry.id;
        button.setOnClick([this, screenId] {
            if (_options.onOpen) {
                _options.onOpen(screenId);
            }
        });
        button.setOnHover([this, screenId] {
            if (_options.onPrefetch) {
                _options.onPrefetch(screenId);
            }
        });
    }

    void listEntries() {
        // Entries are listed module by module, a module's groups together, each in order of first appearance.
        std::vector<std::string> moduleOrder;
        for (CatalogEntry const& entry : _options.entries) {
            if (!std::ranges::contains(moduleOrder, entry.module)) {
                moduleOrder.push_back(entry.module);
            }
        }
        for (std::string const& moduleName : moduleOrder) {
            makeModuleRow(moduleName);
            std::vector<std::string> groupOrder;
            for (CatalogEntry const& entry : _options.entries) {
                if (entry.module == moduleName && !std::ranges::contains(groupOrder, entry.group)) {
                    groupOrder.push_back(entry.group);
                }
            }
            for (std::string const& groupName : groupOrder) {
                _groups.push_back(
                    Group{.module = moduleName, .group = groupName, .section = {}, .heading = {}, .items = {}});
                makeGroup(_groups.back());
            }
        }
        refreshVisibility();
    }

    // The module's row of the collapsed form, hidden until module groups are on.
    void makeModuleRow(std::string const& moduleName) {
        Module made;
        made.module = moduleName;
        made.entries =
            static_cast<std::size_t>(std::ranges::count(_options.entries, moduleName, &CatalogEntry::module));
        made.row = _backend->createStack(_root.get(), Axis::Horizontal);
        made.row->setTestId("nav_module_row_" + moduleName);
        made.row->setLayout({.width = Sizing::stretch(), .height = {}});
        made.row->setAlign(Align::Center);
        made.row->setVisible(false);
        made.button = _backend->createButton(made.row.get());
        made.button->setTestId("nav_module_" + moduleName);
        made.button->setStyle(ButtonStyle::Item);
        made.button->setLayout({.width = Sizing::stretch(), .height = {}});
        made.button->setOnClick([this, moduleName] {
            if (_openModule == moduleName) {
                _openModule.clear();
                refreshVisibility();
                changed();
            } else {
                setOpenModule(moduleName);
            }
        });
        ContainerWidget& content = made.button->content();
        made.content = _backend->createStack(&content, Axis::Horizontal);
        made.content->setGap(1);
        made.content->setAlign(Align::Center);
        made.content->setLayout({.width = Sizing::stretch(), .height = {}});
        made.chevron = _backend->createText(made.content.get());
        made.chevron->setTestId("nav_module_chevron_" + moduleName);
        made.label = _backend->createText(made.content.get());
        made.label->setTestId("nav_module_label_" + moduleName);
        made.label->setLayout({.width = Sizing::stretch(), .height = {}});
        made.error = _backend->createText(made.content.get());
        made.error->setTestId("shell_count_error_" + moduleName);
        made.error->setText("!");
        made.error->setVisible(false);
        made.badge = _backend->createBadge(made.content.get());
        made.badge->setTestId("nav_module_badge_" + moduleName);
        made.badge->setVisible(false);
        _modules.push_back(std::move(made));
        Module& stored = _modules.back();
        stored.label->setText(moduleLabel(stored));
        nameModule(stored);
    }

    void makeGroup(Group& group) {
        // The heading and its entries are a column of their own, a step apart from the next group's; the heading
        // sits a step under whatever precedes it, and the entries' own heights keep them apart.
        group.section = _backend->createStack(_root.get(), Axis::Vertical);
        group.section->setGap(1);
        group.section->setTestId("navigator_group_" + group.module + "/" + group.group + "_section");
        group.heading = _backend->createText(group.section.get());
        // An overline, which a theme may draw as small capitals; by default it looks as a section does.
        group.heading->setRole(TextRole::Overline);
        group.heading->setTestId("navigator_group_" + group.module + "/" + group.group);
        group.heading->setText(headingOf(group));
        for (CatalogEntry const& entry : _options.entries) {
            if (entry.module != group.module || entry.group != group.group) {
                continue;
            }
            Item item{.entry = &entry,
                      .titles = titlesOf(entry),
                      .row = _backend->createStack(group.section.get(), Axis::Horizontal),
                      .button = {},
                      .content = {},
                      .label = {},
                      .badge = {},
                      .shown = std::nullopt};
            item.row->setTestId("nav_row_" + entry.id);
            item.row->setLayout({.width = Sizing::stretch(), .height = {}});
            item.row->setAlign(Align::Center);
            item.button = _backend->createButton(item.row.get());
            item.button->setTestId("nav_" + entry.id);
            item.button->setStyle(ButtonStyle::Item);
            item.button->setLayout({.width = Sizing::stretch(), .height = {}});
            wire(*item.button, entry, item.titles.front());
            // The title and the badge are the button's content, so its selection spans both. The badge is hidden
            // until a count is set; the button's accessible name carries the count, so the entry stays one
            // control to assistive technology.
            ContainerWidget& content = item.button->content();
            item.content = _backend->createStack(&content, Axis::Horizontal);
            item.content->setTestId("nav_content_" + entry.id);
            item.content->setGap(1);
            item.content->setAlign(Align::Center);
            item.content->setLayout({.width = Sizing::stretch(), .height = {}});
            item.label = _backend->createText(item.content.get());
            item.label->setText(item.titles.front());
            item.label->setTestId("nav_title_" + entry.id);
            item.label->setLayout({.width = Sizing::stretch(), .height = {}});
            item.badge = _backend->createBadge(item.content.get());
            item.badge->setTestId("nav_badge_" + entry.id);
            item.badge->setVisible(false);
            group.items.push_back(std::move(item));
        }
    }

    void filter(std::string const& text) {
        _searchText = text;
        refreshVisibility();
    }

    [[nodiscard]] Module const* moduleOf(std::string_view module) const {
        auto const found = std::ranges::find(_modules, module, &Module::module);
        return found == _modules.end() ? nullptr : &*found;
    }

    [[nodiscard]] Module* moduleOf(std::string_view module) {
        auto const found = std::ranges::find(_modules, module, &Module::module);
        return found == _modules.end() ? nullptr : &*found;
    }

    [[nodiscard]] bool listsModule(std::string_view module) const { return moduleOf(module) != nullptr; }

    // The heading of the module's first group, through the label: what its row says.
    [[nodiscard]] std::string moduleLabel(Module const& module) const {
        auto const first = std::ranges::find(_groups, module.module, &Group::module);
        if (first != _groups.end()) {
            return headingOf(*first);
        }
        return _options.label ? _options.label(module.module) : module.module;
    }

    // The row's accessible name: the label, the count and whether the module is open.
    void nameModule(Module const& module) const {
        std::string name = moduleLabel(module);
        if (module.shown && module.module != _openModule) {
            name += ", " + module.shown->text;
        }
        if (module.failed) {
            name += ", " + _failedWord;
        }
        name += ", " + (module.module == _openModule ? _openWord : _closedWord);
        module.button->setAccessibleName(name);
    }

    // Shows what the search text, the module form and the open module leave: see `setModuleGroups`.
    void refreshVisibility() {
        std::string const wanted = lower(_searchText);
        bool const collapsed = _moduleGroups && wanted.empty();
        for (Module const& module : _modules) {
            bool const single = module.entries <= 1;
            bool const open = module.module == _openModule;
            module.row->setVisible(collapsed && !single);
            module.chevron->setText(open ? "▾" : "▸");
            module.badge->setVisible(module.shown.has_value() && !open);
            nameModule(module);
        }
        for (Group& group : _groups) {
            Module const* const module = moduleOf(group.module);
            bool const single = module != nullptr && module->entries <= 1;
            bool const open = group.module == _openModule;
            bool any = false;
            for (Item& item : group.items) {
                bool const matches =
                    wanted.empty() || std::ranges::any_of(item.titles, [&wanted](std::string const& title) {
                        return lower(title).contains(wanted);
                    });
                item.row->setVisible(matches);
                any = any || matches;
            }
            bool const firstOfModule = &group == &*std::ranges::find(_groups, group.module, &Group::module);
            group.section->setVisible(any && (!collapsed || single || open));
            // In the collapsed form the first group's heading is the module's row, and a single-entry module is
            // its entry's row alone.
            group.heading->setVisible(!_moduleGroups || !wanted.empty() ? true : !(firstOfModule || single));
        }
    }

    void showPins() {
        _pinButtons.clear();
        for (std::string const& screenId : _pins) {
            CatalogEntry const* const entry = entryOf(screenId);
            auto button = _backend->createButton(_pinRow.get());
            button->setTestId("pin_" + screenId);
            wire(*button, *entry, titlesOf(*entry).front());
            _pinButtons.push_back(std::move(button));
        }
    }

    void pinsChanged() {
        showPins();
        if (_options.onPins) {
            _options.onPins(_pins);
        }
        changed();
    }

    IViewBackend* _backend;
    NavigatorOptions _options;
    std::unique_ptr<StackWidget> _root;
    std::unique_ptr<TextInputWidget> _search;
    std::unique_ptr<StackWidget> _pinRow;
    std::vector<std::unique_ptr<ButtonWidget>> _pinButtons;
    std::vector<Group> _groups;
    std::vector<Module> _modules;
    bool _moduleGroups = false;
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
