// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>
#include <span>
#include <string_view>

/// @file
/// The gallery's screens as the C++ builders write them, and the one list of designs the bundle registers and the
/// index shows. Apart from `lims_gallery.hpp`, so a translation unit that needs only the served bundle does not
/// compile the builders.

namespace lims_gallery {

/// @brief One design of the gallery.
struct Design {
    /// @brief Its number in the design set, `01` to `10`.
    std::string_view number;
    /// @brief The screen's id.
    std::string_view id;
    /// @brief Its name, as the index lists it.
    std::string_view title;
    /// @brief What it shows, in one line.
    std::string_view summary;
    /// @brief Builds the screen; null while the design is not built yet, which the index shows as unavailable.
    morph::ui::author::Screen (*make)() = nullptr;
};

/// @brief Every design, in the design set's order. The bundle registers each one that is built.
/// @return The designs.
[[nodiscard]] std::span<Design const> designs();

/// @brief The index: every design, each opened by a button.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen indexScreen();

/// @brief Design 01: the e-signature dialog that approves a worksheet.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen eSignatureScreen();

/// @brief Design 02: sample receipt at the bench: scan, check, receive, next.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen receiptScreen();

/// @brief Design 03: an instrument's record, with its status, tabs and actions.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen instrumentScreen();

/// @brief Design 04: the audit trail, its facets beside a read-only table.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen auditScreen();

/// @brief Design 05: the sample list, a dense table with bulk actions over the selected rows.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen samplesScreen();

/// @brief Design 06: setting up a stability study, its conditions × timepoints grid beside a live summary.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen stabilityScreen();

/// @brief Design 07: a 96-well plate's layout, samples dragged into wells or filled into the selected ones.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen plateScreen();

/// @brief Design 08: result entry on a worksheet, each change asked for its reason.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen resultsScreen();

/// @brief Design 09: review and approval, the queue beside the worksheet under review.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen reviewScreen();

}  // namespace lims_gallery
