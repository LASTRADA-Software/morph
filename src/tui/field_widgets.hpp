// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/tui/Canvas.hpp>
#include <core/tui/EditAction.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/InputField.hpp>
#include <core/tui/KeyBindings.hpp>
#include <core/tui/KeyCode.hpp>
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
void paintField(::core::tui::InputField& field, FieldOwner const& fields, bool reachable, ::core::tui::Canvas& canvas);

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
class TextInputImpl final : public TuiWidget<ui::TextInputWidget>, private FieldOwner {
public:
    TextInputImpl(Context& context, ui::TextInputMode mode)
        : TuiWidget{context}, _mode{mode}, _field{&adopt(makeFieldView(*this, *this))} {
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
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    void edited() override;
    void submitted() override;
    void left() override {}
    [[nodiscard]] std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& /*key*/) override {
        return std::nullopt;
    }
    [[nodiscard]] std::string_view placeholder() const override { return _placeholder; }
    [[nodiscard]] bool invalid() const override { return false; }

    ui::TextInputMode _mode;
    ::core::tui::InputField* _field;
    std::string _reported;
    std::string _placeholder;
    std::function<void(std::string)> _onChange;
    std::function<void(std::string)> _onSubmit;
};

/// DateTimeInput: an ISO-style field in a display zone. Up/Down step by a day (Date) or a minute (DateTime),
/// PageUp/PageDown by a day. Enter commits what was typed, or clears the value when the field is empty; so does the
/// focus leaving, when the text differs from what was last set or committed. Text that does not parse is marked
/// with a `!` and reported as nothing. What it reports is the instant its text names: local midnight in Date mode, a
/// whole minute in DateTime mode.
class DateTimeInputImpl final : public TuiWidget<ui::DateTimeInputWidget>, private FieldOwner {
public:
    DateTimeInputImpl(Context& context, ui::DateMode mode, int offsetMinutes)
        : TuiWidget{context},
          _mode{mode},
          _offsetMinutes{offsetMinutes},
          _field{&adopt(makeFieldView(*this, *this))} {}
    void setValue(std::optional<morph::time::Timestamp> const& value) override;
    void setOnChange(std::function<void(std::optional<morph::time::Timestamp>)> onChange) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    void edited() override;
    void submitted() override;
    void left() override;
    [[nodiscard]] std::optional<::core::tui::EventResult> intercept(::core::tui::KeyEvent const& key) override;
    [[nodiscard]] std::string_view placeholder() const override { return {}; }
    [[nodiscard]] bool invalid() const override { return _invalid; }
    void step(std::chrono::minutes delta);
    void commitTyped();
    void commit(std::optional<morph::time::DateTime> value);

    ui::DateMode _mode;
    int _offsetMinutes;
    ::core::tui::InputField* _field;
    std::string _committed;  ///< The text last set or committed.
    bool _invalid = false;
    std::function<void(std::optional<morph::time::Timestamp>)> _onChange;
};

/// FilePicker: a path field (a terminal has no file dialog), prompted `Open: ` or `Save: `. Enter reports a
/// non-empty path; so does the focus leaving, when the path differs from what was last set or picked. Whether the
/// file exists is for the application to check.
class FilePickerImpl final : public TuiWidget<ui::FilePickerWidget>, private FieldOwner {
public:
    FilePickerImpl(Context& context, ui::FilePickerMode mode)
        : TuiWidget{context}, _field{&adopt(makeFieldView(*this, *this))} {
        _field->setPrompt(mode == ui::FilePickerMode::Open ? "Open: " : "Save: ");
    }
    void setPath(std::string_view path) override;
    void setOnPicked(std::function<void(std::string)> onPicked) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    void edited() override {}
    void submitted() override;
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
