// SPDX-License-Identifier: Apache-2.0

#include "tui/field_widgets.hpp"

#include <libunicode/convert.h>

#include <algorithm>
#include <core/tui/Buffer.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Theme.hpp>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr std::chrono::minutes kDay{24 * 60};

bool isPlain(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None;
}

/// @p event as the InputField should see it. InputField inserts a typed character only when its KeyCode lies below
/// U+10000, the range where core::tui keeps its special keys, so it would drop a character above the Basic
/// Multilingual Plane (most emoji); such a character goes in as a one-character paste instead.
::core::tui::InputEvent asFieldInput(::core::tui::InputEvent const& event) {
    auto const* const key = std::get_if<::core::tui::KeyEvent>(&event);
    if (key == nullptr || key->codepoint < 0x10000 || static_cast<char32_t>(key->key) != key->codepoint) {
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
        if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
            return owner().pointer(*mouse);
        }
        if (!owner().actionable()) {
            return EventResult::Ignored;
        }
        auto* const fields = _fields;
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
            if (key->key == KeyCode::Tab || key->key == KeyCode::Escape) {
                return EventResult::Ignored;
            }
            if (auto const handled = fields->intercept(*key)) {
                return *handled;
            }
        }
        auto const action = processEvent(asFieldInput(event));
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
    auto text = std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                            static_cast<unsigned>(date.day()));
    if (mode == ui::DateMode::DateTime) {
        std::chrono::hh_mm_ss const clock{local - days};
        text += std::format(" {:02}:{:02}", clock.hours().count(), clock.minutes().count());
    }
    return text;
}

std::optional<morph::time::DateTime> parseLocal(std::string_view text, ui::DateMode mode, int offsetMinutes) {
    std::string iso;
    if (mode == ui::DateMode::Date) {
        if (text.size() != 10) {
            return std::nullopt;
        }
        iso = std::string{text} + "T00:00:00";
    } else {
        if (text.size() != 16 || (text.at(10) != ' ' && text.at(10) != 'T')) {
            return std::nullopt;
        }
        iso = std::string{text.substr(0, 10)} + "T" + std::string{text.substr(11, 5)} + ":00";
    }
    return morph::time::DateTime::fromIso8601(iso).transform(
        [offsetMinutes](morph::time::DateTime local) { return local - std::chrono::minutes{offsetMinutes}; });
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

void DateTimeInputImpl::submitted() {
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
}

void FilePickerImpl::setOnPicked(std::function<void(std::string)> onPicked) { _onPicked = std::move(onPicked); }

::core::tui::Size FilePickerImpl::naturalSize() const {
    return {.width = std::max(30, displayWidth(_field->prompt()) + displayWidth(_field->text()) + 1), .height = 1};
}

std::string FilePickerImpl::probeText() const { return std::string{_field->text()}; }

void FilePickerImpl::paint(::core::tui::Canvas& canvas) { paintField(*_field, *this, actionable(), canvas); }

void FilePickerImpl::submitted() {
    auto const handler = _onPicked;
    if (handler && !_field->text().empty()) {
        handler(std::string{_field->text()});
    }
}

}  // namespace morph::tui::detail
