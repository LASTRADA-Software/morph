// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <glaze/glaze.hpp>
#include <morph/forms/choice.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/core/types.hpp"

/// @file
/// Result entry (README build order §3): a `Quantity` with real units, and
/// the multi-field encoding of `ResultValue = quantity | belowLOD | aboveUDL`.
///
/// @par Why the encoding looks like this
/// The forms palette has no sum types — closed by design, confirmed by the
/// round-5 review — so the sum is spelled as two fields plus one rule:
///
/// @code
/// exactlyOneOf(&CaptureConcentration::value, &CaptureConcentration::qualifier)
/// @endcode
///
/// A reading engages `value` and leaves `qualifier` empty. A *non*-reading
/// leaves `value` empty and engages `qualifier` with the code naming which
/// non-reading it is. `exactlyOneOf` is what makes "a number *and* a
/// below-LOD flag" unrepresentable rather than merely discouraged, and it is
/// enforced identically on both sides: the renderer reads it from `x-rules`
/// in the served schema, and the server re-runs the same compiled rule list
/// because `validate()` calls `allRulesSatisfied`.
///
/// @par The three "no number" meanings
/// `notMeasured`, `belowLOD` and `aboveUDL` are three different scientific
/// claims — "we did not look", "we looked and it is under the instrument's
/// floor", "we looked and it is over its ceiling" — and the README requires
/// them to stay distinguishable through the wire, the journal, and the
/// offline queue's opaque payloads. They are distinct strings in one
/// `Choice`, not a single "empty" flag, which is what makes that mechanical
/// rather than a convention.

namespace lims {

/// @brief Wire code for `ResultQualifier::NotMeasured`.
inline constexpr std::string_view kQualifierNotMeasured = "notMeasured";

/// @brief Wire code for `ResultQualifier::BelowLOD`.
inline constexpr std::string_view kQualifierBelowLod = "belowLOD";

/// @brief Wire code for `ResultQualifier::AboveUDL`.
inline constexpr std::string_view kQualifierAboveUdl = "aboveUDL";

/// @brief Decodes a qualifier wire code, **fail-closed**.
///
/// An unrecognised code is `std::nullopt`, never a default: silently
/// resolving an unknown code to `NotMeasured` would turn a client the server
/// does not understand into a lab result that says "we did not look", which
/// is a fabricated claim rather than a parse failure.
/// @param code The wire code from a `qualifier` `Choice`.
/// @return The qualifier it names, or `std::nullopt` if the code is unknown.
[[nodiscard]] constexpr std::optional<ResultQualifier> qualifierFromCode(std::string_view code) noexcept {
    if (code == kQualifierNotMeasured) {
        return ResultQualifier::NotMeasured;
    }
    if (code == kQualifierBelowLod) {
        return ResultQualifier::BelowLOD;
    }
    if (code == kQualifierAboveUdl) {
        return ResultQualifier::AboveUDL;
    }
    return std::nullopt;
}

/// @brief The wire code for @p qualifier, or an empty view for `Measured`
///        (which is encoded as an engaged `value`, not as a code).
/// @param qualifier The stored qualifier.
/// @return Its wire code, or `""` for `Measured`.
[[nodiscard]] constexpr std::string_view qualifierCode(ResultQualifier qualifier) noexcept {
    switch (qualifier) {
        case ResultQualifier::NotMeasured:
            return kQualifierNotMeasured;
        case ResultQualifier::BelowLOD:
            return kQualifierBelowLod;
        case ResultQualifier::AboveUDL:
            return kQualifierAboveUdl;
        case ResultQualifier::Measured:
        default:
            return {};
    }
}

/// @brief The stable ascii name of @p qualifier, for display and logging.
///
/// Distinct from `qualifierCode`, which is the *wire* code and is empty for
/// `Measured` (a reading is encoded as an engaged value, not as a code). A
/// display needs a word for every case, including that one.
/// @param qualifier The stored qualifier.
/// @return Its name.
[[nodiscard]] constexpr std::string_view qualifierName(ResultQualifier qualifier) noexcept {
    switch (qualifier) {
        case ResultQualifier::Measured:
            return "measured";
        case ResultQualifier::NotMeasured:
            return kQualifierNotMeasured;
        case ResultQualifier::BelowLOD:
            return kQualifierBelowLod;
        case ResultQualifier::AboveUDL:
            return kQualifierAboveUdl;
        default:
            return "unknown";
    }
}

/// @brief One row of the qualifier picklist a `QualifierChoice` renders from.
struct QualifierOption {
    /// @brief The wire code submitted as the field's value.
    std::string id;

    /// @brief The text shown to the operator.
    std::string name;
};

/// @brief Serves the qualifier picklist. The options action named in
///        `QualifierChoice`'s own type.
struct ListResultQualifiers {
    /// @brief Always ready: a picklist takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The qualifier picklist.
struct ListResultQualifiersResult {
    /// @brief The three "no number" codes, with display text.
    std::vector<QualifierOption> qualifiers;
};

/// @brief The "this is not a number, and here is which kind of not-a-number"
///        half of the result encoding.
using QualifierChoice = ::morph::forms::Choice<std::string, "ListResultQualifiers", "id", "name">;

/// @brief Wire code meaning the aliquot was measured undiluted.
inline constexpr std::string_view kDilutionNeat = "neat";

/// @brief Wire code meaning the aliquot was diluted before measuring.
inline constexpr std::string_view kDilutionDiluted = "diluted";

/// @brief How the aliquot was prepared. The conditional-logic driver (README
///        build order §5): a dilution factor is required, and shown, only when
///        this says the aliquot was diluted.
using DilutionChoice = ::morph::forms::Choice<std::string, "ListDilutionModes", "id", "name">;

/// @brief One row of the dilution picklist.
struct DilutionOption {
    /// @brief The wire code submitted as the field's value.
    std::string id;

    /// @brief The text shown to the operator.
    std::string name;
};

/// @brief Serves the dilution picklist.
struct ListDilutionModes {
    /// @brief Always ready: a picklist takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The dilution picklist.
struct ListDilutionModesResult {
    /// @brief The preparation modes, with display text.
    std::vector<DilutionOption> modes;
};

/// @brief Captures a concentration result against the attached sample.
///
/// The value is submitted in the analysis version's **canonical** unit
/// (mg/L), which is the renderer contract: `x-unitAlternatives` lets an
/// operator type µg/L or ng/L, and the client converts before submitting.
/// The server therefore never has to guess what unit a bare number is in —
/// see `test_result_entry.cpp`'s entry-unit tests, which check the ratio the
/// schema advertises against the one `Quantity::operator Quantity<To>()`
/// applies, so a client honouring the schema and the server agree exactly.
struct CaptureConcentration {
    /// @brief The exact analysis *version* this reading was captured under.
    AnalysisVersionId analysisVersionId;

    /// @brief The reading, in mg/L. Empty for every non-reading.
    Concentration value;

    /// @brief Which non-reading this is. Empty for a reading.
    QualifierChoice qualifier;

    /// @brief How the aliquot was prepared. Empty means "not stated", which
    ///        the rules treat exactly like `neat`: `equals` is **not** vacuous
    ///        on an unengaged field, so an empty `dilution` makes the
    ///        `requiredWhen` below vacuously satisfied rather than failing.
    DilutionChoice dilution;

    /// @brief The factor the aliquot was diluted by — required, and shown,
    ///        only when `dilution` says `diluted`.
    ///
    /// Load-bearing rather than decorative: the model multiplies the reading
    /// by it before storing, exactly (`Rational`, never a `double`), so a
    /// mis-declared dilution changes the reported concentration.
    DilutionFactor dilutionFactor;

    /// @brief Neither half of the sum is *unconditionally* required — the
    ///        `exactlyOneOf` rule below is the only gate on them.
    ///
    /// Without this, `schemaJson`'s required-ness rule (required by default
    /// for any non-`std::optional` empty-capable member) puts both `value`
    /// and `qualifier` in the schema's `required` array, where they directly
    /// contradict the `x-rules` entry beside them: a renderer honouring
    /// `required` would demand both fields, and a payload satisfying it would
    /// then fail `exactlyOneOf` on the server. The opt-out below is what makes
    /// the rule the only gate on the pair; since morph#165 `schemaJson` also
    /// *rejects* the contradiction (`UnsatisfiableFormError`) instead of
    /// serving a form nobody can submit, so omitting it is now a loud error
    /// rather than a silent one.
    static constexpr std::array optionalFields{std::string_view{"value"}, std::string_view{"qualifier"},
                                               std::string_view{"dilution"}, std::string_view{"dilutionFactor"}};

    /// @brief The sum-type encoding, as a cross-field rule the client and the
    ///        server both evaluate from one declaration.
    static constexpr auto formRules = ::morph::forms::ruleList(
        ::morph::forms::exactlyOneOf(&CaptureConcentration::value, &CaptureConcentration::qualifier),
        // The conditional pair. They are declared *separately* and neither
        // implies the other, which is the framework's stated contract: an
        // author who wants "hidden ⇒ also not required" says so twice, on
        // purpose. See the rung README's §5 decision on clear-on-hide.
        ::morph::forms::requiredWhen(&CaptureConcentration::dilutionFactor,
                                     ::morph::forms::equals(&CaptureConcentration::dilution, "diluted")),
        ::morph::forms::visibleWhen(&CaptureConcentration::dilutionFactor,
                                    ::morph::forms::equals(&CaptureConcentration::dilution, "diluted")));

    /// @brief Whether this capture is well-formed.
    ///
    /// Deliberately *not* `allRequiredEngaged`: that would demand both fields
    /// at once, which is precisely what the encoding forbids.
    /// @return `true` when a version is named and exactly one of
    ///         value/qualifier is engaged.
    [[nodiscard]] bool validate() const noexcept {
        return analysisVersionId.hasValue() && ::morph::forms::allRulesSatisfied(*this);
    }
};

/// @brief One captured result, as served back.
struct ResultView {
    /// @brief The result row's id.
    ResultId id;

    /// @brief The sample it belongs to.
    SampleId sampleId;

    /// @brief The analysis version it was captured under — never the analysis
    ///        identity, which is the whole point of the versioning design.
    AnalysisVersionId analysisVersionId;

    /// @brief What the result says. `Measured` iff `value` is engaged.
    ResultQualifier qualifier = ResultQualifier::NotMeasured;

    /// @brief The reading in mg/L, or empty for every non-reading.
    Concentration value;

    /// @brief The principal who captured it.
    std::string capturedBy;

    /// @brief When it was captured.
    ::morph::time::Timestamp capturedAt;
};

/// @brief Lists every result captured against the attached sample.
struct ListResults {
    /// @brief Always ready: the listing takes no arguments.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The attached sample's results.
struct ListResultsResult {
    /// @brief Every captured result, oldest first.
    std::vector<ResultView> results;
};

}  // namespace lims

/// @brief Reflects `ResultQualifier` as its name rather than an integer.
///
/// The three "no number" meanings have to survive a round trip through the
/// journal and the offline queue *distinguishably*; names do that even if the
/// enum later gains an enumerator, whereas `"qualifier":2` silently
/// re-points at whatever then sits at index 2.
template <>
struct glz::meta<lims::ResultQualifier> {
    using enum lims::ResultQualifier;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Measured, NotMeasured, BelowLOD, AboveUDL);
};
