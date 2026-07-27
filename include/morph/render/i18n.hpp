// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file render/i18n.hpp
/// @brief The renderer-side translation-catalog seam over morph's derived
///        message keys (`morph::forms::i18n`, `forms/i18n.hpp`).
///
/// `morph::render` is client-side only and never appears on the wire — it is
/// the namespace the planned per-field widget-override registry
/// (`gui_renderer_toolkit.md`'s `SlotRegistry`) will eventually share. morph
/// ships this seam and the resolution algorithm below; it defines **no**
/// translation storage format. A host adapts whatever catalog it already
/// owns (Qt `QTranslator`/`.qm`, a JSON bundle, a database) into the one
/// `TranslationProvider` signature.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace morph::render {

/// @brief A host-supplied catalog lookup: `key`/`bcp47Locale` -> translated
///        text, or `std::nullopt` on a miss.
///
/// A default-constructed (empty) `TranslationProvider` means "no catalog
/// installed" — `resolveText` treats it the same as a provider that misses
/// on every key.
using TranslationProvider =
    std::function<std::optional<std::string>(std::string_view key, std::string_view bcp47Locale)>;

/// @brief Resolves one display slot's text: explicit key, then derived key,
///        then the schema literal.
///
/// Tried in order, most specific first:
///   1. @p explicitKey (a host-declared `x-i18nKey`-derived key), if present;
///   2. @p derivedKey (`morph::forms::i18n::fieldKey` or one of its siblings);
///   3. a miss at both falls back to @p schemaLiteral — the schema's
///      authored `title` / `description` / `x-placeholder` / group or step
///      title, unchanged.
/// An empty/unset @p provider ("no catalog installed") skips straight to
/// @p schemaLiteral, matching an unconfigured renderer's behavior exactly.
/// @param provider      The host's catalog lookup.
/// @param bcp47Locale   The locale to resolve against (e.g. `"fr-FR"`).
/// @param explicitKey   The field/group/rule/step's declared `x-i18nKey`-based
///                       key, or `std::nullopt` when none is declared.
/// @param derivedKey    The mechanically-derived key for this slot.
/// @param schemaLiteral The schema's authored fallback text for this slot.
/// @return The resolved display text.
[[nodiscard]] inline std::string resolveText(const TranslationProvider& provider, std::string_view bcp47Locale,
                                             const std::optional<std::string>& explicitKey,
                                             std::string_view derivedKey, std::string_view schemaLiteral) {
    if (provider) {
        if (explicitKey.has_value()) {
            if (auto hit = provider(*explicitKey, bcp47Locale)) {
                return *hit;
            }
        }
        if (auto hit = provider(derivedKey, bcp47Locale)) {
            return *hit;
        }
    }
    return std::string{schemaLiteral};
}

}  // namespace morph::render
