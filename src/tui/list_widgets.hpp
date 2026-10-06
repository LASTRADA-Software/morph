// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/List.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// What a list's view asks the widget that owns it.
class ListOwner {
public:
    ListOwner() = default;
    virtual ~ListOwner() = default;
    ListOwner(ListOwner const&) = delete;
    ListOwner& operator=(ListOwner const&) = delete;
    ListOwner(ListOwner&&) = delete;
    ListOwner& operator=(ListOwner&&) = delete;

    /// A key the owner handles before the list does; nullopt lets the list have it. It may end in a handler that
    /// destroys the widget: the view touches nothing after it.
    [[nodiscard]] virtual std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) = 0;
};

/// The view of a list widget: a core::tui::List that keeps the highlighted row and moves it on Up, Down, Home, End,
/// PageUp and PageDown, whose rows @p owner paints, and whose input reaches it only while @p owner is actionable. A
/// key goes to @p lists first. Tab and Esc are left to the frontend and to dialogs: List would report Esc as a
/// cancel. Its class lives in list_widgets.cpp, for the reason `makeView` gives.
[[nodiscard]] std::unique_ptr<::core::tui::List> makeListView(WidgetBase& owner, ListOwner& lists);

/// Select, radio style: every option on its own row, `(•)` marking the selected one; Space, Enter or a click on a
/// row selects. A list with fewer rows than options scrolls to keep the highlighted one in view.
///
/// It keeps the key last requested or chosen and marks the option with that key whenever the options have one, so a
/// key and the options may arrive in either order.
class RadioSelectImpl final : public TuiWidget<ui::SelectWidget>, private ListOwner {
public:
    explicit RadioSelectImpl(Context& context) : TuiWidget{context}, _list{&adopt(makeListView(*this, *this))} {}
    void setOptions(std::vector<ui::SelectOption> const& options) override;
    void setSelected(std::optional<ui::Key> const& selected) override;
    void setOnSelect(std::function<void(ui::Key)> onSelect) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    /// The label of the marked option; empty when none is marked.
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    void activate() override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) override;
    void rebuild();
    void choose(std::size_t index);

    ::core::tui::List* _list;
    std::size_t _top = 0;  ///< The first option shown, as of the last frame.
    std::vector<ui::SelectOption> _options;
    std::optional<ui::Key> _selected;
    std::function<void(ui::Key)> _onSelect;
};

/// Select, dropdown style: one row `[label ▾]`; Enter, Space, Down or a click opens the options as a list overlay
/// below it (above it, when there is more room there), which takes the focus. It opens only from a field drawn in the
/// last frame and only where there is room for a row.
///
/// In the list Enter, Space or a click on an option chooses it, the wheel moves the highlight, Esc closes it, and Tab
/// closes it and leaves the key to the frontend; the focus then goes back to the field. The list closes as well when
/// the focus leaves it, and when the user can no longer reach the select (see `closeUnreachablePopups`); it then takes
/// no input. While its field is not drawn (scrolled out of view, squeezed, taken off the screen) the list is not shown
/// and takes no input, and the next input that reaches it closes it. Like the radio style it keeps the key requested
/// apart from the option it marks.
class DropdownSelectImpl final : public TuiWidget<ui::SelectWidget> {
public:
    explicit DropdownSelectImpl(Context& context) : TuiWidget{context}, _popup{makePopup()} { adopt(makeView(*this)); }
    /// @brief Closes the list, if open, taking the focus off it first.
    ~DropdownSelectImpl() override;
    DropdownSelectImpl(DropdownSelectImpl const&) = delete;
    DropdownSelectImpl& operator=(DropdownSelectImpl const&) = delete;
    DropdownSelectImpl(DropdownSelectImpl&&) = delete;
    DropdownSelectImpl& operator=(DropdownSelectImpl&&) = delete;

    /// Replaces the options; an open list shows the new ones, and closes when there are none.
    void setOptions(std::vector<ui::SelectOption> const& options) override;
    /// Requests the option with @p selected; an open list highlights it when it is among the options.
    void setSelected(std::optional<ui::Key> const& selected) override;
    void setOnSelect(std::function<void(ui::Key)> onSelect) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    /// The label of the marked option; empty when none is marked.
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void closePopup() override;
    /// Whether the option list is open.
    [[nodiscard]] bool isOpen() const noexcept { return _open; }

private:
    class Popup;
    /// Where the list goes: its top left corner, in screen cells, and how many rows it may take there.
    struct Placement {
        ::core::tui::Point origin{};  ///< The top left corner.
        int rows = 0;                 ///< The rows it may take; none when there is no room.
    };
    [[nodiscard]] std::unique_ptr<::core::tui::List> makePopup();
    /// Opens the list; whether it is open and has the focus afterwards. When it is not, this select may be gone.
    bool open();
    void close();
    void choose(std::size_t index);
    /// Gives the list the options and highlights the marked one.
    void fillPopup();
    /// Below the field, or above it where there is more room; moved left as far as it takes to fit the screen.
    [[nodiscard]] Placement placement() const;
    /// Moves the open list to `placement()`, and shows it when it is not shown yet.
    void placePopup();
    [[nodiscard]] ::core::tui::Size popupSize() const;
    /// Whether the field was drawn in the last frame, on the screen.
    [[nodiscard]] bool fieldDrawn() const;
    /// Whether the open list can be used: the user can reach the select, its field was drawn in the last frame, and
    /// the list has room for a row.
    [[nodiscard]] bool listUsable() const;

    std::vector<ui::SelectOption> _options;
    std::optional<ui::Key> _selected;
    std::function<void(ui::Key)> _onSelect;
    std::unique_ptr<::core::tui::List> _popup;  ///< A Popup, shown as an overlay while open.
    ::core::tui::Point _popupAt{};              ///< Where the open list is shown, in screen cells.
    int _popupRows = 0;                         ///< How many rows the open list may take where it is shown.
    std::size_t _popupTop = 0;                  ///< The first option the open list shows, as of the last frame.
    bool _open = false;
};

/// Menu: one list of item labels; Enter, Space or a click on a row activates that item. A list with fewer rows than
/// items scrolls to keep the highlighted one in view.
class MenuImpl final : public TuiWidget<ui::MenuWidget>, private ListOwner {
public:
    explicit MenuImpl(Context& context) : TuiWidget{context}, _list{&adopt(makeListView(*this, *this))} {}
    void setItems(std::vector<std::string> const& items) override;
    void setOnActivate(std::function<void(std::size_t)> onActivate) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    /// The label of the highlighted item.
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    void activate() override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) override;

    ::core::tui::List* _list;
    std::size_t _top = 0;  ///< The first item shown, as of the last frame.
    std::vector<std::string> _labels;
    std::function<void(std::size_t)> _onActivate;
};

/// Tabs: a bar above the body. Left and Right while the tabs hold the focus select the previous and the next tab,
/// and a click on a tab's label selects it and focuses the tabs; a key a focused page widget leaves, and a press on
/// the body, leave the tabs alone.
///
/// The children are the page slots the mount makes, in the order their tabs were first shown, stacked; the mount
/// hides every page but the selected one through its own visible flag, so `setSelected` moves the bar's highlight
/// only. A selection past the last tab highlights none, and Right then selects the first, Left the last.
class TabsImpl final : public TuiContainer<ui::TabsWidget> {
public:
    explicit TabsImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setTabs(std::vector<std::string> const& tabs) override;
    void setSelected(std::size_t index) override;
    void setOnSelect(std::function<void(std::size_t)> onSelect) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] bool focusesOnPress() const override { return false; }
    /// The label of the highlighted tab; empty when none is.
    [[nodiscard]] std::string probeText() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::string labelText(std::size_t index) const;
    void select(std::size_t index);

    std::vector<std::string> _labels;
    std::size_t _selected = 0;
    std::function<void(std::size_t)> _onSelect;
};

}  // namespace morph::tui::detail
