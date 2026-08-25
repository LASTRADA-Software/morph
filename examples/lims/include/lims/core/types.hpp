// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string_view>

namespace lims {

/// @brief Macro-free strong id boilerplate, one struct per identity role —
///        identical shape to `ledger::LedgerId` and kanban's `ProjectId`
///        (`std::optional<std::int64_t>` payload, `hasValue()`,
///        `fromOptional()`, `operator*()`, total ordering).
#define LIMS_DEFINE_STRONG_ID(Name)                                                \
    struct Name {                                                                  \
        std::optional<std::int64_t> value{};                                       \
        Name() = default;                                                          \
        explicit Name(std::int64_t v) : value{v} {}                                \
        [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); } \
        [[nodiscard]] std::int64_t operator*() const { return *value; }            \
        static Name fromOptional(std::optional<std::int64_t> v) {                  \
            Name id;                                                               \
            id.value = v;                                                          \
            return id;                                                             \
        }                                                                          \
        auto operator<=>(const Name&) const = default;                             \
    }

LIMS_DEFINE_STRONG_ID(ClientId);
LIMS_DEFINE_STRONG_ID(SampleId);
LIMS_DEFINE_STRONG_ID(AnalysisId);
LIMS_DEFINE_STRONG_ID(AnalysisVersionId);
LIMS_DEFINE_STRONG_ID(ResultId);
LIMS_DEFINE_STRONG_ID(WorksheetId);
LIMS_DEFINE_STRONG_ID(VerificationId);
LIMS_DEFINE_STRONG_ID(ConflictId);

#undef LIMS_DEFINE_STRONG_ID

/// @brief A sample's position in the lab workflow.
///
/// Every transition is a guarded, journaled action (README build order §2);
/// the state machine's legal edges live in `sample_dto.hpp`'s
/// `isLegalTransition`, not here, so this enum stays a plain tag.
enum class SampleState : std::uint8_t {
    Registered,    ///< Logged by the office; not yet physically received.
    Received,      ///< Physically in the lab.
    InProgress,    ///< At least one analysis assigned and being worked.
    ToBeVerified,  ///< All assigned results captured, awaiting four-eyes verification.
    Published,     ///< Verified and released to the client. Terminal.
    Rejected,      ///< Rejected at receipt (broken container, wrong preservative). Terminal.
};

/// @brief What a captured result actually says.
///
/// The forms palette has no sum types (closed by design, round-5 review), so
/// `quantity | belowLOD | aboveUDL` is encoded as a `Quantity` plus this
/// qualifier, glued by `mutuallyExclusive`/`exactlyOneOf` rules. The README
/// calls out that all three "no number" meanings must round-trip
/// *distinguishably* — `NotMeasured` is not the same claim as `BelowLOD`.
enum class ResultQualifier : std::uint8_t {
    Measured,     ///< The quantity carries a real reading.
    NotMeasured,  ///< No attempt made. The quantity is empty.
    BelowLOD,     ///< Below the limit of detection. The quantity is empty.
    AboveUDL,     ///< Above the upper detection limit. The quantity is empty.
};

/// @brief Units this rung's analyses measure in.
///
/// Concentration units are the interesting ones: the ratios between them span
/// 10^9, which is exactly the range the README flags as a strain point for
/// both the QML converter's 1e12 divisor limit and `x-unitAlternatives`'
/// direct-edge-only behaviour.
enum class LimsUnit : std::uint8_t {
    scalar,    ///< Dimensionless (pH, absorbance).
    mg_per_L,  ///< Milligrams per litre — the canonical concentration unit here.
    ug_per_L,  ///< Micrograms per litre.
    ng_per_L,  ///< Nanograms per litre.
    celsius,   ///< Temperature.
    mA,        ///< Milliamps — the InvenTree "1500 mA vs A" flow.
    A,         ///< Amps.
};

}  // namespace lims

/// @brief On the wire each strong id is its nullable underlying integer —
///        same rationale as `ledger`'s identical block: glaze has no
///        reflection for a non-aggregate whose only member is an optional,
///        so every DTO carrying one would fail to compile inside glaze's
///        `to`/`from` templates without this.
#define LIMS_DEFINE_STRONG_ID_WIRE(Name)                  \
    template <>                                           \
    struct glz::meta<lims::Name> {                        \
        static constexpr auto value = &lims::Name::value; \
        static constexpr std::string_view name = #Name;   \
    }

LIMS_DEFINE_STRONG_ID_WIRE(ClientId);
LIMS_DEFINE_STRONG_ID_WIRE(SampleId);
LIMS_DEFINE_STRONG_ID_WIRE(AnalysisId);
LIMS_DEFINE_STRONG_ID_WIRE(AnalysisVersionId);
LIMS_DEFINE_STRONG_ID_WIRE(ResultId);
LIMS_DEFINE_STRONG_ID_WIRE(WorksheetId);
LIMS_DEFINE_STRONG_ID_WIRE(VerificationId);
LIMS_DEFINE_STRONG_ID_WIRE(ConflictId);

#undef LIMS_DEFINE_STRONG_ID_WIRE

/// @brief Unit metadata and the exact conversion graph for `LimsUnit`.
///
/// `relations` lists **direct edges only** — `Quantity`'s converter does not
/// chain them. The README flags this: an InvenTree-style "enter in any
/// compatible unit" flow needs every pair a user may type spelled out, so
/// ng/L↔mg/L is declared explicitly rather than left to compose out of
/// ng/L↔µg/L and µg/L↔mg/L.
template <>
struct morph::units::UnitTraits<lims::LimsUnit> {
    /// @brief Display metadata for @p unit.
    /// @param unit The unit to describe.
    /// @return Its id, display string, and default decimal precision.
    static constexpr morph::units::UnitMeta meta(lims::LimsUnit unit) noexcept {
        switch (unit) {
            case lims::LimsUnit::mg_per_L:
                return {.id = "mg_per_L", .display = "mg/L", .defaultDecimals = 3};
            case lims::LimsUnit::ug_per_L:
                return {.id = "ug_per_L", .display = "µg/L", .defaultDecimals = 3};
            case lims::LimsUnit::ng_per_L:
                return {.id = "ng_per_L", .display = "ng/L", .defaultDecimals = 3};
            case lims::LimsUnit::celsius:
                return {.id = "celsius", .display = "°C", .defaultDecimals = 1};
            case lims::LimsUnit::mA:
                return {.id = "mA", .display = "mA", .defaultDecimals = 0};
            case lims::LimsUnit::A:
                return {.id = "A", .display = "A", .defaultDecimals = 3};
            case lims::LimsUnit::scalar:
            default:
                return {.id = "scalar", .display = "", .defaultDecimals = 2};
        }
    }

    /// @brief Exact within-dimension conversion ratios, as direct edges.
    static constexpr std::array<morph::units::UnitRelation<lims::LimsUnit>, 4> relations{{
        // µg/L -> mg/L : 1/1000
        {lims::LimsUnit::ug_per_L, lims::LimsUnit::mg_per_L,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                               morph::math::DecimalPlaces{3}}},
        // ng/L -> µg/L : 1/1000
        {lims::LimsUnit::ng_per_L, lims::LimsUnit::ug_per_L,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                               morph::math::DecimalPlaces{3}}},
        // ng/L -> mg/L : 1/1000000, spelled out because relations are not chained.
        {lims::LimsUnit::ng_per_L, lims::LimsUnit::mg_per_L,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000000},
                               morph::math::DecimalPlaces{6}}},
        // mA -> A : 1/1000 (the InvenTree flow).
        {lims::LimsUnit::mA, lims::LimsUnit::A,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                               morph::math::DecimalPlaces{3}}},
    }};
};

namespace lims {

/// @brief A concentration reading, in the canonical mg/L at 3 decimal places.
///
/// One compiled alias per *unit family*, not per analysis: `Quantity`'s unit
/// and declared precision are template parameters, so a result-entry action
/// can only ever name a unit the C++ type already knows. A catalogue whose
/// analyses are data therefore cannot grow an action type per analysis — see
/// the rung README's findings section. mg/L is the family's canonical unit;
/// µg/L and ng/L reach it through `UnitTraits::relations`.
using Concentration = ::morph::units::Quantity<LimsUnit::mg_per_L, 3>;

/// @brief A concentration entered in µg/L, for conversion to `Concentration`.
using ConcentrationMicro = ::morph::units::Quantity<LimsUnit::ug_per_L, 3>;

/// @brief A concentration entered in ng/L. Its mg/L ratio is 10^-6 — the
///        fine-ratio range the README flags as a strain point.
using ConcentrationNano = ::morph::units::Quantity<LimsUnit::ng_per_L, 3>;

/// @brief A dilution factor: dimensionless, three decimal places.
///
/// A `Quantity` over the `scalar` unit rather than a bare `Rational` so it
/// joins the forms palette as an empty-capable field — which is what lets the
/// conditional rules in `result_dto.hpp` name it at all.
using DilutionFactor = ::morph::units::Quantity<LimsUnit::scalar, 3>;

/// @brief A current reading, in the canonical amps at 3 decimal places —
///        the InvenTree "1500 mA against a template in A" flow.
using Current = ::morph::units::Quantity<LimsUnit::A, 3>;

/// @brief A current entered in milliamps, as instruments report it.
using CurrentMilli = ::morph::units::Quantity<LimsUnit::mA, 1>;

}  // namespace lims
