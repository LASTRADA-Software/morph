// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
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
/// is a list item (`ButtonStyle::Item`) as wide as the navigator, and `setCurrent` marks the open screen's. An
/// unavailable entry is listed disabled, with its reason as its tooltip. The search shows the entries one of whose
/// titles contains the text, ignoring ASCII case, and the headings of the groups that still show one. Neither
/// copyable nor movable; owned by the owner thread of its backend.
///
/// Test ids: `navigator`, `navigator_search`, `navigator_pins`, `nav_<screen id>`, `pin_<screen id>`, and
/// `navigator_group_<module>/<group>` for a heading.
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
    }

    /// @brief The screen whose entry is marked.
    /// @return Its id; empty when none is.
    [[nodiscard]] std::string const& current() const noexcept { return _current; }

private:
    struct Item {
        CatalogEntry const* entry = nullptr;
        std::vector<std::string> titles;
        std::unique_ptr<ButtonWidget> button;
    };

    struct Group {
        std::string module;
        std::string group;
        std::unique_ptr<TextWidget> heading;
        std::vector<Item> items;
    };

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

    [[nodiscard]] Item const* itemOf(std::string_view screenId) const {
        for (Group const& group : _groups) {
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
            group.heading->setRole(TextRole::Section);
            group.heading->setTestId("navigator_group_" + group.module + "/" + group.group);
            std::string heading = group.module;
            if (!group.group.empty()) {
                heading += heading.empty() ? group.group : " / " + group.group;
            }
            group.heading->setText(heading);
            for (CatalogEntry const& entry : _options.entries) {
                if (entry.module != group.module || entry.group != group.group) {
                    continue;
                }
                Item item{.entry = &entry, .titles = titlesOf(entry), .button = _backend->createButton(_root.get())};
                item.button->setTestId("nav_" + entry.id);
                item.button->setStyle(ButtonStyle::Item);
                item.button->setLayout({.width = Sizing::stretch(), .height = {}});
                wire(*item.button, entry, item.titles.front());
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
                item.button->setVisible(shown);
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
};

}  // namespace morph::ui
