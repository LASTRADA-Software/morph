// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/tui/Canvas.hpp>
#include <core/tui/EditAction.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/InputField.hpp>
#include <core/tui/KeyBindings.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// What a field's view asks the widget that owns it.
class FieldOwner {
public:
    FieldOwner() = default;
    virtual ~FieldOwner() = default;
    FieldOwner(FieldOwner const&) = delete;
    FieldOwner& operator=(FieldOwner const&) = delete;
    FieldOwner(FieldOwner&&) = delete;
    FieldOwner& operator=(FieldOwner&&) = delete;

    /// The buffer or the cursor changed by the user's doing. It may call a handler, which may destroy the widget:
    /// the view touches nothing after it.
    virtual void edited() = 0;
    /// Enter was pressed. Like `edited`, it may end in a handler that destroys the widget.
    virtual void submitted() = 0;
    /// Whether Enter stops at the field. When it does not, the field still commits on Enter, and the key then goes on
    /// outwards to the ancestors' chords (a dialog's default).
    [[nodiscard]] virtual bool submits() const = 0;
    /// The focus left the field while the user could reach it. Like `edited`, it may end in a handler that destroys
    /// the widget.
    virtual void left() = 0;
    /// A key the owner handles before the field does; nullopt lets the field have it. Like `edited`, it may end in a
    /// handler that destroys the widget.
    [[nodiscard]] virtual std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& key) = 0;
    /// Shown while the field is empty.
    [[nodiscard]] virtual std::string_view placeholder() const = 0;
    /// Whether to mark the field as holding text that does not parse.
    [[nodiscard]] virtual bool invalid() const = 0;
};

/// The view of a field widget: a core::tui::InputField that @p owner paints and whose input reaches it only while it
/// is actionable, and that reports edits, Enter, the keys it lets the owner intercept and the focus leaving to
/// @p fields. The focus leaving reports nothing while @p owner is being destroyed or cannot be reached: a closed
/// dialog's field is abandoned, not committed.
///
/// It leaves Tab, Shift+Tab and Esc to the frontend and to dialogs: InputField would clear its buffer on Esc and
/// cycle an agent mode on Shift+Tab. Its class lives in field_widgets.cpp, for the reason `makeView` gives.
[[nodiscard]] std::unique_ptr<::core::tui::InputField> makeFieldView(WidgetBase& owner, FieldOwner& fields);
/// Draws @p field into @p canvas as InputField does (muted while the user cannot reach it), then @p fields'
/// placeholder while it is empty and a `!` in the last column while @p fields marks it invalid. The text cursor is
/// left as it was unless @p field has the focus: InputField always sets it, and the last field drawn would win.
/// The text is bold while @p strong: a prominent input's.
void paintField(::core::tui::InputField& field, FieldOwner const& fields, bool reachable, ::core::tui::Canvas& canvas,
                bool strong = false);

/// @p instant as text in the display zone @p offsetMinutes east of UTC: `YYYY-MM-DD` or `YYYY-MM-DD HH:MM`.
[[nodiscard]] std::string formatLocal(morph::time::DateTime instant, ui::DateMode mode, int offsetMinutes);
/// The instant @p text names in the display zone, or nullopt; DateTime mode accepts a space or a `T`.
[[nodiscard]] std::optional<morph::time::DateTime> parseLocal(std::string_view text, ui::DateMode mode,
                                                              int offsetMinutes);

/// TextInput: single-line, multiline or masked.
///
/// Enter submits a single-line or masked field. In a multiline one Enter starts a new line, as Shift+Enter and
/// Alt+Enter do: most terminals send Shift+Enter as a plain Enter, so it would otherwise be unreachable, and a
/// multiline field never submits. Ctrl+End then Ctrl+U empties the field in every mode.
class TextInputImpl final : public TuiField<ui::TextInputWidget>, private FieldOwner {
public:
    TextInputImpl(Context& context, ui::TextInputMode mode)
        : TuiField{context}, _mode{mode}, _field{&adopt(makeFieldView(*this, *this))} {
        _field->setMasked(mode == ui::TextInputMode::Password);
        _field->setMultiline(mode == ui::TextInputMode::Multiline);
        if (mode == ui::TextInputMode::Multiline) {
            _field->keyBindings().bind(::core::tui::KeyChord::fromKey(::core::tui::KeyCode::Enter),
                                       ::core::tui::EditAction::InsertNewline);
        }
    }
    void setText(std::string_view text) override;
    void setPlaceholder(std::string_view placeholder) override;
    void setOnChange(std::function<void(std::string)> onChange) override;
    void setOnSubmit(std::function<void(std::string)> onSubmit) override;
    void setOnCommit(std::function<void(std::string)> onCommit) override;
    void setUnit(std::string_view unit) override;
    /// The prefix is drawn muted before the text, after the icon.
    void setPrefix(std::string_view prefix) override;
    /// Every face of a terminal is monospace, so a mono field draws nothing new.
    void setMono(bool mono) override { static_cast<void>(mono); }
    /// An end-aligned field draws its text against the field's right edge while the text fits.
    void setAlign(ui::Align align) override;
    void setDecimals(int decimals) override;
    void setIcon(std::string_view icon) override;
    /// A prominent input draws its text bold; the field's background is unchanged.
    void setEmphasis(ui::InputEmphasis emphasis) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
    /// Where the cursor is, as a byte offset into the text shown.
    [[nodiscard]] std::size_t cursor() const noexcept { return _field->cursor(); }
    [[nodiscard]] bool wantsFocus() const override { return true; }
    /// The prefix, the messages, the description, then the unit, so a listener hears the value between them.
    [[nodiscard]] std::string accessibleDescription() const override {
        std::string text = TuiField::accessibleDescription();
        if (!_prefix.empty()) {
            text = text.empty() ? _prefix : _prefix + "\n" + text;
        }
        if (!_unit.empty()) {
            text += text.empty() ? _unit : "\n" + _unit;
        }
        return text;
    }
    [[nodiscard]] std::string probeText() const override;

protected:
    [[nodiscard]] ::core::tui::Size controlSize() const override;
    void paintControl(::core::tui::Canvas& canvas) override;

private:
    void paintText(::core::tui::Canvas& canvas);
    void edited() override;
    void submitted() override;
    /// Only a field given a submit handler takes Enter for itself.
    [[nodiscard]] bool submits() const override { return static_cast<bool>(_onSubmit); }
    void left() override;
    [[nodiscard]] std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& /*key*/) override {
        return std::nullopt;
    }
    [[nodiscard]] std::string_view placeholder() const override { return _placeholder; }
    [[nodiscard]] bool invalid() const override { return false; }

    ui::TextInputMode _mode;
    ::core::tui::InputField* _field;
    std::string _reported;
    std::string _placeholder;
    std::string _unit;  ///< Drawn muted at the right edge, before the marks; the editable width ends a cell before it.
    std::string _prefix;     ///< Drawn muted before the text, after the icon.
    bool _alignEnd = false;  ///< Whether the text is drawn against the field's right edge while it fits.
    std::string _icon;  ///< A framework icon name draws as a glyph before the field; any other name draws nothing.
    std::function<void(std::string)> _onChange;
    std::function<void(std::string)> _onSubmit;
    std::function<void(std::string)> _onCommit;
    bool _uncommitted = false;  ///< Whether the user edited the text since the last commit.
    bool _prominent = false;    ///< Whether it is the screen's focal field.
};

/// DateTimeInput: an ISO-style field in a display zone. Up/Down step by a day (Date) or a minute (DateTime),
/// PageUp/PageDown by a day. Enter commits what was typed, or clears the value when the field is empty; so does the
/// focus leaving, when the text names another instant than the one last set or committed (the same instant spelt
/// differently is only rewritten). Text that does not parse is marked
/// with a `!` and reported as nothing. What it reports is the instant its text names: local midnight in Date mode, a
/// whole minute in DateTime mode.
class DateTimeInputImpl final : public TuiField<ui::DateTimeInputWidget>, private FieldOwner {
public:
    DateTimeInputImpl(Context& context, ui::DateMode mode, int offsetMinutes)
        : TuiField{context}, _mode{mode}, _offsetMinutes{offsetMinutes}, _field{&adopt(makeFieldView(*this, *this))} {}
    void setValue(std::optional<morph::time::Timestamp> const& value) override;
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override;
    /// Shows the value and reads typed text in @p pattern; ISO is always read too.
    void setPattern(std::string_view pattern) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;

protected:
    [[nodiscard]] ::core::tui::Size controlSize() const override;
    void paintControl(::core::tui::Canvas& canvas) override;

private:
    /// The text an instant shows as: in the pattern, else as `formatLocal` writes it.
    [[nodiscard]] std::string shownOf(morph::time::DateTime instant) const;
    /// The instant a text names: in the pattern or ISO, else as `parseLocal` reads it.
    [[nodiscard]] std::optional<morph::time::DateTime> instantOf(std::string_view text) const;
    void edited() override;
    void submitted() override;
    /// Enter commits what was typed, and stops here.
    [[nodiscard]] bool submits() const override { return true; }
    void left() override;
    [[nodiscard]] std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& key) override;
    [[nodiscard]] std::string_view placeholder() const override { return {}; }
    [[nodiscard]] bool invalid() const override { return _invalid; }
    void step(std::chrono::minutes delta);
    void commitTyped();
    void commit(std::optional<morph::time::DateTime> value);

    ui::DateMode _mode;
    int _offsetMinutes;
    std::string _pattern;
    ::core::tui::InputField* _field;
    std::optional<morph::time::DateTime> _committed;  ///< The instant last set or committed; none for an empty field.
    bool _invalid = false;
    std::function<void(std::optional<morph::time::Timestamp>)> _onChange;
};

/// FilePicker: a path field (a terminal has no file dialog), prompted `Open: ` or `Save: `. Enter reports a
/// non-empty path; so does the focus leaving, when the path differs from what was last set or picked. Whether the
/// file exists is for the application to check.
class FilePickerImpl final : public TuiField<ui::FilePickerWidget>, private FieldOwner {
public:
    FilePickerImpl(Context& context, ui::FilePickerMode mode)
        : TuiField{context}, _field{&adopt(makeFieldView(*this, *this))} {
        _field->setPrompt(mode == ui::FilePickerMode::Open ? "Open: " : "Save: ");
    }
    void setPath(std::string_view path) override;
    void setOnPicked(std::function<void(std::string)> onPicked) override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;

protected:
    [[nodiscard]] ::core::tui::Size controlSize() const override;
    void paintControl(::core::tui::Canvas& canvas) override;

private:
    void edited() override {}
    void submitted() override;
    /// Enter picks what was typed, and stops here.
    [[nodiscard]] bool submits() const override { return true; }
    void left() override;
    [[nodiscard]] std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& /*key*/) override {
        return std::nullopt;
    }
    [[nodiscard]] std::string_view placeholder() const override { return {}; }
    [[nodiscard]] bool invalid() const override { return false; }
    void pick();

    ::core::tui::InputField* _field;
    std::string _committed;  ///< The path last set or picked.
    std::function<void(std::string)> _onPicked;
};

}  // namespace morph::tui::detail
