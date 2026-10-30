// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/alignment.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/button_emphasis.hpp>
#include <utility>

#include "lims_gallery/lims_gallery.hpp"

/// @file
/// What every gallery screen shares: the width class it lays out for, and the few building blocks the designs
/// repeat (a captioned input, a status tag, a muted line).

namespace lims_gallery::layout {

namespace ui = morph::ui::author;
using morph::ui::Align;
using morph::ui::ButtonEmphasis;
using morph::ui::Justify;

/// @brief Draws the screen's page area on the white `plainPage` surface while it lays out for the compact width, as
/// the
///        designs' compact windows are; the expanded width keeps the grey page.
/// @param screen The screen.
/// @param width  The screen's width class, as `widthClass` answers it.
inline void plainPageWhenCompact(ui::Scope& screen, ui::AnyHandle const& width) {
    screen.pageSurface(ui::cond(ui::eq(width, "compact"), ui::AnyHandle{"plainPage"}, ui::AnyHandle{nullptr}));
}

/// @brief The width class the screen lays out for: the client's `{"env": "widthClass"}`, and `expanded` where no
///        client sets it.
/// @param screen The screen.
/// @return The width class, a `let` named `widthClass`.
[[nodiscard]] inline ui::AnyHandle widthClass(ui::Scope& screen) {
    return screen.let("widthClass", ui::coalesce({ui::env("widthClass"), ui::AnyHandle{"expanded"}}));
}

/// @brief The compact layout below 80 units, the expanded one otherwise: `medium` takes the expanded
///        layout, as the designs draw only the two.
/// @param width    The width class.
/// @param compact  The compact layout.
/// @param expanded The expanded layout.
/// @return The switch.
[[nodiscard]] inline ui::Node byWidth(ui::AnyHandle const& width, ui::Nodes compact, ui::Nodes const& expanded) {
    return ui::switchOn(width, {ui::Case{"compact", std::move(compact)}}, expanded);
}

/// @brief An input with its caption drawn above it, as every design labels its fields.
/// @param input The input.
/// @return It, with `showLabel` set.
[[nodiscard]] inline ui::Node captioned(ui::Node const& input) { return input.with("showLabel", ui::AnyHandle{true}); }

/// @brief A muted line of text.
/// @param text The text.
/// @return The node.
[[nodiscard]] inline ui::Node muted(ui::AnyHandle const& text) { return ui::text({.text = text, .role = "muted"}); }

/// @brief A heading of a card or a section.
/// @param text The text.
/// @return The node.
[[nodiscard]] inline ui::Node section(ui::AnyHandle const& text) {
    return ui::text({.text = text, .role = "section"});
}

/// @brief A status tag: the designs' small coloured label.
/// @param text The text.
/// @param tone `ok`, `warn`, `err`, `info` or `neutral`.
/// @return The node.
[[nodiscard]] inline ui::Node tag(ui::AnyHandle const& text, ui::AnyHandle const& tone) {
    return ui::badge(text, tone);
}

/// @brief A priority tag: outlined, so a red outline (STAT) reads apart from a red fill (out of specification).
/// @param text The text.
/// @param tone `ok`, `warn`, `err`, `info` or `neutral`.
/// @return The node.
[[nodiscard]] inline ui::Node outlineTag(ui::AnyHandle const& text, ui::AnyHandle const& tone) {
    return ui::badge(text, tone, {}, morph::ui::BadgeStyle::Outline);
}

}  // namespace lims_gallery::layout
