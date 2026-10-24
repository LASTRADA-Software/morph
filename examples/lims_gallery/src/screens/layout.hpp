// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/alignment.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/button_emphasis.hpp>
#include <optional>
#include <string>
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

/// @brief Declares the screen's width param and answers the width class it lays out for: the param when given, the
///        client's `{"env": "widthClass"}` otherwise, and `expanded` when neither is set.
/// @param screen The screen.
/// @return The width class, a `let` named `widthClass`.
[[nodiscard]] inline ui::AnyHandle widthClass(ui::Scope& screen) {
    auto const given = screen.param<std::optional<std::string>>(std::string{kWidthParam});
    return screen.let("widthClass", ui::coalesce({given, ui::env("widthClass"), ui::AnyHandle{"expanded"}}));
}

/// @brief The compact layout below 600 logical pixels, the expanded one otherwise: `medium` takes the expanded
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

}  // namespace lims_gallery::layout
