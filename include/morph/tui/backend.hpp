// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/tui/Screen.hpp>
#include <functional>
#include <memory>

#include "../attributes.hpp"
#include "../ui/backend.hpp"

/// @file
/// @brief `morph::tui::Backend`: the terminal implementation of `ui::IViewBackend`.
///
/// Specified in `docs/spec/tui/frontend.md`, "Widgets".

namespace morph::tui {

/// @brief Builds every widget of a mounted view tree as a `core::tui::Component` on one `Screen`.
///
/// Containers arrange their children in cells when the screen renders them; a Dialog and an open Dropdown are
/// screen overlays; drag-and-drop runs on the mouse with pointer capture. The factories, documented on
/// `ui::IViewBackend`, run on the screen's thread. Widgets must be destroyed before the backend, and the backend
/// before the screen.
///
/// The widgets show no tooltips, so nothing here needs the screen's hover timer (`Screen::tickHover`). A widget that
/// goes drops the screen's hover target, so a caller that does tick it never reaches a destroyed component.
class Backend final : public ui::IViewBackend {
public:
    /// @brief Reads the current time.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// @param screen Where every widget renders. Borrowed: it must outlive the backend.
    /// @param now The clock the widgets read the time from (a table telling a double click from two clicks); empty
    ///        reads `std::chrono::steady_clock`. A test passes one it advances itself.
    explicit Backend(::core::tui::Screen& screen MORPH_LIFETIMEBOUND, Clock now = {});
    /// @brief Destroys the backend; every widget it made must already be gone.
    ~Backend() override;
    Backend(Backend const&) = delete;
    Backend& operator=(Backend const&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    [[nodiscard]] std::unique_ptr<ui::TextWidget> createText(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::ButtonWidget> createButton(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TextInputWidget> createTextInput(ui::ContainerWidget* parent,
                                                                       ui::TextInputMode mode) override;
    [[nodiscard]] std::unique_ptr<ui::CheckboxWidget> createCheckbox(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::SelectWidget> createSelect(ui::ContainerWidget* parent,
                                                                 ui::SelectStyle style) override;
    [[nodiscard]] std::unique_ptr<ui::MenuWidget> createMenu(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::StackWidget> createStack(ui::ContainerWidget* parent, ui::Axis axis) override;
    [[nodiscard]] std::unique_ptr<ui::GridWidget> createGrid(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::SpacerWidget> createSpacer(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::PanelWidget> createPanel(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::ScrollWidget> createScroll(ui::ContainerWidget* parent, ui::Axis axis) override;
    [[nodiscard]] std::unique_ptr<ui::SlotWidget> createSlot(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TabsWidget> createTabs(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::DialogWidget> createDialog(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::BusyWidget> createBusy(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::TableWidget> createTable(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::DateTimeInputWidget> createDateTimeInput(ui::ContainerWidget* parent,
                                                                               ui::DateMode mode,
                                                                               int offsetMinutes) override;
    [[nodiscard]] std::unique_ptr<ui::SliderWidget> createSlider(ui::ContainerWidget* parent) override;
    [[nodiscard]] std::unique_ptr<ui::FilePickerWidget> createFilePicker(ui::ContainerWidget* parent,
                                                                         ui::FilePickerMode mode) override;

    /// @brief Sizes every root widget to the screen's viewport and centres every open dialog; call before each
    ///        `Screen::draw()`, which is how a resize reaches the tree.
    void fit();

    /// @brief Moves keyboard focus to the next focusable widget — inside the innermost open dialog when one is
    ///        open, because `Screen::focusNext` does not know overlays.
    void focusNext();

    /// @brief Moves keyboard focus to the previous focusable widget, with `focusNext`'s dialog rule.
    void focusPrev();

    /// @brief Focuses the first focusable widget when nothing has focus yet, or when the focus is on an open
    ///        dialog's frame (the dialog held nothing focusable when it opened, and has since gained something).
    void focusFirst();

    /// @brief Ends the drag-and-drop gesture in progress, if any, with no drop, as Esc does.
    ///
    /// Esc reaches the gesture through the widget that has the keyboard focus; a frontend calls this for an Esc no
    /// widget took, so that a drag ends while nothing is focused too.
    /// @return True when a drag was in progress and has ended.
    bool endDrag();

    /// @brief Whether any Busy widget is spinning, so the frontend should keep advancing the animation.
    /// @return True while at least one Busy is active.
    [[nodiscard]] bool animating() const noexcept;

    /// @brief Moves every spinning Busy to its next frame; the next draw shows it.
    void advanceAnimation() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace morph::tui
