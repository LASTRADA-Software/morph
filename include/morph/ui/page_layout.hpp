/// @file
/// @brief The geometry of a document page, in millimetres, and the pure functions that lay it out.
///
/// A page is a sheet of fixed physical size; its regions are rectangles of it, pinned to its margin box or placed at
/// an offset from its edges. Both frontends and the recording backend take a page's scale and its regions' rectangles
/// from `layoutPage`, so they cannot disagree about where a footer is; a frontend adds only its density. See
/// `docs/spec/ui/document_page.md`.
#pragma once

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace morph::ui {

/// @brief A length in millimetres, held as an exact count of micrometres.
///
/// A page's geometry is compared in tests and travels in a document; an integer count compares exactly where a
/// floating-point length would make "the footer's bottom edge equals the margin box's" depend on rounding.
class Mm {
public:
    /// @brief Micrometres in a millimetre.
    static constexpr std::int64_t kPerMillimetre = 1000;

    /// @brief Zero.
    constexpr Mm() noexcept = default;

    /// @brief A length in millimetres, rounded to the nearest micrometre.
    /// @param millimetres The length.
    /// @throws std::invalid_argument when it is not finite or its micrometre count does not fit an `int32`.
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions): `.width = 210` writes a length.
    Mm(double millimetres) {
        double const micrometres = std::round(millimetres * static_cast<double>(kPerMillimetre));
        if (!std::isfinite(micrometres) ||
            std::abs(micrometres) > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            throw std::invalid_argument{"morph::ui::Mm: a length must be finite and fit 2147 m"};
        }
        _micrometres = static_cast<std::int32_t>(micrometres);
    }

    /// @brief A length from its micrometre count.
    /// @param micrometres The count.
    /// @return The length.
    [[nodiscard]] static constexpr Mm fromMicrometres(std::int32_t micrometres) noexcept {
        Mm out;
        out._micrometres = micrometres;
        return out;
    }

    /// @brief The length in micrometres.
    /// @return The count.
    [[nodiscard]] constexpr std::int32_t micrometres() const noexcept { return _micrometres; }

    /// @brief The length in millimetres.
    /// @return It as a `double`.
    [[nodiscard]] constexpr double millimetres() const noexcept {
        return static_cast<double>(_micrometres) / static_cast<double>(kPerMillimetre);
    }

    /// @brief The length in the shortest decimal form of millimetres: `210`, `148.5`, `98.46`.
    /// @return The text.
    [[nodiscard]] std::string text() const {
        std::int64_t const magnitude = _micrometres < 0 ? -static_cast<std::int64_t>(_micrometres) : _micrometres;
        std::string out = _micrometres < 0 ? "-" : "";
        out += std::to_string(magnitude / kPerMillimetre);
        std::int64_t fraction = magnitude % kPerMillimetre;
        if (fraction != 0) {
            std::string digits = std::to_string(fraction);
            digits.insert(0, 3 - digits.size(), '0');
            while (digits.back() == '0') {
                digits.pop_back();
            }
            out += '.' + digits;
        }
        return out;
    }

    /// @brief Compares lengths.
    /// @return The order.
    [[nodiscard]] friend constexpr auto operator<=>(Mm const&, Mm const&) noexcept = default;

    /// @brief The sum of two lengths, held at the largest or smallest length instead of overflowing.
    /// @param left  The first.
    /// @param right The second.
    /// @return The sum.
    [[nodiscard]] friend constexpr Mm operator+(Mm left, Mm right) noexcept {
        return saturated(static_cast<std::int64_t>(left._micrometres) + right._micrometres);
    }

    /// @brief The difference of two lengths, held at the largest or smallest length instead of overflowing.
    /// @param left  The first.
    /// @param right The second.
    /// @return The difference.
    [[nodiscard]] friend constexpr Mm operator-(Mm left, Mm right) noexcept {
        return saturated(static_cast<std::int64_t>(left._micrometres) - right._micrometres);
    }

private:
    static constexpr Mm saturated(std::int64_t micrometres) noexcept {
        constexpr std::int64_t kLowest = std::numeric_limits<std::int32_t>::lowest();
        constexpr std::int64_t kHighest = std::numeric_limits<std::int32_t>::max();
        return fromMicrometres(static_cast<std::int32_t>(std::clamp(micrometres, kLowest, kHighest)));
    }

    std::int32_t _micrometres = 0;
};

/// @brief The distances from a sheet's edges to its margin box.
struct PageMargins {
    /// @brief The top margin.
    Mm top{};
    /// @brief The right margin.
    Mm right{};
    /// @brief The bottom margin.
    Mm bottom{};
    /// @brief The left margin.
    Mm left{};

    /// @brief No margins.
    constexpr PageMargins() noexcept = default;

    /// @brief The same margin on every side.
    /// @param all The margin.
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions): `.margins = 20` writes every side.
    PageMargins(Mm all) noexcept : top{all}, right{all}, bottom{all}, left{all} {}

    /// @brief A margin for each side.
    /// @param top_    The top.
    /// @param right_  The right.
    /// @param bottom_ The bottom.
    /// @param left_   The left.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): top, right, bottom, left, as CSS orders them.
    PageMargins(Mm top_, Mm right_, Mm bottom_, Mm left_) noexcept
        : top{top_}, right{right_}, bottom{bottom_}, left{left_} {}

    /// @brief Compares margins.
    /// @return Whether every side is equal.
    [[nodiscard]] friend bool operator==(PageMargins const&, PageMargins const&) noexcept = default;
};

/// @brief The smallest sheet side.
inline constexpr Mm kMinPageSide = Mm::fromMicrometres(10'000);
/// @brief The largest sheet side.
inline constexpr Mm kMaxPageSide = Mm::fromMicrometres(5'000'000);

/// @brief A sheet's physical size and margins.
struct PageGeometry {
    /// @brief The sheet's width, 10 mm to 5000 mm.
    Mm width{};
    /// @brief The sheet's height, 10 mm to 5000 mm.
    Mm height{};
    /// @brief The margin box's distances from the sheet's edges.
    PageMargins margins{};

    /// @brief Compares geometries.
    /// @return Whether every member is equal.
    [[nodiscard]] friend bool operator==(PageGeometry const&, PageGeometry const&) noexcept = default;

    /// @brief The margin box's width.
    /// @return The sheet's width less both side margins.
    [[nodiscard]] Mm marginWidth() const noexcept { return width - margins.left - margins.right; }

    /// @brief The margin box's height.
    /// @return The sheet's height less both vertical margins.
    [[nodiscard]] Mm marginHeight() const noexcept { return height - margins.top - margins.bottom; }

    /// @brief Why this geometry is refused.
    /// @return The reason, or nothing when it is valid: a side outside 10 mm to 5000 mm, a negative margin, or an
    ///         empty margin box.
    [[nodiscard]] std::optional<std::string> problem() const {
        if (width < kMinPageSide || width > kMaxPageSide || height < kMinPageSide || height > kMaxPageSide) {
            return "a page side is 10 mm to 5000 mm";
        }
        for (Mm const margin : {margins.top, margins.right, margins.bottom, margins.left}) {
            if (margin < Mm{}) {
                return "a page margin is not negative";
            }
        }
        if (marginWidth() <= Mm{} || marginHeight() <= Mm{}) {
            return "a page's margins leave no margin box";
        }
        return std::nullopt;
    }
};

/// @brief Where a region is anchored.
enum class PageAnchor : std::uint8_t {
    Top,     ///< Its top edge at the margin box's top edge, spanning the margin box's width.
    Bottom,  ///< Its bottom edge at the margin box's bottom edge, spanning the margin box's width.
    Offset,  ///< Its top-left corner at `(x, y)` from the sheet's top-left corner.
};

/// @brief The name of an anchor: `Top`, `Bottom` or `Offset`.
/// @param anchor The anchor.
/// @return Its name.
[[nodiscard]] constexpr char const* anchorName(PageAnchor anchor) noexcept {
    return anchor == PageAnchor::Bottom ? "Bottom" : anchor == PageAnchor::Offset ? "Offset" : "Top";
}

/// @brief A rectangle of the sheet, in the sheet's own millimetres: the origin is its top-left corner, y downwards.
struct PageRect {
    /// @brief The left edge.
    Mm x{};
    /// @brief The top edge.
    Mm y{};
    /// @brief The width.
    Mm width{};
    /// @brief The height.
    Mm height{};

    /// @brief Compares rectangles.
    /// @return Whether every member is equal.
    [[nodiscard]] friend bool operator==(PageRect const&, PageRect const&) noexcept = default;

    /// @brief The rectangle as `x,y,width,height` in the shortest decimal form.
    /// @return The text.
    [[nodiscard]] std::string text() const {
        return x.text() + ',' + y.text() + ',' + width.text() + ',' + height.text();
    }
};

/// @brief A region's place on a sheet.
struct RegionGeometry {
    /// @brief The anchor.
    PageAnchor anchor = PageAnchor::Top;
    /// @brief The distance from the sheet's left edge; only an `Offset` region takes one.
    Mm x{};
    /// @brief The distance from the sheet's top edge; only an `Offset` region takes one.
    Mm y{};
    /// @brief The width; absent: the margin box's width for `Top` and `Bottom`, and the distance from `x` to the
    ///        margin box's right edge for `Offset`.
    std::optional<Mm> width;
    /// @brief The height; absent: the content's. A given height is the region's extent and clips its content.
    std::optional<Mm> height;

    /// @brief Compares geometries.
    /// @return Whether every member is equal.
    [[nodiscard]] friend bool operator==(RegionGeometry const&, RegionGeometry const&) noexcept = default;

    /// @brief Why this region is refused on @p page.
    /// @param page The sheet it is on, valid.
    /// @return The reason, or nothing when it is valid: a negative or zero extent, an offset on a pinned region, or a
    ///         stated rectangle that leaves the sheet.
    [[nodiscard]] std::optional<std::string> problem(PageGeometry const& page) const {
        if (x < Mm{} || y < Mm{}) {
            return "a page region's offset is not negative";
        }
        if (anchor != PageAnchor::Offset && (x != Mm{} || y != Mm{})) {
            return "a pinned page region has no offset";
        }
        if ((width && *width <= Mm{}) || (height && *height <= Mm{})) {
            return "a page region's width and height are above zero";
        }
        Mm const left = anchor == PageAnchor::Offset ? x : page.margins.left;
        if (anchor == PageAnchor::Offset && !width && x >= page.width - page.margins.right) {
            return "an offset page region with no width starts at or past the margin box's right edge";
        }
        if (width && left + *width > page.width) {
            return "a page region leaves the sheet's width";
        }
        if (height) {
            Mm const top = anchor == PageAnchor::Offset ? y
                           : anchor == PageAnchor::Top  ? page.margins.top
                                                        : page.height - page.margins.bottom - *height;
            if (top < Mm{} || top + *height > page.height) {
                return "a page region leaves the sheet's height";
            }
        }
        return std::nullopt;
    }
};

/// @brief A region as the layout reads it: its geometry and the height of its content at its width.
struct RegionSpec {
    /// @brief The region's place.
    RegionGeometry geometry{};
    /// @brief The content's height; `0` where the caller has none.
    Mm contentHeight{};
};

/// @brief A region's resolved place.
struct RegionLayout {
    /// @brief The rectangle, in the sheet's own millimetres.
    PageRect rect{};
    /// @brief Whether the content does not fit: taller than the stated height, or the rectangle reaches past the
    ///        sheet's top or bottom edge.
    bool overflow = false;
};

/// @brief A sheet laid out: its scale, its displayed size and its regions.
struct PageLayout {
    /// @brief `min(1, available / width)`; 1 for an unlimited width.
    double scale = 1.0;
    /// @brief The sheet's width times the scale.
    Mm displayedWidth{};
    /// @brief The sheet's height times the scale.
    Mm displayedHeight{};
    /// @brief One entry per region, in node order.
    std::vector<RegionLayout> regions;
};

/// @brief Lays a page out.
/// @param page           The sheet, valid.
/// @param regions        The regions in node order, each valid on @p page.
/// @param availableWidth The width the container gives the sheet; absent is unlimited.
/// @return The scale, the displayed size and each region's rectangle.
[[nodiscard]] inline PageLayout layoutPage(PageGeometry const& page, std::span<RegionSpec const> regions,
                                           std::optional<Mm> availableWidth) {
    PageLayout out;
    if (availableWidth) {
        out.scale = std::clamp(availableWidth->millimetres() / page.width.millimetres(), 0.0, 1.0);
    }
    out.displayedWidth = Mm{page.width.millimetres() * out.scale};
    out.displayedHeight = Mm{page.height.millimetres() * out.scale};
    Mm const right = page.width - page.margins.right;
    out.regions.reserve(regions.size());
    for (RegionSpec const& spec : regions) {
        RegionGeometry const& geometry = spec.geometry;
        RegionLayout region;
        Mm const height = geometry.height.value_or(spec.contentHeight);
        if (geometry.anchor == PageAnchor::Offset) {
            region.rect = {geometry.x, geometry.y, geometry.width.value_or(std::max(Mm{}, right - geometry.x)),
                           height};
        } else {
            Mm const top =
                geometry.anchor == PageAnchor::Top ? page.margins.top : page.height - page.margins.bottom - height;
            region.rect = {page.margins.left, top, geometry.width.value_or(page.marginWidth()), height};
        }
        region.overflow = geometry.height ? spec.contentHeight > *geometry.height
                                          : region.rect.y < Mm{} || region.rect.y + height > page.height;
        out.regions.push_back(region);
    }
    return out;
}

/// @brief How many cells a terminal draws to 25.4 mm.
struct CellPitch {
    /// @brief Columns to 25.4 mm.
    int columns = 10;
    /// @brief Rows to 25.4 mm.
    int rows = 6;
};

/// @brief A rectangle of cells.
struct CellRect {
    /// @brief The first column.
    int column = 0;
    /// @brief The first row.
    int row = 0;
    /// @brief The width in columns.
    int width = 0;
    /// @brief The height in rows.
    int height = 0;

    /// @brief Compares rectangles.
    /// @return Whether every member is equal.
    [[nodiscard]] friend bool operator==(CellRect const&, CellRect const&) noexcept = default;
};

namespace detail {

/// @brief A length as a cell edge: `round(length · cells / 25.4 mm)`, halves away from zero.
/// @param length The length.
/// @param cells  The cells to 25.4 mm.
/// @return The cell count.
[[nodiscard]] constexpr int cellEdge(Mm length, int cells) noexcept {
    constexpr std::int64_t kInch = 25'400;
    std::int64_t const scaled = static_cast<std::int64_t>(length.micrometres()) * cells;
    std::int64_t const half = kInch / 2;
    return static_cast<int>(scaled >= 0 ? (scaled + half) / kInch : -((-scaled + half) / kInch));
}

}  // namespace detail

/// @brief The cells a width in millimetres spans: `round(width · columns / 25.4)`.
/// @param width The width.
/// @param pitch The terminal's pitch.
/// @return The columns.
[[nodiscard]] constexpr int columnsOf(Mm width, CellPitch pitch) noexcept {
    return detail::cellEdge(width, pitch.columns);
}

/// @brief The cells a height in millimetres spans: `round(height · rows / 25.4)`.
/// @param height The height.
/// @param pitch  The terminal's pitch.
/// @return The rows.
[[nodiscard]] constexpr int rowsOf(Mm height, CellPitch pitch) noexcept {
    return detail::cellEdge(height, pitch.rows);
}

/// @brief A rectangle in cells, by rounding its edges and not its sizes: two rectangles that touch in millimetres
///        touch in cells, and a rounded size never drifts from a neighbour's edge.
/// @param rect  The rectangle in the sheet's millimetres.
/// @param pitch The terminal's pitch.
/// @return The cells.
[[nodiscard]] constexpr CellRect toCells(PageRect const& rect, CellPitch pitch) noexcept {
    int const left = detail::cellEdge(rect.x, pitch.columns);
    int const right = detail::cellEdge(rect.x + rect.width, pitch.columns);
    int const top = detail::cellEdge(rect.y, pitch.rows);
    int const bottom = detail::cellEdge(rect.y + rect.height, pitch.rows);
    return {left, top, right - left, bottom - top};
}

/// @brief The cells of a region of no stated height whose content is @p contentRows rows tall: the anchored edge (the
///        top of a `Top` or `Offset` region, the bottom of a `Bottom` one) is the rounded edge and the other edge is
///        @p contentRows away, so a footer of three rows is three rows after rounding.
/// @param rect        The rectangle in millimetres, its height the content's.
/// @param anchor      The region's anchor.
/// @param pitch       The terminal's pitch.
/// @param contentRows The content's height in rows.
/// @return The cells.
[[nodiscard]] constexpr CellRect toCells(PageRect const& rect, PageAnchor anchor, CellPitch pitch,
                                         int contentRows) noexcept {
    CellRect cells = toCells(rect, pitch);
    cells.height = contentRows;
    if (anchor == PageAnchor::Bottom) {
        cells.row = detail::cellEdge(rect.y + rect.height, pitch.rows) - contentRows;
    }
    return cells;
}

}  // namespace morph::ui
