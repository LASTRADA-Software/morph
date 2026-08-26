// SPDX-License-Identifier: Apache-2.0

/// @file
/// @brief Opt-in, compiler-independent shape tag for a type with a custom
///        Glaze codec.
///
/// `morph::model::payloadShape` decomposes a payload struct from `std::` type
/// traits and Glaze's *reflected key strings*, never from a compiler-spelled
/// type name — that portability is the whole reason a journal written by one
/// build is readable by another. The cost is that a type carrying its own
/// `glz::meta` has no reflected members to decompose, so it renders as the
/// single opaque tag `x`, and a retype between two such types (`Rational` for
/// `Timestamp`, `Quantity<Gram>` for `Quantity<Litre>`) changes nothing about
/// the fingerprint. For `Quantity` that is the *only* place such a swap could
/// be caught at all: neither the unit nor the declared precision travels on
/// the wire, so the JSON is byte-identical either way.
///
/// This header is the seam that closes it. A custom-codec type specialises
/// `PayloadShapeTag` to declare a short, stable, author-written name, and
/// `payloadShape` renders that name instead of the bare `x`. The name is
/// spelled in this repository's own sources, so it does not vary with the
/// compiler, the standard library, or the platform — which a `glz::name_v`
/// tag would, and a journal readable only by the compiler that wrote it is
/// the worse failure.
///
/// It is opt-in, and therefore incomplete by construction: a *new* custom-codec
/// type that declares nothing still renders as `x`. That is the residual
/// boundary, and `docs/spec/journal/journal.md`'s "What the fingerprint does
/// not catch" states it.
///
/// The trait lives in its own header, separate from `core/payload_schema.hpp`,
/// so that `morph/util/rational.hpp` and its peers can declare their tags
/// without any of them depending on the journal machinery.
///
/// **Visibility rule.** A specialisation must be visible wherever
/// `payloadShape` is instantiated for that type, or two translation units
/// would render the same payload differently. In practice this is automatic:
/// a payload struct with a `Rational` member is only a complete type where
/// `morph/util/rational.hpp` has been included, and that header carries the
/// specialisation.

#pragma once
#include <concepts>
#include <string_view>

namespace morph::model {

/// @brief Declares a stable shape tag for a type `T` whose JSON shape
///        `payloadShape` cannot decompose.
///
/// The primary template is deliberately empty: an unspecialised type has no
/// declared tag and keeps rendering as the opaque `x`. A specialisation
/// provides
///
/// ```cpp
/// static std::string_view name();          // required
/// using Inner = <type>;                    // optional
/// ```
///
/// `name()` must return a name that is stable for the lifetime of the returned
/// view (a literal, or a function-local `static const std::string`), and must
/// be spelled in these sources rather than derived from a compiler builtin.
/// `Inner`, when present, names a type whose shape is rendered *inside* the
/// tag — for a transparent wrapper such as `Tagged<T, Tag>`, whose JSON simply
/// *is* `T`'s, that keeps the underlying shape visible instead of hiding it
/// behind the wrapper's name.
///
/// @tparam T Type the tag is declared for.
template <typename T>
struct PayloadShapeTag {};

/// @brief Satisfied when `T` declares a stable shape tag.
template <typename T>
concept HasPayloadShapeTag = requires {
    { PayloadShapeTag<T>::name() } -> std::convertible_to<std::string_view>;
};

/// @brief Satisfied when `T`'s tag also names an inner type to render inside
///        it (a transparent wrapper).
template <typename T>
concept HasPayloadShapeInner = HasPayloadShapeTag<T> && requires { typename PayloadShapeTag<T>::Inner; };

}  // namespace morph::model
