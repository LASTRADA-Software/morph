// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QString>
#include <Qt>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace morph::qt_quick::detail {

/// UTF-8 to QString.
[[nodiscard]] QString toQString(std::string_view text);

/// QString to UTF-8.
[[nodiscard]] std::string toUtf8(QString const& text);

/// A key as the text QML holds it: `i:<int64>` or `s:<text>`. QML never sees a key as a number, so an id above
/// 2^53 crosses unchanged.
[[nodiscard]] QString encodeKey(ui::Key const& key);

/// The key `encodeKey` wrote, or none for text it cannot have written.
[[nodiscard]] std::optional<ui::Key> decodeKey(QString const& text);

/// Whether pressing @p key with @p modifiers is the chord a document wrote as @p chord, such as `"Ctrl+S"`.
///
/// A chord is modifiers (`Ctrl`, `Shift`, `Alt`, `Meta`) joined to one key name by `+`: a letter or a digit, `F1` to
/// `F24`, or `Enter`, `Esc`, `Tab`, `Up`, `Down`, `Left`, `Right`, `Space`, `Delete`. Letters match without regard
/// to case, the modifiers must be exactly those pressed (the keypad flag is not a modifier), and `Enter` is either
/// Return key. A chord that does not parse matches nothing.
[[nodiscard]] bool chordMatches(std::string_view chord, int key, Qt::KeyboardModifiers modifiers);

/// The key and modifiers pressing @p chord sends; none for a chord that does not parse.
[[nodiscard]] std::optional<QKeyCombination> chordKey(std::string_view chord);

}  // namespace morph::qt_quick::detail
