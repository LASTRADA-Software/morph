// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/alignment.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/button_emphasis.hpp>
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

/// @brief Draws the screen's page area on the white `plainPage` surface while it lays out for the compact width, as
/// the
///        designs' compact windows are; the expanded width keeps the grey page.
/// @param screen The screen.
/// @param width  The screen's width class, as `widthClass` answers it.
inline void plainPageWhenCompact(ui::Scope& screen, ui::AnyHandle const& width) {
    screen.pageSurface(ui::cond(ui::eq(width, "compact"), ui::AnyHandle{"plainPage"}, ui::AnyHandle{nullptr}));
}

/// @brief The width class the screen lays out for: `morph::ui::author::widthClass`.
/// @param screen The screen.
/// @return The width class.
[[nodiscard]] inline ui::AnyHandle widthClass(ui::Scope& screen) { return ui::widthClass(screen); }

/// @brief The compact layout below 80 units, the expanded one otherwise: `morph::ui::author::byWidth`.
/// @param width    The width class.
/// @param compact  The compact layout.
/// @param expanded The expanded layout.
/// @return The switch.
[[nodiscard]] inline ui::Node byWidth(ui::AnyHandle const& width, ui::Nodes compact, ui::Nodes const& expanded) {
    return ui::byWidth(width, std::move(compact), expanded);
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

/// @brief A quiet tag: the muted outlined label of the routine priority beside the urgent ones.
/// @param text The text.
/// @return The node.
[[nodiscard]] inline ui::Node quietTag(ui::AnyHandle const& text) {
    return ui::badge(text, {}, {}, morph::ui::BadgeStyle::Quiet);
}

/// @brief A priority tag: STAT in an outlined error tag, Urgent in an outlined warning tag, and any other priority a
///        quiet one.
/// @param priority The priority's name.
/// @return The node.
[[nodiscard]] inline ui::Node priorityTag(ui::AnyHandle const& priority) {
    ui::Handle<bool> const stat = ui::eq(priority, "STAT");
    ui::Handle<bool> const urgent = ui::eq(priority, "Urgent");
    return ui::row({outlineTag(priority, ui::Handle<std::string>{"err"}).visible(stat),
                    outlineTag(priority, ui::Handle<std::string>{"warn"}).visible(urgent),
                    quietTag(priority).visible(!(stat || urgent))});
}

}  // namespace lims_gallery::layout
