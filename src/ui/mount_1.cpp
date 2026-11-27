// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/mount.hpp>.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/stretch_columns.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

Windowing windowingOf(Virtualization const& virtualization, std::size_t rows, std::size_t visibleColumns,
                      bool serverMode) {
    // rows × columns > cellThreshold, compared as a quotient so no product overflows.
    bool const overCells = visibleColumns != 0 && rows > virtualization.cellThreshold / visibleColumns;
    return Windowing{.rows = serverMode || rows >= virtualization.threshold || overCells,
                     .columns = visibleColumns > virtualization.columnThreshold || overCells};
}

void ColumnExtents::assign(std::span<int const> widths) {
    _widths.assign(widths.begin(), widths.end());
    for (int& width : _widths) {
        width = std::max(width, 0);
    }
    _user.assign(_widths.size(), false);
    _tree.assign(_widths.size() + 1, 0);
    for (std::size_t at = 0; at < _widths.size(); ++at) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at` runs below `_widths.size()`, the loop condition above
        add(at, _widths[at]);
    }
}

bool ColumnExtents::grow(std::size_t column, int width) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column >= _widths.size()` returns first, and `assign` sizes `_user` with `_widths`
    if (column >= _widths.size() || _user[column] || width <= _widths[column]) {
        return false;
    }
    add(column, width - _widths[column]);
    _widths[column] = width;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return true;
}

bool ColumnExtents::fit(std::size_t column, int width) {
    width = std::max(width, 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column >= _widths.size()` returns first, and `assign` sizes `_user` with `_widths`
    if (column >= _widths.size() || _user[column] || width == _widths[column]) {
        return false;
    }
    add(column, width - _widths[column]);
    _widths[column] = width;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return true;
}

void ColumnExtents::set(std::size_t column, int width) {
    if (column >= _widths.size()) {
        return;
    }
    width = std::max(width, 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column >= _widths.size()` returned above, and `assign` sizes `_user` with `_widths`
    add(column, width - _widths[column]);
    _widths[column] = width;
    _user[column] = true;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

std::pair<std::size_t, std::size_t> ColumnExtents::columnsAt(int offset, int viewport) const {
    std::size_t const count = _widths.size();
    std::size_t const first = std::min(below(std::max(offset, 0)), count);
    std::size_t last = first;
    if (viewport > 0) {
        int const end = std::max(offset, 0) + viewport - 1;
        last = std::min(below(end) + 1, count);
    }
    return {first, std::max(first, last)};
}

int ColumnExtents::offsetOf(std::size_t column) const {
    std::size_t index = std::min(column, _widths.size());
    int sum = 0;
    for (; index > 0; index -= index & (~index + 1)) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `index` starts at most at `_widths.size()`, one below `_tree.size()`, and only descends
        sum += _tree[index];
    }
    return sum;
}

int ColumnExtents::widthOf(std::size_t column) const {
    ++_reads;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): guarded by `column < _widths.size()` in the same expression
    return column < _widths.size() ? _widths[column] : 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`column`, `delta`): called from `assign`, `grow`, `fit` and `set`, each passing the column first and the signed width change second
void ColumnExtents::add(std::size_t column, int delta) {
    for (std::size_t at = column + 1; at < _tree.size(); at += at & (~at + 1)) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at` stays below `_tree.size()` by the loop condition
        _tree[at] += delta;
    }
}

std::size_t ColumnExtents::below(int offset) const {
    std::size_t index = 0;
    std::size_t step = std::bit_floor(std::max<std::size_t>(_tree.size(), 1));
    int left = offset;
    for (; step > 0; step >>= 1U) {
        std::size_t const next = index + step;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `next < _tree.size()` is the first operand of this condition
        if (next < _tree.size() && _tree[next] <= left) {
            index = next;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): reached only under `next < _tree.size()`, tested on the line above
            left -= _tree[next];
        }
    }
    return index;
}

ColumnWindow columnWindow(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`pinned`, `scrollX`): a column count then a scroll offset, passed by name from the table state at the one call site
    ColumnExtents const& extents, std::size_t pinned, int scrollX,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`viewport`, `overscan`): a width then a column count, passed by name from the table state at the one call site
    int viewport, std::size_t overscan) {
    ColumnWindow window;
    std::size_t const count = extents.size();
    pinned = std::min(pinned, count);
    auto [first, last] = extents.columnsAt(scrollX, viewport);
    first = first > overscan ? first - overscan : 0;
    last = std::min(count, last + overscan);
    first = std::max(first, pinned);
    for (std::size_t at = 0; at < pinned; ++at) {
        window.columns.push_back(at);
    }
    for (std::size_t at = first; at < last; ++at) {
        window.columns.push_back(at);
    }
    window.leading = first < last ? extents.offsetOf(first) - extents.offsetOf(pinned) : 0;
    window.trailing = extents.totalWidth() - extents.offsetOf(std::max(first, last));
    return window;
}

}  // namespace morph::ui

namespace morph::ui::detail {

// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
void appendBandRuns(std::span<table::BandInfo const> bands, std::span<std::string const> visible, std::size_t pinned,
                    std::size_t level, std::vector<BandRun>& out) {
    for (table::BandInfo const& band : bands) {
        std::optional<std::size_t> first;
        std::size_t count = 0;
        auto const close = [&] {
            if (first) {
                out.push_back(BandRun{.level = level,
                                      .label = band.label,
                                      .unit = band.unit,
                                      .first = *first,
                                      .count = count,
                                      .pinned = *first < pinned});
            }
            first.reset();
            count = 0;
        };
        for (std::size_t at = 0; at < visible.size(); ++at) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at` is below `visible.size()`, the loop condition
            bool const own = std::ranges::contains(band.columns, visible[at]);
            // A run ends at a foreign column, and at the pinned edge.
            if (!own || (at == pinned && first)) {
                close();
            }
            if (own) {
                first = first.value_or(at);
                ++count;
            }
        }
        close();
        appendBandRuns(band.bands, visible, pinned, level + 1, out);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
void enclosingBands(std::span<table::BandInfo const> bands, std::string_view columnId,
                    std::vector<table::BandInfo const*>& out) {
    for (table::BandInfo const& band : bands) {
        if (std::ranges::contains(band.columns, columnId)) {
            out.push_back(&band);
            enclosingBands(band.bands, columnId, out);
            return;
        }
    }
}

}  // namespace morph::ui::detail

namespace morph::ui {

std::vector<BandRun> bandRuns(std::span<table::BandInfo const> bands, std::span<std::string const> visible,
                              std::size_t pinned) {
    std::vector<BandRun> out;
    detail::appendBandRuns(bands, visible, pinned, 0, out);
    std::ranges::stable_sort(out, [](BandRun const& left, BandRun const& right) {
        return left.level != right.level ? left.level < right.level : left.first < right.first;
    });
    return out;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`columnId`, `unit`): a column id then its unit id, both read from the same TableColumn at every call
bool columnUnitShown(std::span<table::BandInfo const> bands, std::string_view columnId, std::string_view unit) {
    if (unit.empty()) {
        return true;
    }
    std::vector<table::BandInfo const*> enclosing;
    detail::enclosingBands(bands, columnId, enclosing);
    return std::ranges::none_of(enclosing, [unit](table::BandInfo const* band) { return band->unit == unit; });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`columnId`, `unit`): a column id then its unit id, both read from the same TableColumn at every call
std::string bandedHeaderName(std::span<table::BandInfo const> bands, std::string_view columnId,
                             std::string_view label) {
    std::vector<table::BandInfo const*> enclosing;
    detail::enclosingBands(bands, columnId, enclosing);
    std::string out;
    for (table::BandInfo const* band : enclosing) {
        out += band->label + ", ";
    }
    return out + std::string{label};
}

std::size_t cutColumns(CellSpan const& span, std::span<std::string const> declared,
                       std::span<std::string const> visible) {
    auto const shown = std::ranges::find(visible, span.column);
    if (shown == visible.end()) {
        return 0;
    }
    auto const own = std::ranges::find(declared, span.column);
    if (own == declared.end()) {
        return 1;
    }
    auto const place = static_cast<std::size_t>(shown - visible.begin());
    auto const order = static_cast<std::size_t>(own - declared.begin());
    std::size_t count = 1;
    while (
        count < span.columns && place + count < visible.size() && order + count < declared.size() &&
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `place + count < visible.size()` and `order + count < declared.size()` are earlier operands of the same condition
        visible[place + count] == declared[order + count]) {
        ++count;
    }
    return count;
}

}  // namespace morph::ui

namespace morph::ui::detail {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a span.
ResolvedSpans resolveRowSpans(std::vector<SpanRow> const& rows, std::size_t columns,
                              std::span<std::optional<std::size_t> const> placeOf) {
    ResolvedSpans out;
    out.effective.resize(rows.size());
    auto const placeOfColumn = [&placeOf](std::size_t column) -> std::optional<std::size_t> {
        if (placeOf.empty()) {
            return column;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): guarded by `column < placeOf.size()` in the same expression
        return column < placeOf.size() ? placeOf[column] : std::nullopt;
    };
    // Until which row each visible place is covered.
    std::vector<std::size_t> coveredUntil(columns, 0);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        SpanRow const& row = rows.at(index);
        std::vector<bool> named(columns, false);
        for (RowSpan const& span : row.requested) {
            auto const refuse = [&](char const* const site, std::string_view why) {
                out.refused.emplace_back(
                    site, row.name + ": column " + std::to_string(span.column) + " " + std::string{why});
            };
            if (span.column >= columns || span.rows == 0 || span.columns == 0 || row.heading ||
                named.at(span.column)) {
                refuse(site::kRowSpanInvalid, "is not a span of this table");
                continue;
            }
            named.at(span.column) = true;
            std::optional<std::size_t> const place = placeOfColumn(span.column);
            if (!place || *place >= columns) {
                continue;
            }
            std::size_t const wide = std::min(span.columns, columns - *place);
            if (coveredUntil.at(*place) > index) {
                refuse(site::kRowSpanOverlap, "starts under another span");
                continue;
            }
            if (span.rows == 1 && wide == 1) {
                continue;
            }
            if (std::any_of(coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place),
                            coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place + wide),
                            [index](std::size_t until) { return until > index; })) {
                refuse(site::kRowSpanOverlap, "covers a cell another span covers");
                continue;
            }
            std::size_t const end = index + span.rows;
            bool const crossesHeading =
                end <= rows.size() && std::any_of(rows.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                                                  rows.begin() + static_cast<std::ptrdiff_t>(end),
                                                  [](SpanRow const& covered) { return covered.heading; });
            if (end > rows.size() || crossesHeading) {
                refuse(site::kRowSpanOverrun, "runs past the last row or over a heading");
                continue;
            }
            out.effective.at(index).push_back(span);
            std::fill(coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place),
                      coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place + wide), end);
        }
        std::ranges::sort(out.effective.at(index), {}, &RowSpan::column);
    }
    return out;
}

KeyedRows::~KeyedRows() {
    while (!list.empty()) {
        list.pop_back();
    }
}

std::vector<bool> stableTargets(std::vector<std::size_t> const& sequence) {
    constexpr auto kNone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> tails;  // tails.at(n - 1): where the smallest tail of a run of length n sits
    std::vector<std::size_t> previous(sequence.size(), kNone);
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        auto const place = std::ranges::lower_bound(tails, sequence.at(i), {},
                                                    [&sequence](std::size_t entry) { return sequence.at(entry); });
        auto const length = static_cast<std::size_t>(place - tails.begin());
        if (length > 0) {
            previous.at(i) = tails.at(length - 1);
        }
        if (length == tails.size()) {
            tails.push_back(i);
        } else {
            tails.at(length) = i;
        }
    }
    std::vector<bool> stable(sequence.size(), false);
    for (std::size_t cursor = tails.empty() ? kNone : tails.back(); cursor != kNone; cursor = previous.at(cursor)) {
        stable.at(sequence.at(cursor)) = true;
    }
    return stable;
}

void reorderChildren(ContainerWidget& container, std::vector<Widget*>& current, std::vector<Widget*> const& target) {
    std::unordered_map<Widget const*, std::size_t> targetOf;
    for (std::size_t i = 0; i < target.size(); ++i) {
        targetOf.emplace(target.at(i), i);
    }
    std::vector<std::size_t> sequence;
    sequence.reserve(current.size());
    for (Widget const* const child : current) {
        sequence.push_back(targetOf.at(child));
    }
    std::vector<bool> const stable = stableTargets(sequence);
    auto const indexOf = [&current](Widget const* child) {
        return static_cast<std::size_t>(std::ranges::find(current, child) - current.begin());
    };
    // From the back: each moved child goes directly before its successor in the target order, which is already
    // in place, so one move per unstable child suffices.
    for (std::size_t position = target.size(); position-- > 0;) {
        if (stable.at(position)) {
            continue;
        }
        Widget* const moving = target.at(position);
        std::size_t const from = indexOf(moving);
        std::size_t destination = current.size() - 1;
        if (position + 1 < target.size()) {
            std::size_t const before = indexOf(target.at(position + 1));
            destination = from < before ? before - 1 : before;
        }
        if (destination == from) {
            continue;
        }
        container.moveChild(*moving, destination);
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(from));
        current.insert(current.begin() + static_cast<std::ptrdiff_t>(destination), moving);
    }
}

Widget* Mounter::mount(reactive::Scope& scope, Node const& node, ContainerWidget* parent) {
    if (!node) {
        return nullptr;
    }
    return std::visit(
        [&](auto const& spec) -> Widget* {
            if constexpr (requires { spec.common.layout.alignSelf; }) {
                checkPlacement(spec.common, parent);
                checkShare(spec.common, parent);
            }
            Widget& widget = this->mountKind(scope, spec, parent);
            link(scope, widget, parent, takesFocus(spec));
            return &widget;
        },
        node->kind);
}

bool Mounter::focusAlong(std::string_view focusId, bool forward) {
    auto const found = _focusable.lower_bound(focusId);
    if (found == _focusable.end() || found->first != focusId) {
        return false;
    }
    return focusAlong(*found->second, forward);
}

bool Mounter::focusAlong(Widget const& from, bool forward) {
    // The chain of the mount the widget is in: Tab in a dialog stays in the dialog.
    Widget const* top = &from;
    for (auto found = _chain.find(top); found != _chain.end() && found->second.parent != nullptr;
         found = _chain.find(top)) {
        top = found->second.parent;
    }
    bool const rooted = std::ranges::contains(_chainRoots, top);
    std::vector<std::pair<Widget*, bool>> order;
    for (Widget* const root : _chainRoots) {
        if (root == top || !rooted) {
            chainOrder(*root, true, order);
        }
    }
    auto const hit = std::ranges::find(order, &from, [](auto const& entry) { return entry.first; });
    if (hit == order.end()) {
        return false;
    }
    auto const index = static_cast<std::size_t>(hit - order.begin());
    for (std::size_t step = 1; step < order.size(); ++step) {
        if (!forward && step > index) {
            break;
        }
        std::size_t const next = forward ? index + step : index - step;
        if (next >= order.size()) {
            break;
        }
        if (order.at(next).second) {
            try {
                order.at(next).first->focus();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Focus is a request: a backend that cannot move it leaves it where it was.
            }
            return true;
        }
    }
    return false;
}

bool Mounter::focus(std::string_view focusId) {
    // lower_bound, not find: of equal names, find may answer any; lower_bound answers the first inserted.
    auto const found = _focusable.lower_bound(focusId);
    if (found == _focusable.end() || found->first != focusId) {
        return false;
    }
    try {
        found->second->focus();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Focus is a request: a backend that cannot move it leaves it where it was.
    }
    return true;
}

bool Mounter::placeCursor(std::string_view focusId, std::size_t offset) {
    auto const found = _focusable.lower_bound(focusId);
    if (found == _focusable.end() || found->first != focusId) {
        return false;
    }
    auto* const field = dynamic_cast<TextInputWidget*>(found->second);
    if (field == nullptr) {
        return false;
    }
    field->setCursor(offset);
    try {
        field->focus();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Focus is a request: a backend that cannot move it leaves it where it was.
    }
    // After the focus: a scroll that follows focus has moved to the field as a whole by now, and the cursor's
    // line is what is then brought into view.
    field->revealCursor();
    return true;
}

ContainerWidget* Mounter::overlayFooter() const {
    if (auto* const dialog = dynamic_cast<DialogWidget*>(_overlay)) {
        return &dialog->footer();
    }
    if (auto* const drawer = dynamic_cast<DrawerWidget*>(_overlay)) {
        return &drawer->footer();
    }
    return nullptr;
}

void Mounter::checkLayout(Common const& common) {
    LayoutHints const& hints = common.layout;
    auto const refuse = [&common](std::string const& what) {
        throw std::logic_error{"morph::ui::mount: " + what + " on the node" +
                               (common.testId.empty() ? std::string{} : " `" + common.testId + "`")};
    };
    auto const check = [&refuse](Sizing sizing, std::optional<int> minimum, std::optional<int> maximum,
                                 std::string const& axis) {
        if (minimum.value_or(0) < 0 || maximum.value_or(0) < 0) {
            refuse("a negative " + axis + " bound");
        }
        if (minimum && maximum && *minimum > *maximum) {
            refuse("a minimum " + axis + " above its maximum");
        }
        if (sizing.kind == Sizing::Kind::Fixed &&
            (sizing.amount < minimum.value_or(sizing.amount) || sizing.amount > maximum.value_or(sizing.amount))) {
            refuse("a fixed " + axis + " outside its bounds");
        }
    };
    check(hints.width, hints.minWidth, hints.maxWidth, "width");
    check(hints.height, hints.minHeight, hints.maxHeight, "height");
    if (hints.width.kind == Sizing::Kind::Percent && (hints.width.amount < 1 || hints.width.amount > 100)) {
        refuse("a percent width outside 1 to 100");
    }
    if (hints.height.kind == Sizing::Kind::Percent) {
        refuse("a percent height; a percent sizes a width");
    }
}

void Mounter::checkShare(Common const& common, ContainerWidget const* parent) const {
    if (common.layout.width.kind == Sizing::Kind::Percent && !_shareParents.contains(parent)) {
        std::string const named = common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)";
        throw std::logic_error{
            "morph::ui::mount: `percent` sizes a width along a row or a horizontal splitter; "
            "nothing here takes a share" +
            named};
    }
}

void Mounter::checkNoShare(std::span<GridTrack const> tracks) {
    if (std::ranges::any_of(tracks,
                            [](GridTrack const& track) { return track.width.kind == Sizing::Kind::Percent; })) {
        throw std::logic_error{
            "morph::ui::mount: a grid track is sized by the grid; `percent` sizes a width "
            "along a row or a horizontal splitter"};
    }
}

void Mounter::checkNoShare(std::span<TableColumn const> columns) {
    if (std::ranges::any_of(columns,
                            [](TableColumn const& column) { return column.width.kind == Sizing::Kind::Percent; })) {
        throw std::logic_error{
            "morph::ui::mount: a table column is sized by the table; `percent` sizes a width "
            "along a row or a horizontal splitter"};
    }
}

void Mounter::checkOverlayLayout(Common const& common, std::string_view kind) {
    LayoutHints const& hints = common.layout;
    auto const refuse = [&common, kind](std::string const& axis) {
        throw std::logic_error{"morph::ui::mount: a " + std::string{kind} +
                               " is sized by the frontend, not by its container; its " + axis + " cannot stretch" +
                               (common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)")};
    };
    if (hints.width.kind == Sizing::Kind::Stretch) {
        refuse("width");
    }
    if (hints.width.kind == Sizing::Kind::Percent) {
        throw std::logic_error{"morph::ui::mount: a " + std::string{kind} +
                               " is sized by the frontend, not by its container; its width takes no share"};
    }
    if (hints.height.kind == Sizing::Kind::Stretch) {
        refuse("height");
    }
}

void Mounter::checkPlacement(Common const& common, ContainerWidget const* parent) {
    if (common.layout.alignSelf && dynamic_cast<StackWidget const*>(parent) == nullptr &&
        dynamic_cast<GridWidget const*>(parent) == nullptr) {
        std::string const named = common.testId.empty() ? std::string{} : " `" + common.testId + "`";
        throw std::logic_error{"morph::ui::mount: `alignSelf` places a node in its row, column or grid, and the node" +
                               named + " is not in one"};
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): applies each common property (visible, enabled, tooltip, keys and so on) to the widget; each binds against the same scope
void Mounter::applyCommon(reactive::Scope& scope, Widget& widget, Common const& common) {
    // The focus chain passes over a hidden or disabled widget and what is inside it.
    ChainFlags const* const flags = common.visible.isBound() || !common.visible.constant() ||
                                            common.enabled.isBound() || !common.enabled.constant()
                                        ? &scope.make<ChainFlags>(_chainFlags, &widget)
                                        : nullptr;
    if (common.visible.isBound() || !common.visible.constant()) {
        bind(scope, common.visible, [&widget, flags](bool shown) {
            flags->set(true, shown);
            widget.setVisible(shown);
        });
    }
    if (common.enabled.isBound() || !common.enabled.constant()) {
        bind(scope, common.enabled, [&widget, flags](bool enabled) {
            flags->set(false, enabled);
            widget.setEnabled(enabled);
        });
    }
    if (common.layout != LayoutHints{}) {
        checkLayout(common);
        widget.setLayout(common.layout);
    }
    if (common.dragKey.isBound() || common.dragKey.constant().has_value()) {
        bind(scope, common.dragKey, [&widget](std::optional<Key> const& key) { widget.setDragKey(key); });
    }
    if (common.onDrop) {
        applyDrop(widget, common);
    }
    if (common.a11y.name.isBound() || !common.a11y.name.constant().empty()) {
        bind(scope, common.a11y.name, [&widget](std::string const& name) { widget.setAccessibleName(name); });
    }
    if (!common.a11y.role.empty()) {
        widget.setAccessibleRole(common.a11y.role);
    }
    if (!common.testId.empty()) {
        widget.setTestId(common.testId);
    }
    if (common.tooltip.isBound() || !common.tooltip.constant().empty()) {
        bind(scope, common.tooltip, [&widget](std::string const& text) { widget.setTooltip(text); });
    }
    if (common.surface.isBound() || !common.surface.constant().empty()) {
        bind(scope, common.surface, [&widget](std::string const& surface) { widget.setSurface(surface); });
    }
    if (!common.keys.empty()) {
        applyKeys(widget, common.keys);
    }
    if (common.autofocus && _autofocus == nullptr) {
        _autofocus = &widget;
    }
    if (common.focusId.isBound()) {
        // Registered under the name it has now, and again under each new one.
        auto& focusable = scope.make<Focusable>(_focusable, std::nullopt);
        bind(scope, common.focusId, [this, &focusable, &widget](std::string const& name) {
            focusable.reset(name.empty() ? std::nullopt : std::optional{_focusable.emplace(name, &widget)});
        });
    } else if (!common.focusId.constant().empty()) {
        scope.make<Focusable>(_focusable, _focusable.emplace(common.focusId.constant(), &widget));
    }
}

void Mounter::ChainFlags::set(bool visible, bool value) const {
    auto& entry = flags->at(of);
    (visible ? entry.first : entry.second) = value;
}

void Mounter::link(reactive::Scope& scope, Widget& widget, ContainerWidget const* parent, bool focusable) {
    // A source table's cells, and an editor in one, are inside the table, which is one stop of the chain.
    if (parent != nullptr && _offChain.contains(parent)) {
        return;
    }
    ChainEntry& entry = _chain[&widget];
    if (entry.linked) {
        entry.takesFocus = entry.takesFocus || focusable;
        return;
    }
    entry.linked = true;
    entry.parent = parent;
    entry.takesFocus = focusable;
    if (parent != nullptr) {
        _chain[parent].children.push_back(&widget);
    } else {
        _chainRoots.push_back(&widget);
    }
    scope.make<ChainLink>(*this, &widget);
}

void Mounter::unlink(Widget* widget) noexcept {
    auto const found = _chain.find(widget);
    if (found == _chain.end()) {
        return;
    }
    Widget const* const parent = found->second.parent;
    found->second.linked = false;
    found->second.parent = nullptr;
    if (found->second.children.empty()) {
        _chain.erase(found);
    }
    std::erase(_chainRoots, widget);
    if (parent == nullptr) {
        return;
    }
    if (auto const above = _chain.find(parent); above != _chain.end()) {
        std::erase(above->second.children, widget);
        if (!above->second.linked && above->second.children.empty()) {
            _chain.erase(above);
        }
    }
}

void Mounter::reorderChain(Widget const& container, std::vector<Widget*> const& order) {
    auto const found = _chain.find(&container);
    if (found == _chain.end()) {
        return;
    }
    std::vector<Widget*>& children = found->second.children;
    std::vector<Widget*> next;
    next.reserve(children.size());
    std::ranges::copy_if(order, std::back_inserter(next),
                         [&children](Widget const* child) { return std::ranges::contains(children, child); });
    std::ranges::copy_if(children, std::back_inserter(next),
                         [&next](Widget const* child) { return !std::ranges::contains(next, child); });
    children = std::move(next);
}

// NOLINTNEXTLINE(misc-no-recursion): a container's children are walked in turn.
void Mounter::chainOrder(Widget& widget, bool shown, std::vector<std::pair<Widget*, bool>>& out) const {
    bool here = shown;
    if (auto const flags = _chainFlags.find(&widget); flags != _chainFlags.end()) {
        here = here && flags->second.first && flags->second.second;
    }
    auto const found = _chain.find(&widget);
    if (found == _chain.end()) {
        return;
    }
    if (found->second.takesFocus) {
        out.emplace_back(&widget, here);
    }
    for (Widget* const child : found->second.children) {
        chainOrder(*child, here, out);
    }
}

void Mounter::Focusable::reset(std::optional<FocusMap::iterator> next) noexcept {
    if (entry) {
        map->erase(*entry);
    }
    entry = next;
}

void Mounter::applyDrop(Widget& widget, Common const& common) {
    std::function<bool(Key const&)> accepts = [](Key const&) { return true; };
    if (common.accepts) {
        accepts = [runtime = _rt, predicate = common.accepts](Key const& key) {
            bool accepted = false;
            static_cast<void>(runCallback(*runtime, [&] { accepted = predicate(key); }));
            return accepted;
        };
    }
    widget.setDropHandler(std::move(accepts), event(common.onDrop));
}

void Mounter::applyKeys(Widget& widget, std::vector<KeyBinding> const& keys) {
    std::vector<std::string> chords;
    chords.reserve(keys.size());
    for (KeyBinding const& binding : keys) {
        chords.push_back(binding.chord);
    }
    widget.setKeys(chords, [runtime = _rt, keys](std::string const& chord) {
        auto const bound = std::ranges::find(keys, chord, &KeyBinding::chord);
        if (bound != keys.end() && bound->onPress) {
            static_cast<void>(runCallback(*runtime, bound->onPress));
        }
    });
}

void Mounter::mountChildren(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container) {
    for (Node const& child : children) {
        static_cast<void>(mount(scope, child, &container));
    }
}

Widget& Mounter::mountStack(reactive::Scope& scope, Axis axis, std::vector<Node> const& children, int gap,
                            Common const& common, ContainerWidget* parent, Align align, Justify justify, bool wrap,
                            bool divided, Padding padding, std::optional<Prop<bool>> const& selected, bool holder) {
    StackWidget& widget = scope.adopt(_backend->createStack(parent, axis));
    if (holder) {
        widget.setPassesBleed(true);
    }
    if (axis == Axis::Horizontal && !wrap && dynamic_cast<TableWidget const*>(parent) == nullptr) {
        scope.make<ShareParent>(_shareParents, &widget);
    }
    if (wrap) {
        widget.setWrap(true);
    }
    if (divided) {
        widget.setDivided(true);
    }
    if (Padding const inset = clampedPadding(padding); inset != Padding{}) {
        widget.setPadding(inset);
    }
    applyCommon(scope, widget, common);
    if (selected) {
        bind(scope, *selected, [&widget](bool chosen) { widget.setSelected(chosen); });
    }
    widget.setGap(spacingStep(gap));
    if (align != Align::Stretch) {
        widget.setAlign(align);
    }
    if (justify != Justify::Start) {
        widget.setJustify(justify);
    }
    if (startsHidden(common) && dynamic_cast<TableWidget const*>(parent) == nullptr) {
        mountWhenShown(scope, children, widget, common.visible);
    } else {
        mountChildren(scope, children, widget);
    }
    return widget;
}

void Mounter::mountWhenShown(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container,
                             Prop<bool> const& visible) {
    if (!visible.isBound()) {
        return;
    }
    reactive::Scope& content = scope.child();
    bool& mounted = scope.make<bool>(false);
    bind(scope, visible, [this, &content, &container, &mounted, children](bool shown) {
        if (!shown || mounted) {
            return;
        }
        mounted = true;
        _rt->untracked([&] {
            mountPass([&] {
                mountChildren(content, children, container);
                return 0;
            });
        });
    });
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<MenuEntry> Mounter::shownLevel(std::vector<MenuItem> const& items, MenuLevelAt const& where,
                                           std::map<MenuPath, MenuPath>& sources) {
    std::vector<MenuEntry> entries;
    auto const record = [&](std::size_t source) {
        MenuPath shownPath = where.shown;
        shownPath.push_back(entries.size() - 1);
        MenuPath sourcePath = where.source;
        sourcePath.push_back(source);
        sources.insert_or_assign(std::move(shownPath), std::move(sourcePath));
    };
    for (std::size_t source = 0; source < items.size(); ++source) {
        MenuItem const& item = items.at(source);
        if (item.separator) {
            if (!entries.empty() && !entries.back().separator) {
                entries.push_back(MenuEntry{.separator = true});
                record(source);
            }
            continue;
        }
        if (!item.visible.evaluate()) {
            continue;
        }
        MenuLevelAt child = where;
        child.shown.push_back(entries.size());
        child.source.push_back(source);
        entries.push_back(MenuEntry{.label = item.label.evaluate(),
                                    .icon = item.icon,
                                    .keys = item.keys,
                                    .checked = item.checked.evaluate(),
                                    .enabled = item.enabled.evaluate(),
                                    .items = shownLevel(item.items, child, sources),
                                    .description = item.description.evaluate()});
        record(source);
    }
    if (!entries.empty() && entries.back().separator) {
        MenuPath last = where.shown;
        last.push_back(entries.size() - 1);
        sources.erase(last);
        entries.pop_back();
    }
    return entries;
}

Mounter::ShownMenu Mounter::shownOf(std::vector<MenuItem> const& items) {
    ShownMenu shown;
    shown.entries = shownLevel(items, MenuLevelAt{}, shown.sources);
    return shown;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
bool Mounter::entryBound(MenuItem const& item) {
    return item.label.isBound() || item.description.isBound() || item.checked.isBound() || item.enabled.isBound() ||
           item.visible.isBound() || anyBound(item.items);
}

MenuItem const* Mounter::itemAt(std::vector<MenuItem> const& items, std::map<MenuPath, MenuPath> const& sources,
                                MenuPath const& path) {
    auto const found = sources.find(path);
    if (found == sources.end()) {
        return nullptr;
    }
    std::vector<MenuItem> const* level = &items;
    MenuItem const* item = nullptr;
    for (std::size_t const index : found->second) {
        if (index >= level->size()) {
            return nullptr;
        }
        item = &level->at(index);
        level = &item->items;
    }
    return item == nullptr || item->separator ? nullptr : item;
}

void Mounter::applyField(reactive::Scope& scope, FieldWidget& widget, FieldState const& field) {
    if (field.readonly.isBound() || field.readonly.constant()) {
        bind(scope, field.readonly, [&widget](bool readonly) { widget.setReadOnly(readonly); });
    }
    if (field.required.isBound() || field.required.constant()) {
        bind(scope, field.required, [&widget](bool required) { widget.setRequired(required); });
    }
    if (field.errors.isBound() || !field.errors.constant().empty()) {
        bind(scope, field.errors, [&widget](std::vector<std::string> const& errors) { widget.setErrors(errors); });
    }
    if (field.stale.isBound() || field.stale.constant()) {
        bind(scope, field.stale, [&widget](bool stale) { widget.setStale(stale); });
    }
    if (field.description.isBound() || !field.description.constant().empty()) {
        bind(scope, field.description,
             [&widget](std::string const& description) { widget.setDescription(description); });
    }
    if (field.labelHint.isBound() || !field.labelHint.constant().empty()) {
        bind(scope, field.labelHint, [&widget](std::string const& hint) { widget.setCaptionHint(hint); });
    }
    if (field.tone.isBound() || field.tone.constant() != Tone::Neutral) {
        bind(scope, field.tone, [&widget](Tone tone) { widget.setTone(tone); });
    }
}

Widget& Mounter::mountKind(reactive::Scope& scope, Text const& spec, ContainerWidget* parent) {
    if (spec.field.description.isBound() || !spec.field.description.constant().empty()) {
        throw std::logic_error{"morph::ui: a text is not described; put the line in the text"};
    }
    if (spec.field.labelHint.isBound() || !spec.field.labelHint.constant().empty()) {
        throw std::logic_error{"morph::ui: a text has no caption to hint after; put the hint in the text"};
    }
    TextWidget& widget = scope.adopt(_backend->createText(parent));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    // Spans replace the text, so a text that has them is never told its plain text.
    if (hasSpans(spec.spans)) {
        bind(scope, spec.spans, [&widget](std::vector<TextSpan> const& spans) { widget.setSpans(spans); });
    } else {
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
    }
    bind(scope, spec.role, [&widget](TextRole role) { widget.setRole(role); });
    if (spec.align) {
        widget.setAlign(*spec.align);
    }
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Button const& spec, ContainerWidget* parent) {
    if (spec.style == ButtonStyle::Push && spec.child != nullptr) {
        throw std::logic_error{"morph::ui: only a list entry holds content; a push button has a label"};
    }
    ButtonWidget& widget = scope.adopt(_backend->createButton(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
    if (spec.emphasis != ButtonEmphasis::Normal) {
        widget.setEmphasis(spec.emphasis);
    }
    if (!spec.icon.empty()) {
        widget.setIcon(spec.icon);
    }
    if (spec.style == ButtonStyle::Item) {
        widget.setStyle(spec.style);
    }
    if (spec.shape != ControlShape::Default) {
        widget.setShape(spec.shape);
    }
    if (spec.size != ButtonSize::Normal) {
        widget.setSize(spec.size);
    }
    if (spec.mono) {
        widget.setMono(true);
    }
    if (spec.weight != 0) {
        widget.setWeight(spec.weight);
    }
    if (spec.selected.isBound() || spec.selected.constant()) {
        bind(scope, spec.selected, [&widget](bool selected) { widget.setSelected(selected); });
    }
    widget.setOnClick(event(spec.onClick));
    if (spec.child != nullptr) {
        static_cast<void>(mount(scope, spec.child, &widget.content()));
    }
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, TextInput const& spec, ContainerWidget* parent) {
    bool const hasUnit = spec.unit.isBound() || !spec.unit.constant().empty();
    bool const hasPrefix = spec.prefix.isBound() || !spec.prefix.constant().empty();
    bool const hasDecimals = spec.decimals.isBound() || spec.decimals.constant() >= 0;
    if ((hasUnit || hasPrefix || hasDecimals || !spec.icon.empty()) && spec.mode == TextInputMode::Multiline) {
        throw std::logic_error{
            "morph::ui: a multiline text input holds no number, so it takes no unit, prefix, places or icon"};
    }
    if (spec.align != Align::Start && spec.mode == TextInputMode::Multiline) {
        throw std::logic_error{"morph::ui: a multiline text input's text starts at the leading edge"};
    }
    if (spec.emphasis != InputEmphasis::Normal && spec.mode == TextInputMode::Multiline) {
        throw std::logic_error{"morph::ui: a multiline text input's size is its own; it takes no emphasis"};
    }
    TextInputWidget& widget = scope.adopt(_backend->createTextInput(parent, spec.mode));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    auto reassert = controlled(scope, spec.value, [&widget](std::string const& value) { widget.setText(value); });
    bind(scope, spec.placeholder, [&widget](std::string const& text) { widget.setPlaceholder(text); });
    // An input without a unit or places is not told any, so a backend sees only what a field declares.
    if (hasUnit) {
        bind(scope, spec.unit, [&widget](std::string const& unit) { widget.setUnit(unit); });
    }
    if (hasPrefix) {
        bind(scope, spec.prefix, [&widget](std::string const& prefix) { widget.setPrefix(prefix); });
    }
    if (hasDecimals) {
        bind(scope, spec.decimals, [&widget](int decimals) { widget.setDecimals(decimals); });
    }
    if (!spec.icon.empty()) {
        widget.setIcon(spec.icon);
    }
    if (spec.emphasis != InputEmphasis::Normal) {
        widget.setEmphasis(spec.emphasis);
    }
    if (spec.mono) {
        widget.setMono(true);
    }
    if (!spec.wrap) {
        widget.setWrap(false);
    }
    if (spec.align != Align::Start) {
        widget.setAlign(spec.align);
    }
    applyCaption(scope, widget, spec.label, spec.showLabel);
    widget.setOnChange(inputEvent(spec.onChange, reassert));
    // An empty submit handler tells the backend the field does not submit, so Enter goes on to its ancestors'
    // chords (a dialog's default) once the field has committed.
    widget.setOnSubmit(spec.onSubmit ? inputEvent(spec.onSubmit, std::move(reassert))
                                     : std::function<void(std::string)>{});
    widget.setOnCommit(event(spec.onCommit));
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Checkbox const& spec, ContainerWidget* parent) {
    CheckboxWidget& widget = scope.adopt(_backend->createCheckbox(parent));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
    widget.setOnToggle(inputEvent(
        spec.onToggle, controlled(scope, spec.checked, [&widget](bool checked) { widget.setChecked(checked); })));
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Select const& spec, ContainerWidget* parent) {
    SelectWidget& widget = scope.adopt(_backend->createSelect(parent, spec.style));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    bind(scope, spec.options, [&widget](std::vector<SelectOption> const& options) { widget.setOptions(options); });
    widget.setOnSelect(inputEvent(
        spec.onSelect,
        controlled(scope, spec.selected, [&widget](std::optional<Key> const& key) { widget.setSelected(key); })));
    widget.setOnOpen(event(spec.onOpen));
    applyCaption(scope, widget, spec.label, spec.showLabel);
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Menu const& spec, ContainerWidget* parent) {
    MenuWidget& widget = scope.adopt(_backend->createMenu(parent, spec.style));
    applyCommon(scope, widget, spec.common);
    if (spec.style == MenuStyle::Button) {
        bind(scope, spec.label, [&widget, icon = spec.icon, emphasis = spec.emphasis](std::string const& label) {
            widget.setTrigger(MenuTrigger{.label = label, .icon = icon, .emphasis = emphasis});
        });
    }
    std::vector<MenuItem> const& items = spec.items;
    // One prop for the whole tree: a bound label, description, mark, enablement or visibility anywhere re-sends
    // every entry, which keeps setItems the one setter. A reported path is read against what was last sent.
    auto sent = std::make_shared<std::map<MenuPath, MenuPath>>();
    Prop<ShownMenu> const shown = anyBound(items) ? Prop<ShownMenu>([items = items] { return shownOf(items); })
                                                  : Prop<ShownMenu>(shownOf(items));
    bind(scope, shown, [&widget, sent](ShownMenu const& menu) {
        *sent = menu.sources;
        widget.setItems(menu.entries);
    });
    widget.setOnActivate(event(
        std::function<void(std::vector<std::size_t>)>{[items = items, sent](std::vector<std::size_t> const& path) {
            MenuItem const* const item = itemAt(items, *sent, path);
            if (item != nullptr && item->onSelect) {
                item->onSelect();
            }
        }}));
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Column const& spec, ContainerWidget* parent) {
    return mountStack(scope, Axis::Vertical, spec.children, spec.gap, spec.common, parent, spec.align, spec.justify,
                      false, spec.divided, spec.padding, spec.selected, spec.holder);
}

Widget& Mounter::mountKind(reactive::Scope& scope, Row const& spec, ContainerWidget* parent) {
    if ((spec.tone || spec.label || spec.heading || spec.spans) && dynamic_cast<TableWidget*>(parent) == nullptr) {
        throw std::logic_error{
            "morph::ui: a row's tone, label, heading and spans are a table row's; this row is not in a table"};
    }
    if (spec.selected && dynamic_cast<TableWidget*>(parent) != nullptr) {
        throw std::logic_error{"morph::ui: a table row's selection is the table's; this row gives its own"};
    }
    return mountStack(scope, Axis::Horizontal, spec.children, spec.gap, spec.common, parent, spec.align, spec.justify,
                      spec.wrap, spec.divided && !spec.wrap, spec.padding, spec.selected);
}

Widget& Mounter::mountKind(reactive::Scope& scope, Grid const& spec, ContainerWidget* parent) {
    GridWidget& widget = scope.adopt(_backend->createGrid(parent));
    applyCommon(scope, widget, spec.common);
    checkNoShare(spec.tracks);
    if (spec.tracks.empty()) {
        widget.setColumns(spec.columns);
    } else {
        widget.setTracks(spec.tracks);
    }
    if (spec.minColumnWidth > 0 && spec.tracks.empty()) {
        widget.setMinColumnWidth(spec.minColumnWidth);
    }
    widget.setGap(spacingStep(spec.gap));
    if (spec.align != Align::Stretch) {
        widget.setAlign(spec.align);
    }
    for (GridCell const& cell : spec.cells) {
        Widget* const child = mount(scope, cell.node, &widget);
        if (child != nullptr && cell.span != 1) {
            widget.setSpan(*child, cell.span);
        }
    }
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Spacer const& spec, ContainerWidget* parent) {
    SpacerWidget& widget = scope.adopt(_backend->createSpacer(parent));
    applyCommon(scope, widget, spec.common);
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Panel const& spec, ContainerWidget* parent) {
    PanelWidget& widget = scope.adopt(_backend->createPanel(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
    if (Widget* const header = mount(scope, spec.header, &widget); header != nullptr) {
        widget.setHeader(*header);
    }
    widget.setPadding(spacingStep(spec.padding));
    widget.setCollapsible(spec.collapsible);
    if (spec.collapsible) {
        widget.setOnToggle(inputEvent(spec.onToggle, controlled(scope, spec.collapsed, [&widget](bool collapsed) {
                                          widget.setCollapsed(collapsed);
                                      })));
    }
    static_cast<void>(mount(scope, spec.child, &widget));
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Scroll const& spec, ContainerWidget* parent) {
    ScrollWidget& widget = scope.adopt(_backend->createScroll(parent, spec.axis));
    applyCommon(scope, widget, spec.common);
    static_cast<void>(mount(scope, spec.child, &widget));
    return widget;
}

std::unique_ptr<reactive::Scope> Mounter::mountContent(std::size_t depth, Node const& node, ContainerWidget& host) {
    return mountPass([&] {
        auto content = std::make_unique<reactive::Scope>(*_rt, depth);
        _rt->untracked([&] { static_cast<void>(mount(*content, node, &host)); });
        return content;
    });
}

Widget& Mounter::mountKind(reactive::Scope& scope, Switch const& spec, ContainerWidget* parent) {
    SlotWidget& widget = scope.adopt(_backend->createSlot(parent));
    applyCommon(scope, widget, spec.common);
    auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
    bind(scope, spec.selector,
         [this, &widget, &content, depth = scope.depth() + 1, cases = spec.cases, fallback = spec.fallback,
          overlay = _overlay](Key const& key) {
             content.reset();
             auto const chosen = std::ranges::find(cases, key, &SwitchCase::key);
             Node const& node = chosen == cases.end() ? fallback : chosen->node;
             if (node) {
                 content = underOverlay(overlay, [&] { return mountContent(depth, node, widget); });
             }
         });
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Tabs const& spec, ContainerWidget* parent) {
    TabsWidget& widget = scope.adopt(_backend->createTabs(parent));
    applyCommon(scope, widget, spec.common);
    if (spacingStep(spec.inset) != 0) {
        widget.setInset(spacingStep(spec.inset));
    }
    if (spec.pageInset) {
        widget.setPageInset(spacingStep(*spec.pageInset));
    }
    if (!spec.stripSurface.empty()) {
        widget.setStripSurface(spec.stripSurface);
    }
    // One prop over every caption, as a menu's entries: a bound caption re-sends them all. The count never
    // changes, so the widget keeps its highlight, the shown page and the focus across a relabel.
    std::vector<Prop<std::string>> captions;
    captions.reserve(spec.tabs.size());
    for (Tab const& tab : spec.tabs) {
        captions.push_back(tab.label);
    }
    auto labelsOf = [captions] {
        std::vector<std::string> labels;
        labels.reserve(captions.size());
        for (Prop<std::string> const& caption : captions) {
            labels.push_back(caption.evaluate());
        }
        return labels;
    };
    Prop<std::vector<std::string>> const labels = std::ranges::any_of(captions, &Prop<std::string>::isBound)
                                                      ? Prop<std::vector<std::string>>(std::move(labelsOf))
                                                      : Prop<std::vector<std::string>>(labelsOf());
    bind(scope, labels, [&widget](std::vector<std::string> const& shown) { widget.setTabs(shown); });
    if (spec.moreLabel.isBound() || spec.moreLabel.constant() != "More") {
        bind(scope, spec.moreLabel, [&widget](std::string const& label) { widget.setMoreLabel(label); });
    }
    auto& pageScope = scope.child();
    auto& pages = scope.make<Pages>();
    pages.pageSlots.resize(spec.tabs.size(), nullptr);
    // A bound index is controlled: after a pick, the bar highlights the page that is shown, so a pick the
    // document refuses moves the highlight back. A constant index is not a slot, and the bar keeps the pick.
    std::function<void(std::size_t const&)> reassert;
    if (spec.selected.isBound()) {
        reassert = reasserter<std::size_t>(
            scope, pages.highlighted,
            [&pages] { return pages.shown.value_or(std::numeric_limits<std::size_t>::max()); },
            [&widget, &pages](std::size_t index) { highlight(widget, pages, index); });
    }
    widget.setOnSelect(inputEvent(spec.onSelect, reassert));
    pages.onSelect = inputEvent(spec.onSelect, std::move(reassert));
    auto show = [this, &widget, &pageScope, &pages, pageSpecs = spec.tabs, overlay = _overlay](std::size_t index) {
        _rt->untracked([&] { showPage(widget, pageScope, pages, pageSpecs, index, overlay); });
    };
    if (!spec.selected.isBound()) {
        show(spec.selected.constant());
    } else {
        scope.effect([binding = spec.selected.binding(), show = std::move(show)] { show(binding()); });
    }
    return widget;
}

void Mounter::showPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, std::vector<Tab> const& pageSpecs,
                       std::size_t index, ContainerWidget* overlay) {
    bool const exists = index < pageSpecs.size();
    bool const isNew = exists && pages.pageSlots.at(index) == nullptr;
    if (isNew) {
        try {
            underOverlay(overlay, [&] { mountPage(widget, pageScope, pages, pageSpecs.at(index).node, index); });
        } catch (...) {
            if (pages.shown.has_value()) {
                restoreSelection(widget, pages, *pages.shown);
            }
            throw;
        }
    }
    if (pages.shown != index) {
        if (pages.shown.has_value() && *pages.shown < pages.pageSlots.size()) {
            if (SlotWidget* const previous = pages.pageSlots.at(*pages.shown); previous != nullptr) {
                previous->setVisible(false);
            }
        }
        if (exists && !isNew) {
            pages.pageSlots.at(index)->setVisible(true);
        }
    }
    pages.shown = index;
    highlight(widget, pages, index);
}

void Mounter::highlight(TabsWidget& widget, Pages& pages, std::size_t index) {
    widget.setSelected(index);
    pages.highlighted = index;
}

void Mounter::restoreSelection(TabsWidget& widget, Pages& pages, std::size_t shown) noexcept {
    try {
        highlight(widget, pages, shown);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // The page's failure is the one reported; the bar keeps whatever highlight it had.
    }
    try {
        std::function<void(std::size_t)> const onSelect = pages.onSelect;
        if (onSelect) {
            onSelect(shown);
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Only copying the wrapper can throw here; the application then keeps the failed index.
    }
}

void Mounter::mountPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, Node const& node,
                        std::size_t index) {
    auto page = mountPass([&] {
        auto built = std::make_unique<reactive::Scope>(*_rt, pageScope.depth());
        SlotWidget& slot = built->adopt(_backend->createSlot(&widget));
        // A page fills the area under the strip, pads itself by the node's `pageInset` and scrolls itself while
        // its content overflows, as a workspace's page does.
        slot.setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        slot.setScrollable(true);
        static_cast<void>(mount(*built, node, &slot));
        return std::pair{std::move(built), &slot};
    });
    pageScope.adopt(std::move(page.first));
    pages.pageSlots.at(index) = page.second;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Dialog const& spec, ContainerWidget* parent) {
    checkOverlayLayout(spec.common, "dialog");
    DialogWidget& widget = scope.adopt(_backend->createDialog(parent));
    applyCommon(scope, widget, spec.common);
    if (spec.sheet.isBound() || spec.sheet.constant()) {
        bind(scope, spec.sheet, [&widget](bool sheet) { widget.setSheet(sheet); });
    }
    return mountOverlay(scope, spec, widget);
}

Widget& Mounter::mountKind(reactive::Scope& scope, Drawer const& spec, ContainerWidget* parent) {
    checkOverlayLayout(spec.common, "drawer");
    DrawerWidget& widget = scope.adopt(_backend->createDrawer(parent, spec.side));
    applyCommon(scope, widget, spec.common);
    return mountOverlay(scope, spec, widget);
}

void Mounter::dismissFailed(std::function<void()> const& onDismiss) noexcept {
    try {
        std::function<void()> const dismiss = onDismiss;
        if (dismiss) {
            dismiss();
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Only copying the wrapper can throw here; the application then keeps `open` true.
    }
}

Widget& Mounter::mountKind(reactive::Scope& scope, ForEach const& spec, ContainerWidget* parent) {
    StackWidget& widget = scope.adopt(_backend->createStack(parent, spec.axis));
    // A horizontal ForEach directly in an action bar is part of the row of actions, so its rows are the action
    // gap apart, whatever gap the ForEach says.
    bool const actionRow = spec.axis == Axis::Horizontal && _actionBar != nullptr && parent == _actionBar;
    if (actionRow) {
        widget.setActionRow();
    }
    if (spec.axis == Axis::Horizontal && !spec.wrap) {
        scope.make<ShareParent>(_shareParents, &widget);
    }
    if (spec.wrap && spec.axis == Axis::Horizontal) {
        widget.setWrap(true);
    }
    if (spec.divided && spec.axis == Axis::Vertical) {
        widget.setDivided(true);
    }
    applyCommon(scope, widget, spec.common);
    widget.setGap(spacingStep(actionRow ? 2 : spec.gap));
    mountRows(scope, spec.model, widget, {}, nullptr);
    return widget;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): resolves spans against the table's rows in one pass: what the widget is told depends on the difference from the previous resolve, and refusals report when their set changes
void Mounter::resolveSpans(TableWidget& widget, SpanTable& table) {
    if (table.reconciling != 0 || !table.anyAsked) {
        return;
    }
    std::vector<SpanRow> rows;
    rows.reserve(table.order.size());
    for (SpanTable::Placed const& placed : table.order) {
        auto const found = table.rows.find(placed.key);
        SpanRow row{.name = "row " + std::visit([](auto const& key) { return std::format("{}", key); }, placed.key)};
        if (found != table.rows.end()) {
            row.heading = found->second.heading;
            row.requested = found->second.requested;
        }
        rows.push_back(std::move(row));
    }
    bool const virtualized = table.windowed || table.order.size() >= table.threshold;
    ResolvedSpans resolved;
    if (table.sorted) {
        resolved.effective.resize(rows.size());
    } else if (virtualized) {
        resolved.effective.resize(rows.size());
        if (std::ranges::any_of(rows, [](SpanRow const& row) {
                return std::ranges::any_of(row.requested,
                                           [](RowSpan const& span) { return span.rows >= 2 || span.columns >= 2; });
            })) {
            resolved.refused.emplace_back(site::kRowSpanVirtualized, "the table is virtualized");
        }
    } else {
        resolved = resolveRowSpans(rows, table.columns, table.placeOf);
    }
    std::set<std::string> refused;
    for (auto const& [reason, text] : resolved.refused) {
        refused.insert(std::string{reason} + " | " + text);
    }
    if (refused != table.lastRefused) {
        std::set<char const*> sites;
        for (auto const& [reason, text] : resolved.refused) {
            sites.insert(reason);
        }
        for (char const* const reason : sites) {
            _rt->core()->report(reason);
        }
        table.lastRefused = std::move(refused);
    }
    for (std::size_t index = 0; index < table.order.size(); ++index) {
        SpanTable::Placed const& placed = table.order.at(index);
        auto const found = table.rows.find(placed.key);
        if (placed.widget == nullptr || found == table.rows.end()) {
            continue;
        }
        RowSpanEntry& entry = found->second;
        if (entry.told != placed.widget) {
            entry.told = placed.widget;
            entry.sent.clear();
        }
        if (resolved.effective.at(index) != entry.sent) {
            entry.sent = resolved.effective.at(index);
            widget.setRowSpans(*placed.widget, entry.sent);
        }
    }
}

}  // namespace morph::ui::detail
