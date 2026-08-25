// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file util/quantity.hpp
/// @brief Unit-tagged, optionally-empty exact values with a derivation trail.
///
/// `morph::units::Quantity<U>` wraps an *optional* `morph::math::Rational` and
/// knows three things about itself: its **unit** (a compile-time template tag),
/// its **exactness** (an exact `Rational`, with a declared and a runtime notion
/// of precision, and `std::nullopt` as a real "not entered" state), and **how
/// it was computed** — its derivation, exposed through `equation()`.
///
/// See `docs/spec/util/quantity_type.md` for the full design. The essentials:
///
/// - **Units are types.** `Quantity<Unit::kg>` and `Quantity<Unit::m3>` cannot
///   be mixed; `operator*` / `operator/` deduce the result unit from the
///   application-defined `consteval` algebra on the unit enum. Within-dimension
///   scaling is declared as `UnitTraits<E>::relations` (a flat list of exact
///   `UnitRelation` ratios), which drive `convert`, conversion *chaining*, and
///   the schema's display-unit selector.
/// - **Precision is declared in the type; actual precision is runtime data.**
///   `Quantity<Unit::m3, 4>` overrides the unit default; the value's actual
///   precision is the runtime `DecimalPlaces` tag inside the `Rational` and
///   max-propagates through arithmetic. Magnitude is bounded by `int64`;
///   overflow is a documented limitation, only division-by-zero yields empty.
/// - **Empty propagates** (SQL-NULL semantics). `==` is total; relational
///   ordering has a precondition that both operands are engaged.
/// - **Provenance** is a build-wide toggle, `MORPH_QUANTITY_PROVENANCE`
///   (default `1`). With it `0`, no derivation nodes are allocated and the
///   provenance API returns empty results; `Quantity<U>` is the same type
///   either way.
/// - **Wire.** On the morph JSON wire a quantity is its nullable `Rational`
///   payload; the unit lives in generated schemas (`ExtUnits`) and C++ types.

/// @brief Build-wide provenance toggle. Default on; define to `0` to compile
///        the derivation DAG out (no `ASTNode` allocations).
#ifndef MORPH_QUANTITY_PROVENANCE
#define MORPH_QUANTITY_PROVENANCE 1
#endif

#include <array>
#include <compare>
#include <cstdint>
#include <format>
#include <glaze/glaze.hpp>
#include <morph/core/payload_shape_tag.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if MORPH_QUANTITY_PROVENANCE
#include <memory>
#include <unordered_map>
#endif

#include "../detail/fixed_string.hpp"
#include "rational.hpp"

namespace morph::units {

namespace detail {

/// @brief Renders a `Rational` as a shortest exact decimal at its own
///        `DecimalPlaces` (trailing zeros and a bare point trimmed) — the form
///        `Quantity` formatting and `equation()` use everywhere.
///
/// The decimal is produced by **exact integer long division** of the canonical
/// `numerator/denominator` to `decimalPlaces` digits — the value never passes
/// through `double`, so both large exact integers (beyond the 2^53 double
/// mantissa) and non-terminating quotients render without floating-point drift.
/// Non-terminating quotients are rounded **half away from zero** at
/// `decimalPlaces` (matching `Rational::toDouble`'s `std::round`), and the
/// rounding carry is allowed to propagate into the integer part. Trailing zeros
/// and a bare decimal point are then trimmed to the shortest form.
///
/// The division is a **digit-at-a-time** long division: the integer part comes
/// from `|numerator| / denominator` in 64-bit, and each fractional digit from
/// `remainder * 10 / denominator`, carrying the exact integer remainder forward.
/// `remainder * 10` can exceed 64 bits when the denominator is near `INT64_MAX`,
/// so each fractional step is computed with `mulTenDivMod` — a portable helper
/// that never forms `remainder * 10` at all: since the quotient digit is in
/// 0..9, it accumulates `remainder` ten times, reducing modulo the denominator
/// as it goes, so every step stays within 64 bits (no 128-bit type, no
/// `__udivti3` runtime dependency) and behaves identically under MSVC and clang-cl.
/// @param value The rational to render.
/// @return The decimal string (`"2"`, `"0.3"`, `"1.36"`, `"-4.2"`).
[[nodiscard]] inline std::string formatRationalDecimal(const morph::math::Rational& value) {
    // Work on magnitudes; the sign is reattached at the end. The canonical
    // invariant guarantees denominator > 0, so only the numerator carries sign.
    bool const negative = value.numerator < 0;
    // Negating INT64_MIN would overflow int64; widen before taking the absolute
    // value so the magnitude is always representable.
    auto const num =
        negative ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(static_cast<std::uint64_t>(value.numerator)))
                 : static_cast<std::uint64_t>(value.numerator);
    auto const den = static_cast<std::uint64_t>(value.denominator);
    auto const places = static_cast<std::uint32_t>(value.decimalPlaces.value);

    // Integer part and the exact running remainder (both < den, so 64-bit safe).
    std::uint64_t const integerPart = num / den;
    std::uint64_t remainder = num % den;

    // floor(rem*10 / den) and (rem*10 % den) with `rem < den`. `rem*10` can need
    // up to 67 bits, but we never form it: the digit q = floor(rem*10/den) is in
    // 0..9 (because rem < den), so we compute it by adding `rem` ten times and
    // reducing modulo `den` as we go. The running accumulator stays below `den`
    // before each add and below `2*den <= 2^64` after, so every step is exact in
    // 64-bit with no overflow and no 128-bit type — identical under MSVC and
    // clang-cl (whose MSVC runtime lacks the `__udivti3` 128-bit divide helper).
    auto const mulTenDivMod = [den](std::uint64_t rem, std::uint64_t& quotientDigit) -> std::uint64_t {
        std::uint64_t acc = 0;
        std::uint64_t q = 0;
        for (int k = 0; k < 10; ++k) {
            acc += rem;  // acc < den before the add (rem < den), so acc < 2*den <= 2^64 after
            while (acc >= den) {
                acc -= den;
                ++q;
            }
        }
        quotientDigit = q;
        return acc;
    };

    std::string frac;
    frac.reserve(places);
    for (std::uint32_t i = 0; i < places; ++i) {
        std::uint64_t digit = 0;
        remainder = mulTenDivMod(remainder, digit);
        frac.push_back(static_cast<char>('0' + static_cast<int>(digit)));
    }

    // Round half away from zero at `places`: if twice the leftover remainder is
    // >= den, the truncated result rounds up. Propagate the carry through the
    // fractional digits and, if it reaches the top, into the integer part.
    std::uint64_t roundedInteger = integerPart;
    if (remainder * 2 >= den) {
        bool carry = true;
        for (std::size_t i = frac.size(); i-- > 0 && carry;) {
            if (frac[i] == '9') {
                frac[i] = '0';
            } else {
                ++frac[i];
                carry = false;
            }
        }
        if (carry) {
            ++roundedInteger;
        }
    }

    std::string text = std::to_string(roundedInteger);
    if (places > 0) {
        text.push_back('.');
        text += frac;
    }

    // Trim trailing zeros and a now-bare decimal point (shortest form).
    if (auto const dot = text.find('.'); dot != std::string::npos) {
        std::size_t last = text.find_last_not_of('0');
        if (last == dot) {
            --last;
        }
        text.erase(last + 1);
    }

    // A rounded-to-zero magnitude must not print as "-0".
    if (negative && text != "0") {
        text.insert(text.begin(), '-');
    }
    return text;
}

/// @brief Formats an optional payload as `equation()` and the formatter print
///        it. The shared implementation behind `units::toDecimalString` (and so
///        behind `toString` and `std::formatter<Quantity>`), which is why the
///        empty case is the `"N/A"` literal rather than an empty string.
/// @param value The optional payload.
/// @return The decimal string, or `"N/A"` when empty.
[[nodiscard]] inline std::string formatOptionalDecimal(const std::optional<morph::math::Rational>& value) {
    return value ? formatRationalDecimal(*value) : std::string{"N/A"};
}

}  // namespace detail

/// @brief Static description of one unit, supplied by `UnitTraits`.
struct UnitMeta {
    /// @brief Stable ascii identifier, e.g. `"kg_per_m3"`. Appears in JSON
    ///        Schemas (`ExtUnits.unitAscii`); part of the protocol vocabulary.
    std::string_view id;

    /// @brief Human display text, e.g. `"kg/m³"` (`ExtUnits.unitUnicode`).
    std::string_view display;

    /// @brief Default decimal places for parsing/formatting values of this
    ///        unit; surfaces in schemas as `x-decimalPlaces`.
    std::uint32_t defaultDecimals{3};
};

/// @brief One exact within-dimension conversion ratio between two peer units.
///
/// `1` unit of `from` equals `fromTo` units of `to`. `fromTo` must be strictly
/// positive; `from` and `to` must differ. Declared in `UnitTraits<E>::relations`
/// and used in both directions (the reverse multiplies by the reciprocal).
/// @tparam E The application's unit enum type.
template <typename E>
struct UnitRelation {
    /// @brief Source unit of the ratio.
    E from{};

    /// @brief Target unit of the ratio.
    E to{};

    /// @brief Exact ratio: `1 from == fromTo · to`. Strictly positive.
    morph::math::Rational fromTo{};
};

/// @brief A derived per-unit view of one convertible display/entry unit.
///
/// Not declared by the application — computed from `UnitTraits<E>::relations`
/// by `Quantity::unitAlternatives()`. `value_in_canonical = value_in_unit *
/// num/den`, where the canonical unit is the field's own unit.
/// @tparam E The application's unit enum type.
template <typename E>
struct UnitAlternative {
    /// @brief The alternative (convertible) unit.
    E unit{};

    /// @brief Numerator of the exact alternative-to-canonical ratio.
    std::int64_t num{1};

    /// @brief Denominator of the exact alternative-to-canonical ratio.
    std::int64_t den{1};
};

/// @brief Optional physical/wire bounds for one unit: the inclusive range a
///        decoded value must fall within to be accepted. Returned by the
///        optional `UnitTraits<E>::bounds(E)` customisation point (detected
///        by `HasUnitBounds`) — e.g. a percentage unit bounding itself to
///        `[0, 100]`, or a mass unit rejecting a negative reading no scale
///        can physically produce. Absent by default: a unit with no declared
///        bounds accepts any value its precision allows, exactly as before
///        this feature existed.
struct QuantityBounds {
    /// @brief Inclusive lower bound.
    morph::math::Rational min;
    /// @brief Inclusive upper bound.
    morph::math::Rational max;
};

/// @brief Customisation point: the application specialises this for its unit
///        enum, returning a `UnitMeta` per enumerator and (optionally) a
///        `relations` array of `UnitRelation` entries and/or a `bounds(E)`
///        static method.
/// @tparam E The application's unit enum type.
template <typename E>
struct UnitTraits;

/// @brief Concept: an enum with a `UnitTraits` specialisation (template param `E`).
template <typename E>
concept UnitEnum = std::is_enum_v<E> && requires(E unit) {
    { UnitTraits<E>::meta(unit) } -> std::convertible_to<UnitMeta>;
};

/// @brief Concept: `UnitTraits<E>` also declares within-dimension `relations` (template param `E`).
template <typename E>
concept HasUnitRelations = requires {
    { UnitTraits<E>::relations };
};

/// @brief Concept: `UnitTraits<E>` declares an optional `static constexpr
///        QuantityBounds bounds(E)` — the pre-decode validation seam a field's
///        unit opts into. A unit enum with no `bounds()` simply has none:
///        every value its precision allows is accepted, unchanged from
///        before this feature existed. `E` is the application's unit enum type.
template <typename E>
concept HasUnitBounds = requires(E unit) {
    { UnitTraits<E>::bounds(unit) } -> std::convertible_to<QuantityBounds>;
};

namespace detail {

/// @brief Result of a compile-time conversion-ratio search.
/// @tparam E The unit enum type.
template <typename E>
struct RatioResult {
    /// @brief Whether a connecting path exists in the relation graph.
    bool found{false};

    /// @brief Composed ratio `k` such that `value_to = value_from · k`.
    morph::math::Rational ratio{};
};

/// @brief Compile-time guard that a `UnitRelation::fromTo` ratio is strictly
///        positive (both numerator and denominator > 0).
///
/// A `UnitRelation` declares `1 from == fromTo · to`; a zero or negative ratio
/// is physically meaningless and, worse, silently corrupts conversions: a zero
/// numerator makes the reverse edge's denominator `0`, which `Rational` clamps
/// to `1` (see rational.hpp), and a negative ratio inverts the direction of the
/// conversion — either way `den:0`/garbage leaks into `x-unitAlternatives`.
///
/// This is invoked from the `consteval` `conversionRatio` search for every
/// relation edge it touches, so a bad ratio is a **compile error** (the
/// `throw` in a constant-evaluation context is ill-formed) rather than silent
/// runtime corruption. `fromTo` is a canonicalised `Rational` whose denominator
/// is already `> 0` by the type invariant, so the failing case in practice is a
/// numerator `<= 0` (a `0`/negative ratio the application declared).
/// @param ratio The declared `fromTo` ratio to validate.
/// @return `true` when @p ratio is strictly positive; otherwise the call is
///         ill-formed in a constant-evaluation context (a compile error).
[[nodiscard]] consteval bool requirePositiveRatio(const morph::math::Rational& ratio) {
    // Rational canonicalises so denominator > 0 always; the numerator carries
    // the sign and can be zero/negative if the application declared a bad ratio.
    if (ratio.numerator <= 0 || ratio.denominator <= 0) {
        throw "UnitRelation::fromTo must be strictly positive (numerator > 0 and denominator > 0)";
    }
    return true;
}

/// @brief Reciprocal of a strictly-positive rational (swaps numerator and
///        denominator), keeping its precision tag.
///
/// Guards, at compile time, that @p value is strictly positive: a zero or
/// negative input would otherwise produce a degenerate reciprocal (a `0`
/// denominator that `Rational` clamps to `1`, or a sign-flipped ratio). Because
/// this is `consteval`, a non-positive @p value is a **compile error** at the
/// relation-consuming call site rather than silent corruption.
/// @param value A strictly-positive rational.
/// @return `1 / value`.
[[nodiscard]] consteval morph::math::Rational reciprocal(const morph::math::Rational& value) {
    static_cast<void>(requirePositiveRatio(value));  // compile error on a bad ratio
    return morph::math::Rational{morph::math::Numerator{value.denominator}, morph::math::Denominator{value.numerator},
                                 value.decimalPlaces};
}

/// @brief Breadth-first search of the relation graph for a `from → to` ratio.
///
/// Treats `UnitTraits<E>::relations` as an undirected graph (each entry usable
/// forward with `fromTo` and backward with its reciprocal), returning the
/// fewest-hops composed ratio. Ties break by declaration order.
/// @tparam E The unit enum type (must satisfy `HasUnitRelations`).
/// @param from Source unit.
/// @param to   Target unit.
/// @return The composed ratio and whether a path was found.
template <typename E>
[[nodiscard]] consteval RatioResult<E> conversionRatio(E from, E to) {
    using morph::math::Rational;
    if (from == to) {
        return RatioResult<E>{true, Rational::one(morph::math::DecimalPlaces{1})};
    }
    constexpr std::size_t relationCount = UnitTraits<E>::relations.size();
    constexpr std::size_t capacity = 2 * relationCount + 1;
    std::array<E, capacity> nodes{};
    std::array<Rational, capacity> ratios{};
    std::size_t count = 0;
    std::size_t head = 0;
    nodes[count] = from;
    ratios[count] = Rational::one(morph::math::DecimalPlaces{1});
    ++count;
    while (head < count) {
        E const current = nodes[head];
        Rational const currentRatio = ratios[head];
        ++head;
        for (auto const& relation : UnitTraits<E>::relations) {
            E neighbour{};
            Rational edge{};
            bool touches = false;
            if (relation.from == current) {
                neighbour = relation.to;
                static_cast<void>(requirePositiveRatio(relation.fromTo));  // compile error on a bad ratio
                edge = relation.fromTo;
                touches = true;
            } else if (relation.to == current) {
                neighbour = relation.from;
                edge = reciprocal(relation.fromTo);  // reciprocal also guards positivity
                touches = true;
            }
            if (!touches) {
                continue;
            }
            bool seen = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (nodes[i] == neighbour) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            Rational const neighbourRatio = currentRatio * edge;
            if (neighbour == to) {
                return RatioResult<E>{true, neighbourRatio};
            }
            nodes[count] = neighbour;
            ratios[count] = neighbourRatio;
            ++count;
        }
    }
    return RatioResult<E>{false, Rational{}};
}

/// @brief Number of `UnitAlternative`s directly convertible to unit `U`.
/// @tparam U The canonical unit enumerator.
/// @return Count of relation entries touching `U`.
template <auto U>
[[nodiscard]] consteval std::size_t alternativeCount() {
    using E = decltype(U);
    std::size_t total = 0;
    if constexpr (HasUnitRelations<E>) {
        for (auto const& relation : UnitTraits<E>::relations) {
            if (relation.from == U || relation.to == U) {
                ++total;
            }
        }
    }
    return total;
}

/// @brief Builds the derived `UnitAlternative` view for unit `U` from
///        `relations` (direct neighbours, both directions).
/// @tparam U The canonical unit enumerator.
/// @return The alternatives array (empty when the unit system declares none).
template <auto U>
[[nodiscard]] consteval std::array<UnitAlternative<decltype(U)>, alternativeCount<U>()> makeAlternatives() {
    using E = decltype(U);
    std::array<UnitAlternative<E>, alternativeCount<U>()> result{};
    if constexpr (HasUnitRelations<E>) {
        std::size_t index = 0;
        for (auto const& relation : UnitTraits<E>::relations) {
            if (relation.to == U) {
                result[index] =
                    UnitAlternative<E>{relation.from, relation.fromTo.numerator, relation.fromTo.denominator};
                ++index;
            } else if (relation.from == U) {
                result[index] =
                    UnitAlternative<E>{relation.to, relation.fromTo.denominator, relation.fromTo.numerator};
                ++index;
            }
        }
    }
    return result;
}

/// @brief Stable storage for unit `U`'s derived alternatives.
/// @tparam U The canonical unit enumerator.
template <auto U>
inline constexpr auto kUnitAlternatives = makeAlternatives<U>();

/// @brief Concept: two enumerators of the same enum with distinct values (template params `A`, `B`).
template <auto A, auto B>
concept SameEnumDistinct = std::same_as<decltype(A), decltype(B)> && (A != B);

}  // namespace detail

// Forward declaration (carrying the default arg) so `convert` and the
// conversion operator can name `Quantity<From>` before the full definition.
template <auto U, std::uint32_t DeclaredDecimals = UnitTraits<decltype(U)>::meta(U).defaultDecimals>
    requires UnitEnum<decltype(U)>
struct Quantity;

/// @brief Concept: the auto-generated ratio `convert` applies to `From → To` (template params `From`, `To`).
template <auto From, auto To>
concept RatioConvertible = detail::SameEnumDistinct<From, To> && UnitEnum<decltype(From)> &&
                           HasUnitRelations<decltype(From)> && detail::conversionRatio(From, To).found;

/// @brief Auto-generated within-dimension conversion (ratio + chaining).
///
/// A **constrained template**; a `UnitTraits<E>::convert` static takes
/// precedence for the pairs it names. Performs the value math only, on an
/// **engaged** operand — empty-propagation and provenance are added by the
/// caller (the conversion operator).
/// @tparam From Source unit enumerator.
/// @tparam To   Target unit enumerator.
/// @param in  Engaged source quantity.
/// @param out Receives the converted value.
template <auto From, auto To>
    requires RatioConvertible<From, To>
void convert(const Quantity<From>& in, Quantity<To>& out);

/// @brief Concept: the application supplied a `UnitTraits<E>::convert` static
///        for this pair (a non-ratio override, e.g. °C → °F) (template params `From`, `To`).
template <auto From, auto To>
concept HasUserConvert =
    requires(Quantity<From> from, Quantity<To>& to) { UnitTraits<decltype(From)>::convert(from, to); };

/// @brief Concept: `From → To` is convertible — a user override takes
///        precedence over an auto-generated ratio path (template params `From`, `To`).
template <auto From, auto To>
concept Convertible = detail::SameEnumDistinct<From, To> && (HasUserConvert<From, To> || RatioConvertible<From, To>);

#if MORPH_QUANTITY_PROVENANCE

namespace detail {

/// @brief One recorded step of a derivation.
struct ASTUnit {
    /// @brief Operator token (`"+"`, `"-"`, `"*"`, `"/"`, `"convert a -> b"`);
    ///        empty for a plain leaf.
    std::string operation;

    /// @brief First operand value (the leaf value for a leaf node).
    std::optional<morph::math::Rational> lhs;

    /// @brief Second operand value; empty for single-operand steps.
    std::optional<morph::math::Rational> rhs;

    /// @brief The step's result value (stored, not recomputed).
    std::optional<morph::math::Rational> result;
};

/// @brief A node in the (immutable, shared) derivation DAG.
struct ASTNode {
    /// @brief This node's own step.
    ASTUnit current;

    /// @brief When set, an opaque named symbol: `equation()` shows the name
    ///        instead of expanding the derivation below it.
    std::optional<std::string> name;

    /// @brief Left operand's derivation (shared).
    std::shared_ptr<ASTNode> left;

    /// @brief Right operand's derivation (shared); null for unary/scalar steps.
    std::shared_ptr<ASTNode> right;
};

/// @brief The per-`Quantity` handle onto the root of its derivation.
struct Context {
    /// @brief Root node; null means "no recorded derivation".
    std::shared_ptr<ASTNode> node;
};

}  // namespace detail

#endif

/// @cond INTERNAL
#if MORPH_QUANTITY_PROVENANCE
#define MORPH_Q_NODE(quantity) (quantity)._ctx.node
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define MORPH_Q_BUILD(out, op, lhsValue, rhsValue, resultValue, leftNode, rightNode) \
    do {                                                                             \
        auto morphProvNode = std::make_shared<::morph::units::detail::ASTNode>();    \
        morphProvNode->current.operation = (op);                                     \
        morphProvNode->current.lhs = (lhsValue);                                     \
        morphProvNode->current.rhs = (rhsValue);                                     \
        morphProvNode->current.result = (resultValue);                               \
        morphProvNode->left = (leftNode);                                            \
        morphProvNode->right = (rightNode);                                          \
        (out)._ctx.node = std::move(morphProvNode);                                  \
    } while (0)
// clang-format on

#else

#define MORPH_Q_NODE(quantity) nullptr
// clang-format off -- public macro surface: hand-aligned on purpose.
// These definitions are the framework's documented API; contributors read them
// as reference, and the continuation backslashes line up so the body is legible
// as a block. Leaving them to the formatter means any unrelated edit nearby
// re-wraps the whole definition, and in one case it broke a token-paste
// invocation apart. Freeze them; realign by hand if a body changes.
#define MORPH_Q_BUILD(out, op, lhsValue, rhsValue, resultValue, leftNode, rightNode) \
    do {                                                                             \
    } while (0)
// clang-format on

#endif
/// @endcond

/// @brief A unit-tagged, optionally-empty exact value with a declared precision
///        in its type and (optionally) a recorded derivation.
/// @tparam U                An enumerator of an application unit enum
///                          satisfying `UnitEnum`.
/// @tparam DeclaredDecimals Declared decimal places of the field; defaults to
///                          the unit's `UnitMeta::defaultDecimals`.
template <auto U, std::uint32_t DeclaredDecimals>
    requires UnitEnum<decltype(U)>
struct Quantity {
    static_assert(DeclaredDecimals <= math::kMaxDecimalPlaces,
                  "declared decimals must be within [0, kMaxDecimalPlaces]");

    /// @brief The payload; `std::nullopt` means "not entered / not measured".
    std::optional<math::Rational> payload;

#if MORPH_QUANTITY_PROVENANCE
    /// @brief Handle onto this value's derivation (internal).
    detail::Context _ctx;
#endif

    /// @brief Constructs the empty state (no recorded derivation).
    constexpr Quantity() noexcept = default;

    /// @brief Engages with @p engaged (which keeps its own runtime precision).
    /// @param engaged The exact value to hold.
    Quantity(math::Rational engaged) : payload{engaged} { recordLeaf(); }

    /// @brief Adopts an optional payload as-is.
    /// @param adopted Engaged or empty payload.
    Quantity(std::optional<math::Rational> adopted) : payload{std::move(adopted)} { recordLeaf(); }

    /// @brief Converts from the same unit at another declared precision; the
    ///        value, its runtime tag, and its derivation carry over unchanged.
    /// @param other Same-unit quantity with a different declared precision.
    template <std::uint32_t OtherDecimals>
        requires(OtherDecimals != DeclaredDecimals)
    Quantity(const Quantity<U, OtherDecimals>& other) : payload{other.payload} {
#if MORPH_QUANTITY_PROVENANCE
        _ctx = other._ctx;
#endif
    }

    /// @brief The unit this quantity is denominated in (compile-time).
    static constexpr auto unit = U;

    /// @brief Declared decimal places of this field (compile-time).
    static constexpr std::uint32_t declaredDecimals = DeclaredDecimals;

    /// @brief The unit's static metadata (id, display text, default decimals).
    /// @return The `UnitMeta` from the application's `UnitTraits` specialisation.
    [[nodiscard]] static constexpr UnitMeta unitMeta() noexcept { return UnitTraits<decltype(U)>::meta(U); }

    /// @brief The declared precision as a `DecimalPlaces` strong type.
    /// @return `DecimalPlaces{declaredDecimals}`.
    [[nodiscard]] static constexpr math::DecimalPlaces declaredPrecision() noexcept {
        return math::DecimalPlaces{DeclaredDecimals};
    }

    /// @brief The convertible display/entry units for this field's unit,
    ///        derived from `UnitTraits::relations`.
    /// @return Exact-ratio alternatives (empty when the unit system declares none).
    [[nodiscard]] static constexpr std::span<const UnitAlternative<decltype(U)>> unitAlternatives() noexcept {
        return std::span<const UnitAlternative<decltype(U)>>{detail::kUnitAlternatives<U>};
    }

    /// @brief Converts a floating-point reading at the declared precision.
    /// @param raw The value to convert.
    /// @return The engaged quantity, or empty when @p raw is not finite or does
    ///         not fit `int64` (empty-propagation philosophy: no error channel).
    [[nodiscard]] static Quantity fromDouble(double raw) noexcept {
        if (auto converted = math::Rational::fromFloat(raw, declaredPrecision()); converted.has_value()) {
            return Quantity{*converted};
        }
        return {};
    }

    /// @brief Wraps an optional payload, preserving the declared precision.
    /// @param adopted Engaged or empty payload.
    /// @return The quantity (empty in → empty out).
    [[nodiscard]] static Quantity fromOptional(std::optional<math::Rational> adopted) {
        return Quantity{std::move(adopted)};
    }

    /// @brief Whether a value has been entered/measured.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] constexpr bool hasValue() const noexcept { return payload.has_value(); }

    /// @brief Pre-decode validation seam: whether the current payload falls
    ///        within this field's unit-declared bounds (`UnitTraits<E>::bounds`),
    ///        when the unit declares any. An empty payload, or a unit with no
    ///        declared `bounds()` (`HasUnitBounds` not satisfied), is always
    ///        within bounds — this is an *opt-in* check, not a new default
    ///        restriction, so a unit system that declares no bounds behaves
    ///        exactly as it did before this feature existed.
    ///
    ///        Comparison is on the exact `Rational` value only (never a lossy
    ///        `double`), consistent with every other exact comparison in this
    ///        header.
    /// @return `true` when empty, when the unit declares no bounds, or when
    ///         the engaged value satisfies `min <= value <= max`.
    [[nodiscard]] constexpr bool withinDeclaredBounds() const noexcept {
        if constexpr (HasUnitBounds<decltype(U)>) {
            if (!payload.has_value()) {
                return true;
            }
            auto const bounds = UnitTraits<decltype(U)>::bounds(U);
            return (*payload <=> bounds.min) != std::strong_ordering::less &&
                   (*payload <=> bounds.max) != std::strong_ordering::greater;
        } else {
            return true;
        }
    }

    /// @brief The payload, for pattern-matching / `->` access.
    /// @return Const reference to the optional payload.
    [[nodiscard]] constexpr const std::optional<math::Rational>& value() const noexcept { return payload; }

    /// @brief The payload if engaged, otherwise a caller-supplied fallback.
    /// @param fallback Value to return when empty.
    /// @return The engaged value or @p fallback.
    [[nodiscard]] constexpr math::Rational value_or(const math::Rational& fallback) const noexcept {
        return payload.value_or(fallback);
    }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly like
    ///        `std::optional::operator*`).
    /// @return The engaged exact value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] constexpr const math::Rational& operator*() const noexcept { return *payload; }

    /// @brief Retags the value's *runtime* precision (the exact value itself is
    ///        unchanged — precision only affects rounding and formatting).
    ///
    /// This is the **display-hint** half of the pair: after it, the value shown
    /// at @p newPrecision and the value stored may differ. Use
    /// `roundedToDecimalPlaces` when the stored value must be the one displayed.
    /// @param newPrecision Runtime precision; silently clamped to the valid range.
    /// @return The retagged quantity, or empty if this is empty.
    [[nodiscard]] Quantity withDecimalPlaces(math::DecimalPlaces newPrecision) const {
        if (!payload) {
            return *this;
        }
        auto adjusted = *payload;
        adjusted.decimalPlaces = math::DecimalPlaces{math::detail::clampWireDecimalPlaces(newPrecision.value)};
        Quantity out;
        out.payload = adjusted;
#if MORPH_QUANTITY_PROVENANCE
        out._ctx = _ctx;
#endif
        return out;
    }

    /// @brief Rounds the value to a runtime precision, **changing the exact
    ///        value** as well as the tag — unlike `withDecimalPlaces`, which
    ///        retags only and leaves display and storage free to disagree.
    ///
    /// Delegates to `math::roundToDecimalPlaces`, and so shares its saturation
    /// behaviour: a value whose scaled form leaves `int64` range clamps and logs
    /// instead of overflowing. A value already representable at @p newPrecision
    /// is unchanged apart from its tag.
    /// @param newPrecision Runtime precision; silently clamped to the valid range.
    /// @param mode Tie-breaking rule; defaults to half away from zero, matching
    ///        the decimal display path.
    /// @return The rounded quantity, or empty if this is empty.
    [[nodiscard]] Quantity roundedToDecimalPlaces(
        math::DecimalPlaces newPrecision, math::RoundingMode mode = math::RoundingMode::HalfAwayFromZero) const {
        if (!payload) {
            return *this;
        }
        Quantity out;
        out.payload = math::roundToDecimalPlaces(*payload, newPrecision, mode);
#if MORPH_QUANTITY_PROVENANCE
        out._ctx = _ctx;
#endif
        return out;
    }

    /// @brief Rounds the value to this field's **declared** precision — the one
    ///        the generated schema advertises as `x-decimalPlaces`.
    ///
    /// The enforcement half of the `x-decimalPlaces` contract: the stored value
    /// becomes the value that renders at the declared precision, so a handler
    /// never persists digits the form does not show. Rounds half away from zero,
    /// the rule the decimal formatter uses.
    /// @return The rounded quantity, or empty if this is empty.
    [[nodiscard]] Quantity atDeclaredPrecision() const { return roundedToDecimalPlaces(declaredPrecision()); }

    /// @brief Seals this value with a name: `equation()` shows the name instead
    ///        of expanding its derivation. Builds a fresh node (never mutates a
    ///        shared one); a no-op returning empty on empty or with tracing off.
    /// @param label The symbol name.
    /// @return A same-unit quantity carrying the name.
    [[nodiscard]] Quantity named([[maybe_unused]] std::string label) const {
        Quantity out;
        out.payload = payload;
#if MORPH_QUANTITY_PROVENANCE
        if (payload) {
            auto node = std::make_shared<detail::ASTNode>();
            node->name = std::move(label);
            node->current.lhs = payload;
            node->current.result = payload;
            node->left = _ctx.node;
            out._ctx.node = std::move(node);
        }
#endif
        return out;
    }

    /// @brief The worked derivation as print-ready lines (see `docs/spec/util/quantity_type.md`).
    /// @return `[0]` formula, `[1]` substitution, `[2]` result, `[3..]` `where`
    ///         legend; a single formatted-value element for a degenerate root
    ///         (empty, named root, bare leaf, or tracing off).
    [[nodiscard]] std::vector<std::string> equation() const
#if MORPH_QUANTITY_PROVENANCE
        ;
#else
    {
        return {detail::formatOptionalDecimal(payload)};
    }
#endif

    /// @brief Implicit within-dimension conversion to another unit; delegates to
    ///        `convert`, propagates empty, and records a provenance step.
    /// @tparam To Target unit enumerator (same enum, different value, convertible).
    /// @return The converted quantity in unit `To`.
    template <auto To>
        requires Convertible<U, To>
    [[nodiscard]] operator Quantity<To>() const {
        Quantity<To> out;
        if (payload) {
            Quantity<U> self{payload};
            if constexpr (HasUserConvert<U, To>) {
                UnitTraits<decltype(U)>::convert(self, out);
            } else {
                convert(self, out);
            }
            MORPH_Q_BUILD(out,
                          std::string{"convert "} + std::string{unitMeta().id} + " -> " +
                              std::string{Quantity<To>::unitMeta().id},
                          payload, std::nullopt, out.payload, MORPH_Q_NODE(*this), nullptr);
        }
        return out;
    }

private:
    /// @brief Records a leaf node for this value (no-op with tracing off).
    void recordLeaf() {
#if MORPH_QUANTITY_PROVENANCE
        auto node = std::make_shared<detail::ASTNode>();
        node->current.lhs = payload;
        _ctx.node = std::move(node);
#endif
    }
};

/// @brief Trait: is `T` some `Quantity<U, Decimals>`?
namespace detail {
template <typename T>
struct IsQuantity : std::false_type {};

template <auto U, std::uint32_t Decimals>
struct IsQuantity<Quantity<U, Decimals>> : std::true_type {};
}  // namespace detail

/// @brief `true` when `T` (cvref-stripped) is a `morph::units::Quantity`.
/// @tparam T Candidate type.
template <typename T>
inline constexpr bool isQuantity = detail::IsQuantity<std::remove_cvref_t<T>>::value;

// Definition of the auto-generated ratio convert (declared above; defined here
// where `Quantity` is complete).
template <auto From, auto To>
    requires RatioConvertible<From, To>
void convert(const Quantity<From>& in, Quantity<To>& out) {
    constexpr auto ratioResult = detail::conversionRatio(From, To);
    auto scaled = *in.value() * ratioResult.ratio;
    scaled.decimalPlaces = in.value()->decimalPlaces;
    out = Quantity<To>{scaled};
}

// ---------------------------------------------------------------------------
// Comparison. `==` is total; ordering requires both operands engaged.
// ---------------------------------------------------------------------------

/// @brief Equality across same-unit quantities of any declared precision.
///        Ignores the runtime precision tag; empty==empty is `true`.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return `true` when both empty or both engaged with equal value.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] constexpr bool operator==(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) noexcept {
    return lhs.payload == rhs.payload;
}

/// @brief Ordering across same-unit quantities. Precondition: both operands are
///        engaged; comparing an empty operand throws `std::logic_error` (a
///        defined diagnostic — ordering an absent value is a programming error).
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The ordering of the two exact values.
/// @throws std::logic_error when either operand is empty.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] constexpr std::strong_ordering operator<=>(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) {
    if (!lhs.payload || !rhs.payload) {
        throw std::logic_error{"morph::units::Quantity: relational comparison requires engaged operands"};
    }
    return *lhs.payload <=> *rhs.payload;
}

// ---------------------------------------------------------------------------
// Arithmetic. Empty propagates; division by zero yields empty. Binary results
// carry the unit's default declared precision (the value's runtime precision
// still max-propagates inside Rational).
// ---------------------------------------------------------------------------

/// @brief Same-unit sum across any declared precisions.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact sum, or empty when either operand is empty.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] Quantity<U> operator+(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) {
    std::optional<math::Rational> result =
        (lhs.payload && rhs.payload) ? std::optional<math::Rational>{*lhs.payload + *rhs.payload} : std::nullopt;
    Quantity<U> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "+", lhs.payload, rhs.payload, result, MORPH_Q_NODE(lhs), MORPH_Q_NODE(rhs));
    return out;
}

/// @brief Same-unit difference across any declared precisions.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact difference, or empty when either operand is empty.
template <auto U, std::uint32_t DecA, std::uint32_t DecB>
[[nodiscard]] Quantity<U> operator-(const Quantity<U, DecA>& lhs, const Quantity<U, DecB>& rhs) {
    std::optional<math::Rational> result =
        (lhs.payload && rhs.payload) ? std::optional<math::Rational>{*lhs.payload - *rhs.payload} : std::nullopt;
    Quantity<U> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "-", lhs.payload, rhs.payload, result, MORPH_Q_NODE(lhs), MORPH_Q_NODE(rhs));
    return out;
}

/// @brief Mixed-unit sum: converts @p rhs to @p lhs's unit first.
/// @param lhs Left operand (its unit is the result unit).
/// @param rhs Right operand (a convertible unit).
/// @return The exact sum in `A`'s unit, or empty.
template <auto A, std::uint32_t DecA, auto B, std::uint32_t DecB>
    requires(detail::SameEnumDistinct<A, B> && requires(Quantity<B, DecB> value) { static_cast<Quantity<A>>(value); })
[[nodiscard]] Quantity<A> operator+(const Quantity<A, DecA>& lhs, const Quantity<B, DecB>& rhs) {
    return lhs + static_cast<Quantity<A>>(rhs);
}

/// @brief Mixed-unit difference: converts @p rhs to @p lhs's unit first.
/// @param lhs Left operand (its unit is the result unit).
/// @param rhs Right operand (a convertible unit).
/// @return The exact difference in `A`'s unit, or empty.
template <auto A, std::uint32_t DecA, auto B, std::uint32_t DecB>
    requires(detail::SameEnumDistinct<A, B> && requires(Quantity<B, DecB> value) { static_cast<Quantity<A>>(value); })
[[nodiscard]] Quantity<A> operator-(const Quantity<A, DecA>& lhs, const Quantity<B, DecB>& rhs) {
    return lhs - static_cast<Quantity<A>>(rhs);
}

/// @brief Negation. Empty stays empty; the declared precision is kept.
/// @param operand Value to negate.
/// @return The negated value, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] Quantity<U, Dec> operator-(const Quantity<U, Dec>& operand) {
    std::optional<math::Rational> result =
        operand.payload ? std::optional<math::Rational>{-*operand.payload} : std::nullopt;
    Quantity<U, Dec> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "-", operand.payload, std::nullopt, result, MORPH_Q_NODE(operand), nullptr);
    return out;
}

/// @brief Cross-unit product; the result unit is `A * B` per the application's
///        consteval unit algebra. Removed from overload resolution when the
///        algebra rejects the combination.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @return The exact product in the deduced unit, or empty.
template <auto A, std::uint32_t DecA, auto B, std::uint32_t DecB>
    requires requires { typename std::type_identity_t<Quantity<A * B>>; }
[[nodiscard]] Quantity<A * B> operator*(const Quantity<A, DecA>& lhs, const Quantity<B, DecB>& rhs) {
    std::optional<math::Rational> result =
        (lhs.payload && rhs.payload) ? std::optional<math::Rational>{*lhs.payload * *rhs.payload} : std::nullopt;
    Quantity<A * B> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "*", lhs.payload, rhs.payload, result, MORPH_Q_NODE(lhs), MORPH_Q_NODE(rhs));
    return out;
}

/// @brief Cross-unit quotient; the result unit is `A / B` per the application's
///        consteval unit algebra. Empty when either side is empty or the
///        divisor is zero.
/// @param lhs Dividend.
/// @param rhs Divisor.
/// @return The exact quotient in the deduced unit, or empty.
template <auto A, std::uint32_t DecA, auto B, std::uint32_t DecB>
    requires requires { typename std::type_identity_t<Quantity<A / B>>; }
[[nodiscard]] Quantity<A / B> operator/(const Quantity<A, DecA>& lhs, const Quantity<B, DecB>& rhs) {
    std::optional<math::Rational> result;
    if (lhs.payload && rhs.payload) {
        if (auto quotient = lhs.payload->dividedBy(*rhs.payload); quotient.has_value()) {
            result = *quotient;
        }
    }
    Quantity<A / B> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "/", lhs.payload, rhs.payload, result, MORPH_Q_NODE(lhs), MORPH_Q_NODE(rhs));
    return out;
}

/// @brief Scales by a dimensionless rational (unit and declared precision kept).
/// @param lhs    Quantity to scale.
/// @param factor Dimensionless factor.
/// @return The scaled quantity, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] Quantity<U, Dec> operator*(const Quantity<U, Dec>& lhs, const math::Rational& factor) {
    std::optional<math::Rational> result =
        lhs.payload ? std::optional<math::Rational>{*lhs.payload * factor} : std::nullopt;
    Quantity<U, Dec> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "*", lhs.payload, std::optional<math::Rational>{factor}, result, MORPH_Q_NODE(lhs), nullptr);
    return out;
}

/// @brief Scales by a dimensionless rational (unit and declared precision kept).
/// @param factor Dimensionless factor.
/// @param rhs    Quantity to scale.
/// @return The scaled quantity, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] Quantity<U, Dec> operator*(const math::Rational& factor, const Quantity<U, Dec>& rhs) {
    return rhs * factor;
}

/// @brief Divides by a dimensionless rational (unit and declared precision
///        kept). Empty when the quantity is empty or the divisor is zero.
/// @param lhs     Quantity to divide.
/// @param divisor Dimensionless divisor.
/// @return The divided quantity, or empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] Quantity<U, Dec> operator/(const Quantity<U, Dec>& lhs, const math::Rational& divisor) {
    std::optional<math::Rational> result;
    if (lhs.payload) {
        if (auto quotient = lhs.payload->dividedBy(divisor); quotient.has_value()) {
            result = *quotient;
        }
    }
    Quantity<U, Dec> out;
    out.payload = result;
    MORPH_Q_BUILD(out, "/", lhs.payload, std::optional<math::Rational>{divisor}, result, MORPH_Q_NODE(lhs), nullptr);
    return out;
}

namespace detail {

/// @brief Fixed-capacity string usable as a non-type template parameter.
///
/// An alias for the shared `morph::detail::FixedString` — the same underlying
/// type the forms layer's `Choice` uses (`morph::forms::FixedString`), so there
/// is one definition, not two look-alikes.
/// @tparam N Buffer length including the terminating null.
template <std::size_t N>
using FixedString = ::morph::detail::FixedString<N>;

}  // namespace detail

/// @brief A `Quantity` that names itself on construction and slices losslessly
///        back to a plain `Quantity`. The name lives in the shared history.
/// @tparam Name Compile-time symbol name.
/// @tparam U    Unit enumerator.
template <detail::FixedString Name, auto U>
struct NamedQuantity : Quantity<U> {
    /// @brief The plain base quantity type.
    using Base = Quantity<U>;

    /// @brief Constructs empty, then names.
    NamedQuantity() : Base{Base{}.named(std::string{Name.view()})} {}

    /// @brief Constructs from a payload, then names.
    /// @param adopted Engaged or empty payload.
    NamedQuantity(std::optional<math::Rational> adopted)
        : Base{Base{std::move(adopted)}.named(std::string{Name.view()})} {}

    /// @brief Constructs from a plain quantity, then names.
    /// @param quantity The source quantity.
    NamedQuantity(Base quantity) : Base{quantity.named(std::string{Name.view()})} {}

    /// @brief Names a floating-point reading.
    ///
    /// Not `noexcept`: unlike `Base::fromDouble`, this wraps the result in a
    /// `NamedQuantity`, whose construction calls `named()` — and with
    /// `MORPH_QUANTITY_PROVENANCE` enabled `named()` allocates an `ASTNode`
    /// (`std::make_shared`), which can throw `std::bad_alloc`.
    /// @param raw The value to convert.
    /// @return The named quantity.
    [[nodiscard]] static NamedQuantity fromDouble(double raw) { return NamedQuantity{Base::fromDouble(raw)}; }
};

/// @brief Renders @p quantity's exact decimal **alone**, with no unit suffix
///        (`"5.2"`, `"6"`, `"N/A"`).
///
/// The numeric half of `toString`, and the only public way to obtain it. A view
/// that places the number and the unit separately — a table with the unit in
/// its column header, a right-aligned suffix, an editable field whose adjacent
/// label carries the unit — asks for the two halves independently instead of
/// concatenating them and then chopping the suffix back off. The unit half is
/// `Quantity::unitMeta().display`.
///
/// This is the same exact-decimal path `std::format("{}", quantity)` takes: an
/// integer long division of the canonical `Rational` at its **runtime**
/// `DecimalPlaces`, trailing zeros trimmed. It is deliberately **not**
/// `std::format("{}", *quantity.value())` (which prints a `num/den` fraction)
/// nor `std::format("{:.Nf}", *quantity.value())` (which delegates to
/// `std::formatter<double>` via `toDouble()` and so leaves the exact domain —
/// `9007199254740993` renders as `…992` there and as itself here).
///
/// **An empty quantity renders as the fixed literal `"N/A"`, not as an empty
/// string.** That keeps the identity `toString(q) == toDecimalString(q) +
/// unitMeta().display` true for every value, engaged or not, so `toString` can
/// be — and is — implemented in terms of this function rather than growing a
/// second rendering path that can drift from it. Rendering an absent value as
/// blank is a presentation policy and belongs to the layer that has a layout to
/// decide it for; the value type only reports the fact that nothing is there.
/// A caller that wants a blank writes `q.hasValue() ? toDecimalString(q) : ""`.
/// @tparam U   Unit enumerator.
/// @tparam Dec Declared decimals.
/// @param quantity The value to render.
/// @return The exact decimal text, or `"N/A"` when the quantity is empty.
template <auto U, std::uint32_t Dec>
[[nodiscard]] inline std::string toDecimalString(const Quantity<U, Dec>& quantity) {
    return detail::formatOptionalDecimal(quantity.value());
}

/// @brief Renders @p quantity as value + unit (`5.2kW`, `N/A%`) — the same
///        text `std::format("{}", quantity)` produces via the `std::formatter`
///        specialization just below, exposed as a plain function so a caller
///        can render a `Quantity` without going through `std::format` itself.
///
/// Exists because Emscripten's bundled libc++ (older than the Linux/Windows
/// standard library this project otherwise builds against — see
/// `.github/workflows/wasm-ladder.yml`'s `EMSDK_VERSION`) has a known
/// limitation recognising `std::formatter` partial specializations
/// parameterized over an `auto` non-type template parameter (`U` here) for
/// `std::format`'s compile-time formattability check — `std::format("{}",
/// someQuantity)` fails to compile there with "the supplied type is not
/// formattable" even though the specialization is valid and the identical
/// call compiles and runs correctly on every other toolchain this project
/// targets. `toString()` bypasses that check entirely: it calls the same
/// underlying logic directly instead of through `std::format`'s trait
/// machinery, so it works identically everywhere, WASM included.
///
/// Defined as `toDecimalString(quantity) + unitMeta().display` — the number and
/// the unit are one concatenation with a single implementation of each half, so
/// a caller that needs them apart calls `toDecimalString` rather than undoing
/// the join.
/// @tparam U   Unit enumerator.
/// @tparam Dec Declared decimals.
/// @param quantity The value to render.
/// @return The formatted text.
template <auto U, std::uint32_t Dec>
[[nodiscard]] inline std::string toString(const Quantity<U, Dec>& quantity) {
    constexpr auto display = UnitTraits<decltype(U)>::meta(U).display;
    return toDecimalString(quantity) + std::string{display};
}

}  // namespace morph::units

#if MORPH_QUANTITY_PROVENANCE
#include "../detail/quantity_equation.hpp"
#endif

/// @brief Renders value + unit (`5.2kW`, `N/A%`); no `operator<<` is provided.
///        Delegates to `morph::units::toString` so the two never drift.
/// @tparam U   Unit enumerator.
/// @tparam Dec Declared decimals.
template <auto U, std::uint32_t Dec>
struct std::formatter<morph::units::Quantity<U, Dec>> {
    /// @brief Accepts the empty format spec.
    /// @param ctx Parse context.
    /// @return Iterator past the parsed spec.
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /// @brief Formats the quantity.
    /// @param quantity The value to format.
    /// @param ctx      Format context.
    /// @return Output iterator past the written text.
    auto format(const morph::units::Quantity<U, Dec>& quantity, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", morph::units::toString(quantity));
    }
};

/// @brief Forwards to the `Quantity` formatter.
/// @tparam Name Symbol name.
/// @tparam U    Unit enumerator.
template <morph::units::detail::FixedString Name, auto U>
struct std::formatter<morph::units::NamedQuantity<Name, U>> : std::formatter<morph::units::Quantity<U>> {
    /// @brief Formats via the base `Quantity` formatter.
    /// @param quantity The named value.
    /// @param ctx      Format context.
    /// @return Output iterator past the written text.
    auto format(const morph::units::NamedQuantity<Name, U>& quantity, std::format_context& ctx) const {
        return std::formatter<morph::units::Quantity<U>>::format(quantity, ctx);
    }
};

// ---------------------------------------------------------------------------
// Glaze integration: wire shape + schema units.
// ---------------------------------------------------------------------------

/// @brief On the wire a Quantity is its nullable Rational payload — neither the
///        unit nor the declared precision travels.
/// @tparam U   Unit enumerator.
/// @tparam Dec Declared decimals.
template <auto U, std::uint32_t Dec>
struct glz::meta<morph::units::Quantity<U, Dec>> {
    /// @brief The single wire field: the payload.
    static constexpr auto value = &morph::units::Quantity<U, Dec>::payload;

    /// @brief The unit's ascii id as the type name.
    static constexpr std::string_view name = morph::units::UnitTraits<decltype(U)>::meta(U).id;
};

namespace glz::detail {

/// @brief Schema generation for `Quantity<U, Dec>`: the nullable-Rational schema
///        with the unit stamped on as `ExtUnits`, sourced from `UnitTraits`.
/// @tparam U   Unit enumerator.
/// @tparam Dec Declared decimals.
template <auto U, std::uint32_t Dec>
struct to_json_schema<morph::units::Quantity<U, Dec>> {
    /// @brief Emits the schema.
    /// @tparam Opts Glaze options.
    /// @param outSchema Schema being built.
    /// @param defs      Schema definitions.
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<std::optional<morph::math::Rational>>::template op<Opts>(outSchema, defs);
        constexpr auto unitMeta = morph::units::UnitTraits<decltype(U)>::meta(U);
        outSchema.ExtUnits = ExtUnits{.unitAscii = unitMeta.id, .unitUnicode = unitMeta.display};
    }
};

}  // namespace glz::detail

/// @brief Stable shape tag for `Quantity<U, Dec>`, carrying the unit's ascii id
///        and the declared decimals.
///
/// This is the one place a `Quantity` retype can be caught at all. Neither the
/// unit nor the declared precision travels on the wire (see the `glz::meta`
/// above — a `Quantity` *is* its nullable `Rational` payload), so swapping
/// `Quantity<Unit::Gram>` for `Quantity<Unit::Litre>` in a recorded action
/// produces byte-identical JSON: no decode can notice, and before this tag no
/// fingerprint could either. `UnitMeta::id` is an author-declared ascii
/// identifier that is already part of the protocol vocabulary, so it is stable
/// across compilers in a way `glz::name_v` is not. See
/// `morph/core/payload_shape_tag.hpp`.
/// @tparam U   Unit enumerator.
/// @tparam Dec Declared decimals.
template <auto U, std::uint32_t Dec>
struct morph::model::PayloadShapeTag<morph::units::Quantity<U, Dec>> {
    /// @brief This type's stable shape name, e.g. `"quantity.gram.3"`.
    /// @return The name; the referenced storage lives for the whole process.
    static std::string_view name() {
        static const std::string kName =
            "quantity." + std::string{morph::units::UnitTraits<decltype(U)>::meta(U).id} + '.' + std::to_string(Dec);
        return kName;
    }
};
