// SPDX-License-Identifier: Apache-2.0

#include "tui/chord.hpp"

#include <array>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

namespace morph::tui::detail {

namespace {

using ::core::tui::KeyCode;
using ::core::tui::Modifier;

struct NamedKey {
    std::string_view name;
    KeyCode code;
};

constexpr std::array kNamedKeys{
    NamedKey{.name = "enter", .code = KeyCode::Enter},
    NamedKey{.name = "return", .code = KeyCode::Enter},
    NamedKey{.name = "tab", .code = KeyCode::Tab},
    NamedKey{.name = "backspace", .code = KeyCode::Backspace},
    NamedKey{.name = "delete", .code = KeyCode::Delete},
    NamedKey{.name = "del", .code = KeyCode::Delete},
    NamedKey{.name = "escape", .code = KeyCode::Escape},
    NamedKey{.name = "esc", .code = KeyCode::Escape},
    NamedKey{.name = "up", .code = KeyCode::Up},
    NamedKey{.name = "down", .code = KeyCode::Down},
    NamedKey{.name = "left", .code = KeyCode::Left},
    NamedKey{.name = "right", .code = KeyCode::Right},
    NamedKey{.name = "home", .code = KeyCode::Home},
    NamedKey{.name = "end", .code = KeyCode::End},
    NamedKey{.name = "pageup", .code = KeyCode::PageUp},
    NamedKey{.name = "pagedown", .code = KeyCode::PageDown},
    NamedKey{.name = "insert", .code = KeyCode::Insert},
    NamedKey{.name = "ins", .code = KeyCode::Insert},
    NamedKey{.name = "f1", .code = KeyCode::F1},
    NamedKey{.name = "f2", .code = KeyCode::F2},
    NamedKey{.name = "f3", .code = KeyCode::F3},
    NamedKey{.name = "f4", .code = KeyCode::F4},
    NamedKey{.name = "f5", .code = KeyCode::F5},
    NamedKey{.name = "f6", .code = KeyCode::F6},
    NamedKey{.name = "f7", .code = KeyCode::F7},
    NamedKey{.name = "f8", .code = KeyCode::F8},
    NamedKey{.name = "f9", .code = KeyCode::F9},
    NamedKey{.name = "f10", .code = KeyCode::F10},
    NamedKey{.name = "f11", .code = KeyCode::F11},
    NamedKey{.name = "f12", .code = KeyCode::F12},
    NamedKey{.name = "space", .code = ::core::tui::keyCodeFromCodepoint(U' ')},
};

constexpr auto kChordModifiers =
    static_cast<std::uint8_t>(static_cast<std::uint8_t>(Modifier::Shift) | static_cast<std::uint8_t>(Modifier::Alt) |
                              static_cast<std::uint8_t>(Modifier::Ctrl) | static_cast<std::uint8_t>(Modifier::Super));

[[nodiscard]] constexpr char lower(char letter) noexcept {
    return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
}

[[nodiscard]] constexpr bool equalsIgnoringCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (lower(left.at(i)) != lower(right.at(i))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::uint8_t> modifierNamed(std::string_view name) noexcept {
    if (equalsIgnoringCase(name, "ctrl") || equalsIgnoringCase(name, "control")) {
        return static_cast<std::uint8_t>(Modifier::Ctrl);
    }
    if (equalsIgnoringCase(name, "alt")) {
        return static_cast<std::uint8_t>(Modifier::Alt);
    }
    if (equalsIgnoringCase(name, "shift")) {
        return static_cast<std::uint8_t>(Modifier::Shift);
    }
    if (equalsIgnoringCase(name, "super") || equalsIgnoringCase(name, "meta") || equalsIgnoringCase(name, "cmd")) {
        return static_cast<std::uint8_t>(Modifier::Super);
    }
    return std::nullopt;
}

/// The key a chord names: a named key's code, or a single ASCII character, lowered.
[[nodiscard]] std::optional<KeyCode> keyNamed(std::string_view name) noexcept {
    if (name.size() == 1) {
        auto const letter = static_cast<unsigned char>(lower(name.front()));
        return letter < 0x80 ? std::optional{::core::tui::keyCodeFromCodepoint(letter)} : std::nullopt;
    }
    for (NamedKey const& named : kNamedKeys) {
        if (equalsIgnoringCase(name, named.name)) {
            return named.code;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<::core::tui::KeyEvent> chordKey(std::string_view chord) noexcept {
    std::uint8_t wanted = 0;
    std::string_view rest = chord;
    // The key name is what follows the last `+` that is not the key itself (`Ctrl++`).
    while (rest.size() > 1) {
        auto const plus = rest.find('+', 1);
        if (plus == std::string_view::npos) {
            break;
        }
        auto const modifier = modifierNamed(rest.substr(0, plus));
        if (!modifier) {
            return std::nullopt;
        }
        wanted = static_cast<std::uint8_t>(wanted | *modifier);
        rest.remove_prefix(plus + 1);
    }
    auto const named = keyNamed(rest);
    if (!named) {
        return std::nullopt;
    }
    auto const code = static_cast<std::uint32_t>(*named);
    return ::core::tui::KeyEvent{.key = *named,
                                 .modifiers = static_cast<Modifier>(wanted),
                                 .codepoint = code < 0x10000 ? static_cast<char32_t>(code) : U'\0'};
}

bool matchesChord(std::string_view chord, ::core::tui::KeyEvent const& key) noexcept {
    auto const wanted = chordKey(chord);
    if (!wanted) {
        return false;
    }
    auto pressed = static_cast<std::uint8_t>(static_cast<std::uint8_t>(key.modifiers) & kChordModifiers);
    auto code = static_cast<std::uint32_t>(key.key);
    if (code >= U'A' && code <= U'Z') {
        code = code - U'A' + U'a';
        pressed = static_cast<std::uint8_t>(pressed | static_cast<std::uint8_t>(Modifier::Shift));
    }
    return code == static_cast<std::uint32_t>(wanted->key) && pressed == static_cast<std::uint8_t>(wanted->modifiers);
}

}  // namespace morph::tui::detail
