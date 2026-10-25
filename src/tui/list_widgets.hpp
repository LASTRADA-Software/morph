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
class RadioSelectImpl final : public TuiField<ui::SelectWidget>, private ListOwner {
public:
    explicit RadioSelectImpl(Context& context) : TuiField{context}, _list{&adopt(makeListView(*this, *this))} {}
    void setOptions(std::vector<ui::SelectOption> const& options) override;
    void setSelected(std::optional<ui::Key> const& selected) override;
    void setOnSelect(std::function<void(ui::Key)> onSelect) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
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
class DropdownSelectImpl final : public TuiField<ui::SelectWidget> {
public:
    explicit DropdownSelectImpl(Context& context) : TuiField{context}, _popup{makePopup()} { adopt(makeView(*this)); }
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
    /// Calls @p onOpen when the user opens the list, also when there are no options and so no list to show.
    void setOnOpen(ui::Action onOpen) override;
    void setCaption(std::string_view label, bool shown) override { showCaption(label, shown); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    /// The label of the marked option; empty when none is marked.
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void closePopup() override;
    [[nodiscard]] ::core::tui::Component const* openPopup() const override { return _open ? _popup.get() : nullptr; }
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
    ui::Action _onOpen;
    std::unique_ptr<::core::tui::List> _popup;  ///< A Popup, shown as an overlay while open.
    ::core::tui::Point _popupAt{};              ///< Where the open list is shown, in screen cells.
    int _popupRows = 0;                         ///< How many rows the open list may take where it is shown.
    std::size_t _popupTop = 0;                  ///< The first option the open list shows, as of the last frame.
    bool _open = false;
};

/// Menu: the entries of one level as a list, which drills down into submenus. Enter, Space, Right or a click on an
/// entry with a submenu opens it; Left or Backspace goes back to the level above. Enter, Space or a click on an
/// entry without one chooses it, when it and every entry above it are enabled, and shows the top level again. A
/// checkable entry shows its mark, an entry with a submenu a `>`, and an entry's chord shows after its label. A list
/// with fewer rows than entries scrolls to keep the highlighted one in view.
class MenuImpl final : public TuiWidget<ui::MenuWidget>, private ListOwner {
public:
    explicit MenuImpl(Context& context) : TuiWidget{context}, _list{&adopt(makeListView(*this, *this))} {}
    void setItems(std::vector<ui::MenuEntry> const& entries) override;
    void setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    /// The label of the highlighted entry.
    [[nodiscard]] std::string probeText() const override;
    void paint(::core::tui::Canvas& canvas) override;
    void activate() override;
    void click(::core::tui::Point cell) override;
    /// The path of the level shown, an index per level from the top; empty at the top.
    [[nodiscard]] std::vector<std::size_t> const& level() const noexcept { return _path; }
    /// Highlights the entry at @p index of the level shown, when there is one.
    void highlight(std::size_t index);

private:
    [[nodiscard]] std::optional<::core::tui::EventResult> listKey(::core::tui::KeyEvent const& key) override;
    /// The entries of the level shown.
    [[nodiscard]] std::vector<ui::MenuEntry> const& shown() const;
    /// Whether every entry on the path to the level shown is enabled.
    [[nodiscard]] bool levelEnabled() const;
    void showLevel(std::size_t highlighted);
    void enter(std::size_t index);
    void leave();

    ::core::tui::List* _list;
    std::size_t _top = 0;  ///< The first entry shown, as of the last frame.
    std::vector<ui::MenuEntry> _entries;
    std::vector<std::size_t> _path;
    std::function<void(std::vector<std::size_t>)> _onActivate;
};

/// Tabs: a bar above the body. Left and Right while the tabs hold the focus select the previous and the next tab,
/// and a click on a tab's label selects it and focuses the tabs; a key a focused page widget leaves, and a press on
/// the body, leave the tabs alone.
///
/// The children are the page slots the mount makes, in the order their tabs were first shown, stacked; the mount
/// hides every page but the selected one through its own visible flag, so `setSelected` moves the bar's highlight
/// only. A selection past the last tab highlights none, and Right then selects the first, Left the last.
///
/// A bar narrower than its labels scrolls: it draws whole labels from a first visible one, which painting moves so
/// the highlighted label fits, with `‹` at the start and `›` at the end while labels are cut there. A click on a label
/// maps through that offset, and a click on `‹` or `›` selects the previous or next tab, as Left and Right do.
///
/// A hidden bar gives its row to the body and takes no focus: there is no strip to choose a tab on.
class TabsImpl final : public TuiContainer<ui::TabsWidget> {
public:
    explicit TabsImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setTabs(std::vector<std::string> const& tabs) override;
    void setSelected(std::size_t index) override;
    void setOnSelect(std::function<void(std::size_t)> onSelect) override;
    void setBarShown(bool shown) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return _barShown; }
    [[nodiscard]] bool focusesOnPress() const override { return false; }
    /// The label of the highlighted tab; empty when none is.
    [[nodiscard]] std::string probeText() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void click(::core::tui::Point cell) override;

private:
    [[nodiscard]] std::string labelText(std::size_t index) const;
    [[nodiscard]] int labelsWidth(std::size_t first, std::size_t last) const;
    void scrollToSelected(int width);
    void select(std::size_t index);

    // The columns a painted label or marker covers, and what a click there chooses: a label its own tab, a marker
    // the tab before or after the highlighted one (`step` -1 or 1).
    struct Span {
        int start = 0;
        int end = 0;
        std::size_t index = 0;
        int step = 0;
    };

    std::vector<std::string> _labels;
    std::size_t _selected = 0;
    // The first label the bar draws; moved by painting so the highlighted one fits.
    std::size_t _first = 0;
    // What the last paint drew, left to right, for clicks.
    std::vector<Span> _spans;
    std::function<void(std::size_t)> _onSelect;
    bool _barShown = true;
};

}  // namespace morph::tui::detail
