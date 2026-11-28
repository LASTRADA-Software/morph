// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/field_control.hpp
/// @brief The built-in control of a scalar field: the forms control a form's cell and a grid's cell editor both draw.
///
/// Specified in `docs/spec/forms/engine.md`, "Rendering".

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../../ui/view.hpp"
#include "form_session.hpp"

namespace morph::forms::detail::engine {

/// @brief Whether a text property says something: bound, or a constant that is not empty.
/// @param text The property.
/// @return Whether it is set.
[[nodiscard]] inline bool isSet(ui::Prop<std::string> const& text) {
    return text.isBound() || !text.constant().empty();
}

/// @brief The last reply or failure.
/// @param session The session.
/// @return `error: <message>`, `ok: <reply>`, or empty before the first answer.
[[nodiscard]] MORPH_API std::string resultText(FormSession const& session);

/// @brief A draft wall clock (`YYYY-MM-DDTHH:MM[:SS]`, or a `YYYY-MM-DD` date) as the instant it denotes.
/// @param text          The draft.
/// @param date          Whether the field holds a date.
/// @param offsetMinutes The display zone.
/// @return The instant, or `std::nullopt` while the draft is not a complete one.
[[nodiscard]] MORPH_API std::optional<time::Timestamp> draftInstant(std::string_view text, bool date,
                                                                    int offsetMinutes);

/// @brief The draft a date-time control writes for an instant (blank for none).
/// @param value         The instant.
/// @param date          Whether the field holds a date.
/// @param offsetMinutes The display zone.
/// @return `YYYY-MM-DDTHH:MM:SS` in the zone, or `YYYY-MM-DD`.
[[nodiscard]] MORPH_API std::string instantDraft(std::optional<time::Timestamp> const& value, bool date,
                                                 int offsetMinutes);

/// @brief The view-tree field state of a field: read-only, required, its error as the one message shown, and its
///        help as the control's description, which assistive technology associates with the control.
/// @param field The field.
/// @return The state, bound to the field.
[[nodiscard]] MORPH_API ui::FieldState fieldState(FieldState const* field);

/// @brief The `Common` of a field's control: its test id (`field_<path>`) and accessible name (its label).
/// @param field The field.
/// @return The common part.
[[nodiscard]] MORPH_API ui::Common controlCommon(FieldState const* field);

/// @brief A Select over the field's options (an Enum's, or a Choice's as last fetched), keyed by each option's
///        JSON literal, so an id past 2^53 is never a number on the way.
/// @param field The field.
/// @param style Dropdown or radio.
/// @return The control.
[[nodiscard]] MORPH_API ui::Node optionSelect(FieldState* field, ui::SelectStyle style);

/// @brief The built-in control of a scalar field (the table in `docs/spec/forms/engine.md`, "Rendering"): every kind
///        but an Object and an ObjectArray, each of which draws its members' cells.
/// @param session The session: a date or date-time control reads its display zone and its pattern.
/// @param state   The field.
/// @return The control.
// One case per kind.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] MORPH_API ui::Node scalarControl(FormSession& session, FieldState& state);

}  // namespace morph::forms::detail::engine
