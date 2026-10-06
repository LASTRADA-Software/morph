// SPDX-License-Identifier: Apache-2.0

#include "tui/field_widgets.hpp"

#include <libunicode/convert.h>

#include <algorithm>
#include <chrono>
#include <core/tui/Buffer.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "tui/drag.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr std::chrono::minutes kDay{24 * 60};

bool isPlain(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None;
}

/// Whether InputField would drop @p codepoint typed as a key: it takes a character only below U+10000, where
/// core::tui's special keys start, and outside the private-use area, which the kitty keyboard protocol uses for keys.
constexpr bool isDroppedCharacter(char32_t codepoint) noexcept {
    return codepoint >= 0x10000 || (codepoint >= 0xE000 && codepoint <= 0xF8FF);
}

/// @p event as the field should see it: a character InputField would drop (most emoji, private-use characters) goes
/// in as a one-character paste. A typed character has its codepoint as its KeyCode, which can equal a special key's
/// (U+10001 is Tab's), so this comes before anything looks at the KeyCode.
::core::tui::InputEvent asFieldInput(::core::tui::InputEvent const& event) {
    auto const* const key = std::get_if<::core::tui::KeyEvent>(&event);
    if (key == nullptr || !isDroppedCharacter(key->codepoint) || static_cast<char32_t>(key->key) != key->codepoint) {
        return event;
    }
    auto const modifiers = ::core::tui::withoutLockKeys(key->modifiers);
    if (modifiers != ::core::tui::Modifier::None && modifiers != ::core::tui::Modifier::Shift) {
        return event;
    }
    return ::core::tui::PasteEvent{.text = unicode::convert_to<char>(std::u32string_view{&key->codepoint, 1})};
}

/// The view of a field widget (see `makeFieldView`).
class FieldView final : public Hosted<::core::tui::InputField> {
public:
    FieldView(WidgetBase& owner, FieldOwner& fields) : Hosted{owner}, _fields{&fields} {}

    /// A field view has no children, so unlike a root View it has no drawn bounds below it to forget.
    void render(::core::tui::Canvas& canvas) override { owner().render(canvas); }

    /// An edit, Enter or an intercepted key may end in a handler that destroys the owner and this view with it, so
    /// nothing is touched after those calls.
    [[nodiscard]] EventResult onEvent(::core::tui::InputEvent const& event) override {
        if (escapeEndsDrag(owner().context(), event)) {
            return EventResult::Handled;
        }
        if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
            return owner().pointer(*mouse);
        }
        if (!owner().actionable()) {
            return EventResult::Ignored;
        }
        auto* const fields = _fields;
        auto const input = asFieldInput(event);
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&input)) {
            if (key->codepoint == 0 && (key->key == KeyCode::Tab || key->key == KeyCode::Escape)) {
                return EventResult::Ignored;
            }
            if (auto const handled = fields->intercept(*key)) {
                return *handled;
            }
        }
        auto const action = processEvent(input);
        if (action == ::core::tui::InputFieldAction::Changed) {
            invalidate();
            fields->edited();
            return EventResult::Handled;
        }
        if (action == ::core::tui::InputFieldAction::Submit) {
            fields->submitted();
            return EventResult::Handled;
        }
        // None, and the agent-shell actions (Ctrl+T, Ctrl+G, …) that mean nothing here, bubble on.
        return EventResult::Ignored;
    }

    /// Completes what was typed, unless the owner is being destroyed (it unregisters this view before clearing the
    /// focus) or the user cannot reach it. The owner's handler may destroy it, so nothing is touched afterwards.
    void onBlur() override {
        if (owner().context().ownerOf(this) != &owner() || !owner().actionable()) {
            return;
        }
        _fields->left();
    }

private:
    FieldOwner* _fields;
};

}  // namespace

std::unique_ptr<::core::tui::InputField> makeFieldView(WidgetBase& owner, FieldOwner& fields) {
    return std::make_unique<FieldView>(owner, fields);
}

void paintField(::core::tui::InputField& field, FieldOwner const& fields, bool reachable,
                ::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    auto& buffer = canvas.buffer();
    auto const cursor = buffer.cursor();
    bool const cursorShown = buffer.cursorVisible();
    field.setStyles(reachable ? ::core::tui::InputFieldStyles{}
                              : ::core::tui::InputFieldStyles{.text = theme.textMuted,
                                                              .ghost = std::nullopt,
                                                              .selection = std::nullopt,
                                                              .background = std::nullopt});
    field.InputField::render(canvas);
    if (!field.focused()) {
        buffer.setCursor(cursor.y, cursor.x);
        buffer.setCursorVisible(cursorShown);
    }
    if (field.text().empty() && !fields.placeholder().empty()) {
        canvas.putString(0, displayWidth(field.prompt()), fields.placeholder(), theme.inputPlaceholder);
    }
    if (fields.invalid() && canvas.width() > 0) {
        canvas.put(0, canvas.width() - 1, "!", theme.error);
    }
}

std::string formatLocal(morph::time::DateTime instant, ui::DateMode mode, int offsetMinutes) {
    auto const local = std::chrono::floor<std::chrono::minutes>(instant.value + std::chrono::minutes{offsetMinutes});
    auto const days = std::chrono::floor<std::chrono::days>(local);
    std::chrono::year_month_day const date{days};
    int const year = static_cast<int>(date.year());
    auto text = std::format("{}{:04}-{:02}-{:02}", year < 0 ? "-" : "", std::abs(year),
                            static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    if (mode == ui::DateMode::DateTime) {
        std::chrono::hh_mm_ss const clock{local - days};
        text += std::format(" {:02}:{:02}", clock.hours().count(), clock.minutes().count());
    }
    return text;
}

namespace {

/// @p text as a number, when it is one to nine ASCII digits.
std::optional<int> digitsValue(std::string_view text) {
    if (text.empty() || text.size() > 9) {
        return std::nullopt;
    }
    int value = 0;
    for (char const digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        value = (value * 10) + (digit - '0');
    }
    return value;
}

/// The day @p text names as `formatLocal` writes it: at least four year digits, `-` before a year below zero, then
/// `-MM-DD`; a date the calendar does not have is none.
std::optional<std::chrono::sys_days> parseDay(std::string_view text) {
    if (text.size() < 10 || text.at(text.size() - 3) != '-' || text.at(text.size() - 6) != '-') {
        return std::nullopt;
    }
    auto yearText = text.substr(0, text.size() - 6);
    bool const negative = yearText.starts_with('-');
    if (negative) {
        yearText.remove_prefix(1);
    }
    auto const year = yearText.size() >= 4 ? digitsValue(yearText) : std::nullopt;
    auto const month = digitsValue(text.substr(text.size() - 5, 2));
    auto const day = digitsValue(text.substr(text.size() - 2));
    if (!year || !month || !day) {
        return std::nullopt;
    }
    std::chrono::year_month_day const date{std::chrono::year{negative ? -*year : *year},
                                           std::chrono::month{static_cast<unsigned>(*month)},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date};
}

/// The time of day @p text names as `HH:MM`.
std::optional<std::chrono::minutes> parseClock(std::string_view text) {
    if (text.size() != 5 || text.at(2) != ':') {
        return std::nullopt;
    }
    auto const hours = digitsValue(text.substr(0, 2));
    auto const minutes = digitsValue(text.substr(3, 2));
    if (!hours || !minutes || *hours > 23 || *minutes > 59) {
        return std::nullopt;
    }
    return std::chrono::hours{*hours} + std::chrono::minutes{*minutes};
}

}  // namespace

// The year may be wider than four digits, as `formatLocal` writes it past 9999, so the fields are found from the end.
std::optional<morph::time::DateTime> parseLocal(std::string_view text, ui::DateMode mode, int offsetMinutes) {
    auto dayText = text;
    std::chrono::minutes clock{0};
    if (mode == ui::DateMode::DateTime) {
        if (text.size() < 16) {
            return std::nullopt;
        }
        auto const separator = text.at(text.size() - 6);
        auto const time = parseClock(text.substr(text.size() - 5));
        if ((separator != ' ' && separator != 'T') || !time) {
            return std::nullopt;
        }
        clock = *time;
        dayText = text.substr(0, text.size() - 6);
    }
    auto const day = parseDay(dayText);
    if (!day) {
        return std::nullopt;
    }
    return morph::time::DateTime{*day + clock - std::chrono::minutes{offsetMinutes}};
}

// The guard is what keeps a controlled input usable: a binding that writes back the text the user just typed must
// not replace the buffer, which would move the cursor to the end.
void TextInputImpl::setText(std::string_view text) {
    if (text != _field->text()) {
        _field->setText(text);
        refresh();
    }
    _reported = std::string{_field->text()};
}

void TextInputImpl::setPlaceholder(std::string_view placeholder) {
    _placeholder = std::string{placeholder};
    refresh();
}

void TextInputImpl::setOnChange(std::function<void(std::string)> onChange) { _onChange = std::move(onChange); }

void TextInputImpl::setOnSubmit(std::function<void(std::string)> onSubmit) { _onSubmit = std::move(onSubmit); }

::core::tui::Size TextInputImpl::naturalSize() const {
    int const width = std::max({20, displayWidth(_field->text()) + 1, displayWidth(_placeholder)});
    int const height = _mode == ui::TextInputMode::Multiline ? std::max(3, _field->lineCount()) : 1;
    return {.width = width, .height = height};
}

std::string TextInputImpl::probeText() const { return std::string{_field->text()}; }

void TextInputImpl::paint(::core::tui::Canvas& canvas) { paintField(*_field, *this, actionable(), canvas); }

void TextInputImpl::edited() {
    std::string current{_field->text()};
    if (current == _reported) {
        return;
    }
    _reported = current;
    auto const handler = _onChange;
    if (handler) {
        handler(std::move(current));
    }
}

void TextInputImpl::submitted() {
    auto const handler = _onSubmit;
    if (handler) {
        handler(std::string{_field->text()});
    }
}

void DateTimeInputImpl::setValue(std::optional<morph::time::Timestamp> const& value) {
    auto const text = value && value->hasValue() ? formatLocal(**value, _mode, _offsetMinutes) : std::string{};
    if (text != _field->text()) {
        _field->setText(text);
    }
    _committed = parseLocal(text, _mode, _offsetMinutes);
    _invalid = false;
    refresh();
}

void DateTimeInputImpl::setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) {
    _onChange = std::move(onChange);
}

::core::tui::Size DateTimeInputImpl::naturalSize() const {
    return {.width = _mode == ui::DateMode::Date ? 12 : 18, .height = 1};
}

std::string DateTimeInputImpl::probeText() const { return std::string{_field->text()}; }

void DateTimeInputImpl::paint(::core::tui::Canvas& canvas) { paintField(*_field, *this, actionable(), canvas); }

void DateTimeInputImpl::edited() { _invalid = false; }

void DateTimeInputImpl::submitted() { commitTyped(); }

// What is compared is the instant, not the text: what was set is shown to the minute, and `T` and a space spell the
// same instant.
void DateTimeInputImpl::left() {
    auto const text = _field->text();
    auto const typed = text.empty() ? std::nullopt : parseLocal(text, _mode, _offsetMinutes);
    if (!text.empty() && !typed) {
        commitTyped();
        return;
    }
    if (typed != _committed) {
        commit(typed);
        return;
    }
    auto const shown = typed ? formatLocal(*typed, _mode, _offsetMinutes) : std::string{};
    if (shown != text) {
        _field->setText(shown);
        refresh();
    }
}

void DateTimeInputImpl::commitTyped() {
    if (_field->text().empty()) {
        commit(std::nullopt);
        return;
    }
    if (auto const parsed = parseLocal(_field->text(), _mode, _offsetMinutes); parsed.has_value()) {
        commit(parsed);
        return;
    }
    _invalid = true;
    refresh();
}

std::optional<EventResult> DateTimeInputImpl::intercept(::core::tui::KeyEvent const& key) {
    if (!isPlain(key)) {
        return std::nullopt;
    }
    auto const unit = _mode == ui::DateMode::Date ? kDay : std::chrono::minutes{1};
    if (key.key == KeyCode::Up) {
        step(unit);
    } else if (key.key == KeyCode::Down) {
        step(-unit);
    } else if (key.key == KeyCode::PageUp) {
        step(kDay);
    } else if (key.key == KeyCode::PageDown) {
        step(-kDay);
    } else {
        return std::nullopt;
    }
    return EventResult::Handled;
}

// An empty field steps from now; text that does not parse is left alone and marked.
void DateTimeInputImpl::step(std::chrono::minutes delta) {
    auto const text = _field->text();
    auto const current =
        text.empty() ? std::optional{morph::time::DateTime::now()} : parseLocal(text, _mode, _offsetMinutes);
    if (!current) {
        _invalid = true;
        refresh();
        return;
    }
    commit(*current + delta);
}

// What is reported is read back from the text shown, so it is the instant the user sees: local midnight in Date
// mode, the whole minute in DateTime mode.
void DateTimeInputImpl::commit(std::optional<morph::time::DateTime> value) {
    auto const text = value ? formatLocal(*value, _mode, _offsetMinutes) : std::string{};
    auto const shown = parseLocal(text, _mode, _offsetMinutes);
    _invalid = false;
    _field->setText(text);
    _committed = shown;
    refresh();
    auto const handler = _onChange;
    if (handler) {
        handler(shown.transform([](morph::time::DateTime instant) { return morph::time::Timestamp{instant}; }));
    }
}

void FilePickerImpl::setPath(std::string_view path) {
    if (path != _field->text()) {
        _field->setText(path);
        refresh();
    }
    _committed = std::string{_field->text()};
}

void FilePickerImpl::setOnPicked(std::function<void(std::string)> onPicked) { _onPicked = std::move(onPicked); }

::core::tui::Size FilePickerImpl::naturalSize() const {
    return {.width = std::max(30, displayWidth(_field->prompt()) + displayWidth(_field->text()) + 1), .height = 1};
}

std::string FilePickerImpl::probeText() const { return std::string{_field->text()}; }

void FilePickerImpl::paint(::core::tui::Canvas& canvas) { paintField(*_field, *this, actionable(), canvas); }

void FilePickerImpl::submitted() { pick(); }

void FilePickerImpl::left() {
    if (_field->text() != _committed) {
        pick();
    }
}

void FilePickerImpl::pick() {
    std::string path{_field->text()};
    if (path.empty()) {
        return;
    }
    _committed = path;
    auto const handler = _onPicked;
    if (handler) {
        handler(std::move(path));
    }
}

}  // namespace morph::tui::detail
