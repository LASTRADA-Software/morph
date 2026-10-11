// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/InputEvent.hpp>
#include <optional>
#include <string_view>

namespace morph::tui::detail {

/// Whether @p key is the chord a document wrote as @p chord, such as `"Ctrl+S"`, `"Shift+Tab"` or `"F5"`.
///
/// A chord is modifiers joined to a key name by `+`: `Ctrl` (or `Control`), `Alt`, `Shift` and `Super` (or `Meta`,
/// `Cmd`), then one character or a named key (`Enter`, `Tab`, `Escape`, `Space`, `Backspace`, `Delete`, `Insert`,
/// `Home`, `End`, `PageUp`, `PageDown`, the arrows `Up` `Down` `Left` `Right`, and `F1` to `F12`). Names and letters
/// match without regard to case, and the modifiers must be exactly those pressed; the lock keys are not modifiers.
/// A capital letter typed without a reported Shift counts as Shift and the letter, since a terminal that speaks no
/// keyboard protocol reports Shift+S as `S`. A chord that does not parse matches nothing.
/// @param chord The chord as written.
/// @param key The key pressed.
/// @return True when they are the same chord.
[[nodiscard]] bool matchesChord(std::string_view chord, ::core::tui::KeyEvent const& key) noexcept;

/// The key event pressing @p chord sends, written as `matchesChord` reads it: the key's code and the modifiers, a
/// letter lowered, and a character's codepoint.
/// @param chord The chord as written.
/// @return The event, or none for a chord that does not parse.
[[nodiscard]] std::optional<::core::tui::KeyEvent> chordKey(std::string_view chord) noexcept;

}  // namespace morph::tui::detail
