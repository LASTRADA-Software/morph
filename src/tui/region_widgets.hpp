// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Collapsible: a title row, `▾ title` while open and `▸ title` while closed, with the header child beside the
/// title, and the other children stacked below it while open. It takes the focus; Enter or Space while it has it, or
/// a click on the title, opens or closes it and calls `onToggle` with the state asked for. While it is closed its
/// body is hidden and takes no input; the header stays shown and live.
class CollapsibleImpl final : public TuiContainer<ui::CollapsibleWidget> {
public:
    explicit CollapsibleImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setTitle(std::string_view title) override;
    void setHeader(ui::Widget& header) override;
    void setOpen(bool open) override;
    void setOnToggle(std::function<void(bool)> onToggle) override { _onToggle = std::move(onToggle); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] bool focusesOnPress() const override { return false; }
    [[nodiscard]] std::string probeText() const override { return _title; }
    [[nodiscard]] bool letsChildrenAct() const override { return _open; }
    [[nodiscard]] bool letsChildAct(WidgetBase const& child) const override { return _open || &child == _header; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void click(::core::tui::Point cell) override;
    [[nodiscard]] bool isOpen() const noexcept { return _open; }

protected:
    void childAttached(WidgetBase& child) override;
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] std::string heading() const;
    [[nodiscard]] std::vector<WidgetBase*> body() const;
    void syncChildren();

    std::string _title;
    WidgetBase* _header = nullptr;
    bool _open = true;
    std::function<void(bool)> _onToggle;
};

/// Splitter: the panes along its axis, a handle (`│` between side-by-side panes, `─` between stacked ones) after
/// each but the last. Dragging a handle with the pointer moves it, showing the new sizes as it goes; the release
/// calls `onResize` with every pane's size. A pane is at least one cell.
class SplitterImpl final : public TuiContainer<ui::SplitterWidget> {
public:
    SplitterImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} { adopt(makeView(*this)); }
    void setSizes(std::vector<int> const& sizes) override;
    void setOnResize(std::function<void(std::vector<int>)> onResize) override { _onResize = std::move(onResize); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool takesPress() const override { return true; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    void pressAt(::core::tui::Point cell) override;
    void dragTo(::core::tui::Point cell) override;
    [[nodiscard]] bool pressEnded() override;
    /// Every pane's extent along the axis, as last laid out.
    [[nodiscard]] std::vector<int> const& extents() const noexcept { return _extents; }

private:
    [[nodiscard]] std::vector<int> layoutExtents(int total) const;
    [[nodiscard]] int along(::core::tui::Point cell) const noexcept;

    ui::Axis _axis;
    std::vector<int> _sizes;
    mutable std::vector<int> _extents;  ///< The panes' extents as last laid out.
    std::optional<std::size_t> _dragging;
    bool _moved = false;
    std::function<void(std::vector<int>)> _onResize;
};

/// Drop zone: its children stacked, or `Drop files here` while it has none, in a dashed frame. A terminal reports a
/// file dragged onto it as the file's path pasted, so the zone takes the focus, and a paste while it has it is a
/// drop: the pasted text read as paths (`parsePastedPaths`). A drop with a file outside the accepted extensions, or
/// with several files when only one is taken, is refused whole.
class DropZoneImpl final : public TuiContainer<ui::DropZoneWidget> {
public:
    explicit DropZoneImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setAccept(std::vector<std::string> const& extensions) override { _accept = extensions; }
    void setMultiple(bool multiple) override { _multiple = multiple; }
    void setOnDrop(std::function<void(std::vector<std::string>)> onDrop) override { _onDrop = std::move(onDrop); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] bool focusesOnPress() const override { return true; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult paste(std::string_view text) override;
    /// Applies the rules to a drop of @p paths and calls `onDrop` when it takes it. The handler may destroy the zone.
    void drop(std::vector<std::string> paths);

private:
    [[nodiscard]] bool takes(std::string_view path) const;

    std::vector<std::string> _accept;
    bool _multiple = false;
    std::function<void(std::vector<std::string>)> _onDrop;
};

/// The paths in text a terminal pasted for files dropped onto it: separated by whitespace, each possibly quoted with
/// `'` or `"`, or with spaces escaped by `\`, or written as a `file://` URI whose `%XX` escapes are decoded.
[[nodiscard]] std::vector<std::string> parsePastedPaths(std::string_view text);

}  // namespace morph::tui::detail
