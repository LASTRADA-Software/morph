// SPDX-License-Identifier: Apache-2.0
//
// How a document's chord, such as "Ctrl+S", is matched against the key a terminal reports.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include "tui/chord.hpp"

using core::tui::KeyCode;
using core::tui::KeyEvent;
using core::tui::Modifier;
using morph::tui::detail::chordKey;
using morph::tui::detail::matchesChord;

namespace {

KeyEvent letter(char32_t codepoint, Modifier modifiers = Modifier::None) {
    return KeyEvent{.key = core::tui::keyCodeFromCodepoint(codepoint), .modifiers = modifiers, .codepoint = codepoint};
}

KeyEvent special(KeyCode code, Modifier modifiers = Modifier::None) {
    return KeyEvent{.key = code, .modifiers = modifiers, .codepoint = 0};
}

}  // namespace

// Mutation: compare the key without the modifiers (plain s and Ctrl+Shift+S then match Ctrl+S).
TEST_CASE("tui chord: the modifiers must be exactly those pressed", "[tui][chord]") {
    CHECK(matchesChord("Ctrl+S", letter(U's', Modifier::Ctrl)));
    CHECK_FALSE(matchesChord("Ctrl+S", letter(U's')));
    CHECK_FALSE(matchesChord("Ctrl+S", letter(U's', Modifier::Ctrl | Modifier::Shift)));
    CHECK_FALSE(matchesChord("Ctrl+S", letter(U's', Modifier::Ctrl | Modifier::Alt)));
    CHECK(matchesChord("Ctrl+Alt+Delete", special(KeyCode::Delete, Modifier::Ctrl | Modifier::Alt)));
}

// Mutation: compare the modifiers with the lock keys in (Caps Lock on then breaks every chord).
TEST_CASE("tui chord: the lock keys are not modifiers", "[tui][chord]") {
    CHECK(matchesChord("Ctrl+S", letter(U's', Modifier::Ctrl | Modifier::CapsLock | Modifier::NumLock)));
}

// Mutation: compare names with case (lower-case chords and upper-case letters stop matching).
TEST_CASE("tui chord: names and letters match without regard to case", "[tui][chord]") {
    CHECK(matchesChord("ctrl+s", letter(U's', Modifier::Ctrl)));
    CHECK(matchesChord("CTRL+S", letter(U's', Modifier::Ctrl)));
    CHECK(matchesChord("Esc", special(KeyCode::Escape)));
    CHECK(matchesChord("escape", special(KeyCode::Escape)));
    CHECK(matchesChord("Control+Return", special(KeyCode::Enter, Modifier::Ctrl)));
}

// Mutation: drop the capital-letter rule (a terminal's `S` no longer matches Shift+S).
TEST_CASE("tui chord: a capital letter without a reported Shift is Shift and the letter", "[tui][chord]") {
    CHECK(matchesChord("Shift+S", letter(U'S')));
    CHECK(matchesChord("Shift+S", letter(U's', Modifier::Shift)));
    CHECK_FALSE(matchesChord("S", letter(U'S')));
}

TEST_CASE("tui chord: named keys, function keys and space", "[tui][chord]") {
    CHECK(matchesChord("Shift+Tab", special(KeyCode::Tab, Modifier::Shift)));
    CHECK(matchesChord("F5", special(KeyCode::F5)));
    CHECK(matchesChord("PageDown", special(KeyCode::PageDown)));
    CHECK(matchesChord("Space", letter(U' ')));
    CHECK(matchesChord("Super+K", letter(U'k', Modifier::Super)));
    CHECK(matchesChord("Cmd+K", letter(U'k', Modifier::Super)));
}

// Mutation: split the chord at its last `+` (Ctrl++ then names an empty key and matches nothing).
TEST_CASE("tui chord: a plus is a key of its own", "[tui][chord]") {
    CHECK(matchesChord("Ctrl++", letter(U'+', Modifier::Ctrl)));
    CHECK(matchesChord("+", letter(U'+')));
}

// Mutation: treat an unknown modifier as none (Hyper+S then matches a plain s).
TEST_CASE("tui chord: a chord that does not parse matches nothing", "[tui][chord]") {
    CHECK_FALSE(matchesChord("Hyper+S", letter(U's')));
    CHECK_FALSE(matchesChord("Ctrl+Banana", letter(U'b', Modifier::Ctrl)));
    CHECK_FALSE(matchesChord("", letter(U's')));
    CHECK_FALSE(chordKey("Ctrl+").has_value());
}

// Mutation: leave the codepoint out of a character's event (a field would take it as no character).
TEST_CASE("tui chord: the key event a chord sends is the one it matches", "[tui][chord]") {
    for (char const* const chord : {"Ctrl+S", "Shift+Tab", "F12", "Alt+Enter", "Space", "Ctrl++"}) {
        INFO(chord);
        auto const event = chordKey(chord);
        REQUIRE(event.has_value());
        CHECK(matchesChord(chord, *event));
    }
    auto const save = chordKey("Ctrl+S");
    REQUIRE(save.has_value());
    CHECK(save->codepoint == U's');
    CHECK(save->modifiers == Modifier::Ctrl);
    auto const tab = chordKey("Shift+Tab");
    REQUIRE(tab.has_value());
    CHECK(tab->codepoint == 0);
}
