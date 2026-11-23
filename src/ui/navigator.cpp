// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

Navigator::Navigator(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, NavigatorOptions options)
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
    arrange();
    makeCustomSection();
    std::vector<std::string> stored = std::move(_options.pins);
    _options.pins.clear();
    for (std::string& screenId : stored) {
        if (_pins.size() < _options.maxPins && known(screenId) && !pinned(screenId)) {
            _pins.push_back(std::move(screenId));
        }
    }
    showPins();
}

bool Navigator::pin(std::string const& screenId) {
    if (!known(screenId) || pinned(screenId) || _pins.size() >= _options.maxPins) {
        return false;
    }
    _pins.push_back(screenId);
    pinsChanged();
    return true;
}

bool Navigator::unpin(std::string const& screenId) {
    if (std::erase(_pins, screenId) == 0) {
        return false;
    }
    pinsChanged();
    return true;
}

void Navigator::setCurrent(std::string_view screenId) {
    // Any other screen, or none, cancels a custom screen still waiting for its list.
    if (screenId != _pendingCustom) {
        _pendingCustom.clear();
    }
    if (screenId == _current) {
        return;
    }
    if (Item const* const previous = itemOf(_current)) {
        previous->button->setSelected(false);
    }
    if (CustomItem const* const previous = customItemOf(_current)) {
        previous->button->setSelected(false);
    }
    _current = std::string{screenId};
    if (Item const* const next = itemOf(_current)) {
        next->button->setSelected(true);
        if (CatalogEntry const* const entry = entryOf(_current); entry != nullptr && _openModule != entry->module) {
            _openModule = entry->module;
            refreshVisibility();
        }
    } else if (CustomItem const* const shown = customItemOf(_current)) {
        shown->button->setSelected(true);
    } else {
        // A custom screen may be open before the list that names it has arrived: its entry is marked when it does.
        if (_current.starts_with(kCustomScreenIdPrefix)) {
            _pendingCustom = std::move(_current);
        }
        _current.clear();
    }
    changed();
}

bool Navigator::setBadge(std::string_view screenId, std::optional<NavigatorBadge> const& badge) {
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

std::optional<NavigatorBadge> Navigator::badgeOf(std::string_view screenId) const {
    Item const* const item = itemOf(screenId);
    return item == nullptr ? std::nullopt : item->shown;
}

bool Navigator::choose(std::vector<std::size_t> const& path) {
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
    if (customItemOf(slot->screenId) == nullptr) {
        CatalogEntry const* const entry = entryOf(slot->screenId);
        if (entry == nullptr || !entry->available) {
            return false;
        }
    }
    if (_options.onOpen) {
        _options.onOpen(slot->screenId);
    }
    return true;
}

void Navigator::setModuleGroups(bool grouped) {
    if (_moduleGroups == grouped) {
        return;
    }
    _moduleGroups = grouped;
    arrange();
    refreshVisibility();
    changed();
}

bool Navigator::setOpenModule(std::string_view module) {
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

bool Navigator::reveal(std::string_view screenId) {
    CatalogEntry const* found = nullptr;
    for (Group const& group : _groups) {
        for (Item const& item : group.items) {
            if (item.entry->id == screenId) {
                found = item.entry;
            }
        }
    }
    if (found == nullptr) {
        return false;
    }
    _search->setText({});
    filter(std::string{});
    if (_moduleGroups) {
        static_cast<void>(setOpenModule(found->module));
    }
    return true;
}

void Navigator::setModuleTexts(std::string closed, std::string open, std::string countFailed) {
    _closedWord = std::move(closed);
    _openWord = std::move(open);
    _failedWord = std::move(countFailed);
    for (Module const& module : _modules) {
        nameModule(module);
    }
}

bool Navigator::setModuleBadge(std::string_view module, std::optional<NavigatorBadge> const& badge, bool failed) {
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

std::optional<NavigatorBadge> Navigator::moduleBadgeOf(std::string_view module) const {
    Module const* const found = moduleOf(module);
    return found == nullptr ? std::nullopt : found->shown;
}

void Navigator::setSearchShown(bool shown) {
    if (!shown) {
        _search->setText({});
        filter({});
    }
    _search->setVisible(shown);
}

void Navigator::relabel() {
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

MenuEntry Navigator::menuEntry(CatalogEntry const& entry) const {
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

Navigator::Menu Navigator::menu() const {
    Menu out;
    for (std::string const& screenId : _pins) {
        out.entries.push_back(menuEntry(*entryOf(screenId)));
        out.targets.push_back(MenuSlot{.screenId = screenId, .items = {}});
    }
    if (!_pins.empty() && !_groups.empty()) {
        out.entries.push_back(MenuEntry{
            .label = {},
            .icon = {},
            .keys = {},
            .checked = std::nullopt,
            .enabled = true,
            .items = {},
            .separator = true,
            .description = {},
        });
        out.targets.push_back(MenuSlot{});
    }
    if (_moduleGroups) {
        moduleMenu(out);
    } else {
        groupMenu(out);
    }
    if (!_customItems.empty()) {
        MenuEntry submenu;
        submenu.label = _options.customHeading;
        MenuSlot slot;
        for (CustomItem const& item : _customItems) {
            MenuEntry made;
            // The mark leads the label, so a custom "Home" never reads as an application's "Home"; the author and the
            // reason are the entry's help line.
            made.label = _options.customMark + ": " + item.entry.title;
            if (!item.entry.mine) {
                made.description = _options.customAuthor + item.entry.savedBy;
            }
            if (!item.entry.broken.empty()) {
                made.description += (made.description.empty() ? "" : "; ") + item.entry.broken;
            }
            if (item.entry.id == _current) {
                made.checked = true;
            }
            submenu.items.push_back(std::move(made));
            slot.items.push_back(MenuSlot{.screenId = item.entry.id, .items = {}});
        }
        out.entries.push_back(std::move(submenu));
        out.targets.push_back(std::move(slot));
    }
    return out;
}

void Navigator::groupMenu(Menu& out) const {
    for (Group const* const listed : catalogOrder()) {
        Group const& group = *listed;
        std::string const heading = headingOf(group);
        MenuEntry submenu;
        MenuSlot slot;
        for (Item const& item : group.items) {
            (heading.empty() ? out.entries : submenu.items).push_back(menuEntry(*item.entry));
            (heading.empty() ? out.targets : slot.items).push_back(MenuSlot{.screenId = item.entry->id, .items = {}});
        }
        if (!heading.empty()) {
            submenu.label = heading;
            out.entries.push_back(std::move(submenu));
            out.targets.push_back(std::move(slot));
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one block per module and per group within it.
void Navigator::moduleMenu(Menu& out) const {
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
            if (auto const& shown = module.shown) {
                submenu.label += " · " + shown->text;
            }
            if (module.failed) {
                submenu.label += " · !";
            }
            out.entries.push_back(std::move(submenu));
            out.targets.push_back(std::move(slot));
        }
    }
}

std::string Navigator::headingOf(Group const& group) const {
    std::string const& text = group.group.empty() ? group.module : group.group;
    return _options.label ? _options.label(text) : text;
}

void Navigator::changed() const {
    if (_onChanged) {
        _onChanged();
    }
}

std::string Navigator::lower(std::string_view text) {
    std::string out{text};
    std::ranges::transform(
        out, out.begin(), [](char byte) { return static_cast<char>(std::tolower(static_cast<unsigned char>(byte))); });
    return out;
}

std::vector<std::string> Navigator::titlesOf(CatalogEntry const& entry) const {
    if (_options.titles) {
        std::vector<std::string> titles = _options.titles(entry);
        if (!titles.empty()) {
            return titles;
        }
    }
    return catalogTitles(entry, nullptr);
}

CatalogEntry const* Navigator::entryOf(std::string_view screenId) const {
    auto const found = std::ranges::find(_options.entries, screenId, &CatalogEntry::id);
    return found == _options.entries.end() ? nullptr : &*found;
}

void Navigator::wire(ButtonWidget& button, CatalogEntry const& entry, std::string const& title) {
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

void Navigator::listEntries() {
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

void Navigator::makeModuleRow(std::string const& moduleName) {
    Module made;
    made.module = moduleName;
    made.entries = static_cast<std::size_t>(std::ranges::count(_options.entries, moduleName, &CatalogEntry::module));
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

void Navigator::makeGroup(Group& group) {
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
        Item item{
            .entry = &entry,
            .titles = titlesOf(entry),
            .row = _backend->createStack(group.section.get(), Axis::Horizontal),
            .button = {},
            .content = {},
            .label = {},
            .badge = {},
            .shown = std::nullopt,
        };
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

std::vector<Navigator::Group const*> Navigator::catalogOrder() const {
    std::vector<Group const*> out;
    for (CatalogEntry const& entry : _options.entries) {
        auto const found = std::ranges::find_if(_groups, [&entry](Group const& candidate) {
            return candidate.module == entry.module && candidate.group == entry.group;
        });
        if (found != _groups.end() && !std::ranges::contains(out, &*found)) {
            out.push_back(&*found);
        }
    }
    return out;
}

void Navigator::arrange() {
    std::vector<Group const*> const listed = catalogOrder();
    bool interleaved = false;
    for (std::size_t index = 0; index < listed.size(); ++index) {
        interleaved = interleaved || listed.at(index) != &_groups.at(index);
    }
    bool const moduleMajor = _moduleGroups || !interleaved;
    if (moduleMajor == _moduleMajor) {
        return;
    }
    _moduleMajor = moduleMajor;
    // After the search field and the pins.
    std::size_t slot = 2;
    if (moduleMajor) {
        for (Module& module : _modules) {
            _root->moveChild(*module.row, slot++);
            for (Group& group : _groups) {
                if (group.module == module.module) {
                    _root->moveChild(*group.section, slot++);
                }
            }
        }
        return;
    }
    for (Module& module : _modules) {
        _root->moveChild(*module.row, slot++);
    }
    for (Group const* const group : listed) {
        _root->moveChild(*group->section, slot++);
    }
}

void Navigator::filter(std::string const& text) {
    _searchText = text;
    refreshVisibility();
}

Navigator::Module const* Navigator::moduleOf(std::string_view module) const {
    auto const found = std::ranges::find(_modules, module, &Module::module);
    return found == _modules.end() ? nullptr : &*found;
}

Navigator::Module* Navigator::moduleOf(std::string_view module) {
    auto const found = std::ranges::find(_modules, module, &Module::module);
    return found == _modules.end() ? nullptr : &*found;
}

std::string Navigator::moduleLabel(Module const& module) const {
    auto const first = std::ranges::find(_groups, module.module, &Group::module);
    if (first != _groups.end()) {
        return headingOf(*first);
    }
    return _options.label ? _options.label(module.module) : module.module;
}

void Navigator::nameModule(Module const& module) const {
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

void Navigator::refreshVisibility() {
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
    bool anyCustom = false;
    for (CustomItem const& item : _customItems) {
        bool const matches = wanted.empty() || lower(item.entry.title).contains(wanted);
        item.row->setVisible(matches);
        anyCustom = anyCustom || matches;
    }
    if (_customSection) {
        _customSection->setVisible(anyCustom);
    }
}

void Navigator::showPins() {
    _pinButtons.clear();
    for (std::string const& screenId : _pins) {
        CatalogEntry const* const entry = entryOf(screenId);
        auto button = _backend->createButton(_pinRow.get());
        button->setTestId("pin_" + screenId);
        wire(*button, *entry, titlesOf(*entry).front());
        _pinButtons.push_back(std::move(button));
    }
}

void Navigator::pinsChanged() {
    showPins();
    if (_options.onPins) {
        _options.onPins(_pins);
    }
    changed();
}

Navigator::CustomItem const* Navigator::customItemOf(std::string_view screenId) const {
    auto const found = std::ranges::find_if(
        _customItems, [screenId](CustomItem const& item) { return !screenId.empty() && item.entry.id == screenId; });
    return found == _customItems.end() ? nullptr : &*found;
}

void Navigator::makeCustomSection() {
    _customSection = _backend->createStack(_root.get(), Axis::Vertical);
    _customSection->setGap(1);
    _customSection->setTestId("navigator_custom_section");
    _customSection->setVisible(false);
    _customHeading = _backend->createText(_customSection.get());
    _customHeading->setRole(TextRole::Overline);
    _customHeading->setTestId("navigator_custom");
    _customHeading->setText(_options.customHeading);
}

void Navigator::setCustom(std::vector<NavigatorCustomEntry> entries) {
    std::vector<NavigatorCustomEntry> accepted;
    for (NavigatorCustomEntry& entry : entries) {
        if (!entry.id.starts_with(kCustomScreenIdPrefix)) {
            log::logWarn("[navigator] a custom entry is skipped: its id {} does not start with {}", entry.id,
                         kCustomScreenIdPrefix);
        } else if (std::ranges::contains(accepted, entry.id, &NavigatorCustomEntry::id)) {
            log::logWarn("[navigator] a custom entry is skipped: its id {} is listed twice", entry.id);
        } else {
            accepted.push_back(std::move(entry));
        }
    }
    if (accepted == _custom) {
        return;
    }
    _customItems.clear();
    _custom = std::move(accepted);
    for (NavigatorCustomEntry const& entry : _custom) {
        CustomItem item{.entry = entry,
                        .row = _backend->createStack(_customSection.get(), Axis::Horizontal),
                        .button = {},
                        .content = {},
                        .mark = {},
                        .label = {},
                        .author = {},
                        .broken = {}};
        item.row->setTestId("nav_row_" + entry.id);
        item.row->setLayout({.width = Sizing::stretch(), .height = {}});
        item.row->setAlign(Align::Center);
        item.button = _backend->createButton(item.row.get());
        item.button->setTestId("nav_" + entry.id);
        item.button->setStyle(ButtonStyle::Item);
        item.button->setLayout({.width = Sizing::stretch(), .height = {}});
        item.button->setLabel(entry.title);
        item.button->setAccessibleName(_options.customMark + ", " + entry.title +
                                       (entry.mine ? std::string{} : ", " + _options.customAuthor + entry.savedBy) +
                                       (entry.broken.empty() ? std::string{} : ", " + _options.customBrokenName));
        if (!entry.broken.empty()) {
            item.button->setTooltip(entry.broken);
        }
        std::string const screenId = entry.id;
        item.button->setOnClick([this, screenId] {
            // A copy: the handler may list the custom screens again, which destroys this closure and its capture.
            std::string const opening = screenId;  // NOLINT(performance-unnecessary-copy-initialization): see above
            if (_options.onOpen) {
                _options.onOpen(opening);
            }
        });
        // The mark, the title, the author and the badge are the button's content, so its selection spans them all.
        ContainerWidget& content = item.button->content();
        item.content = _backend->createStack(&content, Axis::Horizontal);
        item.content->setTestId("nav_content_" + entry.id);
        item.content->setGap(1);
        item.content->setAlign(Align::Center);
        item.content->setLayout({.width = Sizing::stretch(), .height = {}});
        item.mark = _backend->createBadge(item.content.get());
        item.mark->setTestId("nav_mark_" + entry.id);
        item.mark->setText(_options.customMark);
        item.mark->setTone(Tone::Info);
        item.label = _backend->createText(item.content.get());
        item.label->setTestId("nav_title_" + entry.id);
        item.label->setText(entry.title);
        item.label->setLayout({.width = Sizing::stretch(), .height = {}});
        item.author = _backend->createText(item.content.get());
        item.author->setTestId("nav_author_" + entry.id);
        item.author->setRole(TextRole::Muted);
        item.author->setText(_options.customAuthor + entry.savedBy);
        item.author->setVisible(!entry.mine);
        item.broken = _backend->createBadge(item.content.get());
        item.broken->setTestId("nav_broken_" + entry.id);
        item.broken->setText(_options.customBrokenMark);
        item.broken->setTone(Tone::Err);
        item.broken->setVisible(!entry.broken.empty());
        if (entry.id == _current) {
            item.button->setSelected(true);
        }
        _customItems.push_back(std::move(item));
    }
    if (!_pendingCustom.empty()) {
        if (CustomItem const* const arrived = customItemOf(_pendingCustom)) {
            _current = std::move(_pendingCustom);
            _pendingCustom.clear();
            arrived->button->setSelected(true);
        }
    }
    if (!_current.empty() && itemOf(_current) == nullptr && customItemOf(_current) == nullptr) {
        _current.clear();
    }
    refreshVisibility();
    changed();
}
}  // namespace morph::ui
