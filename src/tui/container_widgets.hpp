// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <morph/ui/backend.hpp>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Column or Row (and ForEach, and a Table row): children along one axis, with a gap.
class StackImpl final : public TuiContainer<ui::StackWidget> {
public:
    StackImpl(Context& context, ui::Axis axis);
    void setGap(int gap) override;
    [[nodiscard]] ui::Axis axis() const noexcept { return _axis; }
    [[nodiscard]] int gap() const noexcept { return _gap; }
    /// Leading shown children given no area: what a Scroll around this stack has scrolled past.
    void setSkip(std::size_t skip);
    [[nodiscard]] std::size_t skip() const noexcept { return _skip; }
    /// Fixed main-axis extents and their gap, for a Table row; an empty list returns the row to the solver.
    void setColumnLayout(std::vector<int> extents, int gap);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    ui::Axis _axis;
    int _gap = 0;
    std::size_t _skip = 0;
    std::vector<int> _extents;
    int _extentGap = 0;
};

/// The slot a Switch case or a Tabs page mounts into: its children stacked vertically, no gap.
class SlotImpl final : public TuiContainer<ui::SlotWidget> {
public:
    explicit SlotImpl(Context& context);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    void paint(::core::tui::Canvas& canvas) override;
};

}  // namespace morph::tui::detail
