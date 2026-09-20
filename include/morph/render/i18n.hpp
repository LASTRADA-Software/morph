// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file render/i18n.hpp
/// @brief The renderer-side translation-catalog seam over morph's derived
///        message keys (`morph::forms::i18n`, `forms/i18n.hpp`).
///
/// `morph::render` is client-side only and never appears on the wire — it is
/// the C++ side of the client-side rendering seam. The per-field
/// widget-override registry that pairs with it is *not* C++ at all: it is
/// `SlotRegistry`, a QML type in module `MorphForms`
/// (`src/qt/forms/qml/SlotRegistry.qml`, documented in
/// docs/spec/forms/forms.md), so do not look for it under this namespace. morph
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
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
// `derivedKey` and `schemaLiteral` are adjacent `std::string_view`s, and the
// check is right that swapping them would be silent and wrong: the derived key
// would be rendered to the user as display text, and the schema's authored
// title would be looked up in the catalog, miss, and fall back to the key. No
// type or arity error would catch it.
//
// They stay in this order because the order *is* the contract. The three
// parameters are the resolution chain in precedence order -- explicit key, then
// derived key, then the schema literal as the fallback -- which is how the
// @brief above states it, how the body below tries them, and how
// docs/spec/forms/forms.md specifies it. The QML renderer carries a mirror of
// this function with the same parameters in the same order
// (src/qt/forms/qml/DynamicForm.qml, `resolveText(explicitKey, derivedKey,
// literal)`), and the two are meant to be read against each other. Reordering
// to break the adjacency here would desynchronise that pair and leave the
// signature the only place in the stack that does not read as the chain --
// trading a mistake that no caller in the tree is positioned to make for one a
// reader of both renderers would.
//
// Strong types would remove the hazard outright, but a `TranslationKey` wrapper
// on this seam would have to be threaded through every caller in morph::forms,
// which is a design change to the renderer boundary and not a lint fix. If that
// is ever done, delete this suppression rather than widening it.
[[nodiscard]] inline std::string resolveText(const TranslationProvider& provider, std::string_view bcp47Locale,
                                             const std::optional<std::string>& explicitKey,
                                             std::string_view derivedKey, std::string_view schemaLiteral) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
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
