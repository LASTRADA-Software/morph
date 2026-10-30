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
/// Entries are grouped by module, then group, in the order each first appears; each group has a heading. Each entry
/// is a row as wide as the navigator holding a list item (`ButtonStyle::Item`) and, while `setBadge` gives it one, a
/// badge at its end; `setCurrent` marks the open screen's. An unavailable entry is listed disabled, with its reason as
/// its tooltip. The search shows the entries one of whose titles contains the text, ignoring ASCII case, and the
/// headings of the groups that still show one. `menuEntries` and `choose` give the same list as a menu, for a frame
/// too narrow for the navigator. Neither copyable nor movable; owned by the owner thread of its backend.
///
/// Test ids: `navigator`, `navigator_search`, `navigator_pins`, `nav_row_<screen id>`, `nav_<screen id>`,
/// `nav_badge_<screen id>`, `pin_<screen id>`, and `navigator_group_<module>/<group>` for a heading.
class Navigator {
public:
    /// @brief The navigator, appended to @p parent.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param parent  The container it is appended to; null makes it a backend root.
    /// @param options The entries, the pins and the handlers.
    Navigator(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, NavigatorOptions options)
        : _backend{&backend}, _options{std::move(options)}, _root{backend.createStack(parent, Axis::Vertical)} {
        _root->setTestId("navigator");
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

    /// @brief Sets what is called after anything `menuEntries()` lists changed: a count, the current entry, a pin.
    /// @param onChanged The handler; empty calls nothing.
    void setOnChanged(std::function<void()> onChanged) { _onChanged = std::move(onChanged); }

private:
    struct Item {
        CatalogEntry const* entry = nullptr;
        std::vector<std::string> titles;
        // The row goes after the button and the badge it holds.
        std::unique_ptr<StackWidget> row;
        std::unique_ptr<ButtonWidget> button;
        std::unique_ptr<BadgeWidget> badge;
        std::optional<NavigatorBadge> shown;
    };

    struct Group {
        std::string module;
        std::string group;
        std::unique_ptr<TextWidget> heading;
        std::vector<Item> items;
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

    [[nodiscard]] static std::string headingOf(Group const& group) {
        std::string heading = group.module;
        if (!group.group.empty()) {
            heading += heading.empty() ? group.group : " / " + group.group;
        }
        return heading;
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
        for (CatalogEntry const& entry : _options.entries) {
            bool const seen = std::ranges::any_of(_groups, [&entry](Group const& candidate) {
                return candidate.module == entry.module && candidate.group == entry.group;
            });
            if (!seen) {
                _groups.push_back(Group{.module = entry.module, .group = entry.group, .heading = {}, .items = {}});
            }
        }
        for (Group& group : _groups) {
            group.heading = _backend->createText(_root.get());
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
                          .row = _backend->createStack(_root.get(), Axis::Horizontal),
                          .button = {},
                          .badge = {},
                          .shown = std::nullopt};
                item.row->setTestId("nav_row_" + entry.id);
                item.row->setLayout({.width = Sizing::stretch(), .height = {}});
                item.row->setAlign(Align::Center);
                item.row->setGap(1);
                item.button = _backend->createButton(item.row.get());
                item.button->setTestId("nav_" + entry.id);
                item.button->setStyle(ButtonStyle::Item);
                item.button->setLayout({.width = Sizing::stretch(), .height = {}});
                wire(*item.button, entry, item.titles.front());
                // Hidden until a count is set; the button's accessible name carries the count, so the entry stays one
                // control to assistive technology.
                item.badge = _backend->createBadge(item.row.get());
                item.badge->setTestId("nav_badge_" + entry.id);
                item.badge->setVisible(false);
                group.items.push_back(std::move(item));
            }
        }
    }

    void filter(std::string const& text) {
        std::string const wanted = lower(text);
        for (Group& group : _groups) {
            bool any = false;
            for (Item& item : group.items) {
                bool const shown =
                    wanted.empty() || std::ranges::any_of(item.titles, [&wanted](std::string const& title) {
                        return lower(title).contains(wanted);
                    });
                item.row->setVisible(shown);
                any = any || shown;
            }
            group.heading->setVisible(any);
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
    std::vector<std::string> _pins;
    std::string _current;
    std::function<void()> _onChanged;
};

}  // namespace morph::ui
