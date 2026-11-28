// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
#include "../text_cursor.hpp"
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
        if (entry.separator) {
            formatted += ";separator";
            continue;
        }
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
        if (!entry.description.empty()) {
            formatted += ";description=" + text(entry.description);
        }
        if (entry.heading) {
            formatted += ";heading";
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
/// @return `content`, `fixed(n)`, `stretch(n)` or `percent(n)`.
[[nodiscard]] inline std::string formatSizing(Sizing sizing) {
    switch (sizing.kind) {
        case Sizing::Kind::Content:
            return "content";
        case Sizing::Kind::Fixed:
            return "fixed(" + std::to_string(sizing.amount) + ")";
        case Sizing::Kind::Stretch:
            return "stretch(" + std::to_string(sizing.amount) + ")";
        case Sizing::Kind::Percent:
            return "percent(" + std::to_string(sizing.amount) + ")";
        default:
            return "unknown";
    }
}

/// @brief Formats an alignment.
/// @param align The alignment.
/// @return `stretch`, `start`, `center` or `end`.
[[nodiscard]] inline std::string formatAlign(Align align) {
    switch (align) {
        case Align::Start:
            return "start";
        case Align::Center:
            return "center";
        case Align::End:
            return "end";
        case Align::Stretch:
        default:
            return "stretch";
    }
}

/// @brief Formats a justification.
/// @param justify The justification.
/// @return `start`, `center`, `end` or `between`.
[[nodiscard]] inline std::string formatJustify(Justify justify) {
    switch (justify) {
        case Justify::Center:
            return "center";
        case Justify::End:
            return "end";
        case Justify::Between:
            return "between";
        case Justify::Start:
        default:
            return "start";
    }
}

/// @brief Formats a size request.
/// @param hints The request.
/// @return `width/height`, each as `formatSizing` gives it and followed by ` min=<n>` and ` max=<n>` for the bounds
///         it has, then ` self=<align>` when the node places itself, such as `stretch(1) max=44/content self=center`.
[[nodiscard]] inline std::string formatLayout(LayoutHints const& hints) {
    auto const bounds = [](std::optional<int> minimum, std::optional<int> maximum) {
        std::string text;
        if (minimum) {
            text += " min=" + std::to_string(*minimum);
        }
        if (maximum) {
            text += " max=" + std::to_string(*maximum);
        }
        return text;
    };
    return formatSizing(hints.width) + bounds(hints.minWidth, hints.maxWidth) + "/" + formatSizing(hints.height) +
           bounds(hints.minHeight, hints.maxHeight) +
           (hints.alignSelf ? " self=" + formatAlign(*hints.alignSelf) : std::string{}) +
           (hints.bleed ? " bleed" : std::string{});
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

/// @brief A button emphasis's name.
/// @param emphasis The emphasis.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(ButtonEmphasis emphasis) {
    switch (emphasis) {
        case ButtonEmphasis::Primary:
            return "Primary";
        case ButtonEmphasis::Danger:
            return "Danger";
        case ButtonEmphasis::Ghost:
            return "Ghost";
        case ButtonEmphasis::DangerOutline:
            return "DangerOutline";
        case ButtonEmphasis::Normal:
        default:
            return "Normal";
    }
}

/// @brief A button style's name.
/// @param style The style.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(ButtonStyle style) { return style == ButtonStyle::Item ? "Item" : "Push"; }

/// @brief A menu trigger's marker name.
/// @param caret The marker.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(MenuCaret caret) {
    return caret == MenuCaret::Down ? "Down" : caret == MenuCaret::UpDown ? "UpDown" : "None";
}

/// @brief A table's sort chain as the dump shows it: each key's column index and direction, most significant first,
///        or `none`.
/// @param chain The keys.
/// @return Such as `0:Ascending,2:Descending`, or `none` for an empty chain.
[[nodiscard]] inline std::string formatSort(std::vector<TableSort> const& chain) {
    if (chain.empty()) {
        return "none";
    }
    std::string text;
    for (TableSort const& key : chain) {
        text += (text.empty() ? "" : ",") + std::to_string(key.column) +
                (key.direction == SortDirection::Ascending ? ":Ascending" : ":Descending");
    }
    return text;
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
        case TextRole::Section:
            return "Section";
        case TextRole::Mono:
            return "Mono";
        case TextRole::Display:
            return "Display";
        case TextRole::Small:
            return "Small";
        case TextRole::Overline:
            return "Overline";
        case TextRole::Caption:
            return "Caption";
        case TextRole::Label:
            return "Label";
        case TextRole::Headline:
            return "Headline";
        default:
            return "unknown";
    }
}

/// @brief A text alignment's name.
/// @param align The alignment.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(TextAlign align) {
    switch (align) {
        case TextAlign::Center:
            return "Center";
        case TextAlign::End:
            return "End";
        case TextAlign::Start:
        default:
            return "Start";
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
        case SelectStyle::Segmented:
            return "Segmented";
        default:
            return "unknown";
    }
}

/// @brief A tone's name.
/// @param tone The tone.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(Tone tone) {
    switch (tone) {
        case Tone::Neutral:
            return "Neutral";
        case Tone::Info:
            return "Info";
        case Tone::Ok:
            return "Ok";
        case Tone::Warn:
            return "Warn";
        case Tone::Err:
            return "Err";
        default:
            return "unknown";
    }
}

/// @brief Formats runs of text: `[Lot ,L-26-0381;mono,7;tone=Err]`, each run its text followed by `;strong`, `;mono`
///        and `;tone=<tone>` when set.
/// @param spans The runs.
/// @return Their text; `=`, `,`, `[`, `]` and `;` in a run are escaped, so a run cannot forge a flag.
[[nodiscard]] inline std::string formatSpans(std::vector<TextSpan> const& spans) {
    std::string formatted = "[";
    for (TextSpan const& span : spans) {
        formatted += formatted.size() == 1 ? "" : ",";
        formatted += escape(span.text, [](char character) { return std::string_view{"=,[];"}.contains(character); });
        formatted += span.strong ? ";strong" : "";
        formatted += span.mono ? ";mono" : "";
        formatted += span.action ? ";action" : "";
        if (span.tone != Tone::Neutral) {
            formatted += ";tone=";
            formatted += enumName(span.tone);
        }
    }
    return formatted + "]";
}

/// @brief The characters of runs as `formatSpans` records them: each run's text up to its first unescaped `;`, joined.
/// @param recorded The recorded runs, `[a,b;strong]`; anything shorter than the brackets reads as no characters.
/// @return Their text, unescaped.
[[nodiscard]] inline std::string spansCharacters(std::string_view recorded) {
    std::string joined;
    if (recorded.size() < 2) {
        return joined;
    }
    std::string_view const inner = recorded.substr(1, recorded.size() - 2);
    bool flags = false;
    for (std::size_t at = 0; at < inner.size(); ++at) {
        char const character = inner.at(at);
        if (character == '\\' && at + 1 < inner.size()) {
            ++at;
            if (!flags) {
                joined += inner.at(at) == 'n' ? '\n' : inner.at(at);
            }
        } else if (character == ',') {
            flags = false;
        } else if (character == ';') {
            flags = true;
        } else if (!flags) {
            joined += character;
        }
    }
    return joined;
}

/// @brief A side's name.
/// @param side The side.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(Side side) {
    switch (side) {
        case Side::Start:
            return "Start";
        case Side::End:
            return "End";
        default:
            return "unknown";
    }
}

/// @brief A step state's name.
/// @param state The state.
/// @return The enumerator's name.
[[nodiscard]] inline std::string enumName(StepState state) {
    switch (state) {
        case StepState::Pending:
            return "Pending";
        case StepState::Active:
            return "Active";
        case StepState::Done:
            return "Done";
        case StepState::Failed:
            return "Failed";
        default:
            return "unknown";
    }
}

/// @brief A fraction as its shortest decimal text, or `none`.
/// @param value The fraction.
/// @return The text, such as `0.25`.
[[nodiscard]] inline std::string formatFraction(std::optional<double> value) {
    if (!value.has_value()) {
        return "none";
    }
    return std::format("{}", *value);
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
    /// @brief From `CheckboxWidget::setOnToggle`, `PanelWidget::setOnToggle` or `CollapsibleWidget::setOnToggle`.
    std::function<void(bool)> toggle;
    /// @brief From `SelectWidget::setOnSelect`.
    std::function<void(Key)> select;
    /// @brief From `SelectWidget::setOnOpen`.
    Action open;
    /// @brief From `TabsWidget::setOnSelect`.
    std::function<void(std::size_t)> index;
    /// @brief From `DateTimeInputWidget::setOnChange`.
    std::function<void(std::optional<morph::time::Timestamp>)> dateTime;
    /// @brief From `SliderWidget::setOnChange`.
    std::function<void(std::int64_t)> slide;
    /// @brief From `SliderWidget::setOnCommit`.
    std::function<void(std::int64_t)> slideCommit;
    /// @brief From `FilePickerWidget::setOnPicked`.
    std::function<void(std::string)> picked;
    /// @brief From `DialogWidget::setOnDismiss`, `DrawerWidget::setOnDismiss` or `BannerWidget::setOnDismiss`.
    Action dismiss;
    /// @brief From `TableWidget::setOnSelectionChange`.
    std::function<void(std::vector<Key>, SelectionChange)> selection;
    /// @brief From `TableWidget::setOnActivate`.
    std::function<void(Key)> activate;
    /// @brief From `TableWidget::setOnSort`.
    std::function<void(std::size_t, HeaderClick)> sort;
    /// @brief From `TableWidget::setOnRetry`.
    Action retry;
    /// @brief From `TableWidget::setOnFocusOut`: called when focus moves from inside the table to outside it.
    Action focusOut;
    /// @brief The drop handler's predicate, from `Widget::setDropHandler`.
    std::function<bool(Key const&)> accepts;
    /// @brief The drop handler, from `Widget::setDropHandler`.
    std::function<void(Key)> drop;
    /// @brief From `CustomWidget::setOnEvent`: the event's name and its payload.
    std::function<void(std::string, std::string)> component;
    /// @brief The chord handler, from `Widget::setKeys`.
    std::function<void(std::string)> chord;
    /// @brief From `Widget::setOnHover`.
    std::function<void()> hover;
    /// @brief A menu's activation handler, from `MenuWidget::setOnActivate`.
    std::function<void(std::vector<std::size_t>)> path;
    /// @brief An action button's handler, from `BannerWidget::setOnAction` or `EmptyStateWidget::setOnAction`.
    Action action;
    /// @brief From `SplitterWidget::setOnResize`.
    std::function<void(std::vector<int>)> resize;
    /// @brief From `DropZoneWidget::setOnDrop`.
    std::function<void(std::vector<std::string>)> files;
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
    int create(std::string kind, Widget const* parent, Widget& widget, PropList initial) {
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
        tellListeners();
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
        if (movesTargets(name)) {
            tellListeners();
        }
    }

    /// @brief Sets a property the user changed through the widget itself; no setter ran, so nothing is logged.
    /// @param widgetId The widget.
    /// @param name The property.
    /// @param value Its formatted value.
    void setByUser(int widgetId, std::string_view name, std::string value) {
        at(widgetId).props.insert_or_assign(std::string{name}, std::move(value));
        if (movesTargets(name)) {
            tellListeners();
        }
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
        tellListeners();
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
        tellListeners();
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

    /// @brief Registers a listener told after each change that can move a spotlight's target: a widget made, moved or
    ///        destroyed, and a `testId`, `visible`, `open` or `collapsed` property set by a setter or the user.
    /// @param listener The listener; it may change properties, and a change it makes is told after it returns.
    /// @return Its key, for `unlisten`.
    int listen(std::function<void()> listener) {
        int const key = ++_nextListener;
        _listeners.insert_or_assign(key, std::move(listener));
        return key;
    }

    /// @brief Stops telling a listener.
    /// @param key What `listen` returned.
    void unlisten(int key) { _listeners.erase(key); }

    /// @brief Records that keyboard focus moved to a widget, and logs `focus`.
    /// @param widgetId The widget that took focus.
    /// @throws std::out_of_range for an id that names no live widget.
    void focus(int widgetId) {
        _log.push_back("focus " + label(at(widgetId)));
        int const previous = std::exchange(_focused, widgetId);
        if (previous == 0 || previous == widgetId) {
            return;
        }
        // Focus that leaves a table and everything inside it tells the table, innermost first.
        std::vector<std::function<void()>> leaving;
        for (auto found = _records.find(previous); found != _records.end();
             found = _records.find(found->second.parent)) {
            if (found->second.callbacks.focusOut && !within(widgetId, found->first)) {
                leaving.push_back(found->second.callbacks.focusOut);
            }
        }
        for (auto const& leave : leaving) {
            leave();
        }
    }

    /// @brief Whether a widget is a container or lies inside it.
    /// @param widgetId  The widget.
    /// @param container The container.
    /// @return `true` when @p container is @p widgetId or one of its ancestors.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`widgetId`, `container`): the widget then its candidate ancestor, as the name reads at each call
    [[nodiscard]] bool within(int widgetId, int container) const {
        for (auto found = _records.find(widgetId); found != _records.end();
             found = _records.find(found->second.parent)) {
            if (found->first == container) {
                return true;
            }
        }
        return false;
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
    [[nodiscard]] static bool movesTargets(std::string_view name) noexcept {
        return name == "testId" || name == "visible" || name == "open" || name == "collapsed";
    }

    // Tells every listener. A change a listener makes while it is told is told once more after the round, not inside
    // it, so a listener never runs inside itself.
    void tellListeners() {
        if (_listeners.empty()) {
            return;
        }
        if (_notifying) {
            _again = true;
            return;
        }
        // A listener that throws must not leave the store deaf, so the flag is cleared on every way out of the round.
        struct Round {
            explicit Round(bool& flag) : notifying{&flag} { *notifying = true; }
            Round(Round const&) = delete;
            Round& operator=(Round const&) = delete;
            Round(Round&&) = delete;
            Round& operator=(Round&&) = delete;
            ~Round() { *notifying = false; }
            bool* notifying;
        };
        Round const round{_notifying};
        _again = true;
        while (_again) {
            _again = false;
            std::vector<int> keys;
            keys.reserve(_listeners.size());
            for (auto const& [key, listener] : _listeners) {
                static_cast<void>(listener);
                keys.push_back(key);
            }
            for (int const key : keys) {
                if (auto const found = _listeners.find(key); found != _listeners.end()) {
                    auto const listener = found->second;
                    listener();
                }
            }
        }
    }

    std::map<int, std::function<void()>> _listeners;
    int _nextListener = 0;
    bool _notifying = false;
    bool _again = false;

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
    /// @param parent The container it is appended to (or the button whose content it is), or null for a root.
    /// @param initial Properties fixed at creation.
    FakeLeaf(std::shared_ptr<RecordStore> store, std::string kind, Widget const* parent, PropList initial = {})
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

    /// @brief Keeps the handler for `RecordingBackend::hover`.
    /// @param onHover The handler.
    void setOnHover(std::function<void()> onHover) override { _store->at(_id).callbacks.hover = std::move(onHover); }

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

    /// @brief The store, shared: what a part this widget makes (an overlay's footer) records into.
    /// @return The store.
    [[nodiscard]] std::shared_ptr<RecordStore> const& sharedStore() const noexcept { return _store; }

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

/// @brief The `FieldWidget` half of every fake field: properties `readonly`, `required`, `errors`, `stale`,
///        `description` and `tone`.
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

    /// @brief Records property `description`.
    /// @param description The line.
    void setDescription(std::string_view description) override { this->set("description", formatText(description)); }

    /// @brief Records property `captionHint`.
    /// @param hint The hint.
    void setCaptionHint(std::string_view hint) override { this->set("captionHint", formatText(hint)); }

    /// @brief Records property `tone`.
    /// @param tone The tone.
    void setTone(Tone tone) override { this->set("tone", enumName(tone)); }
};

/// @brief A fake `TextWidget`: properties `text`, `spans` (as `formatSpans` gives them), `role`, `align`, `tone`.
class FakeText final : public FakeField<TextWidget> {
public:
    using FakeField<TextWidget>::FakeField;
    void setText(std::string_view text) override { set("text", formatText(text)); }
    void setSpans(std::vector<TextSpan> const& spans) override { set("spans", formatSpans(spans)); }
    /// @brief Records property `align`.
    /// @param align The alignment.
    void setAlign(TextAlign align) override { set("align", enumName(align)); }
    /// @brief Refuses a description: a text is not described.
    /// @param description The description, ignored.
    /// @throws std::logic_error always.
    void setDescription(std::string_view description) override {
        static_cast<void>(description);
        throw std::logic_error{"RecordingBackend: a text is not described"};
    }
    /// @brief Refuses a caption hint: a text has no caption.
    /// @param hint The hint, ignored.
    /// @throws std::logic_error always.
    void setCaptionHint(std::string_view hint) override {
        static_cast<void>(hint);
        throw std::logic_error{"RecordingBackend: a text has no caption to hint after"};
    }
    void setRole(TextRole role) override { set("role", enumName(role)); }
};

/// @brief A fake `SlotWidget`: property `scrollable`, recorded once set.
class FakeSlot final : public FakeContainer<SlotWidget> {
public:
    using FakeContainer<SlotWidget>::FakeContainer;
    void setScrollable(bool scrollable) override { set("scrollable", scrollable ? "true" : "false"); }
};

/// @brief A fake `ButtonWidget`: properties `label`, `emphasis`, `icon`, `style`, `selected`, `shape`, `size` and
/// `mono`; a `Column` child
///        once its content is asked for.
class FakeButton final : public FakeLeaf<ButtonWidget> {
public:
    using FakeLeaf<ButtonWidget>::FakeLeaf;
    /// @brief Destroys the content before the button, which the content names as its parent.
    ~FakeButton() override { _content.reset(); }
    FakeButton(FakeButton const&) = delete;
    FakeButton& operator=(FakeButton const&) = delete;
    FakeButton(FakeButton&&) = delete;
    FakeButton& operator=(FakeButton&&) = delete;
    /// @brief The entry's content, a `Column` child of the button.
    /// @return It.
    ContainerWidget& content() override {
        if (_content == nullptr) {
            _content = std::make_unique<FakeSlot>(sharedStore(), "Column", this);
        }
        return *_content;
    }
    void setLabel(std::string_view label) override { set("label", formatText(label)); }
    void setOnClick(Action onClick) override { callbacks().click = std::move(onClick); }
    void setEmphasis(ButtonEmphasis emphasis) override { set("emphasis", enumName(emphasis)); }
    void setIcon(std::string_view icon) override { set("icon", formatText(icon)); }
    void setStyle(ButtonStyle style) override { set("style", enumName(style)); }
    void setSelected(bool selected) override { set("selected", selected ? "true" : "false"); }
    void setShape(ControlShape shape) override { set("shape", std::string{shapeName(shape)}); }
    void setSize(ButtonSize size) override { set("size", std::string{sizeName(size)}); }
    void setMono(bool mono) override { set("mono", formatBool(mono)); }
    void setWeight(int weight) override { set("weight", std::to_string(weight)); }

private:
    std::unique_ptr<FakeSlot> _content;
};

/// @brief A fake `TextInputWidget`: properties `mode`, `text`, `placeholder`, `unit`, `prefix`, `decimals`, `icon`,
///        `emphasis`, `mono`, `align`,
///        `caption`, `captionShown`, the field state, and a text cursor.
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

    /// @brief Moves the cursor to @p offset, clamped to the text's size and moved back to the start of a code point;
    ///        nothing is logged, as for a user's move.
    /// @param offset A byte offset into the text shown.
    void setCursor(std::size_t offset) override { _cursor = snapToCodePointStart(_shown, offset); }

    /// @brief Where the cursor is.
    /// @return A byte offset into the text shown.
    [[nodiscard]] std::size_t cursor() const noexcept { return _cursor; }

    /// @brief The text the field shows.
    /// @return The text, unformatted.
    [[nodiscard]] std::string const& text() const noexcept { return _shown; }

    void setPlaceholder(std::string_view placeholder) override { set("placeholder", formatText(placeholder)); }
    void setUnit(std::string_view unit) override { set("unit", formatText(unit)); }
    void setPrefix(std::string_view prefix) override { set("prefix", formatText(prefix)); }
    void setMono(bool mono) override { set("mono", formatBool(mono)); }
    void setWrap(bool wrap) override { set("wrap", formatBool(wrap)); }
    void setAlign(Align align) override {
        if (align == Align::End) {
            set("align", "end");
        } else {
            set("align", align == Align::Center ? "center" : "start");
        }
    }
    void setDecimals(int decimals) override { set("decimals", std::to_string(decimals)); }
    void setIcon(std::string_view icon) override { set("icon", formatText(icon)); }
    void setEmphasis(InputEmphasis emphasis) override {
        set("emphasis", emphasis == InputEmphasis::Prominent ? "Prominent" : "Normal");
    }
    void setCaption(std::string_view label, bool shown) override {
        set("caption", formatText(label));
        set("captionShown", formatBool(shown));
    }
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

/// @brief A fake `SelectWidget`: properties `style`, `options`, `selected`, `caption`, `captionShown`.
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

    /// @brief Stores the handler `RecordingBackend::open` calls.
    /// @param onOpen The handler.
    void setOnOpen(Action onOpen) override { callbacks().open = std::move(onOpen); }
    void setCaption(std::string_view label, bool shown) override {
        set("caption", formatText(label));
        set("captionShown", formatBool(shown));
    }

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

    /// @brief The key of the option @p step places from the marked one, as Left and Right on a segmented select move.
    /// @param step -1 for the previous option, 1 for the next.
    /// @return The neighbour's key; `nullopt` when none is marked or the marked one is at that end.
    [[nodiscard]] std::optional<Key> neighbour(int step) const {
        if (!_requested.has_value() || !_requested->has_value()) {
            return std::nullopt;
        }
        auto const found = std::ranges::find(_options, **_requested, &SelectOption::key);
        if (found == _options.end()) {
            return std::nullopt;
        }
        auto const index = std::distance(_options.begin(), found) + step;
        if (index < 0 || index >= std::ssize(_options)) {
            return std::nullopt;
        }
        return _options.at(static_cast<std::size_t>(index)).key;
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

/// @brief A fake `MenuWidget`: property `items`, and `style`, `label`, `icon` and `emphasis` of a button-style menu.
class FakeMenu final : public FakeLeaf<MenuWidget> {
public:
    using FakeLeaf<MenuWidget>::FakeLeaf;
    void setItems(std::vector<MenuEntry> const& entries) override {
        store().at(id()).menu = entries;
        set("items", formatMenu(entries));
    }

    /// @brief Records properties `label`, `icon` (when given), `emphasis` (when not `Normal`), `detail` (when given),
    ///        `avatar` and `caret` of the trigger.
    /// @param trigger The trigger.
    void setTrigger(MenuTrigger const& trigger) override {
        set("label", formatText(trigger.label));
        if (!trigger.icon.empty()) {
            set("icon", formatText(trigger.icon));
        }
        if (trigger.emphasis != ButtonEmphasis::Normal) {
            set("emphasis", enumName(trigger.emphasis));
        }
        if (!trigger.detail.empty()) {
            set("detail", formatText(trigger.detail));
        }
        // Set whatever they are, so a trigger that loses its avatar or marker records that it has.
        set("avatar", formatText(trigger.avatar));
        set("caret", enumName(trigger.caret));
    }
    void setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) override {
        callbacks().path = std::move(onActivate);
    }
};

/// @brief A fake `StackWidget`, of kind `Column` or `Row`: properties `gap`, `align`, `justify`, `wrap`, `divided`,
/// `padding`.
class FakeStack final : public FakeContainer<StackWidget> {
public:
    using FakeContainer<StackWidget>::FakeContainer;
    void setGap(int gap) override { set("gap", std::to_string(gap)); }
    void setAlign(Align align) override { set("align", formatAlign(align)); }
    void setJustify(Justify justify) override { set("justify", formatJustify(justify)); }
    void setWrap(bool wrap) override { set("wrap", wrap ? "true" : "false"); }
    void setDivided(bool divided) override { set("divided", divided ? "true" : "false"); }
    void setPadding(Padding padding) override {
        set("padding", padding.vertical == padding.horizontal
                           ? std::to_string(padding.vertical)
                           : std::to_string(padding.vertical) + "/" + std::to_string(padding.horizontal));
    }
    void setSelected(bool selected) override { set("selected", selected ? "true" : "false"); }
    /// @brief Records property `actionRow` as true.
    void setActionRow() override { set("actionRow", "true"); }
    void setPassesBleed(bool passes) override { set("passesBleed", passes ? "true" : "false"); }
    /// @brief Records property `headBar`.
    /// @param headBar Whether the stack is the expanded head bar.
    void setHeadBar(bool headBar) override { set("headBar", headBar ? "true" : "false"); }
};

/// @brief A fake `GridWidget`: properties `columns`, `tracks`, `gap`, `align`, and `span` on a child.
class FakeGrid final : public FakeContainer<GridWidget> {
public:
    using FakeContainer<GridWidget>::FakeContainer;
    void setColumns(int columns) override { set("columns", std::to_string(columns)); }

    /// @brief Records property `tracks`, each as its width, then `;align=` where the track's alignment is not
    ///        `Start`.
    /// @param tracks The tracks, in order.
    void setTracks(std::vector<GridTrack> const& tracks) override {
        set("tracks", formatList(tracks, [](GridTrack const& track) {
                std::string text = formatSizing(track.width);
                if (track.align != Align::Start) {
                    text += ";align=" + formatAlign(track.align);
                }
                return text;
            }));
    }
    /// @brief Records property `minColumnWidth`.
    /// @param units The least column width in units.
    void setMinColumnWidth(int units) override { set("minColumnWidth", std::to_string(units)); }
    void setGap(int gap) override { set("gap", std::to_string(gap)); }
    void setAlign(Align align) override { set("align", formatAlign(align)); }

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

/// @brief A fake `PanelWidget`: properties `title`, `header` (the child's id), `padding`, `collapsible`, `collapsed`.
class FakePanel final : public FakeContainer<PanelWidget> {
public:
    using FakeContainer<PanelWidget>::FakeContainer;
    void setTitle(std::string_view title) override { set("title", formatText(title)); }
    void setPadding(int padding) override { set("padding", std::to_string(padding)); }
    void setCollapsible(bool collapsible) override { set("collapsible", formatBool(collapsible)); }

    /// @brief Records property `header`, the child's id.
    /// @param header One of this widget's children.
    /// @throws std::logic_error when @p header is not this widget's child.
    void setHeader(Widget& header) override {
        int const headerId = store().idOf(header);
        if (store().at(headerId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setHeader with a widget that is not this panel's child"};
        }
        set("header", std::to_string(headerId));
    }
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

/// @brief A fake `DateTimeInputWidget`: properties `mode`, `offset`, `value`, `pattern`, `caption`, `captionShown`.
class FakeDateTimeInput final : public FakeField<DateTimeInputWidget> {
public:
    using FakeField<DateTimeInputWidget>::FakeField;
    void setValue(std::optional<morph::time::Timestamp> const& value) override {
        set("value", formatTimestamp(value));
    }
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override {
        callbacks().dateTime = std::move(onChange);
    }
    void setPattern(std::string_view pattern) override { set("pattern", formatText(pattern)); }
    void setCaption(std::string_view label, bool shown) override {
        set("caption", formatText(label));
        set("captionShown", formatBool(shown));
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
    void setOnCommit(std::function<void(std::int64_t)> onCommit) override {
        callbacks().slideCommit = std::move(onCommit);
    }
};

/// @brief A fake `FilePickerWidget`: properties `mode`, `path`.
class FakeFilePicker final : public FakeField<FilePickerWidget> {
public:
    using FakeField<FilePickerWidget>::FakeField;
    void setPath(std::string_view path) override { set("path", formatText(path)); }
    void setOnPicked(std::function<void(std::string)> onPicked) override { callbacks().picked = std::move(onPicked); }
};

/// @brief A fake `TabsWidget`: properties `tabs`, `selected`, `barShown`.
class FakeTabs final : public FakeContainer<TabsWidget> {
public:
    using FakeContainer<TabsWidget>::FakeContainer;
    void setTabs(std::vector<std::string> const& labels) override {
        set("tabs", formatList(labels, [](std::string const& label) { return formatItem(label); }));
    }
    void setSelected(std::size_t index) override { set("selected", std::to_string(index)); }
    void setOnSelect(std::function<void(std::size_t)> onSelect) override { callbacks().index = std::move(onSelect); }
    void setBarShown(bool shown) override { set("barShown", shown ? "true" : "false"); }
    void setMoreLabel(std::string_view label) override { set("moreLabel", formatText(label)); }
    void setInset(int inset) override { set("inset", std::to_string(inset)); }
    void setPageInset(int inset) override { set("pageInset", std::to_string(inset)); }
    void setStripSurface(std::string_view surface) override { set("stripSurface", formatText(surface)); }
};

/// @brief A fake overlay's footer: a `Footer` child, made when it is first asked for, so an overlay that never shows
///        actions logs none.
/// @tparam Interface `DialogWidget` or `DrawerWidget`.
template <typename Interface>
class FakeOverlay : public FakeContainer<Interface> {
public:
    using FakeContainer<Interface>::FakeContainer;

    /// @brief The footer, a `Footer` container child of the overlay.
    /// @return It.
    ContainerWidget& footer() override {
        if (_footer == nullptr) {
            _footer = std::make_unique<FakeSlot>(this->sharedStore(), "Footer", this);
        }
        return *_footer;
    }

private:
    std::unique_ptr<FakeSlot> _footer;
};

/// @brief A fake `DialogWidget`: properties `open`, `title`, `description`, `sheet`, `dismissLabel`, `headNote`,
///        `footerNote` and `closeButton` (when set); a `Footer` child once its footer is asked for.
class FakeDialog final : public FakeOverlay<DialogWidget> {
public:
    using FakeOverlay<DialogWidget>::FakeOverlay;
    void setOpen(bool open) override { set("open", formatBool(open)); }
    void setTitle(std::string_view title) override { set("title", formatText(title)); }
    void setDescription(std::string_view description) override { set("description", formatText(description)); }
    void setDescriptionSpans(std::vector<TextSpan> const& spans) override {
        set("descriptionSpans", formatSpans(spans));
    }
    void setOnDismiss(Action onDismiss) override { callbacks().dismiss = std::move(onDismiss); }
    void setSheet(bool sheet) override { set("sheet", formatBool(sheet)); }
    void setDismissLabel(std::string_view label) override { set("dismissLabel", formatText(label)); }
    void setHeadNote(std::string_view note) override { set("headNote", formatText(note)); }
    void setFooterNote(std::string_view note) override { set("footerNote", formatText(note)); }
    void setCloseButton(bool shown) override { set("closeButton", formatBool(shown)); }
};

/// @brief A fake `BannerWidget`: properties `tone`, `text`, `spans` (as `formatSpans` gives them), `actionLabel`,
///        `dismissible`, and, once told, `actionStyle` and `actionStretch`.
class FakeBanner final : public FakeLeaf<BannerWidget> {
public:
    using FakeLeaf<BannerWidget>::FakeLeaf;
    void setTone(Tone tone) override { set("tone", enumName(tone)); }
    void setText(std::string_view text) override { set("text", formatText(text)); }
    void setSpans(std::vector<TextSpan> const& spans) override { set("spans", formatSpans(spans)); }
    void setActionLabel(std::string_view label) override { set("actionLabel", formatText(label)); }
    void setOnAction(Action onAction) override { callbacks().action = std::move(onAction); }
    void setDismissible(bool dismissible) override { set("dismissible", formatBool(dismissible)); }
    void setOnDismiss(Action onDismiss) override { callbacks().dismiss = std::move(onDismiss); }
    void setActionStyle(BannerActionStyle style, bool stretch) override {
        set("actionStyle", std::string{bannerActionStyleName(style)});
        set("actionStretch", formatBool(stretch));
    }
};

/// @brief A fake `ToastWidget`: properties `text`, `tone`, and `shown` (incremented on every `show`, so a test can
///        tell a replace from a no-op).
class FakeToast final : public FakeLeaf<ToastWidget> {
public:
    using FakeLeaf<ToastWidget>::FakeLeaf;
    void show(std::string_view text, Tone tone) override {
        set("text", formatText(text));
        set("tone", enumName(tone));
        set("shown", std::to_string(++_shown));
    }

private:
    int _shown = 0;
};

/// @brief A fake `BadgeWidget`: properties `tone`, `text`, `icon`, `style`, `shape`, `mono`, and `removable` of a
///        removable one.
class FakeBadge final : public FakeLeaf<BadgeWidget> {
public:
    using FakeLeaf<BadgeWidget>::FakeLeaf;
    void setTone(Tone tone) override { set("tone", enumName(tone)); }
    void setText(std::string_view text) override { set("text", formatText(text)); }
    void setIcon(std::string_view icon) override { set("icon", formatText(icon)); }

    void setStyle(BadgeStyle style) override {
        std::string name = "Filled";
        if (style == BadgeStyle::Outline) {
            name = "Outline";
        } else if (style == BadgeStyle::Quiet) {
            name = "Quiet";
        }
        set("style", name);
    }
    void setShape(ControlShape shape) override { set("shape", std::string{shapeName(shape)}); }
    void setMono(bool mono) override { set("mono", formatBool(mono)); }

    /// @brief Records property `removable`, the remove control's name, and keeps @p onRemove for
    ///        `RecordingBackend::dismiss`.
    /// @param onRemove The handler.
    /// @param label    The remove control's accessible name.
    void setOnRemove(Action onRemove, std::string_view label) override {
        set("removable", formatText(label));
        callbacks().dismiss = std::move(onRemove);
    }
};

/// @brief A fake `ProgressWidget`: properties `value` (as `formatFraction` gives it), `label`.
class FakeProgress final : public FakeLeaf<ProgressWidget> {
public:
    using FakeLeaf<ProgressWidget>::FakeLeaf;
    void setValue(std::optional<double> value) override { set("value", formatFraction(value)); }
    void setLabel(std::string_view label) override { set("label", formatText(label)); }
};

/// @brief A fake `StepsWidget`: properties `steps` (`[label:State,…]`, a nonempty detail following as
/// `;detail=<text>`), `current` (a position, or `none`), `labelled`.
class FakeSteps final : public FakeLeaf<StepsWidget> {
public:
    using FakeLeaf<StepsWidget>::FakeLeaf;
    void setSteps(std::vector<Step> const& steps) override {
        // `;` is escaped too, so a label or a detail cannot forge the detail attribute.
        auto const text = [](std::string_view value) {
            return escape(value, [](char character) { return std::string_view{"=,[];"}.contains(character); });
        };
        set("steps", formatList(steps, [&text](Step const& step) {
                std::string const formatted = text(step.label) + ":" + enumName(step.state);
                return step.detail.empty() ? formatted : formatted + ";detail=" + text(step.detail);
            }));
    }
    void setCurrent(std::optional<std::size_t> index) override {
        set("current", index.has_value() ? std::to_string(*index) : std::string{"none"});
    }
    void setLabelled(bool labelled) override { set("labelled", labelled ? "true" : "false"); }
};

/// @brief Formats free text inside a chart property: also escapes `,`, `[`, `]`, `;`, `@` and `|`, the separators
///        of the chart's formats.
/// @param text The text, UTF-8.
/// @return The escaped text.
[[nodiscard]] inline std::string formatChartText(std::string_view text) {
    return escape(text, [](char character) { return std::string_view{"=,[];@|"}.contains(character); });
}

/// @brief A tone's document name, as a chart's formats write it.
/// @param tone The tone.
/// @return `neutral`, `info`, `ok`, `warn` or `err`.
[[nodiscard]] inline std::string chartToneName(Tone tone) {
    static constexpr std::array<std::string_view, 5> kNames = {"neutral", "info", "ok", "warn", "err"};
    auto const index = static_cast<std::size_t>(tone);
    return index < kNames.size() ? std::string{kNames.at(index)} : std::string{"unknown"};
}

/// @brief A reference line's stroke, as a chart's formats write it.
/// @param style The stroke.
/// @return `solid`, `dashed` or `dotted`.
[[nodiscard]] inline std::string chartStyleName(ChartLineStyle style) {
    if (style == ChartLineStyle::Dashed) {
        return "dashed";
    }
    return style == ChartLineStyle::Dotted ? "dotted" : "solid";
}

/// @brief A chart point's value: the number, `gap` for none, and the number followed by `↑` or `↓` for a point
///        clipped at the axis's upper or lower end.
/// @param point The point.
/// @return The text.
[[nodiscard]] inline std::string formatChartValue(ChartPoint const& point) {
    if (!point.y.has_value()) {
        return "gap";
    }
    std::string text = std::format("{}", *point.y);
    if (point.clip == ChartClip::Above) {
        text += "↑";
    } else if (point.clip == ChartClip::Below) {
        text += "↓";
    }
    return text;
}

/// @brief A chart's series: `[entry,…]`, each entry `label;joined;[point|…]`, followed by `;bars` for a `Bars` series,
///        and each point `xLabel@x,y;tone;note`, the tone empty for an unmarked point.
/// @param series The series.
/// @return The text.
[[nodiscard]] inline std::string formatChartSeries(std::vector<ChartSeries> const& series) {
    std::string text = "[";
    for (ChartSeries const& one : series) {
        text += text.size() == 1 ? "" : ",";
        text += formatChartText(one.label) + ";" + formatBool(one.joined) + ";[";
        bool first = true;
        for (ChartPoint const& point : one.points) {
            text += first ? "" : "|";
            first = false;
            text += formatChartText(point.xLabel) + "@" + std::format("{}", point.x) + "," + formatChartValue(point) +
                    ";" + (point.tone.has_value() ? chartToneName(*point.tone) : std::string{}) + ";" +
                    formatChartText(point.note);
        }
        text += std::string{"]"} + (one.style == ChartStyle::Bars ? ";bars" : "");
    }
    return text + "]";
}

/// @brief A fake `ChartWidget`: properties `yRange` and `xRange` (`[lo,hi]`), `yLabel`, `yTicks`
///        (`[text@value,…]`), `barSlot` (a number), `lines` (`[label@value;tone;style,…]`), `series` (as
///        `formatChartSeries` gives it) and `summary`, each set only when it changed. Numbers are written
///        shortest-round-trip, so a log is the same on every platform.
class FakeChart final : public FakeLeaf<ChartWidget> {
public:
    using FakeLeaf<ChartWidget>::FakeLeaf;
    void setData(ChartData const& data) override {
        update("yRange", std::format("[{},{}]", data.yMin, data.yMax));
        update("xRange", std::format("[{},{}]", data.xMin, data.xMax));
        update("barSlot", std::format("{}", data.barSlot));
        update("yLabel", formatText(data.yLabel));
        update("yTicks", formatList(data.yTicks, [](ChartTick const& tick) {
                   return formatChartText(tick.text) + "@" + std::format("{}", tick.value);
               }));
        update("lines", formatList(data.lines, [](ChartLine const& line) {
                   return formatChartText(line.label) + "@" + std::format("{}", line.value) + ";" +
                          chartToneName(line.tone) + ";" + chartStyleName(line.style);
               }));
        update("series", formatChartSeries(data.series));
        update("summary", formatText(data.summary));
    }

private:
    // Logs a property only when its text differs from what is recorded, so a test sees which parts changed.
    void update(std::string_view name, std::string value) {
        auto const& props = store().at(id()).props;
        auto const found = props.find(name);
        if (found != props.end() && found->second == value) {
            return;
        }
        set(name, std::move(value));
    }
};

/// @brief A fake `KeyValueWidget`: properties `items` (`[label:value,…]`, a value with spans as its runs in brackets,
///        then `;tone=<Tone>` for a toned one) and `captions`.
class FakeKeyValue final : public FakeLeaf<KeyValueWidget> {
public:
    using FakeLeaf<KeyValueWidget>::FakeLeaf;
    void setItems(std::vector<KeyValueItem> const& items) override {
        // `;` is escaped too, so a caption or a value cannot forge a tone.
        auto const text = [](std::string_view value) {
            return escape(value, [](char character) { return std::string_view{"=,[];"}.contains(character); });
        };
        set("items", formatList(items, [&text](KeyValueItem const& item) {
                std::string const formatted =
                    text(item.label) + ":" + (item.spans.empty() ? text(item.value) : formatSpans(item.spans));
                return item.tone == Tone::Neutral ? formatted : formatted + ";tone=" + enumName(item.tone);
            }));
    }
    void setCaptionPlacement(CaptionPlacement placement) override {
        set("captions", placement == CaptionPlacement::Above ? "Above" : "Beside");
    }
    void setCaptionWidth(int units) override { set("captionWidth", std::to_string(units)); }
};

/// @brief A fake `EmptyStateWidget`: properties `title`, `text`, `icon`, `actionLabel`.
class FakeEmptyState final : public FakeLeaf<EmptyStateWidget> {
public:
    using FakeLeaf<EmptyStateWidget>::FakeLeaf;
    void setTitle(std::string_view title) override { set("title", formatText(title)); }
    void setText(std::string_view text) override { set("text", formatText(text)); }
    void setIcon(std::string_view icon) override { set("icon", formatText(icon)); }
    void setActionLabel(std::string_view label) override { set("actionLabel", formatText(label)); }
    void setOnAction(Action onAction) override { callbacks().action = std::move(onAction); }
};

/// @brief A fake `DrawerWidget`: properties `side`, `open`, `title`, `description`, and `headNote`, `footerNote`,
///        `closeButton` and `dismissLabel` (when set); a `Footer` child once its footer is asked for.
class FakeDrawer final : public FakeOverlay<DrawerWidget> {
public:
    using FakeOverlay<DrawerWidget>::FakeOverlay;
    void setOpen(bool open) override { set("open", formatBool(open)); }
    void setTitle(std::string_view title) override { set("title", formatText(title)); }
    void setDescription(std::string_view description) override { set("description", formatText(description)); }
    void setDescriptionSpans(std::vector<TextSpan> const& spans) override {
        set("descriptionSpans", formatSpans(spans));
    }
    void setOnDismiss(Action onDismiss) override { callbacks().dismiss = std::move(onDismiss); }
    void setDismissLabel(std::string_view label) override { set("dismissLabel", formatText(label)); }
    void setHeadNote(std::string_view note) override { set("headNote", formatText(note)); }
    void setFooterNote(std::string_view note) override { set("footerNote", formatText(note)); }
    void setCloseButton(bool shown) override { set("closeButton", formatBool(shown)); }
};

/// @brief A fake `PopoverWidget`: properties `open` and `width`.
class FakePopover final : public FakeContainer<PopoverWidget> {
public:
    using FakeContainer<PopoverWidget>::FakeContainer;
    void setOpen(bool open) override { set("open", formatBool(open)); }
    void setWidth(int units) override { set("width", std::to_string(units)); }
    void setOnDismiss(Action onDismiss) override { callbacks().dismiss = std::move(onDismiss); }
};

/// @brief Whether a record's property holds a boolean value.
/// @param record The record.
/// @param name   The property.
/// @param value  The value asked about.
/// @return True when the property is set and equals @p value.
[[nodiscard]] inline bool propIs(Record const& record, std::string_view name, bool value) {
    auto const found = record.props.find(name);
    return found != record.props.end() && found->second == formatBool(value);
}

/// @brief Whether a record is an overlay that is not open: a dialog, drawer, popover or spotlight shows nothing,
///        itself included, until it is opened. A new overlay kind is added here, once.
/// @param record The widget's record.
/// @return True for a closed overlay.
[[nodiscard]] inline bool isClosedOverlay(Record const& record) {
    return (record.kind == "Dialog" || record.kind == "Drawer" || record.kind == "Popover" ||
            record.kind == "Spotlight") &&
           !propIs(record, "open", true);
}

/// @brief Whether a widget is effectively shown: it and every container around it visible, every dialog, drawer,
///        popover and spotlight around it open, no panel around it collapsed, and no closed collapsible around it
///        unless it is that section's header.
/// @param store  The records.
/// @param record The widget.
/// @return Whether a user could see it.
[[nodiscard]] inline bool shownInStore(RecordStore const& store, Record const& record) {
    if (propIs(record, "visible", false) || isClosedOverlay(record)) {
        return false;
    }
    for (int inner = record.id, outer = record.parent; outer != 0;) {
        Record const& around = store.at(outer);
        if (propIs(around, "visible", false) || propIs(around, "collapsed", true) || isClosedOverlay(around)) {
            return false;
        }
        if (around.kind == "Collapsible" && propIs(around, "open", false)) {
            auto const header = around.props.find("header");
            if (header == around.props.end() || header->second != std::to_string(inner)) {
                return false;
            }
        }
        inner = outer;
        outer = around.parent;
    }
    return true;
}

/// @brief A fake `SpotlightWidget`: properties `open`, `target` (`none` for none) and `targetFocusable`, and, as it
///        resolves its target against the backend's live widgets on every change the store tells, `targetShown` and
///        `ringed` (the id of the widget it rings, `0` for none). Opening moves the recorded focus to its first shown
///        `Button`, or to itself; closing moves it back to the widget that had it.
class FakeSpotlight final : public FakeContainer<SpotlightWidget> {
public:
    /// @brief Records the spotlight and starts following the store.
    /// @param store  Where the widget records itself.
    /// @param parent The container it is appended to, or null for a root.
    FakeSpotlight(std::shared_ptr<RecordStore> store, Widget const* parent)
        : FakeContainer<SpotlightWidget>{std::move(store), "Spotlight", parent},
          _listener{this->store().listen([this] { resolve(); })} {
        set("targetShown", formatBool(false));
        set("ringed", "0");
    }

    /// @brief Stops following the store.
    ~FakeSpotlight() override { this->store().unlisten(_listener); }

    FakeSpotlight(FakeSpotlight const&) = delete;
    FakeSpotlight& operator=(FakeSpotlight const&) = delete;
    FakeSpotlight(FakeSpotlight&&) = delete;
    FakeSpotlight& operator=(FakeSpotlight&&) = delete;

    void setOpen(bool open) override {
        if (open == _open) {
            return;
        }
        _open = open;
        if (open) {
            _restore = store().focused();
        }
        set("open", formatBool(open));
        if (open) {
            focusInside();
            return;
        }
        int const restore = std::exchange(_restore, 0);
        if (restore != 0 && store().records().contains(restore)) {
            store().focus(restore);
        }
    }
    void setTarget(std::optional<std::string> testId) override {
        _target = std::move(testId);
        set("target", _target ? formatText(*_target) : std::string{"none"});
        resolve();
    }
    [[nodiscard]] bool targetShown() const override { return _ringed != 0; }
    void setOnTargetShownChanged(std::function<void(bool)> handler) override { _onChanged = std::move(handler); }
    void setTargetFocusable(bool focusable) override { set("targetFocusable", formatBool(focusable)); }

    /// @brief The widget it rings.
    /// @return Its id, or 0 for none.
    [[nodiscard]] int ringed() const noexcept { return _ringed; }

private:
    // Whether @p record is inside this spotlight.
    [[nodiscard]] bool within(Record const& record) const {
        for (int outer = record.parent; outer != 0; outer = store().at(outer).parent) {
            if (outer == id()) {
                return true;
            }
        }
        return false;
    }

    void focusInside() {
        for (auto const& [candidateId, record] : store().records()) {
            if (record.kind == "Button" && within(record) && shownInStore(store(), record)) {
                store().focus(candidateId);
                return;
            }
        }
        store().focus(id());
    }

    void resolve() {
        if (_resolving) {
            return;
        }
        _resolving = true;
        int found = 0;
        if (_target) {
            std::string const wanted = formatText(*_target);
            for (auto const& [candidateId, record] : store().records()) {
                auto const testId = record.props.find("testId");
                if (testId != record.props.end() && testId->second == wanted && candidateId != id() &&
                    !within(record) && shownInStore(store(), record)) {
                    found = candidateId;
                    break;
                }
            }
        }
        bool const was = _ringed != 0;
        if (found != _ringed) {
            _ringed = found;
            set("ringed", std::to_string(found));
        }
        bool const shown = found != 0;
        if (shown != was) {
            set("targetShown", formatBool(shown));
        }
        _resolving = false;
        if (shown != was && _onChanged) {
            auto const handler = _onChanged;
            handler(shown);
        }
    }

    int _listener;
    std::optional<std::string> _target;
    int _ringed = 0;
    int _restore = 0;
    bool _open = false;
    bool _resolving = false;
    std::function<void(bool)> _onChanged;
};

/// @brief A fake `SplitterWidget`: properties `axis`, `sizes` (`[n,…]`).
class FakeSplitter final : public FakeContainer<SplitterWidget> {
public:
    using FakeContainer<SplitterWidget>::FakeContainer;
    void setSizes(std::vector<int> const& sizes) override {
        set("sizes", formatList(sizes, [](int size) { return std::to_string(size); }));
    }
    void setOnResize(std::function<void(std::vector<int>)> onResize) override {
        callbacks().resize = std::move(onResize);
    }
};

/// @brief A fake `CollapsibleWidget`: properties `title`, `header` (the header child's id), `open`.
class FakeCollapsible final : public FakeContainer<CollapsibleWidget> {
public:
    using FakeContainer<CollapsibleWidget>::FakeContainer;
    void setTitle(std::string_view title) override { set("title", formatText(title)); }

    /// @brief Records property `header`, the child's id.
    /// @param header One of this widget's children.
    /// @throws std::logic_error when @p header is not this widget's child.
    void setHeader(Widget& header) override {
        int const headerId = store().idOf(header);
        if (store().at(headerId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setHeader with a widget that is not this collapsible's child"};
        }
        set("header", std::to_string(headerId));
    }
    void setOpen(bool open) override { set("open", formatBool(open)); }
    void setOnToggle(std::function<void(bool)> onToggle) override { callbacks().toggle = std::move(onToggle); }
};

class FakePageRegion;

/// @brief A fake `PageWidget`: properties `size`, `margins`, `scale` and `displayed`. The derived ones follow the
///        width the backend offers pages (`RecordingBackend::setPageAvailableWidth`) and are logged when they change.
class FakePage final : public FakeContainer<PageWidget> {
public:
    /// @brief Records a page.
    /// @param store     Where it records itself.
    /// @param parent    The container it is appended to, or null.
    /// @param geometry  The sheet.
    /// @param available The width the backend offers pages.
    /// @param pages     The pages of the backend, which this one joins.
    FakePage(std::shared_ptr<RecordStore> store, Widget const* parent, PageGeometry const& geometry,
             std::optional<Mm> available, std::shared_ptr<std::vector<FakePage*>> pages)
        : FakeContainer<PageWidget>(std::move(store), "Page", parent, initialProps(geometry, available)),
          _geometry{geometry},
          _available{available},
          _pages{std::move(pages)} {
        _pages->push_back(this);
    }

    /// @brief Leaves the backend's pages and lets the regions that outlive it know.
    ~FakePage() override;

    FakePage(FakePage const&) = delete;
    FakePage& operator=(FakePage const&) = delete;
    FakePage(FakePage&&) = delete;
    FakePage& operator=(FakePage&&) = delete;

    /// @brief The sheet.
    /// @return Its geometry.
    [[nodiscard]] PageGeometry const& geometry() const noexcept { return _geometry; }

    /// @brief Changes the width the page is offered and records what follows from it.
    /// @param available The width; absent is unlimited.
    void offer(std::optional<Mm> available) {
        _available = available;
        relayout();
    }

    /// @brief Adds a region to the layout, in creation order.
    /// @param region The region.
    void adopt(FakePageRegion& region) { _regions.push_back(&region); }

    /// @brief Forgets a region.
    /// @param region The region.
    void release(FakePageRegion& region) { std::erase(_regions, &region); }

    /// @brief Lays the page out and records each derived property that changed.
    void relayout();

private:
    [[nodiscard]] static PropList initialProps(PageGeometry const& geometry, std::optional<Mm> available) {
        PageLayout const layout = layoutPage(geometry, {}, available);
        PageMargins const& m = geometry.margins;
        return {{"size", geometry.width.text() + "x" + geometry.height.text()},
                {"margins", m.top.text() + "," + m.right.text() + "," + m.bottom.text() + "," + m.left.text()},
                {"scale", formatFraction(layout.scale)},
                {"displayed", layout.displayedWidth.text() + "x" + layout.displayedHeight.text()}};
    }

    void setIfChanged(std::string_view name, std::string value) {
        if (store().at(id()).props[std::string{name}] != value) {
            set(name, std::move(value));
        }
    }

    PageGeometry _geometry;
    std::optional<Mm> _available;
    std::vector<FakePageRegion*> _regions;
    std::shared_ptr<std::vector<FakePage*>> _pages;
};

/// @brief A fake `PageRegionWidget`: properties `anchor`, `rect` and `overflow`; the last two follow the page's
///        layout and the content height the test gives.
class FakePageRegion final : public FakeContainer<PageRegionWidget> {
public:
    /// @brief Records a region.
    /// @param store    Where it records itself.
    /// @param parent   Its page.
    /// @param geometry Its place.
    /// @throws std::logic_error when @p parent is not a page of this backend.
    FakePageRegion(std::shared_ptr<RecordStore> store, ContainerWidget* parent, RegionGeometry const& geometry)
        : FakeContainer<PageRegionWidget>(std::move(store), "PageRegion", parent, initialProps(parent, geometry)),
          _page{dynamic_cast<FakePage*>(parent)},
          _geometry{geometry} {
        _page->adopt(*this);
    }

    /// @brief Leaves the page's layout, if the page still stands.
    ~FakePageRegion() override {
        if (_page != nullptr) {
            _page->release(*this);
        }
    }

    FakePageRegion(FakePageRegion const&) = delete;
    FakePageRegion& operator=(FakePageRegion const&) = delete;
    FakePageRegion(FakePageRegion&&) = delete;
    FakePageRegion& operator=(FakePageRegion&&) = delete;

    /// @brief The region's place.
    /// @return Its geometry.
    [[nodiscard]] RegionGeometry const& geometry() const noexcept { return _geometry; }

    /// @brief The content's height.
    /// @return It; `0` until `setContentHeight`.
    [[nodiscard]] Mm contentHeight() const noexcept { return _contentHeight; }

    /// @brief Gives the content's height and relays the page out.
    /// @param height The height.
    void setContentHeight(Mm height) {
        _contentHeight = height;
        if (_page != nullptr) {
            _page->relayout();
        }
    }

    /// @brief The page the region lays out in.
    /// @return It, or null once the page has been destroyed.
    [[nodiscard]] FakePage const* page() const noexcept { return _page; }

    /// @brief Forgets the page: it is being destroyed before this region.
    void orphan() noexcept { _page = nullptr; }

    /// @brief Records a changed `rect` or `overflow`.
    /// @param layout The region's resolved place.
    void show(RegionLayout const& layout) {
        std::string const rect = layout.rect.text();
        std::string const overflow = formatBool(layout.overflow);
        if (store().at(id()).props["rect"] != rect) {
            set("rect", rect);
        }
        if (store().at(id()).props["overflow"] != overflow) {
            set("overflow", overflow);
        }
    }

private:
    [[nodiscard]] static PropList initialProps(ContainerWidget* parent, RegionGeometry const& geometry) {
        auto const* const page = dynamic_cast<FakePage const*>(parent);
        if (page == nullptr) {
            throw std::logic_error{"RecordingBackend: a page region's parent is a page"};
        }
        RegionSpec const spec{.geometry = geometry, .contentHeight = {}};
        RegionLayout const layout = layoutPage(page->geometry(), std::span{&spec, 1}, std::nullopt).regions.front();
        return {{"anchor", anchorName(geometry.anchor)},
                {"rect", layout.rect.text()},
                {"overflow", formatBool(layout.overflow)}};
    }

    FakePage* _page;
    RegionGeometry _geometry;
    Mm _contentHeight{};
};

inline FakePage::~FakePage() {
    for (FakePageRegion* const region : _regions) {
        region->orphan();
    }
    std::erase(*_pages, this);
}

inline void FakePage::relayout() {
    std::vector<RegionSpec> specs;
    specs.reserve(_regions.size());
    for (FakePageRegion const* region : _regions) {
        specs.push_back({region->geometry(), region->contentHeight()});
    }
    PageLayout const layout = layoutPage(_geometry, specs, _available);
    setIfChanged("scale", formatFraction(layout.scale));
    setIfChanged("displayed", layout.displayedWidth.text() + "x" + layout.displayedHeight.text());
    for (std::size_t i = 0; i < _regions.size(); ++i) {
        _regions[i]->show(layout.regions[i]);
    }
}

/// @brief A fake `DropZoneWidget`: properties `accept` (`[.ext,…]`), `multiple`.
class FakeDropZone final : public FakeContainer<DropZoneWidget> {
public:
    using FakeContainer<DropZoneWidget>::FakeContainer;
    void setAccept(std::vector<std::string> const& extensions) override {
        _accept = extensions;
        set("accept", formatList(extensions, [](std::string const& extension) { return formatItem(extension); }));
    }
    void setMultiple(bool multiple) override {
        _multiple = multiple;
        set("multiple", formatBool(multiple));
    }
    void setOnDrop(std::function<void(std::vector<std::string>)> onDrop) override {
        callbacks().files = std::move(onDrop);
    }

    /// @brief Whether the zone takes a drop of @p paths: one file unless several are allowed, each with an accepted
    ///        extension.
    /// @param paths The paths dropped; not empty.
    /// @return True when the drop is taken.
    [[nodiscard]] bool takes(std::vector<std::string> const& paths) const {
        if (paths.size() > 1 && !_multiple) {
            return false;
        }
        return std::ranges::all_of(paths, [this](std::string const& path) {
            return _accept.empty() || std::ranges::any_of(_accept, [&path](std::string const& extension) {
                       return endsWithIgnoringCase(path, extension);
                   });
        });
    }

private:
    [[nodiscard]] static bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) {
        if (suffix.size() > text.size()) {
            return false;
        }
        auto const lower = [](char character) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        };
        return std::ranges::equal(text.substr(text.size() - suffix.size()), suffix, {}, lower, lower);
    }

    std::vector<std::string> _accept;
    bool _multiple = false;
};

/// @brief A fake loaded `CustomWidget`: property `component`, and each prop as `prop.<name>`.
class FakeCustom final : public FakeLeaf<CustomWidget> {
public:
    using FakeLeaf<CustomWidget>::FakeLeaf;
    void setTextProp(std::string_view name, std::string_view value) override {
        set("prop." + std::string{name}, formatText(value));
    }
    void setBoolProp(std::string_view name, bool value) override {
        set("prop." + std::string{name}, formatBool(value));
    }
    void setNumberProp(std::string_view name, double value) override {
        set("prop." + std::string{name}, std::format("{}", value));
    }
    void setOnEvent(std::function<void(std::string event, std::string payload)> onEvent) override {
        callbacks().component = std::move(onEvent);
    }
};

/// @brief A fake `TableWidget`: properties `columns`, `selectionMode`, `selection`, `sort`, `selectColumn`,
///        `selectAllLabel`, and `rowKey`, `tone`, `label` and `heading` on a row.
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
            _viewRows.erase(childId);
            _headings.erase(childId);
            std::erase_if(_loads, [childId](auto const& entry) { return entry.first.first == childId; });
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

    /// @brief Records property `columns`, each as its escaped caption, a colon and its width, then `;align=` and
    ///        `;valign=` where the column's alignment is not the default (`Start` across, `Center` down), and
    ///        `;sortable=true` for a sortable column, which `RecordingBackend::clickHeader` reports,
    ///        `;filterable=true` for a column with a filter button, which `RecordingBackend::clickFilter` presses,
    ///        `;wrap=false` for a column that keeps its text cells to one line, `;shrink=true` for a stretch column
    ///        that may be narrower than its header, and `;unit=` with the column's unit when it has one.
    /// @param columns The columns, in order.
    void setColumns(std::vector<TableColumn> const& columns) override {
        _columns = columns;
        _sortable.clear();
        for (TableColumn const& column : columns) {
            _sortable.push_back(column.sortable);
        }
        set("columns", formatList(columns, [](TableColumn const& column) {
                std::string text = formatItem(column.label) + ":" + formatSizing(column.width);
                if (column.align != Align::Start) {
                    text += ";align=" + formatAlign(column.align);
                }
                if (column.valign != Align::Center) {
                    text += ";valign=" + formatAlign(column.valign);
                }
                if (column.sortable) {
                    text += ";sortable=true";
                }
                if (column.filterable) {
                    text += ";filterable=true";
                }
                if (!column.wrap) {
                    text += ";wrap=false";
                }
                if (column.shrink) {
                    text += ";shrink=true";
                }
                if (!column.unit.empty()) {
                    text += ";unit=" + formatItem(column.unit);
                }
                return text;
            }));
    }

    /// @brief Records property `showHeader`.
    /// @param shown Whether the header row is drawn.
    void setShowHeader(bool shown) override { set("showHeader", formatBool(shown)); }

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

    /// @brief Records the row's property `heading`; a heading row is never marked, selected or activated.
    /// @param row One of this table's rows.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowHeading(Widget& row) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowHeading of a widget that is not this table's row"};
        }
        store().set(rowId, "heading", "true");
        _headings.insert(rowId);
        remark();
    }

    /// @brief Whether the current row with @p key is a heading.
    /// @param key The key.
    /// @return False when no current row has it, or the row is a data row.
    [[nodiscard]] bool headingKey(Key const& key) const {
        std::optional<int> const row = rowOf(key);
        return row.has_value() && _headings.contains(*row);
    }

    /// @brief Records the row's property `rowSpans`: `column:rows` pairs joined by commas, a span across columns as
    ///        `column:rowsxcolumns` (`0:2x3`), `none` for an empty list.
    /// @param row   One of this table's rows.
    /// @param spans The row's effective spans.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowSpans(Widget& row, std::vector<RowSpan> const& spans) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowSpans of a widget that is not this table's row"};
        }
        std::string text;
        for (RowSpan const& span : spans) {
            text += (text.empty() ? "" : ",") + std::to_string(span.column) + ":" + std::to_string(span.rows);
            if (span.columns != 1) {
                text += "x" + std::to_string(span.columns);
            }
        }
        store().set(rowId, "rowSpans", text.empty() ? std::string{"none"} : text);
    }

    /// @brief Records the row's property `summary` (`"true"`).
    /// @param row One of this table's rows.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowSummary(Widget& row) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowSummary of a widget that is not this table's row"};
        }
        store().set(rowId, "summary", "true");
    }

    /// @brief Records the row's property `tone` (`none` for no tone).
    /// @param row One of this table's rows.
    /// @param tone The tone, or none.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowTone(Widget& row, std::optional<Tone> tone) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowTone of a widget that is not this table's row"};
        }
        store().set(rowId, "tone", tone ? enumName(*tone) : std::string{"none"});
        if (auto const key = _rowKeys.find(rowId); key != _rowKeys.end()) {
            _rowTones.insert_or_assign(key->second, tone ? enumName(*tone) : std::string{"none"});
        }
    }

    /// @brief The last tone `setRowTone` set on the row with a key.
    /// @param key The row's key.
    /// @return The tone's name, `none` for no tone; empty before any.
    [[nodiscard]] std::string rowTone(Key const& key) const {
        auto const found = _rowTones.find(key);
        return found == _rowTones.end() ? std::string{} : found->second;
    }

    /// @brief Records the row's marks by the row's key, which `RecordingBackend::rowMarks` reads.
    /// @param row   One of this table's rows.
    /// @param marks The marks.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowMarks(Widget& row, RowMarks const& marks) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowMarks of a widget that is not this table's row"};
        }
        if (auto const key = _rowKeys.find(rowId); key != _rowKeys.end()) {
            _rowMarks.insert_or_assign(key->second, marks);
        }
    }

    /// @brief The last marks `setRowMarks` set on the row with a key.
    /// @param key The row's key.
    /// @return The marks; none before any.
    [[nodiscard]] std::optional<RowMarks> rowMarks(Key const& key) const {
        auto const found = _rowMarks.find(key);
        return found == _rowMarks.end() ? std::nullopt : std::optional{found->second};
    }

    /// @brief Records property `selectColumn`.
    /// @param shown Whether the select column is drawn.
    void setSelectColumn(bool shown) override { set("selectColumn", formatBool(shown)); }

    /// @brief Records property `selectAllLabel`.
    /// @param label The header box's name.
    void setSelectAllLabel(std::string_view label) override { set("selectAllLabel", formatText(label)); }

    /// @brief Records the row's property `label`.
    /// @param row One of this table's rows.
    /// @param label The row's name.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowLabel(Widget& row, std::string_view label) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowLabel of a widget that is not this table's row"};
        }
        store().set(rowId, "label", formatText(label));
    }

    /// @brief Requests the rows with @p keys, and records property `selection` as the rows it marks.
    /// @param keys The selected keys.
    void setSelection(std::vector<Key> const& keys) override {
        _viewExcept.reset();
        _requested = keys;
        set("selection", marked());
    }

    /// @brief Remembers every key in the table's data, so a selected key whose row is not realized stays selected.
    /// @param keys Every key currently in the data.
    void setExistingKeys(std::vector<Key> const& keys) override {
        _existing = std::unordered_set<Key>{keys.begin(), keys.end()};
        remark();
    }

    /// @brief Marks every row but those of @p except until the next `setSelection`, and records property
    ///        `selection` as the rows it marks.
    /// @param except The keys left out.
    void setSelectionView(std::vector<Key> const& except) override {
        _viewExcept = except;
        set("selection", marked());
    }

    /// @brief Stores the handler `RecordingBackend::chooseStatusEntry` calls.
    /// @param onStatusEntry The handler.
    void setOnStatusEntry(std::function<void(std::string entryId)> onStatusEntry) override {
        _onStatusEntry = std::move(onStatusEntry);
    }

    /// @brief The handler `setOnStatusEntry` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::string)> const& onStatusEntry() const noexcept { return _onStatusEntry; }

    /// @brief Stores the handler `RecordingBackend::selectRows` calls.
    /// @param onSelectionChange The handler.
    void setOnSelectionChange(std::function<void(std::vector<Key>, SelectionChange)> onSelectionChange) override {
        callbacks().selection = std::move(onSelectionChange);
    }

    /// @brief Stores what `Ctrl+A` calls in place of selecting the rows held.
    /// @param onSelectAll The handler; empty to select the rows held.
    void setOnSelectAll(std::function<void()> onSelectAll) override { _onSelectAll = std::move(onSelectAll); }

    /// @brief The handler `setOnSelectAll` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void()> const& onSelectAll() const noexcept { return _onSelectAll; }

    /// @brief Stores the handler `RecordingBackend::activateRow` calls.
    /// @param onActivate The handler.
    void setOnActivate(std::function<void(Key)> onActivate) override { callbacks().activate = std::move(onActivate); }

    /// @brief Records property `sort`: each key's column index and direction, or `none`.
    /// @param chain The sort chain shown.
    void setSort(std::vector<TableSort> const& chain) override {
        _chain = chain;
        set("sort", formatSort(chain));
    }

    /// @brief Stores the handler `RecordingBackend::clickHeader` calls.
    /// @param onSort The handler.
    void setOnSort(std::function<void(std::size_t, HeaderClick)> onSort) override {
        callbacks().sort = std::move(onSort);
    }

    /// @brief Stores the menus `RecordingBackend::menuOf` reads and `RecordingBackend::chooseMenu` chooses from.
    /// @param entries  The entries of a column's menu, or the table's.
    /// @param onChoose The handler for a chosen entry.
    void setMenu(std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> entries,
                 std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose) override {
        _menu = std::move(entries);
        _onMenu = std::move(onChoose);
    }

    /// @brief Stores the cell menu `RecordingBackend::rightClickCell` opens and `RecordingBackend::chooseCellMenu`
    ///        chooses from, and whose entries' chords `RecordingBackend::press` runs on the focused cell.
    /// @param entries The entries of a cell's menu.
    /// @param chosen  The handler for a chosen entry.
    void setCellMenu(std::function<std::vector<MenuEntry>(Key const&, std::size_t)> entries,
                     std::function<void(Key const&, std::size_t, std::vector<std::size_t> const&)> chosen) override {
        _cellMenu = std::move(entries);
        _cellChosen = std::move(chosen);
    }

    /// @brief Stores the chords that ask for the focused cell's entries, which `RecordingBackend::press` checks.
    /// @param chords The chords.
    void setCellMenuKeys(std::vector<std::string> const& chords) override { _cellMenuKeys = chords; }

    /// @brief The chords `setCellMenuKeys` gave.
    /// @return They; empty before any.
    [[nodiscard]] std::vector<std::string> const& cellMenuKeys() const noexcept { return _cellMenuKeys; }

    /// @brief How many times the cell menu's entries were asked for.
    /// @return The count.
    [[nodiscard]] std::size_t cellMenuAsks() const noexcept { return _cellMenuAsks; }

    /// @brief Stores the visible columns, which the drivers check a column against.
    /// @param visible The visible columns, in layout order.
    void setVisibleColumns(std::vector<std::size_t> const& visible) override { _visible = visible; }

    /// @brief Stores the handler `RecordingBackend::moveColumn` calls.
    /// @param handler The handler.
    void setOnColumnMove(std::function<void(std::size_t, std::size_t)> handler) override {
        _onColumnMove = std::move(handler);
    }

    /// @brief Stores the handler `RecordingBackend::resizeColumn` calls.
    /// @param handler The handler.
    void setOnColumnResize(std::function<void(std::size_t, std::optional<int>)> handler) override {
        _onColumnResize = std::move(handler);
    }

    /// @brief Stores the handler `RecordingBackend::clickFilter` calls.
    /// @param handler The handler.
    void setOnFilter(std::function<void(std::size_t)> handler) override { _onFilter = std::move(handler); }

    /// @brief Records property `filtered`, the columns as a list such as `[0,2]`.
    /// @param columns The filtered columns.
    void setFiltered(std::vector<std::size_t> const& columns) override {
        _filtered = columns;
        set("filtered", formatList(columns, [](std::size_t column) { return std::to_string(column); }));
    }

    /// @brief Records property `columnGroups`, each group as `label:first+count`, such as `[Company:0+3]`, with
    ///        `;filtered=true` for a filtered group; `:`, `+` and `;` in a label are escaped as well as a list item's
    ///        characters.
    /// @param groups The groups.
    void setColumnGroups(std::vector<TableColumnGroup> const& groups) override {
        _groups = groups;
        set("columnGroups", formatList(groups, [](TableColumnGroup const& group) {
                std::string const label = escape(
                    group.label, [](char character) { return std::string_view{"=,[]:+;"}.contains(character); });
                return label + ":" + std::to_string(group.first) + "+" + std::to_string(group.count) +
                       (group.filtered ? ";filtered=true" : "");
            }));
    }

    /// @brief Records the property `bands`: each run as `level:label|unit@first+count`, `;pinned` for a pinned run,
    ///        joined by commas; `|unit` only for a run with a unit.
    /// @param runs The runs.
    void setBands(std::vector<BandRun> const& runs) override {
        _bands = runs;
        set("bands", formatList(runs, [](BandRun const& run) {
                std::string const label = escape(
                    run.label, [](char character) { return std::string_view{"=,[]:+;@|"}.contains(character); });
                return std::to_string(run.level) + ":" + label + (run.unit.empty() ? "" : "|" + run.unit) + "@" +
                       std::to_string(run.first) + "+" + std::to_string(run.count) + (run.pinned ? ";pinned" : "");
            }));
    }

    /// @brief The runs `setBands` last gave.
    /// @return They.
    [[nodiscard]] std::vector<BandRun> const& bands() const noexcept { return _bands; }

    /// @brief Stores the handler `RecordingBackend::clickGroupFilter` calls.
    /// @param handler The handler.
    void setOnGroupFilter(std::function<void(std::size_t)> handler) override { _onGroupFilter = std::move(handler); }

    /// @brief Stores the handler `RecordingBackend::pressCopy` and `Ctrl+C` call.
    /// @param onCopy The handler.
    void setOnCopy(std::function<void()> onCopy) override { _onCopy = std::move(onCopy); }

    /// @brief Whether a user can reach @p column's header: it is one of the columns and, once the visible columns
    ///        were told, one of them.
    /// @param column The column.
    /// @return True when its header is drawn.
    [[nodiscard]] bool headerShown(std::size_t column) const {
        return column < _columns.size() && (_visible.empty() || std::ranges::find(_visible, column) != _visible.end());
    }

    /// @brief How many columns are visible: as many as were told, or every column before any were.
    /// @return The count.
    [[nodiscard]] std::size_t visibleCount() const noexcept {
        return _visible.empty() ? _columns.size() : _visible.size();
    }

    /// @brief The visible place of @p column: its place among the visible columns, or its index before any were told.
    /// @param column The column.
    /// @return The place.
    [[nodiscard]] std::size_t placeOf(std::size_t column) const {
        if (_visible.empty()) {
            return column;
        }
        return static_cast<std::size_t>(std::distance(_visible.begin(), std::ranges::find(_visible, column)));
    }

    /// @brief The text a column's header draws: its label, then its sort key's mark (`sortMarkText`).
    /// @param column The column.
    /// @return The text.
    [[nodiscard]] std::string headerText(std::size_t column) const {
        std::string text = column < _columns.size() ? _columns.at(column).label : std::string{};
        for (std::size_t place = 0; place < _chain.size(); ++place) {
            if (_chain.at(place).column == column) {
                text += sortMarkText(_chain.at(place).direction, place + 1, _chain.size());
            }
        }
        return text;
    }

    /// @brief A column header's accessible description: its sort key's (`sortDescription`), or empty.
    /// @param column The column.
    /// @return The description.
    [[nodiscard]] std::string headerDescription(std::size_t column) const {
        for (std::size_t place = 0; place < _chain.size(); ++place) {
            if (_chain.at(place).column == column) {
                return sortDescription(_chain.at(place).direction, place + 1, _chain.size());
            }
        }
        return {};
    }

    /// @brief A column header's accessible name: its label, followed by `, filtered` while it has a filter.
    /// @param column The column.
    /// @return The name.
    [[nodiscard]] std::string headerName(std::size_t column) const {
        std::string name = column < _columns.size() ? _columns.at(column).label : std::string{};
        if (column < _columns.size() && _columns.at(column).filterable &&
            std::ranges::find(_filtered, column) != _filtered.end()) {
            name += ", filtered";
        }
        return name;
    }

    /// @brief The entries of a menu, as `setMenu` gives them.
    /// @param column The column, or none for the table's menu.
    /// @return The entries; none when the table has no menus.
    [[nodiscard]] std::vector<MenuEntry> menuEntries(std::optional<std::size_t> column) const {
        return _menu ? _menu(column) : std::vector<MenuEntry>{};
    }

    /// @brief The entries of a cell's menu, as `setCellMenu` gives them.
    /// @param key    The cell's row.
    /// @param column The cell's column.
    /// @return The entries; none when the table has no cell menu.
    [[nodiscard]] std::vector<MenuEntry> cellMenuEntries(Key const& key, std::size_t column) const {
        if (!_cellMenu) {
            return {};
        }
        ++_cellMenuAsks;
        return _cellMenu(key, column);
    }

    /// @brief The handler `setCellMenu` gave for a chosen entry.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(Key const&, std::size_t, std::vector<std::size_t> const&)> const& cellChosen()
        const {
        return _cellChosen;
    }

    /// @brief The focused cell's row and column, as `setFocusCell` last placed it, its row by the view index
    ///        `setRowIndex` gave a current row.
    /// @return The cell; none before any `setFocusCell`, or while no current row is at its view row.
    [[nodiscard]] std::optional<std::pair<Key, std::size_t>> focusedCellKey() const {
        if (!_focusSet) {
            return std::nullopt;
        }
        for (auto const& [rowId, viewRow] : _viewRows) {
            if (viewRow != _focusCell.first) {
                continue;
            }
            if (auto const key = _rowKeys.find(rowId); key != _rowKeys.end()) {
                return std::pair{key->second, _focusCell.second};
            }
        }
        return std::nullopt;
    }

    /// @brief The handler `setMenu` gave for a chosen entry.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> const& onMenu() const {
        return _onMenu;
    }

    /// @brief The handler `setOnColumnMove` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::size_t, std::size_t)> const& onColumnMove() const { return _onColumnMove; }

    /// @brief The handler `setOnColumnResize` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::size_t, std::optional<int>)> const& onColumnResize() const {
        return _onColumnResize;
    }

    /// @brief The handler `setOnFilter` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::size_t)> const& onFilter() const { return _onFilter; }

    /// @brief The handler `setOnGroupFilter` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::size_t)> const& onGroupFilter() const { return _onGroupFilter; }

    /// @brief How many groups `setColumnGroups` gave.
    /// @return The count.
    [[nodiscard]] std::size_t groupCount() const noexcept { return _groups.size(); }

    /// @brief The handler `setOnCopy` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void()> const& onCopy() const { return _onCopy; }

    /// @brief Whether the column at @p column is sortable, as `setColumns` gave it.
    /// @param column The column's index.
    /// @return False for an index past the last column.
    [[nodiscard]] bool sortable(std::size_t column) const { return column < _sortable.size() && _sortable.at(column); }

    /// @brief Records property `realizedWindow` as `<first>+<count>/<total>`.
    /// @param first The view index of the first realized row.
    /// @param count How many rows are realized, starting at `first`.
    /// @param total How many rows the table's data has, realized or not.
    void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) override {
        set("realizedWindow", std::to_string(first) + "+" + std::to_string(count) + "/" + std::to_string(total));
    }

    /// @brief Stores the handler `RecordingBackend::scrollTable` calls.
    /// @param onVisibleRangeChanged The handler.
    void setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) override {
        _onVisibleRangeChanged = std::move(onVisibleRangeChanged);
    }

    /// @brief Simulates the backend reporting a scroll: calls the handler `setOnVisibleRangeChanged` stored, as a
    ///        real backend's own scroll callback would. Does nothing if nothing is subscribed (a table below its
    ///        virtualization threshold never calls `setOnVisibleRangeChanged`, so this is a silent no-op for it,
    ///        matching a real backend that never virtualizes).
    /// @param first The view index of the first row now wanted.
    /// @param count How many rows from `first` are wanted.
    void scrollTo(std::size_t first, std::size_t count) {
        if (_onVisibleRangeChanged) {
            _onVisibleRangeChanged(first, count);
        }
    }

    /// @brief Records property `status` as `counts|state|retry|message|shown`, each text escaped.
    /// @param status The status line.
    void setStatus(TableStatus const& status) override {
        _status = status;
        set("status", formatText(status.counts) + "|" + formatText(status.state) + "|" + formatBool(status.retry) +
                          "|" + formatText(status.message) + "|" + formatBool(status.shown));
    }

    /// @brief Stores the handler `RecordingBackend::retryTable` calls.
    /// @param onRetry The handler.
    void setOnRetry(std::function<void()> onRetry) override { callbacks().retry = std::move(onRetry); }

    /// @brief Records a cell's load state, which `RecordingBackend::cellLoad` reads, and the row's property
    ///        `load<column>` (`Loaded`, `Unloaded` or `Failed`).
    /// @param row    One of this table's rows.
    /// @param column The cell's column.
    /// @param load   The state.
    /// @throws std::logic_error when @p row is not this table's child.
    void setCellLoad(Widget& row, std::size_t column, CellLoad load) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setCellLoad of a widget that is not this table's row"};
        }
        _loads.insert_or_assign(std::pair{rowId, column}, load);
        store().set(rowId, "load" + std::to_string(column), formatCellLoad(load));
    }

    /// @brief Records a cell's accent, which `RecordingBackend::cellAccent` reads, and the row's property
    ///        `accent<column>`: the tone's name or `-`, the tooltip, and `invalid` or `valid`, joined by `|`.
    /// @param row    One of this table's rows.
    /// @param column The cell's column.
    /// @param accent The accent.
    /// @throws std::logic_error when @p row is not this table's child.
    void setCellAccent(Widget& row, std::size_t column, CellAccent const& accent) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setCellAccent of a widget that is not this table's row"};
        }
        _accents.insert_or_assign(std::pair{rowId, column}, accent);
        std::string const tone = accent.tone ? std::to_string(static_cast<int>(*accent.tone)) : std::string{"-"};
        store().set(rowId, "accent" + std::to_string(column),
                    tone + "|" + formatText(accent.tooltip) + "|" + (accent.invalid ? "invalid" : "valid"));
    }

    /// @brief Records the column window, which `windowColumns`, `totalWidth` and `pinnedShown` read, and the
    ///        properties `columnWindow` (the indices, comma-separated), `totalWidth` and `pinnedShown`.
    /// @param columns    The columns.
    /// @param offsets    Their offsets.
    /// @param widths     Their widths.
    /// @param totalWidth Every visible column's width together.
    /// @param pinned     How many of @p columns are pinned.
    void setColumnWindow(
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides `Table::setColumnWindow`; the declaration says how its spans are told apart
        std::span<std::size_t const> columns, std::span<int const> offsets,
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides `Table::setColumnWindow`; the declaration says how its totals are told apart
        std::span<int const> widths, int totalWidth, std::size_t pinned) override {
        static_cast<void>(offsets);
        static_cast<void>(widths);
        _window = std::vector<std::size_t>(columns.begin(), columns.end());
        _windowSet = true;
        _totalWidth = totalWidth;
        _pinnedShown = pinned;
        std::string text;
        for (std::size_t const column : columns) {
            text += (text.empty() ? "" : ",") + std::to_string(column);
        }
        set("columnWindow", text);
        set("totalWidth", std::to_string(totalWidth));
        set("pinnedShown", std::to_string(pinned));
    }

    /// @brief Stores the handler `RecordingBackend::scrollTableColumns` calls.
    /// @param handler The handler.
    void setOnHorizontalRange(std::function<void(int, int)> handler) override { _onHorizontal = std::move(handler); }

    /// @brief Records property `horizontalOffset`, which `RecordingBackend::horizontalOffset` reads.
    /// @param scrollX The offset.
    void setHorizontalOffset(int scrollX) override {
        _horizontalOffset = scrollX;
        set("horizontalOffset", std::to_string(scrollX));
    }

    /// @brief Stores the handler `RecordingBackend::selectTableRange` calls.
    /// @param handler The handler.
    void setOnSelectRange(std::function<void(std::size_t, std::size_t, SelectionChange)> handler) override {
        _onSelectRange = std::move(handler);
    }

    /// @brief Records property `focusCell` as `<row>:<column>`, which `RecordingBackend::focusCell` reads, and adds
    ///        the cell to the ones `RecordingBackend::focusCells` lists, and the key of the row at that view row now
    ///        to the ones `RecordingBackend::focusKeys` lists.
    /// @param viewRow The row.
    /// @param column  The column.
    void setFocusCell(std::size_t viewRow, std::size_t column) override {
        _focusCell = {viewRow, column};
        _focusSet = true;
        _focusCells.emplace_back(viewRow, column);
        std::optional<Key> named;
        for (int const child : store().at(id()).children) {
            auto const& props = store().at(child).props;
            auto const found = props.find("viewRow");
            auto const key = _rowKeys.find(child);
            if (found != props.end() && found->second == std::to_string(viewRow) && key != _rowKeys.end()) {
                named = key->second;
            }
        }
        _focusKeys.push_back(std::move(named));
        set("focusCell", std::to_string(viewRow) + ":" + std::to_string(column));
    }

    /// @brief Stores the handler `RecordingBackend::measureColumn` calls.
    /// @param handler The handler.
    void setOnColumnMeasured(std::function<void(std::size_t, ColumnMeasure const&)> handler) override {
        _onMeasured = std::move(handler);
    }

    /// @brief Stores the handler `RecordingBackend::focusMove` calls.
    /// @param handler The handler.
    void setOnFocusMove(std::function<void(FocusMove)> handler) override { _onFocusMove = std::move(handler); }

    /// @brief Stores the handler `RecordingBackend::typeInto` calls.
    /// @param onText The handler.
    void setOnText(std::function<void(std::string)> onText) override { _onText = std::move(onText); }

    /// @brief The handler `setOnText` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::string)> const& onText() const noexcept { return _onText; }

    /// @brief Stores the handler `RecordingBackend::clickCell` calls.
    /// @param onClick The handler.
    void setOnCellClick(std::function<void(std::size_t, std::size_t, bool)> onClick) override {
        _onCellClick = std::move(onClick);
    }

    /// @brief The handler `setOnCellClick` stored.
    /// @return It; empty when none.
    [[nodiscard]] std::function<void(std::size_t, std::size_t, bool)> const& onCellClick() const noexcept {
        return _onCellClick;
    }

    /// @brief Stores the handler `RecordingBackend::focusOut` calls, which focus moving from inside the table to a
    ///        widget outside it calls too.
    /// @param onFocusOut The handler.
    void setOnFocusOut(std::function<void()> onFocusOut) override { callbacks().focusOut = std::move(onFocusOut); }

    /// @brief Records the row's property `editor:<column>`: the editor widget's id, empty for none.
    /// @param row    One of this table's rows.
    /// @param column The cell's column.
    /// @param editor The editor, a child of @p row, or null.
    /// @throws std::logic_error when @p row is not this table's child, or @p editor not the row's.
    void setCellEditor(Widget& row, std::size_t column, Widget* editor) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setCellEditor of a widget that is not this table's row"};
        }
        std::string shown;
        if (editor != nullptr) {
            int const editorId = store().idOf(*editor);
            if (store().at(editorId).parent != rowId) {
                throw std::logic_error{"RecordingBackend: setCellEditor of an editor that is not the row's child"};
            }
            shown = std::to_string(editorId);
        }
        store().set(rowId, "editor:" + std::to_string(column), shown);
    }

    /// @brief Records property `cellSelection`: `r<firstRow>+<rowCount>,c<firstPosition>+<positionCount>`, `c`
    /// counting
    ///        visible positions, or empty for none.
    /// @param rect The rectangle, or nothing.
    void setCellSelection(std::optional<std::array<std::size_t, 4>> rect) override {
        set("cellSelection", rect ? "r" + std::to_string(rect->at(0)) + "+" + std::to_string(rect->at(1)) + ",c" +
                                        std::to_string(rect->at(2)) + "+" + std::to_string(rect->at(3))
                                  : std::string{});
    }

    /// @brief Stores the handler `RecordingBackend::selectCells` calls.
    /// @param onSelect The handler.
    void setOnCellSelect(std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)> onSelect) override {
        _onCellSelect = std::move(onSelect);
    }

    /// @brief Calls the handler `setOnCellSelect` stored, as a drag from cell to cell would.
    /// @param anchorRow      The anchor's view row.
    /// @param anchorPosition The anchor's visible position.
    /// @param focusRow       The focus's view row.
    /// @param focusPosition  The focus's visible position.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): two corners, each a row and a position.
    void selectCells(std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow,
                     std::size_t focusPosition) {
        if (_onCellSelect) {
            auto const handler = _onCellSelect;
            handler(anchorRow, anchorPosition, focusRow, focusPosition);
        }
    }

    /// @brief Records the row's property `marks:<column>`: `r`, `m`, `e`, `s` and `u` for the marks set, in that
    ///        order, empty for none.
    /// @param row    One of this table's rows.
    /// @param column The cell's column.
    /// @param marks  The marks.
    /// @throws std::logic_error when @p row is not this table's child.
    void setCellMarks(Widget& row, std::size_t column, CellMarks const& marks) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setCellMarks of a widget that is not this table's row"};
        }
        std::string shown;
        shown += marks.readOnly ? "r" : "";
        shown += marks.modified ? "m" : "";
        shown += marks.error ? "e" : "";
        shown += marks.stale ? "s" : "";
        shown += marks.muted ? "u" : "";
        store().set(rowId, "marks:" + std::to_string(column), shown);
    }

    /// @brief Records the row's property `modified`.
    /// @param row      One of this table's rows.
    /// @param modified Whether the row draws its modified mark.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowModified(Widget& row, bool modified) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowModified of a widget that is not this table's row"};
        }
        store().set(rowId, "modified", formatBool(modified));
    }

    /// @brief Records property `counts` as `<rows or unknown>/<columns>`, which `RecordingBackend::lastCounts` reads.
    /// @param rows    The rows, or nothing.
    /// @param columns The visible columns.
    void setCounts(std::optional<std::int64_t> rows, std::size_t columns) override {
        _counts = {rows, columns};
        set("counts", (rows ? std::to_string(*rows) : std::string{"unknown"}) + "/" + std::to_string(columns));
    }

    /// @brief Records property `scrolledToRow`.
    /// @param viewRow The row.
    void scrollToRow(std::size_t viewRow) override { set("scrolledToRow", std::to_string(viewRow)); }

    /// @brief Records property `highlightedRow`, `none` for no mark.
    /// @param viewRow The row, or nothing.
    void setHighlightedRow(std::optional<std::size_t> viewRow) override {
        set("highlightedRow", viewRow ? std::to_string(*viewRow) : std::string{"none"});
    }

    /// @brief Records the cell's property `column`, `none` for a pooled cell.
    /// @param row    One of this table's rows.
    /// @param cell   One of the row's children.
    /// @param column The column, or nothing.
    /// @throws std::logic_error when @p row is not this table's child, or @p cell not the row's.
    void setCellColumn(Widget& row, Widget& cell, std::optional<std::size_t> column) override {
        int const rowId = store().idOf(row);
        int const cellId = store().idOf(cell);
        if (store().at(rowId).parent != id() || store().at(cellId).parent != rowId) {
            throw std::logic_error{"RecordingBackend: setCellColumn of a widget that is not a cell of this table"};
        }
        store().set(cellId, "column", column ? std::to_string(*column) : std::string{"none"});
    }

    /// @brief Records the row's property `viewRow`.
    /// @param row     One of this table's rows.
    /// @param viewRow The row's view index.
    /// @throws std::logic_error when @p row is not this table's child.
    void setRowIndex(Widget& row, std::size_t viewRow) override {
        int const rowId = store().idOf(row);
        if (store().at(rowId).parent != id()) {
            throw std::logic_error{"RecordingBackend: setRowIndex of a widget that is not this table's row"};
        }
        _viewRows[rowId] = viewRow;
        store().set(rowId, "viewRow", std::to_string(viewRow));
    }

    /// @brief The measure the backend was given, `TableMeasure{}` unless `RecordingBackend::setTableMeasure` set one.
    /// @return It.
    [[nodiscard]] TableMeasure tableMeasure() const override { return _measure ? *_measure : TableMeasure{}; }

    /// @brief Shares the backend's measure, which `tableMeasure` answers.
    /// @param measure The measure; null for the default.
    void measureBy(std::shared_ptr<TableMeasure const> measure) { _measure = std::move(measure); }

    /// @brief Calls the handler `setOnHorizontalRange` stored, as a backend's scroll would.
    /// @param scrollX  The offset.
    /// @param viewport The viewport's width.
    void scrollColumns(int scrollX, int viewport) {
        if (_onHorizontal) {
            _onHorizontal(scrollX, viewport);
        }
    }

    /// @brief Calls the handler `setOnSelectRange` stored, as a Shift+click would, or a Ctrl+Shift+click with
    ///        `Extend`.
    /// @param anchor The anchor's view row.
    /// @param focus  The end's view row.
    /// @param change Whether the range replaces the selection or is added to it.
    void selectRange(std::size_t anchor, std::size_t focus, SelectionChange change = SelectionChange::Replace) {
        if (_onSelectRange) {
            _onSelectRange(anchor, focus, change);
        }
    }

    /// @brief Calls the handler `setOnColumnMeasured` stored, as a cell's layout would.
    /// @param column  The column.
    /// @param measure What was measured.
    void measure(std::size_t column, ColumnMeasure const& measure) {
        if (_onMeasured) {
            _onMeasured(column, measure);
        }
    }

    /// @brief Calls the handler `setOnFocusMove` stored, as a key would.
    /// @param move The move.
    void moveFocus(FocusMove move) {
        if (_onFocusMove) {
            _onFocusMove(move);
        }
    }

    /// @brief The column window last set.
    /// @return Its columns; empty before any.
    [[nodiscard]] std::vector<std::size_t> const& windowColumns() const noexcept { return _window; }

    /// @brief The total width last set.
    /// @return It; 0 before any.
    [[nodiscard]] int totalWidth() const noexcept { return _totalWidth; }

    /// @brief How many leading columns of the last window are pinned.
    /// @return The count.
    [[nodiscard]] std::size_t pinnedShown() const noexcept { return _pinnedShown; }

    /// @brief The header cells drawn: one per column of the window once one is set, one per column before.
    /// @return The count.
    [[nodiscard]] std::size_t headerCells() const noexcept { return _windowSet ? _window.size() : _columns.size(); }

    /// @brief The last horizontal offset set.
    /// @return It, or nothing when never set.
    [[nodiscard]] std::optional<int> horizontalOffset() const noexcept { return _horizontalOffset; }

    /// @brief The focused cell last set.
    /// @return Its view row and column; `{0, 0}` before any.
    [[nodiscard]] std::pair<std::size_t, std::size_t> focusCell() const noexcept { return _focusCell; }

    /// @brief Every focused cell set, oldest first.
    /// @return Their view rows and columns.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> const& focusCells() const noexcept {
        return _focusCells;
    }

    /// @brief For every focused cell set, the key of the row the table held at that view row when it was set.
    /// @return The keys, oldest first; none where no row was at that view row.
    [[nodiscard]] std::vector<std::optional<Key>> const& focusKeys() const noexcept { return _focusKeys; }

    /// @brief The counts last set.
    /// @return The rows (or nothing) and the columns.
    [[nodiscard]] std::pair<std::optional<std::int64_t>, std::size_t> lastCounts() const noexcept { return _counts; }

    /// @brief The status line last set.
    /// @return It; the default before any `setStatus`.
    [[nodiscard]] TableStatus const& status() const noexcept { return _status; }

    /// @brief The columns last set.
    /// @return They; empty before any `setColumns`.
    [[nodiscard]] std::vector<TableColumn> const& columns() const noexcept { return _columns; }

    /// @brief A current row's cell's load state, as `setCellLoad` last set it.
    /// @param key    The row's key.
    /// @param column The cell's column.
    /// @return The state; `Loaded` when none was set.
    /// @throws std::logic_error when no current row has @p key.
    [[nodiscard]] CellLoad cellLoad(Key const& key, std::size_t column) const {
        std::optional<int> const row = rowOf(key);
        if (!row) {
            throw std::logic_error{"RecordingBackend: cellLoad of " + formatKey(key) + ", which Table#" +
                                   std::to_string(id()) + " has no row for"};
        }
        auto const found = _loads.find(std::pair{*row, column});
        return found == _loads.end() ? CellLoad::Loaded : found->second;
    }

    /// @brief A current row's cell's accent, as `setCellAccent` last set it.
    /// @param key    The row's key.
    /// @param column The cell's column.
    /// @return The accent; `CellAccent{}` when none was set.
    /// @throws std::logic_error when no current row has @p key.
    [[nodiscard]] CellAccent cellAccent(Key const& key, std::size_t column) const {
        std::optional<int> const row = rowOf(key);
        if (!row) {
            throw std::logic_error{"RecordingBackend: cellAccent of " + formatKey(key) + ", which Table#" +
                                   std::to_string(id()) + " has no row for"};
        }
        auto const found = _accents.find(std::pair{*row, column});
        return found == _accents.end() ? CellAccent{} : found->second;
    }

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

    /// @brief Marks the rows the user selected, as the requested keys; nothing is logged. The last key becomes the
    ///        anchor of a later Shift-click.
    /// @param keys Keys of current rows.
    void selectByUser(std::vector<Key> const& keys) {
        if (!keys.empty()) {
            _anchor = keys.back();
        }
        select(keys);
    }

    /// @brief Marks the rows a user's click or key selected, as the requested keys, leaving the anchor where the
    ///        input left it; nothing is logged.
    /// @param keys Keys of current rows.
    void select(std::vector<Key> const& keys) {
        _viewExcept.reset();
        _requested = keys;
        remark();
    }

    /// @brief What a click on the row with @p key selects, as every backend's selection model has it, and makes
    ///        the clicked row the anchor unless the click was a range.
    /// @param key   A current row's key.
    /// @param click How the click was modified.
    /// @return The keys then selected, in row order for a range, and the kind of change.
    [[nodiscard]] std::pair<std::vector<Key>, SelectionChange> clicked(Key const& key, RowClick click) {
        if (_mode != SelectionMode::Multiple || click == RowClick::Only) {
            _anchor = key;
            return {{key}, SelectionChange::Replace};
        }
        if (click == RowClick::Toggle) {
            _anchor = key;
            std::vector<Key> next = markedKeys();
            if (auto const found = std::ranges::find(next, key); found != next.end()) {
                next.erase(found);
            } else {
                next.push_back(key);
            }
            return {next, SelectionChange::Extend};
        }
        std::vector<Key> const rows = rowKeys();
        Key const from = _anchor.has_value() && rowOf(*_anchor).has_value() ? *_anchor : key;
        auto const first = std::ranges::find(rows, from);
        auto const last = std::ranges::find(rows, key);
        auto const [low, high] = std::minmax(first, last);
        return {std::vector<Key>{low, std::next(high)}, SelectionChange::Replace};
    }

    /// @brief What Ctrl+A selects in a `Multiple` table: every row, or none when every row is selected already.
    /// @return The keys, in row order.
    [[nodiscard]] std::vector<Key> allOrNone() const {
        std::vector<Key> const rows = rowKeys();
        std::vector<Key> const marked = markedKeys();
        bool const every =
            std::ranges::all_of(rows, [&marked](Key const& key) { return std::ranges::contains(marked, key); });
        return every ? std::vector<Key>{} : rows;
    }

    /// @brief The keys of the current data rows, in row order: a heading row is not one.
    /// @return The keys.
    [[nodiscard]] std::vector<Key> rowKeys() const {
        std::vector<Key> keys;
        for (int const child : store().at(id()).children) {
            if (auto const found = _rowKeys.find(child); found != _rowKeys.end() && !_headings.contains(child)) {
                keys.push_back(found->second);
            }
        }
        return keys;
    }

    /// @brief The keys of the rows the table marks: the requested keys, in the order requested, that a current row
    ///        has.
    /// @return The keys.
    [[nodiscard]] std::vector<Key> markedKeys() const {
        std::vector<Key> keys;
        if (_viewExcept) {
            for (Key const& key : rowKeys()) {
                if (!std::ranges::contains(*_viewExcept, key)) {
                    keys.push_back(key);
                }
            }
            return keys;
        }
        for (Key const& key : _requested.value_or(std::vector<Key>{})) {
            if ((rowOf(key).has_value() || _existing.contains(key)) && !headingKey(key)) {
                keys.push_back(key);
            }
        }
        return keys;
    }

private:
    [[nodiscard]] std::string marked() const {
        return formatList(markedKeys(), [](Key const& key) { return formatKey(key); });
    }

    void remark() {
        if (_requested.has_value()) {
            store().setByUser(id(), "selection", marked());
        }
    }

    SelectionMode _mode = SelectionMode::None;
    std::vector<bool> _sortable;  // per column, as setColumns gave them
    std::set<int> _headings;      // the heading rows' ids
    std::map<int, Key> _rowKeys;  // row id -> key, as setRowKey told it
    // Every key in the data, realized or not; empty below a windowed table's threshold.
    std::unordered_set<Key> _existing;
    // Engaged once `setSelection` ran or the user selected.
    std::optional<std::vector<Key>> _requested;
    // Engaged while the selection is of the view: every row but these keys is marked.
    std::optional<std::vector<Key>> _viewExcept;
    // The last marks of each row, by its key.
    std::map<Key, RowMarks> _rowMarks;
    std::map<Key, std::string> _rowTones;
    std::function<void(std::string)> _onStatusEntry;
    // The row a Shift-click's range starts from: the last row clicked without Shift.
    std::optional<Key> _anchor;
    std::function<void(std::size_t, std::size_t)> _onVisibleRangeChanged;
    std::function<void()> _onSelectAll;
    std::vector<TableSort> _chain;
    std::function<std::vector<MenuEntry>(std::optional<std::size_t>)> _menu;
    std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> _onMenu;
    std::function<std::vector<MenuEntry>(Key const&, std::size_t)> _cellMenu;
    std::function<void(Key const&, std::size_t, std::vector<std::size_t> const&)> _cellChosen;
    std::vector<std::string> _cellMenuKeys;
    mutable std::size_t _cellMenuAsks = 0;  // how many times the cell menu's entries were asked for
    std::map<int, std::size_t> _viewRows;   // row id -> view index, as setRowIndex told it
    bool _focusSet = false;                 // whether setFocusCell ran
    std::vector<std::size_t> _visible;
    std::function<void(std::size_t, std::size_t)> _onColumnMove;
    std::function<void(std::size_t, std::optional<int>)> _onColumnResize;
    std::function<void(std::size_t)> _onFilter;
    std::vector<std::size_t> _filtered;
    std::vector<TableColumnGroup> _groups;
    std::vector<BandRun> _bands;
    std::function<void(std::size_t)> _onGroupFilter;
    std::function<void()> _onCopy;
    TableStatus _status;
    std::vector<TableColumn> _columns;
    std::map<std::pair<int, std::size_t>, CellLoad> _loads;  // (row id, column) -> state, as setCellLoad told it
    std::map<std::pair<int, std::size_t>, CellAccent>
        _accents;                      // (row id, column) -> accent, as setCellAccent told it
    std::vector<std::size_t> _window;  // the column window, as setColumnWindow last gave it
    bool _windowSet = false;
    int _totalWidth = 0;
    std::size_t _pinnedShown = 0;
    std::optional<int> _horizontalOffset;
    std::pair<std::size_t, std::size_t> _focusCell;
    std::vector<std::pair<std::size_t, std::size_t>> _focusCells;
    std::vector<std::optional<Key>> _focusKeys;
    std::pair<std::optional<std::int64_t>, std::size_t> _counts;
    std::function<void(int, int)> _onHorizontal;
    std::function<void(std::size_t, std::size_t, SelectionChange)> _onSelectRange;
    std::function<void(std::size_t, ColumnMeasure const&)> _onMeasured;
    std::function<void(FocusMove)> _onFocusMove;
    std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)> _onCellSelect;
    std::function<void(std::string)> _onText;
    std::function<void(std::size_t, std::size_t, bool)> _onCellClick;
    std::shared_ptr<TableMeasure const> _measure;

    [[nodiscard]] static std::string formatCellLoad(CellLoad load) {
        switch (load) {
            case CellLoad::Unloaded:
                return "Unloaded";
            case CellLoad::Failed:
                return "Failed";
            case CellLoad::Loaded:
            default:
                return "Loaded";
        }
    }
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
    /// @brief A fake menu; one of `MenuStyle::Button` records property `style=Button` at creation.
    /// @param parent The container to append to, or null for a root.
    /// @param style  A bar or one button.
    /// @return The widget.
    [[nodiscard]] std::unique_ptr<MenuWidget> createMenu(ContainerWidget* parent, MenuStyle style) override {
        return std::make_unique<detail::FakeMenu>(
            _store, "Menu", parent,
            style == MenuStyle::Button ? detail::PropList{{"style", "Button"}} : detail::PropList{});
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
        return std::make_unique<detail::FakeSlot>(_store, "Slot", parent);
    }
    [[nodiscard]] std::unique_ptr<TabsWidget> createTabs(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeTabs>(_store, "Tabs", parent);
    }
    [[nodiscard]] std::unique_ptr<DialogWidget> createDialog(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeDialog>(_store, "Dialog", parent);
    }
    [[nodiscard]] std::unique_ptr<ToastWidget> createToast(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeToast>(_store, "Toast", parent);
    }
    [[nodiscard]] std::unique_ptr<PopoverWidget> createPopover(ContainerWidget* parent) override {
        return std::make_unique<detail::FakePopover>(_store, "Popover", parent);
    }
    /// @brief A fake spotlight (`detail::FakeSpotlight`), resolving its target against this backend's live widgets.
    /// @param parent The container to append to, or null for a root.
    /// @return The widget.
    [[nodiscard]] std::unique_ptr<SpotlightWidget> createSpotlight(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeSpotlight>(_store, parent);
    }
    [[nodiscard]] std::unique_ptr<BusyWidget> createBusy(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeBusy>(_store, "Busy", parent);
    }
    [[nodiscard]] std::unique_ptr<TableWidget> createTable(ContainerWidget* parent) override {
        auto table = std::make_unique<detail::FakeTable>(_store, "Table", parent);
        table->measureBy(_tableMeasure);
        return table;
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
    [[nodiscard]] std::unique_ptr<BannerWidget> createBanner(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeBanner>(_store, "Banner", parent);
    }
    [[nodiscard]] std::unique_ptr<BadgeWidget> createBadge(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeBadge>(_store, "Badge", parent);
    }
    [[nodiscard]] std::unique_ptr<ProgressWidget> createProgress(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeProgress>(_store, "Progress", parent);
    }
    [[nodiscard]] std::unique_ptr<StepsWidget> createSteps(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeSteps>(_store, "Steps", parent);
    }
    [[nodiscard]] std::unique_ptr<KeyValueWidget> createKeyValue(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeKeyValue>(_store, "KeyValue", parent);
    }
    [[nodiscard]] std::unique_ptr<EmptyStateWidget> createEmptyState(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeEmptyState>(_store, "EmptyState", parent);
    }
    [[nodiscard]] std::unique_ptr<ChartWidget> createChart(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeChart>(_store, "Chart", parent);
    }
    [[nodiscard]] std::unique_ptr<DrawerWidget> createDrawer(ContainerWidget* parent, Side side) override {
        return std::make_unique<detail::FakeDrawer>(_store, "Drawer", parent,
                                                    detail::PropList{{"side", detail::enumName(side)}});
    }
    [[nodiscard]] std::unique_ptr<SplitterWidget> createSplitter(ContainerWidget* parent, Axis axis) override {
        return std::make_unique<detail::FakeSplitter>(_store, "Splitter", parent,
                                                      detail::PropList{{"axis", detail::enumName(axis)}});
    }
    [[nodiscard]] std::unique_ptr<CollapsibleWidget> createCollapsible(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeCollapsible>(_store, "Collapsible", parent);
    }
    /// @brief A fake page, offered the width `setPageAvailableWidth` gave.
    /// @param parent   The container to append to, or null for a root.
    /// @param geometry The sheet.
    /// @return The widget.
    [[nodiscard]] std::unique_ptr<PageWidget> createPage(ContainerWidget* parent,
                                                         PageGeometry const& geometry) override {
        return std::make_unique<detail::FakePage>(_store, parent, geometry, _pageWidth, _pages);
    }

    /// @brief A fake page region.
    /// @param parent   Its page.
    /// @param geometry Its place.
    /// @return The widget.
    [[nodiscard]] std::unique_ptr<PageRegionWidget> createPageRegion(ContainerWidget* parent,
                                                                     RegionGeometry const& geometry) override {
        return std::make_unique<detail::FakePageRegion>(_store, parent, geometry);
    }

    /// @brief Sets the width every page is offered, as a container of that width would, and relays the pages out.
    /// @param width The width; absent is unlimited, the default.
    void setPageAvailableWidth(std::optional<Mm> width) {
        _pageWidth = width;
        for (detail::FakePage* page : std::vector(*_pages)) {
            page->offer(width);
        }
    }

    /// @brief Gives a page region's content height, which a region of no stated height takes as its own.
    /// @param regionId The region.
    /// @param height   The content's height.
    void setRegionContentHeight(int regionId, Mm height) {
        dynamic_cast<detail::FakePageRegion&>(*_store->at(regionId).widget).setContentHeight(height);
    }

    [[nodiscard]] std::unique_ptr<DropZoneWidget> createDropZone(ContainerWidget* parent) override {
        return std::make_unique<detail::FakeDropZone>(_store, "DropZone", parent);
    }

    /// @brief Loads a component `loadsComponents` named, as a `Custom` widget with properties `component` and
    ///        `minVersion`; any other is not loaded, and the mount shows its fallback.
    /// @param parent  The container to append it to; null makes a root.
    /// @param request The component and its props.
    /// @return The widget, or null.
    [[nodiscard]] std::unique_ptr<CustomWidget> createCustom(ContainerWidget* parent,
                                                             CustomRequest const& request) override {
        if (!_components.contains(request.name)) {
            return nullptr;
        }
        return std::make_unique<detail::FakeCustom>(
            _store, "Custom", parent,
            detail::PropList{{"component", detail::formatText(request.name)},
                             {"minVersion", std::to_string(request.minVersion)}});
    }

    /// @brief The width class it reports: `expanded`, the class of a surface with no width, until `setWidthClass`.
    /// @return The class.
    [[nodiscard]] std::string widthClass() const override { return _widthClass; }

    /// @brief Registers the listener `setWidthClass` tells of a change; it is not called at once.
    /// @param onWidthClass The listener; empty clears it.
    void setOnWidthClass(std::function<void(std::string const&)> onWidthClass) override {
        _onWidthClass = std::move(onWidthClass);
    }

    /// @brief Changes the width class it reports, as a resize would, and tells the listener when the class changed.
    /// @param name The class: `compact`, `medium` or `expanded`.
    void setWidthClass(std::string name) {
        if (name == _widthClass) {
            return;
        }
        _widthClass = std::move(name);
        if (_onWidthClass) {
            // A copy: the listener may replace itself.
            auto const listener = _onWidthClass;
            listener(_widthClass);
        }
    }

    /// @brief Names the components `createCustom` loads; none until called, so a custom node shows its fallback.
    /// @param names The components.
    void loadsComponents(std::vector<std::string> names) {
        _components.clear();
        for (std::string& name : names) {
            _components.insert(std::move(name));
        }
    }

    /// @brief Raises an event of a loaded component, as the component would.
    /// @param widgetId The component.
    /// @param event    The event's name.
    /// @param payload  Its payload, as the backend reports it (`i:<int64>`, `s:<text>`, or the component's text).
    /// @throws std::logic_error when the widget is not a loaded component.
    void raiseEvent(int widgetId, std::string event, std::string payload) {
        detail::Record const& record = expectKind(widgetId, "raiseEvent", {"Custom"});
        if (reachable(record)) {
            invoke(record.callbacks.component, std::move(event), std::move(payload));
        }
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

    /// @brief Presses a keyboard chord with focus on a widget, in the key order every backend follows.
    ///
    /// First a chord @p widgetId declares itself through `setKeys`. Then, inside an open spotlight and except for
    /// Enter and Space, the chords of its ancestors up to the spotlight. Then the built-in keys of its kind: a
    /// `Button`'s `Enter` or `Space` clicks it; a single-line or password `TextInput`'s `Enter` commits, then submits
    /// and stops when the field has a submit handler, and goes on after the commit when it has none; a `Checkbox`'s
    /// `Space` toggles it; a segmented `Select`'s `Left` and `Right` choose the previous or the next option, stopping
    /// at the ends; a `Table`'s `Enter` activates its first marked row, and stops whether or not one is marked; a
    /// `Multiple` table's `Ctrl+A` calls its select-all handler when it has one (`setOnSelectAll`), and otherwise
    /// selects every row, or clears them when every row is selected, as `Extend`. Then
    /// the chords of its ancestors, innermost first. A handler that destroys the focused widget stops the key there.
    /// Like every helper it acts only when a user could reach @p widgetId.
    /// @param widgetId The widget focus is on.
    /// @param chord The chord, as declared, such as `"Ctrl+S"`, `"Enter"` or `"Space"`.
    /// @return True when any step took the chord.
    bool press(int widgetId, std::string const& chord) {
        detail::Record const& focused = _store->at(widgetId);
        if (!reachable(focused)) {
            return false;
        }
        int const parent = focused.parent;
        if (std::ranges::find(focused.chords, chord) != focused.chords.end()) {
            invoke(focused.callbacks.chord, chord);
            return true;
        }
        // Inside an open spotlight the card's chords come before the focused widget's own keys, but Enter and Space
        // stay the focused widget's.
        if (chord != "Enter" && chord != "Space") {
            if (int const spotlight = openSpotlightAround(widgetId); spotlight != 0) {
                for (int candidate = parent; candidate != 0;) {
                    detail::Record const& record = _store->at(candidate);
                    if (std::ranges::find(record.chords, chord) != record.chords.end()) {
                        invoke(record.callbacks.chord, chord);
                        return true;
                    }
                    if (candidate == spotlight) {
                        break;
                    }
                    candidate = record.parent;
                }
            }
        }
        if (ownKey(widgetId, chord) || !exists(widgetId)) {
            return true;
        }
        for (int candidate = parent; candidate != 0;) {
            detail::Record const& record = _store->at(candidate);
            if (std::ranges::find(record.chords, chord) != record.chords.end()) {
                invoke(record.callbacks.chord, chord);
                return true;
            }
            candidate = record.parent;
        }
        return false;
    }

    /// @brief Rests the pointer over a widget: calls its `setOnHover` handler, if it has one and can be reached.
    /// @param widgetId The widget.
    void hover(int widgetId) {
        detail::Record const& record = _store->at(widgetId);
        if (reachable(record)) {
            invoke(record.callbacks.hover);
        }
    }

    /// @brief Activates a button, or the action button of a banner or an empty state. A widget inside a list entry's
    ///        content activates the entry: its content takes no input of its own.
    /// @param widgetId The button, banner or empty state, or a widget inside an entry's content.
    /// @throws std::logic_error when the widget is none of those, or is a banner or empty state that shows no action
    ///         button.
    void click(int widgetId) {
        if (std::string const kind = kindOf(widgetId); kind != "Button" && kind != "Banner" && kind != "EmptyState") {
            for (int outer = _store->at(widgetId).parent; outer != 0; outer = _store->at(outer).parent) {
                if (_store->at(outer).kind == "Button") {
                    widgetId = outer;
                    break;
                }
            }
        }
        detail::Record const& record = expectKind(widgetId, "click", {"Button", "Banner", "EmptyState"});
        if (record.kind != "Button" && prop(widgetId, "actionLabel").empty()) {
            throw std::logic_error{"RecordingBackend: click of " + record.kind + "#" + std::to_string(widgetId) +
                                   ", which shows no action button"};
        }
        if (reachable(record)) {
            invoke(record.kind == "Button" ? record.callbacks.click : record.callbacks.action);
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

    /// @brief Collapses or expands a collapsible panel as a click on its header does: it flips its own `collapsed`,
    ///        then `onToggle` gets the new collapsed state.
    /// @param widgetId The panel.
    /// @throws std::logic_error when the widget is not a panel.
    void togglePanel(int widgetId) {
        if (detail::Record const* const record = editable(widgetId, "togglePanel", {"Panel"}); record != nullptr) {
            bool const collapsed = prop(widgetId, "collapsed") != "true";
            _store->setByUser(widgetId, "collapsed", detail::formatBool(collapsed));
            invoke(record->callbacks.toggle, collapsed);
        }
    }

    /// @brief Opens a dropdown select, as a user would, whether or not it has options: `onOpen` runs. Nothing is
    ///        marked or logged.
    /// @param widgetId The select.
    /// @throws std::logic_error when the widget is not a select.
    void open(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "open", {"Select"});
        if (reachable(record) && !flagIs(record, "readonly", true)) {
            invoke(record.callbacks.open);
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
    /// Nothing happens for a path that names no entry, a separator, an entry with a submenu (it opens rather than
    /// runs), or an entry that is disabled or inside a disabled entry.
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
            if (level == nullptr || index >= level->size() || !level->at(index).enabled ||
                level->at(index).separator) {
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

    /// @brief Dismisses an open dialog or drawer, as Esc does on the TUI: it closes, then `onDismiss` runs. Or
    ///        dismisses a dismissible banner, which stays shown: the application hides it. Or removes a removable
    ///        badge, as its remove control does: `onRemove` runs, and the application takes it away.
    /// @param widgetId The dialog, drawer, banner or badge.
    /// @throws std::logic_error when the widget is none of those, is a banner that is not dismissible, or is a badge
    ///         that is not removable.
    void dismiss(int widgetId) {
        detail::Record const& record =
            expectKind(widgetId, "dismiss", {"Dialog", "Drawer", "Banner", "Badge", "Popover"});
        if (record.kind == "Banner" && !flagIs(record, "dismissible", true)) {
            throw std::logic_error{"RecordingBackend: dismiss of Banner#" + std::to_string(widgetId) +
                                   ", which is not dismissible"};
        }
        if (record.kind == "Badge" && !record.props.contains("removable")) {
            throw std::logic_error{"RecordingBackend: dismiss of Badge#" + std::to_string(widgetId) +
                                   ", which is not removable"};
        }
        if (reachable(record)) {
            if (record.kind != "Banner" && record.kind != "Badge") {
                _store->setByUser(widgetId, "open", detail::formatBool(false));
            }
            invoke(record.callbacks.dismiss);
        }
    }

    /// @brief Opens or closes a collapsible section with its own control, which works while it is closed: it shows
    ///        the state asked for, then `onToggle` gets it.
    /// @param widgetId The section.
    /// @param open The requested state.
    /// @throws std::logic_error when the widget is not a collapsible section.
    void expand(int widgetId, bool open) {
        if (detail::Record const* const record = actionable(widgetId, "expand", {"Collapsible"}); record != nullptr) {
            _store->setByUser(widgetId, "open", detail::formatBool(open));
            invoke(record->callbacks.toggle, open);
        }
    }

    /// @brief Drags a splitter's handles: it shows the sizes, then `onResize` gets them.
    /// @param widgetId The splitter.
    /// @param sizes Every pane's size, in order.
    /// @throws std::logic_error when the widget is not a splitter, or @p sizes does not name one size per pane.
    void resize(int widgetId, std::vector<int> sizes) {
        detail::Record const& record = expectKind(widgetId, "resize", {"Splitter"});
        if (sizes.size() != record.children.size()) {
            throw std::logic_error{"RecordingBackend: resize of Splitter#" + std::to_string(widgetId) + " with " +
                                   std::to_string(sizes.size()) + " sizes for " +
                                   std::to_string(record.children.size()) + " panes"};
        }
        if (reachable(record)) {
            _store->setByUser(widgetId, "sizes",
                              detail::formatList(sizes, [](int size) { return std::to_string(size); }));
            invoke(record.callbacks.resize, std::move(sizes));
        }
    }

    /// @brief Drops files from the operating system onto a drop zone.
    /// @param widgetId The drop zone.
    /// @param paths The files' paths, in the order dropped.
    /// @return True when the zone took the drop and `onDrop` ran; false when it is unreachable or refused the drop.
    /// @throws std::logic_error when the widget is not a drop zone, or @p paths is empty.
    bool dropFiles(int widgetId, std::vector<std::string> paths) {
        detail::Record const& record = expectKind(widgetId, "dropFiles", {"DropZone"});
        if (paths.empty()) {
            throw std::logic_error{"RecordingBackend: dropFiles of no files"};
        }
        auto const& zone = dynamic_cast<detail::FakeDropZone const&>(*record.widget);
        if (!reachable(record) || !zone.takes(paths)) {
            return false;
        }
        invoke(record.callbacks.files, std::move(paths));
        return true;
    }

    /// @brief Selects rows of a table by key: the table marks them, then `onSelectionChange` gets the keys.
    /// @param widgetId The table.
    /// @param keys The keys of every row the user now selects, in the order selected.
    /// @throws std::logic_error when the widget is not a table, or the selection is not one a user could make: a key
    ///         that names no current data row (a heading is not one), a row twice, any row in a `None` table, or more
    ///         than one in a `Single` one.
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
            if (!table->rowOf(key).has_value() || table->headingKey(key) || !seen.insert(key).second) {
                throw std::logic_error{"RecordingBackend: selectRows of " + detail::formatKey(key) + where +
                                       ", which has no such row or selects it twice"};
            }
        }
        if (reachable(record)) {
            table->selectByUser(keys);
            invoke(record.callbacks.selection, std::move(keys), SelectionChange::Replace);
        }
    }

    /// @brief Clicks a row of a table as the user would with the click's modifiers: the table marks what the click
    ///        selects (`TableWidget`'s selection model), then `onSelectionChange` gets the keys and the kind of
    ///        change, `Replace` for a plain click or a range and `Extend` for a toggle. A click on a heading row
    ///        does nothing, as a user's does.
    /// @param widgetId The table.
    /// @param key The row's key.
    /// @param click How the click is modified.
    /// @throws std::logic_error when the widget is not a table, it selects nothing, or no current row has @p key.
    void clickRow(int widgetId, Key const& key, RowClick click) {
        detail::Record const& record = expectKind(widgetId, "clickRow", {"Table"});
        auto* const table = dynamic_cast<detail::FakeTable*>(record.widget);
        std::string const where = " of Table#" + std::to_string(widgetId);
        if (table == nullptr || table->mode() == SelectionMode::None) {
            throw std::logic_error{"RecordingBackend: clickRow" + where + ", which selects nothing"};
        }
        std::optional<int> const rowId = table->rowOf(key);
        if (!rowId.has_value()) {
            throw std::logic_error{"RecordingBackend: clickRow of " + detail::formatKey(key) + where +
                                   ", which has no such row"};
        }
        // A click on a heading row selects nothing, as a user's does.
        if (!reachable(record) || !reachable(_store->at(*rowId)) || table->headingKey(key)) {
            return;
        }
        auto [keys, change] = table->clicked(key, click);
        table->select(keys);
        invoke(record.callbacks.selection, std::move(keys), change);
    }

    /// @brief A plain click on a row: `clickRow` with `RowClick::Only`.
    /// @param widgetId The table.
    /// @param key      The row's key.
    void clickRow(int widgetId, Key const& key) { clickRow(widgetId, key, RowClick::Only); }

    /// @brief A `Ctrl`+click on a row: `clickRow` with `RowClick::Toggle`.
    /// @param widgetId The table.
    /// @param key      The row's key.
    void ctrlClickRow(int widgetId, Key const& key) { clickRow(widgetId, key, RowClick::Toggle); }

    /// @brief The user's select-all on a `Multiple` table (`Ctrl+A`, the select-all box): the handler
    ///        `setOnSelectAll` stored.
    /// @param widgetId The table.
    /// @throws std::logic_error when the widget is not a table or has no select-all handler.
    void selectAll(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "selectAll", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable*>(record.widget);
        if (table == nullptr || !table->onSelectAll()) {
            throw std::logic_error{"RecordingBackend: selectAll of Table#" + std::to_string(widgetId) +
                                   ", which has no select-all handler"};
        }
        if (reachable(record)) {
            std::function<void()> const selectAll = table->onSelectAll();
            selectAll();
        }
    }

    /// @brief Chooses an entry the status line shows: the handler `setOnStatusEntry` stored, with its id.
    /// @param widgetId The table.
    /// @param entry    The entry's id.
    /// @throws std::logic_error when the widget is not a table, has no handler, or its status shows no such entry.
    void chooseStatusEntry(int widgetId, std::string const& entry) {
        detail::Record const& record = expectKind(widgetId, "chooseStatusEntry", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable*>(record.widget);
        bool const shown =
            table != nullptr && (std::ranges::contains(table->status().entries, entry, &StatusEntry::id) ||
                                 std::ranges::contains(table->status().standingEntries, entry, &StatusEntry::id));
        if (!shown || !table->onStatusEntry()) {
            throw std::logic_error{"RecordingBackend: chooseStatusEntry of " + entry + " on Table#" +
                                   std::to_string(widgetId) + ", whose status line shows no such entry"};
        }
        if (reachable(record)) {
            std::function<void(std::string)> const choose = table->onStatusEntry();
            choose(entry);
        }
    }

    /// @brief Clicks a column's header as the user would: a sortable column's reaches `onSort` with its index and
    ///        @p click; any other header does nothing, since a user can click it.
    /// @param widgetId The table.
    /// @param column The column's index among the columns `setColumns` gave.
    /// @param click  Whether `Shift` is held.
    /// @throws std::logic_error when the widget is not a table, or the column's header is not drawn.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void clickHeader(int widgetId, std::size_t column, HeaderClick click = HeaderClick::Plain) {
        detail::Record const& record = expectKind(widgetId, "clickHeader", {"Table"});
        auto const& table = shownHeader(record, widgetId, column, "clickHeader");
        if (table.sortable(column) && reachable(record)) {
            invoke(record.callbacks.sort, column, click);
        }
    }

    /// @brief The entries of a header's menu, or of the table's menu, as a user opening it would see them.
    /// @param widgetId The table.
    /// @param column   The column, or none for the table's menu.
    /// @return The entries; none when the table has no menus or a user cannot reach it.
    /// @throws std::logic_error when the widget is not a table, or the column's header is not drawn.
    [[nodiscard]] std::vector<MenuEntry> menuOf(int widgetId, std::optional<std::size_t> column) {
        detail::Record const& record = expectKind(widgetId, "menuOf", {"Table"});
        detail::FakeTable const& table = column ? shownHeader(record, widgetId, *column, "menuOf") : fakeTable(record);
        return reachable(record) ? table.menuEntries(column) : std::vector<MenuEntry>{};
    }

    /// @brief Chooses an entry of a header's menu, or of the table's menu, by its labels from the top, as a user
    ///        opening each submenu on the way would. Nothing is reported for a disabled entry, an entry inside a
    ///        disabled one, or an entry with a submenu.
    /// @param widgetId The table.
    /// @param column   The column, or none for the table's menu.
    /// @param labels   The entry's label at each level, from the top; not empty.
    /// @throws std::logic_error when the widget is not a table, the column's header is not drawn, or a label names
    ///         no entry of its level.
    void chooseMenu(int widgetId, std::optional<std::size_t> column, std::vector<std::string_view> const& labels) {
        std::vector<MenuEntry> const entries = menuOf(widgetId, column);
        detail::Record const& record = _store->at(widgetId);
        std::vector<MenuEntry> const* level = &entries;
        std::vector<std::size_t> path;
        bool enabled = true;
        MenuEntry const* entry = nullptr;
        for (std::string_view const label : labels) {
            auto const found = std::ranges::find_if(*level, [label](MenuEntry const& candidate) {
                return !candidate.separator && !candidate.heading && candidate.label == label;
            });
            if (found == level->end()) {
                throw std::logic_error{"RecordingBackend: chooseMenu of '" + std::string{label} + "', which Table#" +
                                       std::to_string(widgetId) + "'s menu does not have"};
            }
            path.push_back(static_cast<std::size_t>(std::distance(level->begin(), found)));
            enabled = enabled && found->enabled;
            entry = &*found;
            level = &found->items;
        }
        if (entry != nullptr && enabled && entry->items.empty() && reachable(record)) {
            invoke(fakeTable(record).onMenu(), column, std::move(path));
        }
    }

    /// @brief Opens a cell's menu as a right click on the cell would: in a table that selects, a row that is not
    ///        selected becomes the selection alone, reported as `Replace`; then the entries are asked for, which
    ///        focuses the cell.
    /// @param widgetId The table.
    /// @param key      The cell's row.
    /// @param column   The cell's column: an index into the table's columns.
    /// @return The entries; none when the table has no cell menu or a user cannot reach it.
    /// @throws std::logic_error when the widget is not a table, or no current row has @p key.
    [[nodiscard]] std::vector<MenuEntry> rightClickCell(int widgetId, Key const& key, std::size_t column) {
        detail::Record const& record = expectKind(widgetId, "rightClickCell", {"Table"});
        auto* const table = dynamic_cast<detail::FakeTable*>(record.widget);
        std::optional<int> const rowId = table == nullptr ? std::nullopt : table->rowOf(key);
        if (!rowId.has_value()) {
            throw std::logic_error{"RecordingBackend: rightClickCell of " + detail::formatKey(key) + " of Table#" +
                                   std::to_string(widgetId) + ", which has no such row"};
        }
        if (!reachable(record) || !reachable(_store->at(*rowId))) {
            return {};
        }
        if (table->mode() != SelectionMode::None && !table->headingKey(key) &&
            !std::ranges::contains(table->markedKeys(), key)) {
            table->selectByUser({key});
            invoke(record.callbacks.selection, std::vector<Key>{key}, SelectionChange::Replace);
        }
        return exists(widgetId) ? table->cellMenuEntries(key, column) : std::vector<MenuEntry>{};
    }

    /// @brief How many times a table's cell menu entries were asked for: by an opened menu or by a key.
    /// @param widgetId The table.
    /// @return The count.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::size_t cellMenuAsks(int widgetId) { return fakeTable(widgetId, "cellMenuAsks").cellMenuAsks(); }

    /// @brief Chooses an entry of a cell's menu by its labels from the top, as a user opening it on the cell and each
    ///        submenu on the way would. Nothing is reported for a disabled entry, an entry inside a disabled one, or
    ///        an entry with a submenu.
    /// @param widgetId The table.
    /// @param key      The cell's row.
    /// @param column   The cell's column.
    /// @param labels   The entry's label at each level, from the top; not empty.
    /// @throws std::logic_error when the widget is not a table, or a label names no entry of its level.
    void chooseCellMenu(int widgetId, Key const& key, std::size_t column,
                        std::vector<std::string_view> const& labels) {
        detail::Record const& record = expectKind(widgetId, "chooseCellMenu", {"Table"});
        std::vector<MenuEntry> const entries = fakeTable(record).cellMenuEntries(key, column);
        std::vector<MenuEntry> const* level = &entries;
        std::vector<std::size_t> path;
        bool enabled = true;
        MenuEntry const* entry = nullptr;
        for (std::string_view const label : labels) {
            auto const found = std::ranges::find_if(*level, [label](MenuEntry const& candidate) {
                return !candidate.separator && !candidate.heading && candidate.label == label;
            });
            if (found == level->end()) {
                throw std::logic_error{"RecordingBackend: chooseCellMenu of '" + std::string{label} +
                                       "', which Table#" + std::to_string(widgetId) + "'s cell menu does not have"};
            }
            path.push_back(static_cast<std::size_t>(std::distance(level->begin(), found)));
            enabled = enabled && found->enabled;
            entry = &*found;
            level = &found->items;
        }
        if (entry != nullptr && enabled && entry->items.empty() && reachable(record)) {
            invoke(fakeTable(record).cellChosen(), key, column, path);
        }
    }

    /// @brief Moves a column as a user dragging its header would: `onColumnMove` gets the column and @p position,
    ///        unless the column is at that place already or the place is past the last visible column.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @param position Its new place among the visible columns.
    /// @throws std::logic_error when the widget is not a table, or the column's header is not drawn.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void moveColumn(int widgetId, std::size_t column, std::size_t position) {
        detail::Record const& record = expectKind(widgetId, "moveColumn", {"Table"});
        detail::FakeTable const& table = shownHeader(record, widgetId, column, "moveColumn");
        if (reachable(record) && table.placeOf(column) != position && position < table.visibleCount()) {
            invoke(table.onColumnMove(), column, position);
        }
    }

    /// @brief Resizes a column as a user dragging its header's trailing edge would, or double-clicking it.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @param units    The width in `Sizing` units, or none for the double click.
    /// @throws std::logic_error when the widget is not a table, or the column's header is not drawn.
    void resizeColumn(int widgetId, std::size_t column, std::optional<int> units) {
        detail::Record const& record = expectKind(widgetId, "resizeColumn", {"Table"});
        detail::FakeTable const& table = shownHeader(record, widgetId, column, "resizeColumn");
        if (reachable(record)) {
            invoke(table.onColumnResize(), column, units);
        }
    }

    /// @brief Presses a filterable column's filter button; another column's header has none, so nothing happens.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @throws std::logic_error when the widget is not a table, or the column's header is not drawn.
    void clickFilter(int widgetId, std::size_t column) {
        detail::Record const& record = expectKind(widgetId, "clickFilter", {"Table"});
        detail::FakeTable const& table = shownHeader(record, widgetId, column, "clickFilter");
        if (reachable(record) && table.columns().at(column).filterable) {
            invoke(table.onFilter(), column);
        }
    }

    /// @brief Presses a column group's filter button in the group header row.
    /// @param widgetId The table.
    /// @param group    The group's index among those `setColumnGroups` gave.
    /// @throws std::logic_error when the widget is not a table, or it draws no such group.
    void clickGroupFilter(int widgetId, std::size_t group) {
        detail::Record const& record = expectKind(widgetId, "clickGroupFilter", {"Table"});
        detail::FakeTable const& table = fakeTable(record);
        if (group >= table.groupCount()) {
            throw std::logic_error{"RecordingBackend: clickGroupFilter of group " + std::to_string(group) +
                                   ", which Table#" + std::to_string(widgetId) + " does not draw"};
        }
        if (reachable(record)) {
            invoke(table.onGroupFilter(), group);
        }
    }

    /// @brief Asks to copy as `Ctrl+C` on the focused table does.
    /// @param widgetId The table.
    /// @throws std::logic_error when the widget is not a table.
    void pressCopy(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "pressCopy", {"Table"});
        if (reachable(record)) {
            invoke(fakeTable(record).onCopy());
        }
    }

    /// @brief The text a column's header draws: its label, then its sort key's mark.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @return The text.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::string headerText(int widgetId, std::size_t column) {
        return fakeTable(expectKind(widgetId, "headerText", {"Table"})).headerText(column);
    }

    /// @brief Whether a column's header is drawn: the column is one of the table's and, once the visible columns were
    ///        told, one of them.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @return True when its header is drawn.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] bool headerShown(int widgetId, std::size_t column) {
        return fakeTable(expectKind(widgetId, "headerShown", {"Table"})).headerShown(column);
    }

    /// @brief A column header's accessible description: its sort key's, or empty.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @return The description.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::string headerDescription(int widgetId, std::size_t column) {
        return fakeTable(expectKind(widgetId, "headerDescription", {"Table"})).headerDescription(column);
    }

    /// @brief A column header's accessible name: its label, and `, filtered` while it has a filter.
    /// @param widgetId The table.
    /// @param column   The column.
    /// @return The name.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::string headerName(int widgetId, std::size_t column) {
        return fakeTable(expectKind(widgetId, "headerName", {"Table"})).headerName(column);
    }

    /// @brief Records the text as the clipboard's, which `clipboardText` reads, unless `setClipboard(false)` took
    ///        the clipboard away.
    /// @param text The text.
    /// @return Whether the backend has a clipboard: true unless `setClipboard(false)`.
    bool copyText(std::string_view text) override {
        if (!_hasClipboard) {
            return false;
        }
        _clipboard = std::string{text};
        return true;
    }

    /// @brief The clipboard's text, as `copyText` last recorded it.
    /// @return The text; nothing while `setClipboard(false)` took the clipboard away.
    [[nodiscard]] std::optional<std::string> pasteText() override {
        return _hasClipboard ? std::optional{_clipboard} : std::nullopt;
    }

    /// @brief Gives the backend a clipboard, as it has from the start, or takes it away, as a platform without
    ///        one would: `copyText` then writes nothing and returns false.
    /// @param available Whether there is a clipboard.
    void setClipboard(bool available) { _hasClipboard = available; }

    /// @brief The last text `copyText` was given.
    /// @return It; empty before any.
    [[nodiscard]] std::string clipboardText() const { return _clipboard; }

    /// @brief Chooses "Retry" on a table's status line.
    /// @param widgetId The table.
    /// @throws std::logic_error when the widget is not a table.
    void retryTable(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "retryTable", {"Table"});
        if (reachable(record)) {
            invoke(record.callbacks.retry);
        }
    }

    /// @brief The last marks the mount set on a table's row.
    /// @param widgetId The table.
    /// @param key      The row's key.
    /// @return The marks; none before any `setRowMarks` of that row.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::optional<RowMarks> rowMarks(int widgetId, Key const& key) {
        detail::Record const& record = expectKind(widgetId, "rowMarks", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        return table == nullptr ? std::nullopt : table->rowMarks(key);
    }

    /// @brief The last tone the mount set on a table's row, by the row's key.
    /// @param widgetId The table.
    /// @param key      The row's key.
    /// @return The tone's name, `none` for no tone; empty before any `setRowTone` of that row.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::string rowTone(int widgetId, Key const& key) {
        detail::Record const& record = expectKind(widgetId, "rowTone", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        return table == nullptr ? std::string{} : table->rowTone(key);
    }

    /// @brief A table's status line, as the mount last set it.
    /// @param widgetId The table.
    /// @return The status; the default before any `setStatus`.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] TableStatus tableStatus(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "tableStatus", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        return table == nullptr ? TableStatus{} : table->status();
    }

    /// @brief A table's columns, as the mount last set them.
    /// @param widgetId The table.
    /// @return The columns; empty before any `setColumns`.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::vector<TableColumn> tableColumns(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "tableColumns", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        return table == nullptr ? std::vector<TableColumn>{} : table->columns();
    }

    /// @brief Scrolls a table sideways as the user would: the handler `setOnHorizontalRange` stored gets the offset
    ///        and the viewport's width.
    /// @param widgetId The table.
    /// @param scrollX  The offset.
    /// @param viewport The viewport's width.
    /// @throws std::logic_error when the widget is not a table.
    void scrollTableColumns(int widgetId, int scrollX, int viewport) {
        fakeTable(widgetId, "scrollTableColumns").scrollColumns(scrollX, viewport);
    }

    /// @brief Moves a table's focused cell as a key would.
    /// @param widgetId The table.
    /// @param move     The move.
    /// @throws std::logic_error when the widget is not a table.
    void focusMove(int widgetId, FocusMove move) { fakeTable(widgetId, "focusMove").moveFocus(move); }

    /// @brief Types printable text on a table's focused cell, as a key with no editor open does: calls the handler
    ///        `TableWidget::setOnText` stored, if any.
    /// @param widgetId The table.
    /// @param text     The text.
    /// @throws std::logic_error when the widget is not a table.
    void typeInto(int widgetId, std::string text) {
        detail::FakeTable const& table = fakeTable(widgetId, "typeInto");
        if (reachable(_store->at(widgetId)) && table.onText()) {
            std::function<void(std::string)> const handler = table.onText();
            handler(std::move(text));
        }
    }

    /// @brief Clicks a table's cell: calls the handler `TableWidget::setOnCellClick` stored with `false`, and for a
    ///        double click then again with `true`, as a backend does.
    /// @param widgetId    The table.
    /// @param viewRow     The cell's view row.
    /// @param column      The cell's column: an index into the table's columns.
    /// @param doubleClick Whether the click is a double click.
    /// @throws std::logic_error when the widget is not a table.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a row and a column, as every cell driver takes them.
    void clickCell(int widgetId, std::size_t viewRow, std::size_t column, bool doubleClick = false) {
        detail::FakeTable const& table = fakeTable(widgetId, "clickCell");
        if (!reachable(_store->at(widgetId)) || !table.onCellClick()) {
            return;
        }
        std::function<void(std::size_t, std::size_t, bool)> const handler = table.onCellClick();
        handler(viewRow, column, false);
        if (doubleClick) {
            handler(viewRow, column, true);
        }
    }

    /// @brief Selects a rectangle of cells as a drag from one cell to another does: calls the handler
    ///        `TableWidget::setOnCellSelect` stored, if any.
    /// @param tableId        The table.
    /// @param anchorRow      The anchor's view row.
    /// @param anchorPosition The anchor's place among the visible columns.
    /// @param focusRow       The focus's view row.
    /// @param focusPosition  The focus's place among the visible columns.
    /// @throws std::logic_error when the widget is not a table.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): two corners, each a row and a position.
    void selectCells(int tableId, std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow,
                     std::size_t focusPosition) {
        detail::FakeTable& table = fakeTable(tableId, "selectCells");
        if (reachable(_store->at(tableId))) {
            table.selectCells(anchorRow, anchorPosition, focusRow, focusPosition);
        }
    }

    /// @brief Moves focus out of a table and everything inside it: calls the handler `TableWidget::setOnFocusOut`
    ///        stored, if any.
    /// @param widgetId The table.
    /// @throws std::logic_error when the widget is not a table.
    void focusOut(int widgetId) {
        detail::Record const& record = expectKind(widgetId, "focusOut", {"Table"});
        invoke(record.callbacks.focusOut);
    }

    /// @brief Selects a range of view rows as a Shift+click over rows the backend holds no keys for would: the
    ///        handler `setOnSelectRange` stored gets the two rows and @p change, `Replace` unless the range is added
    ///        to the selection as a Ctrl+Shift+click adds it.
    /// @param widgetId The table.
    /// @param anchor   The anchor's view row.
    /// @param focus    The end's view row.
    /// @param change   Whether the range replaces the selection or is added to it.
    /// @throws std::logic_error when the widget is not a table.
    void selectTableRange(int widgetId, std::size_t anchor, std::size_t focus,
                          SelectionChange change = SelectionChange::Replace) {
        fakeTable(widgetId, "selectTableRange").selectRange(anchor, focus, change);
    }

    /// @brief Reports the width a cell of a column measured, as a backend's layout would.
    /// @param widgetId The table.
    /// @param column   The column: an index into the table's columns.
    /// @param width    The width.
    /// @throws std::logic_error when the widget is not a table.
    void measureColumn(int widgetId, std::size_t column, int width) {
        fakeTable(widgetId, "measureColumn").measure(column, ColumnMeasure{.natural = width, .least = width});
    }

    /// @brief Reports what a column's header and cells measured, as a backend's layout would.
    /// @param widgetId The table.
    /// @param column   The column: an index into the table's columns.
    /// @param measure  What was measured.
    /// @throws std::logic_error when the widget is not a table.
    void measureColumn(int widgetId, std::size_t column, ColumnMeasure const& measure) {
        fakeTable(widgetId, "measureColumn").measure(column, measure);
    }

    /// @brief A table's column window, as the mount last set it.
    /// @param widgetId The table.
    /// @return Its columns, pinned first; empty before any.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::vector<std::size_t> windowColumns(int widgetId) {
        return fakeTable(widgetId, "windowColumns").windowColumns();
    }

    /// @brief A table's total width, as the mount last set it.
    /// @param widgetId The table.
    /// @return The width; 0 before any.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] int totalWidth(int widgetId) { return fakeTable(widgetId, "totalWidth").totalWidth(); }

    /// @brief How many leading columns of a table's last column window the mount marked pinned.
    /// @param widgetId The table.
    /// @return The count.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::size_t pinnedShown(int widgetId) { return fakeTable(widgetId, "pinnedShown").pinnedShown(); }

    /// @brief The header cells a table draws: the column window's once one is set, every column's before.
    /// @param widgetId The table.
    /// @return The count.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::size_t headerCells(int widgetId) { return fakeTable(widgetId, "headerCells").headerCells(); }

    /// @brief Sets the measure every table of this backend answers `TableWidget::tableMeasure` with, before a mount.
    /// @param measure The measure.
    void setTableMeasure(TableMeasure const& measure) { *_tableMeasure = measure; }

    /// @brief A table's horizontal offset, as the mount last moved it.
    /// @param widgetId The table.
    /// @return The offset, or nothing when the mount never moved it.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::optional<int> horizontalOffset(int widgetId) {
        return fakeTable(widgetId, "horizontalOffset").horizontalOffset();
    }

    /// @brief A table's focused cell, as the mount last set it.
    /// @param widgetId The table.
    /// @return Its view row and column.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::pair<std::size_t, std::size_t> focusCell(int widgetId) {
        return fakeTable(widgetId, "focusCell").focusCell();
    }

    /// @brief Every focused cell the mount set on a table, oldest first.
    /// @param widgetId The table.
    /// @return Their view rows and columns.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> focusCells(int widgetId) {
        return fakeTable(widgetId, "focusCells").focusCells();
    }

    /// @brief For every focused cell the mount set on a table, the key of the row at that view row when it was set.
    /// @param widgetId The table.
    /// @return The keys, oldest first; none where no row was at that view row.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::vector<std::optional<Key>> focusKeys(int widgetId) {
        return fakeTable(widgetId, "focusKeys").focusKeys();
    }

    /// @brief A table's counts, as the mount last set them.
    /// @param widgetId The table.
    /// @return The rows (nothing when unknown) and the visible columns.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::pair<std::optional<std::int64_t>, std::size_t> lastCounts(int widgetId) {
        return fakeTable(widgetId, "lastCounts").lastCounts();
    }

    /// @brief A cell's load state in a table whose rows come from a `TableSource`.
    /// @param widgetId The table.
    /// @param rowKey   The row's key.
    /// @param column   The cell's column: an index into the table's columns.
    /// @return The state `setCellLoad` last gave the cell; `Loaded` when none.
    /// @throws std::logic_error when the widget is not a table, or none of its current rows has @p rowKey.
    [[nodiscard]] CellLoad cellLoad(int widgetId, Key const& rowKey, std::size_t column) {
        detail::Record const& record = expectKind(widgetId, "cellLoad", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        if (table == nullptr) {
            throw std::logic_error{"RecordingBackend: cellLoad of a widget that is not a table"};
        }
        return table->cellLoad(rowKey, column);
    }

    /// @brief A cell's accent in a table whose rows come from a `TableSource`.
    /// @param widgetId The table.
    /// @param rowKey   The row's key.
    /// @param column   The cell's column: an index into the table's columns.
    /// @return The accent `setCellAccent` last gave the cell; `CellAccent{}` when none.
    /// @throws std::logic_error when the widget is not a table, or none of its current rows has @p rowKey.
    [[nodiscard]] CellAccent cellAccent(int widgetId, Key const& rowKey, std::size_t column) {
        detail::Record const& record = expectKind(widgetId, "cellAccent", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        if (table == nullptr) {
            throw std::logic_error{"RecordingBackend: cellAccent of a widget that is not a table"};
        }
        return table->cellAccent(rowKey, column);
    }

    /// @brief The widget of a current row of a table.
    /// @param widgetId The table.
    /// @param rowKey   The row's key.
    /// @return The row's widget id; nothing when no current row has @p rowKey.
    /// @throws std::logic_error when the widget is not a table.
    [[nodiscard]] std::optional<int> tableRow(int widgetId, Key const& rowKey) {
        detail::Record const& record = expectKind(widgetId, "tableRow", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        if (table == nullptr) {
            throw std::logic_error{"RecordingBackend: tableRow of a widget that is not a table"};
        }
        return table->rowOf(rowKey);
    }

    /// @brief Activates a row of a table (Enter or a double click).
    /// @param widgetId The table.
    /// @param key The row's key.
    /// @throws std::logic_error when the widget is not a table, or none of its current data rows has @p key.
    void activateRow(int widgetId, Key key) {
        detail::Record const& record = expectKind(widgetId, "activateRow", {"Table"});
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        std::optional<int> const rowId = table == nullptr ? std::nullopt : table->rowOf(key);
        if (!rowId.has_value() || table->headingKey(key)) {
            throw std::logic_error{"RecordingBackend: activateRow of " + detail::formatKey(key) + ", which Table#" +
                                   std::to_string(widgetId) + " has no data row for"};
        }
        if (reachable(_store->at(*rowId))) {
            invoke(record.callbacks.activate, std::move(key));
        }
    }

    /// @brief Simulates the backend reporting a scroll on a virtualized table, as a real Qt Quick table's own
    ///        scroll handling would: calls the handler `IViewBackend::TableWidget::setOnVisibleRangeChanged`
    ///        subscribed, so the mount realizes/releases rows to match. A table whose row count is below its
    ///        `Virtualization::threshold` never subscribes one, so this call does nothing for it, matching a real
    ///        backend that never virtualizes a small table.
    /// @param widgetId The table.
    /// @param first The view index of the first row now wanted.
    /// @param count How many rows from `first` are wanted.
    /// @throws std::logic_error when the widget is not a table.
    void scrollTable(int widgetId, std::size_t first, std::size_t count) {
        detail::Record const& record = expectKind(widgetId, "scrollTable", {"Table"});
        auto* const table = dynamic_cast<detail::FakeTable*>(record.widget);
        if (table == nullptr) {
            throw std::logic_error{"RecordingBackend: scrollTable of Table#" + std::to_string(widgetId) +
                                   ", which is not a table"};
        }
        table->scrollTo(first, count);
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

    /// @brief Moves a slider to @p value, a finished move: `onChange` gets it, then `onCommit`.
    /// @param widgetId The slider.
    /// @param value The value moved to.
    /// @throws std::logic_error when the widget is not a slider.
    void slide(int widgetId, std::int64_t value) {
        if (detail::Record const* const record = editable(widgetId, "slide", {"Slider"}); record != nullptr) {
            _store->setByUser(widgetId, "value", std::to_string(value));
            // The change handler may destroy the slider, so the commit handler is copied before either runs.
            auto const commit = record->callbacks.slideCommit;
            invoke(record->callbacks.slide, value);
            invoke(commit, value);
        }
    }

    /// @brief Moves a slider to @p value while the move is still in progress: `onChange` gets it, and `onCommit` does
    ///        not run until the move finishes. Named apart from `drag`, which drags one widget onto another.
    /// @param widgetId The slider.
    /// @param value The value moved to.
    /// @throws std::logic_error when the widget is not a slider.
    void dragSlider(int widgetId, std::int64_t value) {
        if (detail::Record const* const record = editable(widgetId, "dragSlider", {"Slider"}); record != nullptr) {
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
    // The fake behind a table, for a helper that drives it; throws as `expectKind` does.
    detail::FakeTable& fakeTable(int widgetId, std::string_view helper) {
        return dynamic_cast<detail::FakeTable&>(*expectKind(widgetId, helper, {"Table"}).widget);
    }

    // The fake behind a text field's record.
    static detail::FakeTextInput& textInput(detail::Record const& record) {
        return dynamic_cast<detail::FakeTextInput&>(*record.widget);
    }

    // The built-in keys of the focused widget's kind, `press`'s second step: whether the key stops here. A text input
    // without a submit handler commits on Enter and lets the key go on.
    // Left or Right on a segmented select: chooses the neighbour, if any, as a user's choice.
    void stepSegmented(int widgetId, bool forward) {
        auto const* const select = dynamic_cast<detail::FakeSelect const*>(_store->at(widgetId).widget);
        std::optional<Key> next = select == nullptr ? std::nullopt : select->neighbour(forward ? 1 : -1);
        if (next.has_value()) {
            choose(widgetId, std::move(*next));
        }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): matches a chord against the widget's own keys, then its table's, then its cells'; each tier has the checks the one before leaves
    bool ownKey(int widgetId, std::string const& chord) {
        detail::Record const& record = _store->at(widgetId);
        bool const enter = chord == "Enter";
        if (record.kind == "Button" && (enter || chord == "Space")) {
            invoke(record.callbacks.click);
            return true;
        }
        if (record.kind == "Checkbox" && chord == "Space") {
            toggle(widgetId);
            return true;
        }
        if (record.kind == "Select" && prop(widgetId, "style") == "Segmented" &&
            (chord == "Left" || chord == "Right")) {
            stepSegmented(widgetId, chord == "Right");
            return true;
        }
        if (record.kind == "TextInput" && enter && prop(widgetId, "mode") != "Multiline") {
            if (record.callbacks.submit) {
                submit(widgetId, textInput(record).text());
                return true;
            }
            commit(widgetId);
            return false;
        }
        if (record.kind == "Table" && chord == "Ctrl+C") {
            auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
            if (table == nullptr || !table->onCopy()) {
                return false;
            }
            invoke(table->onCopy());
            return true;
        }
        if (record.kind == "Table" && chord == "Ctrl+A") {
            auto* const table = dynamic_cast<detail::FakeTable*>(record.widget);
            if (table == nullptr || table->mode() != SelectionMode::Multiple) {
                return false;
            }
            if (table->onSelectAll()) {
                std::function<void()> const selectAll = table->onSelectAll();
                selectAll();
                return true;
            }
            std::vector<Key> keys = table->allOrNone();
            table->select(keys);
            invoke(record.callbacks.selection, std::move(keys), SelectionChange::Extend);
            return true;
        }
        if (record.kind == "Table" && enter) {
            auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
            std::vector<Key> const marked = table == nullptr ? std::vector<Key>{} : table->markedKeys();
            if (!marked.empty()) {
                activateRow(widgetId, marked.front());
            }
            return true;
        }
        if (record.kind == "Table") {
            return cellKey(record, chord);
        }
        return false;
    }

    // A chord of the table's cell menu keys runs the focused cell's menu entry that declares it: the first enabled one
    // without a submenu, in the order the menu lists them, a submenu's entries after the entry that opens it. Any
    // other chord asks for nothing.
    static bool cellKey(detail::Record const& record, std::string const& chord) {
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        std::optional<std::pair<Key, std::size_t>> const cell =
            table == nullptr ? std::nullopt : table->focusedCellKey();
        if (!cell || !table->cellChosen() || !std::ranges::contains(table->cellMenuKeys(), chord)) {
            return false;
        }
        std::vector<MenuEntry> const entries = table->cellMenuEntries(cell->first, cell->second);
        std::vector<std::size_t> path;
        std::function<bool(std::vector<MenuEntry> const&)> const find = [&](std::vector<MenuEntry> const& level) {
            for (std::size_t at = 0; at < level.size(); ++at) {
                MenuEntry const& entry = level.at(at);
                if (entry.separator || entry.heading || !entry.enabled) {
                    continue;
                }
                path.push_back(at);
                if (entry.items.empty() ? entry.keys == chord : find(entry.items)) {
                    return true;
                }
                path.pop_back();
            }
            return false;
        };
        if (!find(entries)) {
            return false;
        }
        invoke(table->cellChosen(), cell->first, cell->second, path);
        return true;
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
        if (flagIs(record, "visible", false) || flagIs(record, "enabled", false) || detail::isClosedOverlay(record) ||
            dimmed(record)) {
            return false;
        }
        for (int innerId = record.id, outerId = record.parent; outerId != 0;) {
            detail::Record const& outer = _store->at(outerId);
            if (flagIs(outer, "visible", false) || flagIs(outer, "enabled", false) ||
                flagIs(outer, "collapsed", true) || detail::isClosedOverlay(outer) || closedAround(outer, innerId)) {
                return false;
            }
            innerId = outerId;
            outerId = outer.parent;
        }
        return true;
    }

    // The open spotlight @p widgetId is inside, or 0.
    [[nodiscard]] int openSpotlightAround(int widgetId) const {
        for (int outer = _store->at(widgetId).parent; outer != 0; outer = _store->at(outer).parent) {
            detail::Record const& record = _store->at(outer);
            if (record.kind == "Spotlight" && flagIs(record, "open", true)) {
                return outer;
            }
        }
        return 0;
    }

    // Whether an open spotlight dims @p record: its dimmed area takes no press, so only its card and the widget it
    // rings, with what is inside that widget, take one.
    [[nodiscard]] bool dimmed(detail::Record const& record) const {
        for (auto const& [spotlightId, spotlight] : _store->records()) {
            if (spotlight.kind != "Spotlight" || !flagIs(spotlight, "open", true)) {
                continue;
            }
            int ringed = 0;
            if (auto const found = spotlight.props.find("ringed"); found != spotlight.props.end()) {
                std::from_chars(std::to_address(found->second.begin()), std::to_address(found->second.end()), ringed);
            }
            bool reaches = false;
            for (int at = record.id; at != 0 && !reaches; at = _store->at(at).parent) {
                reaches = at == spotlightId || (ringed != 0 && at == ringed);
            }
            if (!reaches) {
                return true;
            }
        }
        return false;
    }

    // A closed collapsible section hides its content, the child on the way in, unless that child is its header.
    [[nodiscard]] static bool closedAround(detail::Record const& outer, int childId) {
        if (outer.kind != "Collapsible" || !flagIs(outer, "open", false)) {
            return false;
        }
        auto const header = outer.props.find("header");
        return header == outer.props.end() || header->second != std::to_string(childId);
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

    // The fake table behind a table's record.
    static detail::FakeTable const& fakeTable(detail::Record const& record) {
        auto const* const table = dynamic_cast<detail::FakeTable const*>(record.widget);
        if (table == nullptr) {
            throw std::logic_error{"RecordingBackend: Table#" + std::to_string(record.id) + " is not a table"};
        }
        return *table;
    }

    // The fake table, once a user could reach the column's header: a hidden column's is not drawn.
    static detail::FakeTable const& shownHeader(detail::Record const& record, int widgetId, std::size_t column,
                                                std::string_view helper) {
        detail::FakeTable const& table = fakeTable(record);
        if (!table.headerShown(column)) {
            throw std::logic_error{"RecordingBackend: " + std::string{helper} + " of column " +
                                   std::to_string(column) + ", whose header Table#" + std::to_string(widgetId) +
                                   " does not draw"};
        }
        return table;
    }

    std::shared_ptr<detail::RecordStore> _store = std::make_shared<detail::RecordStore>();
    std::string _clipboard;
    bool _hasClipboard = true;
    std::shared_ptr<TableMeasure> _tableMeasure = std::make_shared<TableMeasure>();
    std::optional<Mm> _pageWidth;
    std::shared_ptr<std::vector<detail::FakePage*>> _pages = std::make_shared<std::vector<detail::FakePage*>>();
    std::unordered_set<std::string> _components;
    std::string _widthClass = "expanded";
    std::function<void(std::string const&)> _onWidthClass;
};

}  // namespace morph::ui::testing
