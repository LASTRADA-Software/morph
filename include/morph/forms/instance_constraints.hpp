// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/instance_constraints.hpp
/// @brief Per-*instance* constraints on a form field: the seam by which data
///        that lives in a database row — not in a template parameter — reaches
///        the framework-meaningful keys of a served schema, and is checked
///        against a submitted value by the same declaration that served it.
///
/// `schemaJson<A>()` is a pure function of the compiled action type `A`: every
/// key it emits is derived from `A`'s members, so two rows that describe the
/// *same* action differently — an analysis catalogue whose version 1 declares
/// three decimal places and whose version 2 declares one — cannot be told
/// apart by anything the framework enforces. Before this header the only way
/// out was for the application to patch a *second*, app-private key beside the
/// framework's (`x-versionDecimalPlaces` next to `x-decimalPlaces`) and
/// re-implement the check by hand, which leaves a renderer with two keys for
/// one concept and no way to know which to believe.
///
/// `InstanceConstraints` replaces that with one declaration that does both
/// jobs:
///
/// - `decorate()` rewrites the served schema so the *framework's own* keys
///   carry the instance's values (`x-decimalPlaces`, `x-minimum`,
///   `x-maximum`), and stamps the document-level `x-instanceConstraints` array
///   naming every field whose keys came from instance data rather than from
///   the compiled type — so a renderer can tell the two apart.
/// - `checkAction()` / `checkValue()` report what a submitted value violates,
///   against the identical declaration. Serving a bound and checking it can no
///   longer drift apart, because there is only one place the bound is written.
///
/// @par What this deliberately does not do
/// The framework does **not** decide what a violation means. An out-of-range
/// value is a rejection in one domain and a value to *flag* in another (a lab
/// result outside its specification range is reportable, not refusable), so
/// `checkAction`/`checkValue` return violations and the model applies its own
/// policy. Nor is any of this wired into the dispatch runners: those decode an
/// action from bytes with no access to the row a constraint lives in (see
/// docs/spec/forms/instance_constraints.md, "Why dispatch cannot apply these
/// automatically"). A model that decorates a schema is responsible for
/// checking against the same constraints; the `x-instanceConstraints` stamp is
/// what makes a schema that was decorated visible to a reviewer and a
/// renderer.
///
/// @par Structure is still compile-time
/// Only *values* of existing keys vary per instance. Which fields exist, which
/// are required, and the `x-rules` list all remain functions of the compiled
/// type — an instance cannot add a field. See the spec's "Limitations".

#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../util/quantity.hpp"
#include "../util/rational.hpp"
#include "forms.hpp"

namespace morph::forms {

/// @brief Which framework-meaningful key a submitted value fell foul of.
enum class ConstraintViolationKind : std::uint8_t {
    /// @brief The value is strictly below the declared inclusive `minimum`.
    BelowMinimum,
    /// @brief The value is strictly above the declared inclusive `maximum`.
    AboveMaximum,
    /// @brief The value is not exactly representable at the declared
    ///        `decimalPlaces` — it carries more precision than the instance
    ///        says the field has.
    PrecisionExceeded,
};

/// @brief One violation: which field, and which declared key it broke.
struct ConstraintViolation {
    /// @brief The offending field's wire (JSON) name.
    std::string field;
    /// @brief What was violated.
    ConstraintViolationKind kind{ConstraintViolationKind::BelowMinimum};

    /// @brief Value equality, so violations can be compared in tests.
    /// @param other The violation to compare against.
    /// @return `true` when both the field and the kind match.
    [[nodiscard]] bool operator==(const ConstraintViolation& other) const noexcept = default;
};

/// @brief A stable, machine-readable name for a violation kind — for log
///        lines and model-level error messages.
/// @param kind The violation kind to name.
/// @return `"belowMinimum"`, `"aboveMaximum"` or `"precisionExceeded"`.
[[nodiscard]] inline std::string_view violationKindName(ConstraintViolationKind kind) noexcept {
    switch (kind) {
        case ConstraintViolationKind::BelowMinimum:
            return "belowMinimum";
        case ConstraintViolationKind::AboveMaximum:
            return "aboveMaximum";
        case ConstraintViolationKind::PrecisionExceeded:
            return "precisionExceeded";
        default:
            return "unknown";
    }
}

/// @brief The per-instance constraint declared for one field of an action.
///
/// Every member other than `field` is optional and defaults to "not declared":
/// an absent `decimalPlaces` leaves the compiled `x-decimalPlaces` alone, and
/// an absent bound emits nothing and checks nothing. A constraint naming a
/// field the action does not have is ignored (schema decoration never throws
/// over an application's declaration mistake, exactly like `formLayout`).
struct FieldConstraint {
    /// @brief Wire (JSON) key of the member this entry constrains.
    std::string field;

    /// @brief The instance's decimal precision for this field. Overwrites
    ///        `x-decimalPlaces`, and a submitted value that is not exactly
    ///        representable at this many places is `PrecisionExceeded`.
    std::optional<std::uint32_t> decimalPlaces{};

    /// @brief Inclusive lower bound; emitted as `x-minimum`.
    std::optional<math::Rational> minimum{};

    /// @brief Inclusive upper bound; emitted as `x-maximum`.
    std::optional<math::Rational> maximum{};
};

namespace detail {

/// @brief `10^places`, for the exact-representability test below.
///
/// `Rational` bounds its own `decimalPlaces` to `kMaxDecimalPlaces` (18, the
/// largest power of ten that fits an `std::int64_t`), and callers clamp to the
/// same range before calling this, so it cannot overflow.
/// @param places Decimal places, in `[0, morph::math::kMaxDecimalPlaces]`.
/// @return `10^places`.
[[nodiscard]] constexpr std::int64_t powerOfTenForConstraint(std::uint32_t places) noexcept {
    std::int64_t result = 1;
    for (std::uint32_t i = 0; i < places; ++i) {
        result *= 10;
    }
    return result;
}

/// @brief Whether @p value is *exactly* representable at @p places decimals.
///
/// `Rational` keeps `gcd(|numerator|, denominator) == 1`, so `numerator *
/// 10^places` is divisible by `denominator` exactly when `10^places` is — the
/// test needs no multiplication and therefore cannot overflow.
/// @param value The submitted exact value.
/// @param places The instance's declared decimal places.
/// @return `true` when the value needs no rounding at that precision.
[[nodiscard]] constexpr bool exactAtDecimals(const math::Rational& value, std::uint32_t places) noexcept {
    if (places > math::kMaxDecimalPlaces) {
        return false;
    }
    return powerOfTenForConstraint(places) % value.denominator == 0;
}

/// @brief Renders one exact bound as the same `{"num","den","dp"}` node shape
///        a `Rational` uses on the wire, so a renderer parses a bound exactly
///        the way it parses the value it bounds.
/// @param bound The exact bound.
/// @return The DOM node.
[[nodiscard]] inline glz::generic_u64 boundNode(const math::Rational& bound) {
    glz::generic_u64 node{};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
    node["num"] = bound.numerator;
    node["den"] = bound.denominator;
    node["dp"] = static_cast<std::uint64_t>(bound.decimalPlaces.value);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return node;
}

}  // namespace detail

/// @brief A set of per-field constraints sourced from one instance (a database
///        row, a tenant record, a catalogue version) — the single declaration
///        that both decorates a served schema and checks a submitted value.
///
/// @code{.cpp}
/// morph::forms::InstanceConstraints constraints;
/// constraints.declare({.field = "value",
///                      .decimalPlaces = version.decimalPlaces,
///                      .minimum = version.specLow,
///                      .maximum = version.specHigh});
///
/// // Serving: the framework's own keys now carry the version's values.
/// const auto schema = morph::forms::instanceSchemaJson<CaptureConcentration>(constraints);
///
/// // Checking: the same object, so the served bound and the checked bound
/// // cannot disagree.
/// for (const auto& violation : constraints.checkAction(action)) { ... }
/// @endcode
class InstanceConstraints {
public:
    /// @brief Declares (or replaces, by field name) one field's constraint.
    /// @param constraint The constraint to record.
    /// @return `*this`, so declarations can be chained.
    InstanceConstraints& declare(FieldConstraint constraint) {
        for (auto& existing : _fields) {
            if (existing.field == constraint.field) {
                existing = std::move(constraint);
                return *this;
            }
        }
        _fields.push_back(std::move(constraint));
        return *this;
    }

    /// @brief The constraint declared for @p field, if any.
    /// @param field The wire (JSON) field name to look up.
    /// @return A pointer to the entry, or `nullptr` when the field is
    ///         unconstrained. Invalidated by any later `declare()`.
    [[nodiscard]] const FieldConstraint* forField(std::string_view field) const noexcept {
        for (const auto& entry : _fields) {
            if (entry.field == field) {
                return &entry;
            }
        }
        return nullptr;
    }

    /// @brief Whether nothing has been declared.
    /// @return `true` when no field carries a constraint.
    [[nodiscard]] bool empty() const noexcept { return _fields.empty(); }

    /// @brief Every declared constraint, in declaration order.
    /// @return The constraint entries.
    [[nodiscard]] const std::vector<FieldConstraint>& fields() const noexcept { return _fields; }

    /// @brief Rewrites @p schema so the framework's own keys carry this
    ///        instance's values.
    ///
    /// For each declared field that the schema actually has a property for:
    /// `x-decimalPlaces` is **overwritten** with the declared precision (when
    /// one is declared), and `x-minimum` / `x-maximum` are added as
    /// `{"num","den","dp"}` nodes. The document then carries
    /// `x-instanceConstraints`: the array of field names whose keys came from
    /// instance data, which is how a renderer (and a reviewer) tells a
    /// decorated schema from the compiled one.
    ///
    /// Never throws and never mangles: a @p schema that is not readable JSON
    /// is returned verbatim, and a constraint naming a field the schema has no
    /// property for is skipped — the same "an author's declaration mistake is
    /// not worth a crash" rule `mergeSchemaExtras` follows.
    /// @param schema The compiled schema text, typically `schemaJson<A>()`.
    /// @return The decorated schema text.
    [[nodiscard]] std::string decorate(std::string schema) const {
        if (_fields.empty()) {
            return schema;
        }
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
        // u64 number mode, for the same reason mergeSchemaExtras uses it: the
        // schema carries int64/uint64 bounds in $defs that a double-only DOM
        // would silently round on the round trip.
        glz::generic_u64 dom{};
        if (glz::read_json(dom, schema)) {
            return schema;
        }
        if (!dom.contains("properties")) {
            return schema;
        }
        auto& properties = dom["properties"];
        glz::generic_u64::array_t decorated{};
        for (const auto& entry : _fields) {
            if (!properties.contains(entry.field)) {
                continue;  // names a field the action does not have: ignored, never thrown
            }
            auto& property = properties[entry.field];
            if (entry.decimalPlaces.has_value()) {
                property["x-decimalPlaces"] =
                    static_cast<std::uint64_t>(math::detail::clampWireDecimalPlaces(*entry.decimalPlaces));
            }
            if (entry.minimum.has_value()) {
                property["x-minimum"] = detail::boundNode(*entry.minimum);
            }
            if (entry.maximum.has_value()) {
                property["x-maximum"] = detail::boundNode(*entry.maximum);
            }
            decorated.emplace_back(entry.field);
        }
        if (decorated.empty()) {
            return schema;
        }
        dom["x-instanceConstraints"] = decorated;
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        return glz::write_json(dom).value_or(schema);
    }

    /// @brief Checks one exact value against the constraint declared for
    ///        @p field.
    ///
    /// The value-level entry point, for a model whose stored value is
    /// *derived* from the payload rather than taken from it verbatim (a
    /// reading multiplied by a dilution factor, a total recomputed from line
    /// items): the constraint governs the value the model is about to store,
    /// which is not a member of the action at all.
    /// @param field The wire (JSON) field name the value belongs to.
    /// @param value The exact value to check.
    /// @return Every violation, in `minimum`, `maximum`, `decimalPlaces`
    ///         order; empty when the field is unconstrained or the value
    ///         satisfies every declared key.
    [[nodiscard]] std::vector<ConstraintViolation> checkValue(std::string_view field,
                                                              const math::Rational& value) const {
        std::vector<ConstraintViolation> violations{};
        const auto* entry = forField(field);
        if (entry == nullptr) {
            return violations;
        }
        if (entry->minimum.has_value() && (value <=> *entry->minimum) == std::strong_ordering::less) {
            violations.push_back({.field = std::string{field}, .kind = ConstraintViolationKind::BelowMinimum});
        }
        if (entry->maximum.has_value() && (value <=> *entry->maximum) == std::strong_ordering::greater) {
            violations.push_back({.field = std::string{field}, .kind = ConstraintViolationKind::AboveMaximum});
        }
        if (entry->decimalPlaces.has_value() && !detail::exactAtDecimals(value, *entry->decimalPlaces)) {
            violations.push_back({.field = std::string{field}, .kind = ConstraintViolationKind::PrecisionExceeded});
        }
        return violations;
    }

    /// @brief Checks every constrained `Quantity` member of @p action.
    ///
    /// Walks the action's reflected members in declaration order and runs
    /// `checkValue` for each engaged `Quantity` whose wire name carries a
    /// constraint. An empty `Quantity` is skipped (emptiness is `required`'s
    /// business, not a bound's), as is any member that is not a `Quantity` —
    /// bounds and decimal precision are exact-value concepts, and `Quantity`
    /// is the only field type morph forms carry an exact value in. A no-op for
    /// an action type glaze cannot reflect.
    /// @tparam A     Action type (a reflectable aggregate).
    /// @param action The decoded action to check.
    /// @return Every violation across every constrained field, in member
    ///         declaration order.
    template <typename A>
    [[nodiscard]] std::vector<ConstraintViolation> checkAction(const A& action) const {
        using Plain = std::remove_cvref_t<A>;
        std::vector<ConstraintViolation> violations{};
        if constexpr (glz::reflectable<Plain> || glz::glaze_object_t<Plain>) {
            forms::detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, const auto& member) {
                static_cast<void>(I);
                using Member = std::remove_cvref_t<decltype(member)>;
                if constexpr (units::isQuantity<Member>) {
                    if (member.hasValue()) {
                        for (auto& violation : checkValue(name, *member)) {
                            violations.push_back(std::move(violation));
                        }
                    }
                } else {
                    static_cast<void>(name);
                    static_cast<void>(member);
                }
            });
        } else {
            static_cast<void>(action);
        }
        return violations;
    }

private:
    std::vector<FieldConstraint> _fields{};
};

/// @brief `schemaJson<A>()` decorated with @p constraints — the served form
///        for one instance.
///
/// Not memoised: unlike `schemaJson<A>()` (one cached string per compiled
/// type), the result varies per instance, so the caller caches it if it wants
/// caching. The compiled half is still memoised inside `schemaJson<A>()`, so
/// the per-call cost is the DOM round trip, not schema generation.
/// @tparam A Action type (a reflectable aggregate).
/// @param constraints The instance's constraints.
/// @return The decorated schema text.
template <typename A>
[[nodiscard]] std::string instanceSchemaJson(const InstanceConstraints& constraints) {
    return constraints.decorate(schemaJson<A>());
}

}  // namespace morph::forms
