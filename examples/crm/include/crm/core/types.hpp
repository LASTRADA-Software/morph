// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/quantity.hpp>
#include <optional>
#include <string_view>

namespace crm {

/// @brief Macro-free strong id boilerplate, one struct per identity role —
///        identical shape to `lims::ClientId`/`ledger::LedgerId`/kanban's
///        `ProjectId` (`std::optional<std::int64_t>` payload, `hasValue()`,
///        `fromOptional()`, `operator*()`, total ordering).
#define CRM_DEFINE_STRONG_ID(Name)                                                 \
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

CRM_DEFINE_STRONG_ID(AccountId);
CRM_DEFINE_STRONG_ID(ContactId);
CRM_DEFINE_STRONG_ID(LeadId);
CRM_DEFINE_STRONG_ID(OpportunityId);
CRM_DEFINE_STRONG_ID(QuoteId);
CRM_DEFINE_STRONG_ID(QuoteLineId);
CRM_DEFINE_STRONG_ID(ConflictId);
CRM_DEFINE_STRONG_ID(SavedViewId);

#undef CRM_DEFINE_STRONG_ID

/// @brief An opportunity's position in the sales pipeline.
///
/// Every transition is a guarded, journaled action (README build order §3,
/// modelled on `kanban::BoardModel::execute(MoveTaskPosition)`'s
/// validate/re-check/journal/cascade sequence) — the legal edges live in
/// `pipeline_dto.hpp`'s `isLegalStageTransition`, not here, so this enum
/// stays a plain tag.
enum class OpportunityStage : std::uint8_t {
    Prospecting,    ///< Newly created, not yet qualified.
    Qualification,  ///< Actively being qualified against BANT-style criteria.
    Proposal,       ///< A quote has been sent.
    Negotiation,    ///< Terms are being negotiated.
    Won,            ///< Closed won. Terminal.
    Lost,           ///< Closed lost. Terminal.
};

/// @brief A lead's position before conversion.
enum class LeadStatus : std::uint8_t {
    New,        ///< Captured, not yet worked.
    Working,    ///< Actively being qualified.
    Converted,  ///< Converted into an Account + Contact + Opportunity. Terminal.
    Lost,       ///< Disqualified without conversion. Terminal.
};

/// @brief crm's one unit family — deal-value money (README build order §7:
///        `Opportunity::expectedCloseValue` needs a `morph::units::Quantity`
///        so `primaryContact`'s `requiredWhen` rule has an `EmptyCapableField`/
///        `ComparableField` to condition on; a plain `Rational` or a raw
///        `enum class` (crm's `OpportunityStage`) satisfies neither concept —
///        see `dto/pipeline_dto.hpp`'s adjoining note on why stage itself
///        cannot be the condition).
enum class CrmUnit : std::uint8_t {
    usd,  ///< US dollars, 2 decimal places — the only unit this rung needs.
};

}  // namespace crm

/// @brief Unit metadata for `CrmUnit`. One member, no conversion relations —
///        crm has no multi-unit money (unlike lims's mg/µg/ng ladder), so
///        `relations` stays empty.
template <>
struct morph::units::UnitTraits<crm::CrmUnit> {
    [[nodiscard]] static constexpr morph::units::UnitMeta meta(crm::CrmUnit unit) noexcept {
        switch (unit) {
            case crm::CrmUnit::usd:
            default:
                return {.id = "usd", .display = "$", .defaultDecimals = 2};
        }
    }

    static constexpr std::array<morph::units::UnitRelation<crm::CrmUnit>, 0> relations{};
};

namespace crm {

/// @brief A US-dollar deal value, at 2 decimal places — the `EmptyCapableField`/
///        `ComparableField` `Opportunity::expectedCloseValue` needs so
///        `primaryContact`'s `requiredWhen` rule (`opportunity_dto.hpp`) has
///        a condition to test.
using Money = ::morph::units::Quantity<CrmUnit::usd, 2>;

}  // namespace crm

/// @brief On the wire each strong id is its nullable underlying integer —
///        same rationale as `lims`'s identical block: glaze has no
///        reflection for a non-aggregate whose only member is an optional,
///        so every DTO carrying one would fail to compile inside glaze's
///        `to`/`from` templates without this. Declared at global scope (not
///        nested inside `namespace crm`): MSVC's C2888 rejects a `template<>
///        struct glz::meta<T>` explicit specialization written from inside
///        an unrelated enclosing namespace, even when fully qualified back
///        to `glz::` — the same rule `UnitTraits<crm::CrmUnit>` above already
///        follows.
#define CRM_DEFINE_STRONG_ID_WIRE(Name)                  \
    template <>                                          \
    struct glz::meta<crm::Name> {                        \
        static constexpr auto value = &crm::Name::value; \
        static constexpr std::string_view name = #Name;  \
    }

CRM_DEFINE_STRONG_ID_WIRE(AccountId);
CRM_DEFINE_STRONG_ID_WIRE(ContactId);
CRM_DEFINE_STRONG_ID_WIRE(LeadId);
CRM_DEFINE_STRONG_ID_WIRE(OpportunityId);
CRM_DEFINE_STRONG_ID_WIRE(QuoteId);
CRM_DEFINE_STRONG_ID_WIRE(QuoteLineId);
CRM_DEFINE_STRONG_ID_WIRE(ConflictId);
CRM_DEFINE_STRONG_ID_WIRE(SavedViewId);

#undef CRM_DEFINE_STRONG_ID_WIRE

template <>
struct glz::meta<crm::OpportunityStage> {
    using enum crm::OpportunityStage;
    static constexpr auto value = glz::enumerate(Prospecting, Qualification, Proposal, Negotiation, Won, Lost);
};

template <>
struct glz::meta<crm::LeadStatus> {
    using enum crm::LeadStatus;
    static constexpr auto value = glz::enumerate(New, Working, Converted, Lost);
};
