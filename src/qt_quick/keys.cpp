// SPDX-License-Identifier: Apache-2.0

#include "qt_quick/keys.hpp"

#include <QByteArray>
#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace morph::qt_quick::detail {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string toUtf8(QString const& text) {
    QByteArray const bytes = text.toUtf8();
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString encodeKey(ui::Key const& key) {
    if (auto const* const number = std::get_if<std::int64_t>(&key)) {
        return QStringLiteral("i:") + QString::number(*number);
    }
    return QStringLiteral("s:") + toQString(std::get<std::string>(key));
}

std::optional<ui::Key> decodeKey(QString const& text) {
    if (text.startsWith(QStringLiteral("s:"))) {
        return ui::Key{toUtf8(text.mid(2))};
    }
    if (!text.startsWith(QStringLiteral("i:"))) {
        return std::nullopt;
    }
    std::string const digits = toUtf8(text.mid(2));
    std::int64_t number = 0;
    char const* const last = std::to_address(digits.cend());
    auto const [end, error] = std::from_chars(digits.data(), last, number);
    if (error != std::errc{} || end != last || digits.empty()) {
        return std::nullopt;
    }
    return ui::Key{number};
}

namespace {

struct NamedKey {
    std::string_view name;
    Qt::Key key;
};

// `Escape` is accepted beside the grammar's `Esc`: the interpreter binds a dialog's `cancel` under that name.
// NOLINTBEGIN(modernize-use-designated-initializers): a table of positional pairs, name beside key, reads as one.
constexpr std::array<NamedKey, 10> kNamedKeys = {{{"Enter", Qt::Key_Return},
                                                  {"Esc", Qt::Key_Escape},
                                                  {"Escape", Qt::Key_Escape},
                                                  {"Tab", Qt::Key_Tab},
                                                  {"Up", Qt::Key_Up},
                                                  {"Down", Qt::Key_Down},
                                                  {"Left", Qt::Key_Left},
                                                  {"Right", Qt::Key_Right},
                                                  {"Space", Qt::Key_Space},
                                                  {"Delete", Qt::Key_Delete}}};
// NOLINTEND(modernize-use-designated-initializers)

std::optional<Qt::KeyboardModifier> modifierNamed(std::string_view name) {
    if (name == "Ctrl") {
        return Qt::ControlModifier;
    }
    if (name == "Shift") {
        return Qt::ShiftModifier;
    }
    if (name == "Alt") {
        return Qt::AltModifier;
    }
    if (name == "Meta") {
        return Qt::MetaModifier;
    }
    return std::nullopt;
}

std::optional<Qt::Key> keyNamed(std::string_view name) {
    if (name.size() == 1) {
        char const chr = name.front();
        if (chr >= 'a' && chr <= 'z') {
            return static_cast<Qt::Key>(Qt::Key_A + (chr - 'a'));
        }
        if (chr >= 'A' && chr <= 'Z') {
            return static_cast<Qt::Key>(Qt::Key_A + (chr - 'A'));
        }
        if (chr >= '0' && chr <= '9') {
            return static_cast<Qt::Key>(Qt::Key_0 + (chr - '0'));
        }
        return std::nullopt;
    }
    for (NamedKey const& named : kNamedKeys) {
        if (named.name == name) {
            return named.key;
        }
    }
    if (name.size() >= 2 && name.size() <= 3 && name.front() == 'F') {
        int number = 0;
        std::string_view const digits = name.substr(1);
        char const* const last = std::to_address(digits.cend());
        auto const [end, error] = std::from_chars(std::to_address(digits.cbegin()), last, number);
        if (error == std::errc{} && end == last && number >= 1 && number <= 24) {
            return static_cast<Qt::Key>(Qt::Key_F1 + (number - 1));
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<QKeyCombination> chordKey(std::string_view chord) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t plus = chord.find('+'); plus != std::string_view::npos; plus = chord.find('+', start)) {
        parts.push_back(chord.substr(start, plus - start));
        start = plus + 1;
    }
    Qt::KeyboardModifiers modifiers;
    for (std::string_view const part : parts) {
        std::optional<Qt::KeyboardModifier> const modifier = modifierNamed(part);
        if (!modifier || modifiers.testFlag(*modifier)) {
            return std::nullopt;
        }
        modifiers |= *modifier;
    }
    std::optional<Qt::Key> const key = keyNamed(chord.substr(start));
    if (!key) {
        return std::nullopt;
    }
    return QKeyCombination{modifiers, *key};
}

bool chordMatches(std::string_view chord, int key, Qt::KeyboardModifiers modifiers) {
    std::optional<QKeyCombination> const wanted = chordKey(chord);
    if (!wanted) {
        return false;
    }
    Qt::KeyboardModifiers const pressed = modifiers & ~Qt::KeyboardModifiers{Qt::KeypadModifier};
    if (pressed != wanted->keyboardModifiers()) {
        return false;
    }
    if (wanted->key() == Qt::Key_Return) {
        return key == Qt::Key_Return || key == Qt::Key_Enter;
    }
    return key == wanted->key();
}

}  // namespace morph::qt_quick::detail
