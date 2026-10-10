// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../../util/datetime.hpp"
#include "../backend.hpp"
#include "../view.hpp"

/// @file
/// @brief `morph::ui::testing::RecordingBackend`: a headless widget tree with an operation log, a golden dump and
///        user-interaction helpers — the test double and the reference for every real backend.
///
/// Specified in `docs/spec/ui/backend_contract.md`, "RecordingBackend".

namespace morph::ui::testing {

namespace detail {

/// @brief Properties fixed when a widget is created, as name and formatted value.
using PropList = std::vector<std::pair<std::string, std::string>>;

/// @brief Formats a flag.
/// @param value The flag.
/// @return `true` or `false`.
[[nodiscard]] inline std::string formatBool(bool value) { return value ? "true" : "false"; }

/// @brief Escapes text with a backslash: a line break becomes backslash and `n`, a carriage return backslash and `r`,
///        and a backslash or a character @p isSpecial selects is preceded by a backslash.
/// @tparam Special A callable taking a `char` and returning whether to escape it.
/// @param text The text.
/// @param isSpecial Selects the characters to escape besides the backslash and the two line-ending characters.
/// @return The escaped text.
template <typename Special>
[[nodiscard]] std::string escape(std::string_view text, Special const& isSpecial) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char const character : text) {
        if (character == '\n') {
            escaped += "\\n";
        } else if (character == '\r') {
            escaped += "\\r";
        } else {
            if (character == '\\' || isSpecial(character)) {
                escaped += '\\';
            }
            escaped += character;
        }
    }
    return escaped;
}

/// @brief Formats free text as a property value. Line breaks and `=` are escaped, so the value stays on its own line
///        and no part of it reads as another `name=value`.
/// @param text The text, UTF-8.
/// @return The escaped text.
[[nodiscard]] inline std::string formatText(std::string_view text) {
    return escape(text, [](char character) { return character == '='; });
}

/// @brief Formats free text as an element of a `formatList` list. Also escapes `,`, `[` and `]`, so the text reads as
///        one element.
/// @param text The text, UTF-8.
/// @return The escaped text.
[[nodiscard]] inline std::string formatItem(std::string_view text) {
    return escape(text, [](char character) { return std::string_view{"=,[]"}.contains(character); });
}

/// @brief Formats a menu's entries: `[a,b]`, each entry its label followed by the attributes that differ from the
///        default, as `;icon=x`, `;keys=x`, `;checked=true` or `;checked=false`, and `;enabled=false`, then its
///        submenu in brackets. A flat menu of plain entries reads `[Open,Quit]`.
/// @param entries The entries.
/// @return Their text; `=`, `,`, `[`, `]` and `;` in a label or attribute are escaped.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is formatted as a menu one level down.
[[nodiscard]] inline std::string formatMenu(std::vector<MenuEntry> const& entries) {
    auto const text = [](std::string_view value) {
        return escape(value, [](char character) { return std::string_view{"=,[];"}.contains(character); });
    };
    std::string formatted = "[";
    for (MenuEntry const& entry : entries) {
        formatted += formatted.size() == 1 ? "" : ",";
        formatted += text(entry.label);
        if (!entry.icon.empty()) {
            formatted += ";icon=" + text(entry.icon);
        }
        if (!entry.keys.empty()) {
            formatted += ";keys=" + text(entry.keys);
        }
        if (entry.checked.has_value()) {
            formatted += std::string{";checked="} + (*entry.checked ? "true" : "false");
        }
        if (!entry.enabled) {
            formatted += ";enabled=false";
        }
        if (!entry.items.empty()) {
            formatted += formatMenu(entry.items);
        }
    }
    return formatted + "]";
}

/// @brief Formats a key: an integer in decimal, a string in double quotes, so `7` and `"7"` differ.
/// @param key The key.
/// @return Its text. A string key is escaped as `formatItem` escapes, and a double quote in it is preceded by a
///         backslash.
[[nodiscard]] inline std::string formatKey(Key const& key) {
    if (auto const* const number = std::get_if<std::int64_t>(&key); number != nullptr) {
        return std::to_string(*number);
    }
    return '"' +
           escape(std::get<std::string>(key),
                  [](char character) { return std::string_view{"=,[]\""}.contains(character); }) +
           '"';
}

/// @brief Formats an optional key.
/// @param key The key, or none.
/// @return `formatKey(*key)`, or `none`.
[[nodiscard]] inline std::string formatOptionalKey(std::optional<Key> const& key) {
    return key.has_value() ? formatKey(*key) : std::string{"none"};
}

/// @brief Formats one dimension's sizing.
/// @param sizing The sizing.
/// @return `content`, `fixed(n)` or `stretch(n)`.
[[nodiscard]] inline std::string formatSizing(Sizing sizing) {
    switch (sizing.kind) {
        case Sizing::Kind::Content:
            return "content";
        case Sizing::Kind::Fixed:
            return "fixed(" + std::to_string(sizing.amount) + ")";
        case Sizing::Kind::Stretch:
            return "stretch(" + std::to_string(sizing.amount) + ")";
        default:
            return "unknown";
    }
}

/// @brief Formats a size request.
/// @param hints The request.
/// @return `width/height`, each as `formatSizing` gives it.
[[nodiscard]] inline std::string formatLayout(LayoutHints const& hints) {
    return formatSizing(hints.width) + "/" + formatSizing(hints.height);
}

/// @brief Formats a date-time value.
/// @param value The value.
/// @return `none` for `nullopt`, `empty` for an empty `Timestamp`, else the ISO-8601 UTC instant.
[[nodiscard]] inline std::string formatTimestamp(std::optional<morph::time::Timestamp> const& value) {
    if (!value.has_value()) {
        return "none";
    }
    if (!value->hasValue()) {
        return "empty";
    }
    return (**value).toIso8601();
}

/// @brief Formats a list as `[a,b,c]`.
/// @tparam T The element type.
/// @tparam F A callable turning a `T const&` into a `std::string`.
/// @param items The elements.
/// @param each Formats one element.
/// @return The list's text.
template <typename T, typename F>
[[nodiscard]] std::string formatList(std::vector<T> const& items, F const& each) {
    std::string text = "[";
    bool first = true;
    for (T const& item : items) {
        if (!first) {
            text += ',';
        }
        first = false;
        text += each(item);
    }
    return text + "]";
}

/// @brief A text role's name.
/// @param role The role.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(TextRole role) {
    switch (role) {
        case TextRole::Normal:
            return "Normal";
        case TextRole::Muted:
            return "Muted";
        case TextRole::Heading:
            return "Heading";
        case TextRole::Error:
            return "Error";
        case TextRole::Success:
            return "Success";
        default:
            return "unknown";
    }
}

/// @brief A text input mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(TextInputMode mode) {
    switch (mode) {
        case TextInputMode::SingleLine:
            return "SingleLine";
        case TextInputMode::Multiline:
            return "Multiline";
        case TextInputMode::Password:
            return "Password";
        default:
            return "unknown";
    }
}

/// @brief A select style's name.
/// @param style The style.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(SelectStyle style) {
    switch (style) {
        case SelectStyle::Dropdown:
            return "Dropdown";
        case SelectStyle::Radio:
            return "Radio";
        default:
            return "unknown";
    }
}

/// @brief An axis's name.
/// @param axis The axis.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(Axis axis) {
    switch (axis) {
        case Axis::Vertical:
            return "Vertical";
        case Axis::Horizontal:
            return "Horizontal";
        default:
            return "unknown";
    }
}

/// @brief A date mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(DateMode mode) {
    switch (mode) {
        case DateMode::Date:
            return "Date";
        case DateMode::DateTime:
            return "DateTime";
        default:
            return "unknown";
    }
}

/// @brief A file picker mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(FilePickerMode mode) {
    switch (mode) {
        case FilePickerMode::Open:
            return "Open";
        case FilePickerMode::Save:
            return "Save";
        default:
            return "unknown";
    }
}

/// @brief A selection mode's name.
/// @param mode The mode.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(SelectionMode mode) {
    switch (mode) {
        case SelectionMode::None:
            return "None";
        case SelectionMode::Single:
            return "Single";
        case SelectionMode::Multiple:
            return "Multiple";
        default:
            return "unknown";
    }
}

/// @brief The handlers a widget was given, which the interaction helpers call.
struct Callbacks {
    /// @brief From `ButtonWidget::setOnClick`.
    Action click;
    /// @brief From `TextInputWidget::setOnChange`.
    std::function<void(std::string)> change;
    /// @brief From `TextInputWidget::setOnSubmit`.
    std::function<void(std::string)> submit;
    /// @brief From `TextInputWidget::setOnCommit`.
    std::function<void(std::string)> commit;
    /// @brief From `CheckboxWidget::setOnToggle` or `PanelWidget::setOnToggle`.
    std::function<void(bool)> toggle;
    /// @brief From `SelectWidget::setOnSelect`.
    std::function<void(Key)> select;
    /// @brief From `TabsWidget::setOnSelect`.
    std::function<void(std::size_t)> index;
    /// @brief From `DateTimeInputWidget::setOnChange`.
    std::function<void(std::optional<morph::time::Timestamp>)> dateTime;
    /// @brief From `SliderWidget::setOnChange`.
    std::function<void(std::int64_t)> slide;
    /// @brief From `FilePickerWidget::setOnPicked`.
    std::function<void(std::string)> picked;
    /// @brief From `DialogWidget::setOnDismiss`.
    Action dismiss;
    /// @brief From `TableWidget::setOnSelectionChange`.
    std::function<void(std::vector<Key>)> selection;
    /// @brief From `TableWidget::setOnActivate`.
    std::function<void(Key)> activate;
    /// @brief The drop handler's predicate, from `Widget::setDropHandler`.
    std::function<bool(Key const&)> accepts;
    /// @brief The drop handler, from `Widget::setDropHandler`.
    std::function<void(Key)> drop;
    /// @brief The chord handler, from `Widget::setKeys`.
    std::function<void(std::string)> chord;
    /// @brief A menu's activation handler, from `MenuWidget::setOnActivate`.
    std::function<void(std::vector<std::size_t>)> path;
};

/// @brief One fake widget's state.
struct Record {
    /// @brief The widget's id: its creation number, from 1.
    int id = 0;
    /// @brief The kind shown in the log and the dump.
    std::string kind;
    /// @brief The parent's id; 0 for a root.
    int parent = 0;
    /// @brief The children's ids, in order.
    std::vector<int> children;
    /// @brief Every property set so far, formatted, in name order.
    std::map<std::string, std::string, std::less<>> props;
    /// @brief The handlers set so far.
    Callbacks callbacks;
    /// @brief The drag key, as `setDragKey` last set it.
    std::optional<Key> dragKey;
    /// @brief The chords, as `setKeys` last set them.
    std::vector<std::string> chords;
    /// @brief A menu's entries, as `setItems` last set them.
    std::vector<MenuEntry> menu;
    /// @brief The fake widget object.
    Widget* widget = nullptr;
    /// @brief Called with a child's id once that child is destroyed; a fake table re-marks its selection with it.
    std::function<void(int)> onChildDestroyed;
};

/// @brief The records, the roots and the operation log that every fake widget of one backend writes to.
///
/// Held by `shared_ptr` from the backend and from every fake, so a fake that outlives its backend still has
/// somewhere to record its destruction.
class RecordStore {
public:
    /// @brief Records a new widget appended to @p parent and logs its creation.
    /// @param kind The kind shown in the log and the dump.
    /// @param parent The container it is appended to, or null for a root.
    /// @param widget The fake widget.
    /// @param initial Properties fixed at creation; recorded without a log line.
    /// @return The new widget's id.
    int create(std::string kind, ContainerWidget const* parent, Widget& widget, PropList initial) {
        int const widgetId = ++_nextId;
        int const parentId = parent == nullptr ? 0 : idOf(*parent);
        Record record{.id = widgetId, .kind = std::move(kind), .parent = parentId, .widget = &widget};
        for (auto& [name, value] : initial) {
            record.props.insert_or_assign(std::move(name), std::move(value));
        }
        _log.push_back("create " + label(record) + " in " +
                       (parentId == 0 ? std::string{"root"} : label(at(parentId))));
        siblingsOf(parentId).push_back(widgetId);
        _ids.insert_or_assign(&widget, widgetId);
        _records.insert_or_assign(widgetId, std::move(record));
        return widgetId;
    }

    /// @brief Sets a property through a widget setter, and logs it.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @param value Its formatted value.
    void set(int widgetId, std::string_view name, std::string value) {
        Record& record = at(widgetId);
        _log.push_back("set " + label(record) + " " + std::string{name} + "=" + value);
        record.props.insert_or_assign(std::string{name}, std::move(value));
    }

    /// @brief Sets a property the user changed through the widget itself; no setter ran, so nothing is logged.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @param value Its formatted value.
    void setByUser(int widgetId, std::string_view name, std::string value) {
        at(widgetId).props.insert_or_assign(std::string{name}, std::move(value));
    }

    /// @brief Moves a widget among its siblings, as `ContainerWidget::moveChild` defines it, and logs it.
    /// @param widgetId The widget.
    /// @param index The new position; past the end means last.
    // An id and a position: an `int` and a `std::size_t`, which convert into each other.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void move(int widgetId, std::size_t index) {
        Record const& record = at(widgetId);
        std::vector<int>& siblings = siblingsOf(record.parent);
        std::erase(siblings, widgetId);
        auto const position = static_cast<std::ptrdiff_t>(std::min(index, siblings.size()));
        siblings.insert(siblings.begin() + position, widgetId);
        _log.push_back("move " + label(record) + " to " + std::to_string(index));
    }

    /// @brief Forgets a destroyed widget and logs it. Its remaining children become roots.
    /// @param widgetId The widget; an unknown id does nothing.
    void destroy(int widgetId) {
        auto const found = _records.find(widgetId);
        if (found == _records.end()) {
            return;
        }
        Record const& record = found->second;
        int const parentId = record.parent;
        _log.push_back("destroy " + label(record));
        std::erase(siblingsOf(parentId), widgetId);
        for (int const child : record.children) {
            if (auto const orphan = _records.find(child); orphan != _records.end()) {
                orphan->second.parent = 0;
                _roots.push_back(child);
            }
        }
        if (_focused == widgetId) {
            _focused = 0;
        }
        _ids.erase(record.widget);
        _records.erase(found);
        if (auto const parent = _records.find(parentId); parent != _records.end()) {
            if (auto const notify = parent->second.onChildDestroyed; notify) {
                notify(widgetId);
            }
        }
    }

    /// @brief A widget's record.
    /// @param widgetId The widget.
    /// @return The record.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] Record& at(int widgetId) {
        auto const found = _records.find(widgetId);
        if (found == _records.end()) {
            throw std::out_of_range{"RecordingBackend: no widget #" + std::to_string(widgetId)};
        }
        return found->second;
    }

    /// @brief A widget's record.
    /// @param widgetId The widget.
    /// @return The record.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] Record const& at(int widgetId) const {
        auto const found = _records.find(widgetId);
        if (found == _records.end()) {
            throw std::out_of_range{"RecordingBackend: no widget #" + std::to_string(widgetId)};
        }
        return found->second;
    }

    /// @brief The id of a live widget made by this store's backend.
    /// @param widget The widget.
    /// @return Its id.
    /// @throws std::out_of_range when @p widget is not one of them.
    [[nodiscard]] int idOf(Widget const& widget) const {
        auto const found = _ids.find(&widget);
        if (found == _ids.end()) {
            throw std::out_of_range{"RecordingBackend: not a live widget of this backend"};
        }
        return found->second;
    }

    /// @brief Every live widget's record.
    /// @return The records, by id.
    [[nodiscard]] std::map<int, Record> const& records() const noexcept { return _records; }

    /// @brief The operation log.
    /// @return One line per operation, oldest first.
    [[nodiscard]] std::vector<std::string> const& log() const noexcept { return _log; }

    /// @brief Empties the operation log.
    void clearLog() noexcept { _log.clear(); }

    /// @brief Records that keyboard focus moved to a widget, and logs `focus`.
    /// @param widgetId The widget that took focus.
    /// @throws std::out_of_range for an id that names no live widget.
    void focus(int widgetId) {
        _log.push_back("focus " + label(at(widgetId)));
        _focused = widgetId;
    }

    /// @brief The widget that last took focus.
    /// @return Its id, or 0 when none has, or it has since been destroyed.
    [[nodiscard]] int focused() const noexcept { return _focused; }

    /// @brief The golden tree.
    /// @return One line per live widget, children indented two spaces per depth.
    [[nodiscard]] std::string dump() const {
        std::string text;
        for (int const root : _roots) {
            dumpInto(text, root, 0);
        }
        return text;
    }

private:
    [[nodiscard]] static std::string label(Record const& record) {
        return record.kind + "#" + std::to_string(record.id);
    }

    std::vector<int>& siblingsOf(int parentId) { return parentId == 0 ? _roots : at(parentId).children; }

    // A widget tree is dumped by descending it. The id and the depth are an `int` and a `std::size_t`, which convert
    // into each other.
    // NOLINTNEXTLINE(misc-no-recursion,bugprone-easily-swappable-parameters)
    void dumpInto(std::string& text, int widgetId, std::size_t depth) const {
        Record const& record = at(widgetId);
        text.append(depth * 2, ' ');
        text += label(record);
        for (auto const& [name, value] : record.props) {
            text += ' ';
            text += name;
            text += '=';
            text += value;
        }
        text += '\n';
        for (int const child : record.children) {
            dumpInto(text, child, depth + 1);
        }
    }

    std::map<int, Record> _records;
    std::unordered_map<Widget const*, int> _ids;
    std::vector<int> _roots;
    std::vector<std::string> _log;
    int _nextId = 0;
    int _focused = 0;
};

/// @brief The `Widget` half of every fake: records itself on construction, records `Common`'s setters, and records
///        its destruction.
/// @tparam Interface The widget interface the fake implements.
template <typename Interface>
class FakeLeaf : public Interface {
public:
    /// @brief Records the new widget, appended to @p parent, and logs its creation.
    /// @param store Where the widget records itself.
    /// @param kind The kind shown in the log and the dump.
    /// @param parent The container it is appended to, or null for a root.
    /// @param initial Properties fixed at creation.
    FakeLeaf(std::shared_ptr<RecordStore> store, std::string kind, ContainerWidget const* parent,
             PropList initial = {})
        : _store{std::move(store)}, _id{_store->create(std::move(kind), parent, *this, std::move(initial))} {}

    /// @brief Forgets the widget and logs its destruction; its remaining children become roots.
    ~FakeLeaf() override {
        try {
            _store->destroy(_id);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Logging the destruction failed to allocate; the record stays, and a test reads it as a leak.
        }
    }

    FakeLeaf(FakeLeaf const&) = delete;
    FakeLeaf& operator=(FakeLeaf const&) = delete;
    FakeLeaf(FakeLeaf&&) = delete;
    FakeLeaf& operator=(FakeLeaf&&) = delete;

    /// @brief Records property `visible`.
    /// @param visible Whether it is shown.
    void setVisible(bool visible) override { set("visible", formatBool(visible)); }

    /// @brief Records property `enabled`.
    /// @param enabled Whether it accepts input.
    void setEnabled(bool enabled) override { set("enabled", formatBool(enabled)); }

    /// @brief Records property `layout`, as `formatLayout` gives it.
    /// @param hints The request in both dimensions.
    void setLayout(LayoutHints const& hints) override { set("layout", formatLayout(hints)); }

    /// @brief Records property `dragKey` and keeps the key for `RecordingBackend::drag`.
    /// @param key Engaged: the key a drop delivers. `nullopt`: not draggable.
    void setDragKey(std::optional<Key> const& key) override {
        _store->at(_id).dragKey = key;
        set("dragKey", formatOptionalKey(key));
    }

    /// @brief Records property `drop=handler` and keeps both handlers for `RecordingBackend::drag`.
    /// @param accepts Whether a dragged key may be dropped here.
    /// @param onDrop Called with the key on a drop that `accepts` allowed.
    void setDropHandler(std::function<bool(Key const&)> accepts, std::function<void(Key)> onDrop) override {
        Callbacks& handlers = callbacks();
        handlers.accepts = std::move(accepts);
        handlers.drop = std::move(onDrop);
        set("drop", "handler");
    }

    /// @brief Records property `a11yName`.
    /// @param name The accessible name.
    void setAccessibleName(std::string_view name) override { set("a11yName", formatText(name)); }

    /// @brief Records property `a11yRole`.
    /// @param role The role.
    void setAccessibleRole(std::string_view role) override { set("a11yRole", formatText(role)); }

    /// @brief Records property `testId`.
    /// @param testId The identifier.
    void setTestId(std::string_view testId) override { set("testId", formatText(testId)); }

    /// @brief Records property `tooltip`.
    /// @param text The tooltip.
    void setTooltip(std::string_view text) override { set("tooltip", formatText(text)); }

    /// @brief Records property `surface`.
    /// @param surface The surface's name.
    void setSurface(std::string_view surface) override { set("surface", formatText(surface)); }

    /// @brief Records property `keys`, the chords joined by commas, and keeps the chords and the handler for
    ///        `RecordingBackend::press`.
    /// @param chords The chords.
    /// @param onChord Called with the chord that was pressed.
    void setKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) override {
        Record& record = _store->at(_id);
        record.callbacks.chord = std::move(onChord);
        record.chords = chords;
        std::string joined;
        for (std::string const& chord : chords) {
            joined += joined.empty() ? "" : ",";
            joined += chord;
        }
        set("keys", formatText(joined));
    }

    /// @brief Moves the backend's recorded focus here, and logs `focus`.
    void focus() override { _store->focus(_id); }

protected:
    /// @brief Records a property set through a setter.
    /// @param name The property.
    /// @param value Its formatted value.
    void set(std::string_view name, std::string value) { _store->set(_id, name, std::move(value)); }

    /// @brief This widget's handlers.
    /// @return The handlers, for a setter to store into.
    [[nodiscard]] Callbacks& callbacks() { return _store->at(_id).callbacks; }

    /// @brief The store this widget records into.
    /// @return The store.
    [[nodiscard]] RecordStore& store() const noexcept { return *_store; }

    /// @brief This widget's id.
    /// @return The id.
    [[nodiscard]] int id() const noexcept { return _id; }

private:
    std::shared_ptr<RecordStore> _store;
    int _id;
};

/// @brief The `ContainerWidget` half of every fake container.
/// @tparam Interface The container interface the fake implements.
template <typename Interface>
class FakeContainer : public FakeLeaf<Interface> {
public:
    using FakeLeaf<Interface>::FakeLeaf;

    /// @brief Moves a child among its siblings and logs it.
    /// @param child One of this container's children.
    /// @param index The new position; past the end means last.
    /// @throws std::logic_error when @p child is not this container's child.
    void moveChild(Widget& child, std::size_t index) override {
        RecordStore& records = this->store();
        int const childId = records.idOf(child);
        if (records.at(childId).parent != this->id()) {
            throw std::logic_error{"RecordingBackend: moveChild of a widget that is not this container's child"};
        }
        records.move(childId, index);
    }
};

/// @brief The `FieldWidget` half of every fake field: properties `readonly`, `required`, `errors`, `stale`.
/// @tparam Interface The field interface the fake implements.
template <typename Interface>
class FakeField : public FakeLeaf<Interface> {
public:
    using FakeLeaf<Interface>::FakeLeaf;

    /// @brief Records property `readonly`; `RecordingBackend`'s edits leave a read-only field alone.
    /// @param readonly Whether the user may not change the value.
    void setReadOnly(bool readonly) override { this->set("readonly", formatBool(readonly)); }

    /// @brief Records property `required`.
    /// @param required Whether the field is marked required.
    void setRequired(bool required) override { this->set("required", formatBool(required)); }

    /// @brief Records property `errors`, as `formatList` gives the messages.
    /// @param errors The messages.
    void setErrors(std::vector<std::string> const& errors) override {
        this->set("errors", formatList(errors, [](std::string const& error) { return formatItem(error); }));
    }

    /// @brief Records property `stale`.
    /// @param stale Whether the value is being recomputed.
    void setStale(bool stale) override { this->set("stale", formatBool(stale)); }
};

/// @brief A fake `TextWidget`: properties `text`, `role`.
class FakeText final : public FakeField<TextWidget> {
public:
    using FakeField<TextWidget>::FakeField;
    void setText(std::string_view text) override { set("text", formatText(text)); }
    void setRole(TextRole role) override { set("role", enumName(role)); }
};

/// @brief A fake `ButtonWidget`: property `label`.
class FakeButton final : public FakeLeaf<ButtonWidget> {
public:
    using FakeLeaf<ButtonWidget>::FakeLeaf;
    void setLabel(std::string_view label) override { set("label", formatText(label)); }
    void setOnClick(Action onClick) override { callbacks().click = std::move(onClick); }
};

/// @brief A fake `TextInputWidget`: properties `mode`, `text`, `placeholder`, the field state, and a text cursor.
///
/// The cursor is a byte offset into the text. A user's edit puts it where the edit left it; `setText` with the text
/// the field shows leaves it alone, as the contract requires, and `setText` with another text puts it at the end.
class FakeTextInput final : public FakeField<TextInputWidget> {
public:
    using FakeField<TextInputWidget>::FakeField;

    /// @brief Records property `text`; moves the cursor to the end only when the text changes.
    /// @param text The text, UTF-8.
    void setText(std::string_view text) override {
        if (text != _shown) {
            _shown = std::string{text};
            _cursor = _shown.size();
        }
        set("text", formatText(text));
    }

    /// @brief The text the user typed: the field shows it, and the cursor is at @p cursor; nothing is logged.
    /// @param text The whole new text.
    /// @param cursor Where the cursor ends up; clamped to the text's size.
    void typedByUser(std::string text, std::size_t cursor) {
        _shown = std::move(text);
        _cursor = std::min(cursor, _shown.size());
        store().setByUser(id(), "text", formatText(_shown));
    }

    /// @brief Moves the cursor as the user's arrow keys or a click would.
    /// @param cursor The new position; clamped to the text's size.
    void moveCursorByUser(std::size_t cursor) noexcept { _cursor = std::min(cursor, _shown.size()); }

    /// @brief Where the cursor is.
    /// @return A byte offset into the text shown.
    [[nodiscard]] std::size_t cursor() const noexcept { return _cursor; }

    /// @brief The text the field shows.
    /// @return The text, unformatted.
    [[nodiscard]] std::string const& text() const noexcept { return _shown; }

    void setPlaceholder(std::string_view placeholder) override { set("placeholder", formatText(placeholder)); }
    void setOnChange(std::function<void(std::string)> onChange) override { callbacks().change = std::move(onChange); }
    void setOnSubmit(std::function<void(std::string)> onSubmit) override { callbacks().submit = std::move(onSubmit); }
    void setOnCommit(std::function<void(std::string)> onCommit) override { callbacks().commit = std::move(onCommit); }

private:
    std::string _shown;
    std::size_t _cursor = 0;
};

/// @brief A fake `CheckboxWidget`: properties `label`, `checked`.
class FakeCheckbox final : public FakeField<CheckboxWidget> {
public:
    using FakeField<CheckboxWidget>::FakeField;
    void setLabel(std::string_view label) override { set("label", formatText(label)); }
    void setChecked(bool checked) override { set("checked", formatBool(checked)); }
    void setOnToggle(std::function<void(bool)> onToggle) override { callbacks().toggle = std::move(onToggle); }
};

/// @brief A fake `SelectWidget`: properties `style`, `options`, `selected`.
///
/// `selected` shows the option the widget marks: the requested key while the options contain it, else `none`. The
/// requested key is kept, so options that bring it back mark it again; that re-marking is the widget's own doing and
/// is not logged.
class FakeSelect final : public FakeField<SelectWidget> {
public:
    using FakeField<SelectWidget>::FakeField;

    /// @brief Records property `options`, and re-marks the requested key against them.
    /// @param options The options, in order.
    void setOptions(std::vector<SelectOption> const& options) override {
        _options = options;
        set("options", formatList(options, [](SelectOption const& option) {
                return formatKey(option.key) + ":" + formatItem(option.label);
            }));
        if (_requested.has_value()) {
            store().setByUser(id(), "selected", marked());
        }
    }

    /// @brief Requests the option with @p key, and records property `selected` as the option it marks.
    /// @param key The key, or `nullopt` for no selection.
    void setSelected(std::optional<Key> const& key) override {
        _requested.emplace(key);
        set("selected", marked());
    }

    /// @brief Stores the handler `RecordingBackend::choose` calls.
    /// @param onSelect The handler.
    void setOnSelect(std::function<void(Key)> onSelect) override { callbacks().select = std::move(onSelect); }

    /// @brief Whether one of the current options has @p key.
    /// @param key The key.
    /// @return True when an option has it.
    [[nodiscard]] bool offers(Key const& key) const {
        return std::ranges::any_of(_options, [&key](SelectOption const& option) { return option.key == key; });
    }

    /// @brief Marks the option the user chose, as the requested key; nothing is logged.
    /// @param key The key of one of the current options.
    void chooseByUser(Key const& key) {
        _requested.emplace(key);
        store().setByUser(id(), "selected", marked());
    }

private:
    [[nodiscard]] std::string marked() const {
        if (!_requested.has_value() || !_requested->has_value() || !offers(**_requested)) {
            return "none";
        }
        return formatKey(**_requested);
    }

    std::vector<SelectOption> _options;
    // Engaged once `setSelected` ran or the user chose; the inner value is the requested key, or none.
    std::optional<std::optional<Key>> _requested;
};

/// @brief A fake `MenuWidget`: property `items`.
class FakeMenu final : public FakeLeaf<MenuWidget> {
public:
    using FakeLeaf<MenuWidget>::FakeLeaf;
    void setItems(std::vector<MenuEntry> const& entries) override {
        store().at(id()).menu = entries;
        set("items", formatMenu(entries));
    }
    void setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) override {
        callbacks().path = std::move(onActivate);
    }
};

/// @brief A fake `StackWidget`, of kind `Column` or `Row`: property `gap`.
class FakeStack final : public FakeContainer<StackWidget> {
public:
    using FakeContainer<StackWidget>::FakeContainer;
    void setGap(int gap) override { set("gap", std::to_string(gap)); }
};

/// @brief A fake `GridWidget`: properties `columns`, `gap`, and `span` on a child.
class FakeGrid final : public FakeContainer<GridWidget> {
public:
    using FakeContainer<GridWidget>::FakeContainer;
    void setColumns(int columns) override { set("columns", std::to_string(columns)); }
    void setGap(int gap) override { set("gap", std::to_string(gap)); }

    /// @brief Records property `span` on @p child.
    /// @param child One of this grid's children.
    /// @param span The column count it occupies.
    /// @throws std::logic_error when @p child is not this grid's child.
    void setSpan(Widget& child, int span) override {
        int const childId = store().idOf(child);
        if (store().at(childId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setSpan of a widget that is not this grid's child"};
        }
        store().set(childId, "span", std::to_string(span));
    }
};

/// @brief A fake `PanelWidget`: properties `title`, `padding`, `collapsible`, `collapsed`.
class FakePanel final : public FakeContainer<PanelWidget> {
public:
    using FakeContainer<PanelWidget>::FakeContainer;
    void setTitle(std::string_view title) override { set("title", formatText(title)); }
    void setPadding(int padding) override { set("padding", std::to_string(padding)); }
    void setCollapsible(bool collapsible) override { set("collapsible", formatBool(collapsible)); }
    void setCollapsed(bool collapsed) override { set("collapsed", formatBool(collapsed)); }
    void setOnToggle(std::function<void(bool)> onToggle) override { callbacks().toggle = std::move(onToggle); }
};

/// @brief A fake `BusyWidget`: properties `active`, `label`.
class FakeBusy final : public FakeLeaf<BusyWidget> {
public:
    using FakeLeaf<BusyWidget>::FakeLeaf;
    void setActive(bool active) override { set("active", formatBool(active)); }
    void setLabel(std::string_view label) override { set("label", formatText(label)); }
};

/// @brief A fake `DateTimeInputWidget`: properties `mode`, `offset`, `value`.
class FakeDateTimeInput final : public FakeField<DateTimeInputWidget> {
public:
    using FakeField<DateTimeInputWidget>::FakeField;
    void setValue(std::optional<morph::time::Timestamp> const& value) override {
        set("value", formatTimestamp(value));
    }
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override {
        callbacks().dateTime = std::move(onChange);
    }
};

/// @brief A fake `SliderWidget`: properties `range` (`minimum..maximum/step`), `value`.
class FakeSlider final : public FakeField<SliderWidget> {
public:
    using FakeField<SliderWidget>::FakeField;
    void setRange(std::int64_t minimum, std::int64_t maximum, std::int64_t step) override {
        set("range", std::to_string(minimum) + ".." + std::to_string(maximum) + "/" + std::to_string(step));
    }
    void setValue(std::int64_t value) override { set("value", std::to_string(value)); }
    void setOnChange(std::function<void(std::int64_t)> onChange) override { callbacks().slide = std::move(onChange); }
};

/// @brief A fake `FilePickerWidget`: properties `mode`, `path`.
class FakeFilePicker final : public FakeField<FilePickerWidget> {
public:
    using FakeField<FilePickerWidget>::FakeField;
    void setPath(std::string_view path) override { set("path", formatText(path)); }
    void setOnPicked(std::function<void(std::string)> onPicked) override { callbacks().picked = std::move(onPicked); }
};

/// @brief A fake `TabsWidget`: properties `tabs`, `selected`.
class FakeTabs final : public FakeContainer<TabsWidget> {
public:
    using FakeContainer<TabsWidget>::FakeContainer;
    void setTabs(std::vector<std::string> const& labels) override {
        set("tabs", formatList(labels, [](std::string const& label) { return formatItem(label); }));
    }
    void setSelected(std::size_t index) override { set("selected", std::to_string(index)); }
    void setOnSelect(std::function<void(std::size_t)> onSelect) override { callbacks().index = std::move(onSelect); }
};

/// @brief A fake `DialogWidget`: properties `open`, `title`.
class FakeDialog final : public FakeContainer<DialogWidget> {
public:
    using FakeContainer<DialogWidget>::FakeContainer;
    void setOpen(bool open) override { set("open", formatBool(open)); }
    void setTitle(std::string_view title) override { set("title", formatText(title)); }
    void setOnDismiss(Action onDismiss) override { callbacks().dismiss = std::move(onDismiss); }
};

/// @brief A fake `TableWidget`: properties `columns`, `selectionMode`, `selection`, and `rowKey` on a row.
///
/// `selection` shows the rows the widget marks: the requested keys, in the order requested, that one of its current
/// rows has. The requested keys are kept, so a row that arrives with one of them is marked and a row that goes is
/// unmarked; that re-marking is the widget's own doing and is not logged.
class FakeTable final : public FakeContainer<TableWidget> {
public:
    /// @brief Records the new table, appended to @p parent, and logs its creation.
    /// @param store Where the widget records itself.
    /// @param kind The kind shown in the log and the dump.
    /// @param parent The container it is appended to, or null for a root.
    FakeTable(std::shared_ptr<RecordStore> store, std::string kind, ContainerWidget const* parent)
        : FakeContainer<TableWidget>{std::move(store), std::move(kind), parent} {
        this->store().at(id()).onChildDestroyed = [this](int childId) {
            _rowKeys.erase(childId);
            remark();
        };
    }

    /// @brief Removes the hook that points at this table, so nothing calls it once the table is gone, even if the
    ///        record outlives it because recording the destruction failed.
    ~FakeTable() override {
        try {
            store().at(id()).onChildDestroyed = nullptr;
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // No record left, so no hook to remove.
        }
    }

    FakeTable(FakeTable const&) = delete;
    FakeTable& operator=(FakeTable const&) = delete;
    FakeTable(FakeTable&&) = delete;
    FakeTable& operator=(FakeTable&&) = delete;

    /// @brief Records property `columns`, each as its escaped caption, a colon and its width.
    /// @param columns The columns, in order.
    void setColumns(std::vector<TableColumn> const& columns) override {
        set("columns", formatList(columns, [](TableColumn const& column) {
                return formatItem(column.label) + ":" + formatSizing(column.width);
            }));
    }

    /// @brief Records property `selectionMode`, which `RecordingBackend::selectRows` enforces.
    /// @param mode None, one, or any number.
    void setSelectionMode(SelectionMode mode) override {
        _mode = mode;
        set("selectionMode", enumName(mode));
    }

    /// @brief Records property `rowKey` on @p row, and marks the row when its key is requested.
    /// @param row One of this table's rows.
    /// @param key The row's key.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowKey(Widget& row, Key const& key) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowKey of a widget that is not this table's row"};
        }
        store().set(rowId, "rowKey", formatKey(key));
        _rowKeys.insert_or_assign(rowId, key);
        remark();
    }

    /// @brief Requests the rows with @p keys, and records property `selection` as the rows it marks.
    /// @param keys The selected keys.
    void setSelection(std::vector<Key> const& keys) override {
        _requested = keys;
        set("selection", marked());
    }

    /// @brief Stores the handler `RecordingBackend::selectRows` calls.
    /// @param onSelectionChange The handler.
    void setOnSelectionChange(std::function<void(std::vector<Key>)> onSelectionChange) override {
        callbacks().selection = std::move(onSelectionChange);
    }

    /// @brief Stores the handler `RecordingBackend::activateRow` calls.
    /// @param onActivate The handler.
    void setOnActivate(std::function<void(Key)> onActivate) override { callbacks().activate = std::move(onActivate); }

    /// @brief The selection mode last set.
    /// @return The mode; `None` until `setSelectionMode` runs.
    [[nodiscard]] SelectionMode mode() const noexcept { return _mode; }

    /// @brief The current row with @p key.
    /// @param key The key.
    /// @return The row's id, or `nullopt` when no current row has @p key.
    [[nodiscard]] std::optional<int> rowOf(Key const& key) const {
        for (int const child : store().at(id()).children) {
            if (auto const found = _rowKeys.find(child); found != _rowKeys.end() && found->second == key) {
                return child;
            }
        }
        return std::nullopt;
    }

    /// @brief Marks the rows the user selected, as the requested keys; nothing is logged.
    /// @param keys Keys of current rows.
    void selectByUser(std::vector<Key> const& keys) {
        _requested = keys;
        remark();
    }

private:
    [[nodiscard]] std::string marked() const {
        std::vector<Key> keys;
        for (Key const& key : _requested.value_or(std::vector<Key>{})) {
            if (rowOf(key).has_value()) {
                keys.push_back(key);
            }
        }
        return formatList(keys, [](Key const& key) { return formatKey(key); });
    }

    void remark() {
        if (_requested.has_value()) {
            store().setByUser(id(), "selection", marked());
        }
    }

    SelectionMode _mode = SelectionMode::None;
    std::map<int, Key> _rowKeys;  // row id -> key, as setRowKey told it
    // Engaged once `setSelection` ran or the user selected.
    std::optional<std::vector<Key>> _requested;
};

}  // namespace detail

/// @brief A headless `IViewBackend`: fake widgets that record every operation, a golden dump of the live tree, and
///        helpers that act as the user would.
///
/// Widget ids are creation numbers from 1. A widget ignores every interaction helper, as a real one would, while it
/// is hidden or disabled, or while a container around it is hidden, disabled or collapsed. A dialog, and everything
/// inside it, ignores them until it is opened. A helper used on a kind of widget it does not apply to throws
/// `std::logic_error`: that is a mistake in the test, not something a user can do. A helper copies the handler before
/// calling it, so a handler may destroy its own widget.
///
/// Free text in a property value is escaped as `detail::formatText` and `detail::formatItem` describe, so every
/// value stays on its line and reads as one field, or one list element.
class RecordingBackend final : public IViewBackend {
public:
    [[nodiscard]] std::unique_ptr<TextWidget> createText(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeText>(_store, "Text", parent);
    }
    [[nodiscard]] std::unique_ptr<ButtonWidget> createButton(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeButton>(_store, "Button", parent);
    }
    [[nodiscard]] std::unique_ptr<TextInputWidget> createTextInput(ContainerWidget* parent,
                                                                   TextInputMode mode) override {
        return std::make_unique<detail::FakeTextInput>(_store, "TextInput", parent,
                                                       detail::PropList{{"mode", detail::enumName(mode)}});
    }
    [[nodiscard]] std::unique_ptr<CheckboxWidget> createCheckbox(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeCheckbox>(_store, "Checkbox", parent);
    }
    [[nodiscard]] std::unique_ptr<SelectWidget> createSelect(ContainerWidget* parent, SelectStyle style) override {
        return std::make_unique<detail::FakeSelect>(_store, "Select", parent,
                                                    detail::PropList{{"style", detail::enumName(style)}});
    }
    [[nodiscard]] std::unique_ptr<MenuWidget> createMenu(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeMenu>(_store, "Menu", parent);
    }
    [[nodiscard]] std::unique_ptr<StackWidget> createStack(ContainerWidget* parent, Axis axis) override {
        return std::make_unique<detail::FakeStack>(_store, axis == Axis::Vertical ? "Column" : "Row", parent);
    }
    [[nodiscard]] std::unique_ptr<GridWidget> createGrid(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeGrid>(_store, "Grid", parent);
    }
    [[nodiscard]] std::unique_ptr<SpacerWidget> createSpacer(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeLeaf<SpacerWidget>>(_store, "Spacer", parent);
    }
    [[nodiscard]] std::unique_ptr<PanelWidget> createPanel(ContainerWidget* parent) override {
        return std::make_unique<detail::FakePanel>(_store, "Panel", parent);
    }
    [[nodiscard]] std::unique_ptr<ScrollWidget> createScroll(ContainerWidget* parent, Axis axis) override {
        return std::make_unique<detail::FakeContainer<ScrollWidget>>(
            _store, "Scroll", parent, detail::PropList{{"axis", detail::enumName(axis)}});
    }
    [[nodiscard]] std::unique_ptr<SlotWidget> createSlot(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeContainer<SlotWidget>>(_store, "Slot", parent);
    }
    [[nodiscard]] std::unique_ptr<TabsWidget> createTabs(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeTabs>(_store, "Tabs", parent);
    }
    [[nodiscard]] std::unique_ptr<DialogWidget> createDialog(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeDialog>(_store, "Dialog", parent);
    }
    [[nodiscard]] std::unique_ptr<BusyWidget> createBusy(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeBusy>(_store, "Busy", parent);
    }
    [[nodiscard]] std::unique_ptr<TableWidget> createTable(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeTable>(_store, "Table", parent);
    }
    [[nodiscard]] std::unique_ptr<DateTimeInputWidget> createDateTimeInput(ContainerWidget* parent, DateMode mode,
                                                                           int offsetMinutes) override {
        return std::make_unique<detail::FakeDateTimeInput>(
            _store, "DateTimeInput", parent,
            detail::PropList{{"mode", detail::enumName(mode)}, {"offset", std::to_string(offsetMinutes)}});
    }
    [[nodiscard]] std::unique_ptr<SliderWidget> createSlider(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeSlider>(_store, "Slider", parent);
    }
    [[nodiscard]] std::unique_ptr<FilePickerWidget> createFilePicker(ContainerWidget* parent,
                                                                     FilePickerMode mode) override {
        return std::make_unique<detail::FakeFilePicker>(_store, "FilePicker", parent,
                                                        detail::PropList{{"mode", detail::enumName(mode)}});
    }

    /// @brief The golden tree of the live widgets.
    /// @return One line per widget, `Kind#id name=value …` in property-name order, children indented two spaces
    ///         per depth, each line ending in a newline.
    [[nodiscard]] std::string dump() const { return _store->dump(); }

    /// @brief The operation log since construction or the last `clearLog()`.
    /// @return `create Kind#id in Parent#id` (`in root` for a root), `set Kind#id name=value`,
    ///         `move Kind#id to n`, `destroy Kind#id`, oldest first. Interaction helpers add nothing.
    [[nodiscard]] std::vector<std::string> const& log() const { return _store->log(); }

    /// @brief Empties the operation log.
    void clearLog() { _store->clearLog(); }

    /// @brief The first live widget, by id, of a kind whose property has a value.
    /// @param kind The kind, as the dump shows it.
    /// @param name The property.
    /// @param value Its formatted value.
    /// @return The widget's id, or `nullopt`.
    // The contract's lookup: kind, property name and value, all text.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string_view name, std::string_view value) const {
        for (auto const& [widgetId, record] : _store->records()) {
            if (record.kind != kind) {
                continue;
            }
            if (auto const found = record.props.find(name); found != record.props.end() && found->second == value) {
                return widgetId;
            }
        }
        return std::nullopt;
    }

    /// @brief Every live widget of a kind.
    /// @param kind The kind, as the dump shows it.
    /// @return Their ids, in creation order.
    [[nodiscard]] std::vector<int> all(std::string_view kind) const {
        std::vector<int> ids;
        for (auto const& [widgetId, record] : _store->records()) {
            if (record.kind == kind) {
                ids.push_back(widgetId);
            }
        }
        return ids;
    }

    /// @brief A property's formatted value, escaped as the dump shows it.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @return The value, or an empty string when it was never set; `hasProp` tells the two apart.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] std::string prop(int widgetId, std::string_view name) const {
        auto const& props = _store->at(widgetId).props;
        auto const found = props.find(name);
        return found == props.end() ? std::string{} : found->second;
    }

    /// @brief Whether a property was ever set, by a setter, at creation or by an interaction helper.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @return True once it was set, even to an empty value.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] bool hasProp(int widgetId, std::string_view name) const {
        return _store->at(widgetId).props.contains(name);
    }

    /// @brief Whether a widget is alive.
    /// @param widgetId The widget.
    /// @return False once its destructor ran.
    [[nodiscard]] bool exists(int widgetId) const { return _store->records().contains(widgetId); }

    /// @brief The id of one of this backend's live widgets.
    /// @param widget The widget.
    /// @return Its id.
    /// @throws std::out_of_range when @p widget is not one.
    [[nodiscard]] int idOf(Widget const& widget) const { return _store->idOf(widget); }

    /// @brief A live widget's kind.
    /// @param widgetId The widget.
    /// @return The kind, as the dump shows it.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] std::string kindOf(int widgetId) const { return _store->at(widgetId).kind; }

    /// @brief A live widget's children.
    /// @param widgetId The widget.
    /// @return Their ids, in order.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] std::vector<int> children(int widgetId) const { return _store->at(widgetId).children; }

    /// @brief A live widget object.
    /// @param widgetId The widget.
    /// @return The widget.
    /// @throws std::out_of_range for an id that names no live widget.
    [[nodiscard]] Widget* widget(int widgetId) const { return _store->at(widgetId).widget; }

    /// @brief The widget that last took keyboard focus.
    /// @return Its id, or 0 when no widget has, or it has since been destroyed.
    [[nodiscard]] int focused() const noexcept { return _store->focused(); }

    /// @brief Presses a keyboard chord with focus on a widget.
    ///
    /// The chord goes to the innermost widget, from @p widgetId outwards, that declares it through `setKeys`. Like
    /// every helper it acts only when a user could reach @p widgetId.
    /// @param widgetId The widget focus is on.
    /// @param chord The chord, as declared, such as `"Ctrl+S"`.
    /// @return True when a widget took the chord.
    bool press(int widgetId, std::string const& chord) {
        if (!reachable(_store->at(widgetId))) {
            return false;
        }
        for (int candidate = widgetId; candidate != 0;) {
            detail::Record const& record = _store->at(candidate);
            if (std::ranges::find(record.chords, chord) != record.chords.end()) {
                invoke(record.callbacks.chord, chord);
                return true;
            }
            candidate = record.parent;
        }
        return false;
    }

    /// @brief Activates a button.
    /// @param widgetId The button.
    /// @throws std::logic_error when the widget is not a button.
    void click(int widgetId) {
        if (detail::Record const* const record = actionable(widgetId, "click", {"Button"}); record != nullptr) {
            invoke(record->callbacks.click);
        }
    }

    /// @brief Types into a text field: the field shows @p text with the cursor at its end, then `onChange` gets it.
    /// @param widgetId The field.
    /// @param text The whole new text.
    /// @throws std::logic_error when the widget is not a text field.
    void edit(int widgetId, std::string text) {
        if (detail::Record const* const record = editable(widgetId, "edit", {"TextInput"}); record != nullptr) {
            textInput(*record).typedByUser(text, text.size());
            invoke(record->callbacks.change, std::move(text));
        }
    }

    /// @brief Submits a text field with @p text, as Enter does: the field shows it with the cursor at its end,
    ///        `onCommit` gets it, then `onSubmit`.
    /// @param widgetId The field.
    /// @param text The text submitted.
    /// @throws std::logic_error when the widget is not a text field.
    void submit(int widgetId, std::string text) {
        if (detail::Record const* const record = editable(widgetId, "submit", {"TextInput"}); record != nullptr) {
            textInput(*record).typedByUser(text, text.size());
            invoke(record->callbacks.commit, text);
            invoke(record->callbacks.submit, std::move(text));
        }
    }

    /// @brief Moves a text field's cursor, as the arrow keys or a click would; no handler runs. A read-only field
    ///        takes it too, since moving the cursor changes nothing.
    /// @param widgetId The field.
    /// @param position A byte offset into the text it shows; clamped to the text's size.
    /// @throws std::logic_error when the widget is not a text field.
    // An id and a position: an `int` and a `std::size_t`, which convert into each other.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void moveCursor(int widgetId, std::size_t position) {
        if (detail::Record const* const record = actionable(widgetId, "moveCursor", {"TextInput"});
            record != nullptr) {
            textInput(*record).moveCursorByUser(position);
        }
    }

    /// @brief Where a text field's cursor is.
    /// @param widgetId The field.
    /// @return A byte offset into the text it shows.
    /// @throws std::logic_error when the widget is not a text field.
    [[nodiscard]] std::size_t cursor(int widgetId) {
        return textInput(expectKind(widgetId, "cursor", {"TextInput"})).cursor();
    }

    /// @brief Commits a text field as focus leaving it after an edit does: `onCommit` gets the text it shows.
    /// @param widgetId The field.
    /// @throws std::logic_error when the widget is not a text field.
    void commit(int widgetId) {
        if (detail::Record const* const record = editable(widgetId, "commit", {"TextInput"}); record != nullptr) {
            invoke(record->callbacks.commit, textInput(*record).text());
        }
    }

    /// @brief Toggles a check box: it flips its own `checked`, then `onToggle` gets the new state.
    /// @param widgetId The check box.
    /// @throws std::logic_error when the widget is not a check box.
    void toggle(int widgetId) {
        if (detail::Record const* const record = editable(widgetId, "toggle", {"Checkbox"}); record != nullptr) {
            bool const checked = prop(widgetId, "checked") != "true";
            _store->setByUser(widgetId, "checked", detail::formatBool(checked));
            invoke(record->callbacks.toggle, checked);
        }
    }

    /// @brief Chooses an option of a select: it marks the option, then `onSelect` gets the key.
    /// @param widgetId The select.
    /// @param key The key of one of its current options.
    /// @throws std::logic_error when the widget is not a select, or none of its options has @p key: a user can only
    ///         choose what is offered.
    void choose(int widgetId, Key key) {
        detail::Record const& record = expectKind(widgetId, "choose", {"Select"});
        auto* const select = dynamic_cast<detail::FakeSelect*>(record.widget);
        if (select == nullptr || !select->offers(key)) {
            throw std::logic_error{"RecordingBackend: choose of " + detail::formatKey(key) + ", which Select#" +
                                   std::to_string(widgetId) + " does not offer"};
        }
        if (reachable(record) && !flagIs(record, "readonly", true)) {
            select->chooseByUser(key);
            invoke(record.callbacks.select, std::move(key));
        }
    }

    /// @brief Chooses an entry of a menu by its path, as a user opening each submenu on the way would.
    ///
    /// Nothing happens for a path that names no entry, an entry with a submenu (it opens rather than runs), or an
    /// entry that is disabled or inside a disabled entry.
    /// @param widgetId The menu.
    /// @param path The entry's index at each level, from the top.
    /// @throws std::logic_error when the widget is not a menu.
    void chooseMenuEntry(int widgetId, std::vector<std::size_t> const& path) {
        detail::Record const* const record = actionable(widgetId, "chooseMenuEntry", {"Menu"});
        if (record == nullptr) {
            return;
        }
        std::vector<MenuEntry> const* level = &record->menu;
        MenuEntry const* entry = nullptr;
        for (std::size_t const index : path) {
            if (level == nullptr || index >= level->size() || !level->at(index).enabled) {
                return;
            }
            entry = &level->at(index);
            level = &entry->items;
        }
        if (entry != nullptr && entry->items.empty()) {
            invoke(record->callbacks.path, path);
        }
    }

    /// @brief Chooses an entry of a menu's top level, or a tab of a tab bar (which marks it selected).
    /// @param widgetId The menu or tab bar.
    /// @param index The entry or tab.
    /// @throws std::logic_error when the widget is neither a menu nor a tab bar.
    void chooseIndex(int widgetId, std::size_t index) {
        if (detail::Record const* const record = actionable(widgetId, "chooseIndex", {"Menu", "Tabs"});
            record != nullptr) {
            if (record->kind == "Tabs") {
                _store->setByUser(widgetId, "selected", std::to_string(index));
                invoke(record->callbacks.index, index);
            } else {
                chooseMenuEntry(widgetId, {index});
            }
        }
    }

    /// @brief Collapses or expands a collapsible panel with its own collapse control, which works while the panel is
    ///        collapsed.
    /// @param widgetId The panel.
    /// @param collapsed The requested state.
    /// @throws std::logic_error when the widget is not a panel, or the panel is not collapsible and so has no
    ///         collapse control.
    void collapse(int widgetId, bool collapsed) {
        detail::Record const& record = expectKind(widgetId, "collapse", {"Panel"});
        if (prop(widgetId, "collapsible") != "true") {
            throw std::logic_error{"RecordingBackend: collapse of Panel#" + std::to_string(widgetId) +
                                   ", which is not collapsible"};
        }
        if (reachable(record)) {
            _store->setByUser(widgetId, "collapsed", detail::formatBool(collapsed));
            invoke(record.callbacks.toggle, collapsed);
        }
    }

    /// @brief Dismisses an open dialog, as Esc does on the TUI: the dialog closes, then `onDismiss` runs.
    /// @param widgetId The dialog.
    /// @throws std::logic_error when the widget is not a dialog.
    void dismiss(int widgetId) {
        if (detail::Record const* const record = actionable(widgetId, "dismiss", {"Dialog"}); record != nullptr) {
            _store->setByUser(widgetId, "open", detail::formatBool(false));
            invoke(record->callbacks.dismiss);
        }
    }

    /// @brief Selects rows of a table by key: the table marks them, then `onSelectionChange` gets the keys.
    /// @param widgetId The table.
    /// @param keys The keys of every row the user now selects, in the order selected.
    /// @throws std::logic_error when the widget is not a table, or the selection is not one a user could make: a key
    ///         that names no current row, a row twice, any row in a `None` table, or more than one in a `Single` one.
    void selectRows(int widgetId, std::vector<Key> keys) {
        detail::Record const& record = expectKind(widgetId, "selectRows", {"Table"});
        auto* const table = dynamic_cast<detail::FakeTable*>(record.widget);
        std::string const where = " of Table#" + std::to_string(widgetId);
        if (table == nullptr || table->mode() == SelectionMode::None) {
            throw std::logic_error{"RecordingBackend: selectRows" + where + ", which selects nothing"};
        }
        if (table->mode() == SelectionMode::Single && keys.size() > 1) {
            throw std::logic_error{"RecordingBackend: selectRows of several rows" + where + ", which selects one"};
        }
        std::unordered_set<Key> seen;
        for (Key const& key : keys) {
            if (!table->rowOf(key).has_value() || !seen.insert(key).second) {
                throw std::logic_error{"RecordingBackend: selectRows of " + detail::formatKey(key) + where +
                                       ", which has no such row or selects it twice"};
            }
        }
        if (reachable(record)) {
            table->selectByUser(keys);
            invoke(record.callbacks.selection, std::move(keys));
        }
    }

    /// @brief Activates a row of a table (Enter or a double click).
    /// @param widgetId The table.
    /// @param key The row's key.
    /// @throws std::logic_error when the widget is not a table, or none of its current rows has @p key.
    void activateRow(int widgetId, Key key) {
        detail::Record const& record = expectKind(widgetId, "activateRow", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        std::optional<int> const rowId = table == nullptr ? std::nullopt : table->rowOf(key);
        if (!rowId.has_value()) {
            throw std::logic_error{"RecordingBackend: activateRow of " + detail::formatKey(key) + ", which Table#" +
                                   std::to_string(widgetId) + " has no row for"};
        }
        if (reachable(_store->at(*rowId))) {
            invoke(record.callbacks.activate, std::move(key));
        }
    }

    /// @brief Enters a date-time, or clears the field with `nullopt`.
    /// @param widgetId The field.
    /// @param value The instant entered.
    /// @throws std::logic_error when the widget is not a date-time field.
    void setDateTime(int widgetId, std::optional<morph::time::Timestamp> value) {
        if (detail::Record const* const record = editable(widgetId, "setDateTime", {"DateTimeInput"});
            record != nullptr) {
            _store->setByUser(widgetId, "value", detail::formatTimestamp(value));
            invoke(record->callbacks.dateTime, value);
        }
    }

    /// @brief Moves a slider.
    /// @param widgetId The slider.
    /// @param value The value moved to.
    /// @throws std::logic_error when the widget is not a slider.
    void slide(int widgetId, std::int64_t value) {
        if (detail::Record const* const record = editable(widgetId, "slide", {"Slider"}); record != nullptr) {
            _store->setByUser(widgetId, "value", std::to_string(value));
            invoke(record->callbacks.slide, value);
        }
    }

    /// @brief Picks a path in a file picker.
    /// @param widgetId The picker.
    /// @param path The path picked.
    /// @throws std::logic_error when the widget is not a file picker.
    void pick(int widgetId, std::string path) {
        if (detail::Record const* const record = editable(widgetId, "pick", {"FilePicker"}); record != nullptr) {
            _store->setByUser(widgetId, "path", detail::formatText(path));
            invoke(record->callbacks.picked, std::move(path));
        }
    }

    /// @brief Drags one widget onto another. Any kind of widget can be a source or a target.
    ///
    /// An empty `accepts` refuses every key: the mount always passes a predicate, so a drop that lands without
    /// one shows the mount's accept-everything default at work rather than this backend's leniency.
    /// @param sourceId The dragged widget; it must have a drag key.
    /// @param targetId The widget dropped on; it must be a drop target whose `accepts` holds for the key.
    /// @return True when the drop was accepted and `onDrop` ran.
    /// @throws std::logic_error when @p sourceId and @p targetId are the same widget.
    bool drag(int sourceId, int targetId) {
        if (sourceId == targetId) {
            throw std::logic_error{"RecordingBackend: drag of a widget onto itself"};
        }
        detail::Record const& source = _store->at(sourceId);
        detail::Record const& target = _store->at(targetId);
        if (!reachable(source) || !reachable(target)) {
            return false;
        }
        std::optional<Key> const key = source.dragKey;
        auto const accepts = target.callbacks.accepts;
        auto const drop = target.callbacks.drop;
        if (!key.has_value() || !drop || !accepts || !accepts(*key)) {
            return false;
        }
        drop(*key);
        return true;
    }

private:
    // The fake behind a text field's record.
    static detail::FakeTextInput& textInput(detail::Record const& record) {
        return dynamic_cast<detail::FakeTextInput&>(*record.widget);
    }

    // A helper applies to some kinds only; using it on another kind is a mistake in the test, not a user action.
    detail::Record& expectKind(int widgetId, std::string_view helper, std::initializer_list<std::string_view> kinds) {
        detail::Record& record = _store->at(widgetId);
        if (std::ranges::find(kinds, std::string_view{record.kind}) == kinds.end()) {
            throw std::logic_error{"RecordingBackend: " + std::string{helper} + " does not apply to " + record.kind +
                                   "#" + std::to_string(widgetId)};
        }
        return record;
    }

    // The widget's record when a user could reach it now, else null.
    [[nodiscard]] detail::Record* actionable(int widgetId, std::string_view helper,
                                             std::initializer_list<std::string_view> kinds) {
        detail::Record& record = expectKind(widgetId, helper, kinds);
        return reachable(record) ? &record : nullptr;
    }

    // The widget's record when a user could reach it now and change its value, else null.
    [[nodiscard]] detail::Record* editable(int widgetId, std::string_view helper,
                                           std::initializer_list<std::string_view> kinds) {
        detail::Record* const record = actionable(widgetId, helper, kinds);
        return record != nullptr && !flagIs(*record, "readonly", true) ? record : nullptr;
    }

    // A user reaches a widget that is shown and enabled, inside containers that are all shown, enabled and not
    // collapsed. A collapsed panel's own collapse control stays reachable, so only an outer panel's state counts. A
    // dialog shows nothing, itself included, until it is opened.
    [[nodiscard]] bool reachable(detail::Record const& record) const {
        if (flagIs(record, "visible", false) || flagIs(record, "enabled", false) || isClosedDialog(record)) {
            return false;
        }
        for (int outerId = record.parent; outerId != 0;) {
            detail::Record const& outer = _store->at(outerId);
            if (flagIs(outer, "visible", false) || flagIs(outer, "enabled", false) ||
                flagIs(outer, "collapsed", true) || isClosedDialog(outer)) {
                return false;
            }
            outerId = outer.parent;
        }
        return true;
    }

    [[nodiscard]] static bool isClosedDialog(detail::Record const& record) {
        return record.kind == "Dialog" && !flagIs(record, "open", true);
    }

    [[nodiscard]] static bool flagIs(detail::Record const& record, std::string_view name, bool value) {
        auto const found = record.props.find(name);
        return found != record.props.end() && found->second == detail::formatBool(value);
    }

    // The copy keeps the handler alive while it runs, even when it destroys the widget it was stored on.
    template <typename... Args, typename... Values>
    static void invoke(std::function<void(Args...)> const& handler, Values&&... values) {
        auto const copy = handler;
        if (copy) {
            copy(std::forward<Values>(values)...);
        }
    }

    std::shared_ptr<detail::RecordStore> _store = std::make_shared<detail::RecordStore>();
};

}  // namespace morph::ui::testing
