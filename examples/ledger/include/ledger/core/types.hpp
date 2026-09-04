// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/payload_shape_tag.hpp>
#include <optional>
#include <string_view>

namespace ledger {

/// @brief Macro-free strong id boilerplate, one struct per identity role —
///        matches kanban's `ProjectId` shape
///        (docs/superpowers/specs/2026-08-16-kanban-rung4-design.md §7):
///        `std::optional<std::int64_t>` payload, `hasValue()`,
///        `fromOptional()`, `operator*()`, total ordering.
#define LEDGER_DEFINE_STRONG_ID(Name)                                              \
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

LEDGER_DEFINE_STRONG_ID(LedgerId);
LEDGER_DEFINE_STRONG_ID(AccountId);
LEDGER_DEFINE_STRONG_ID(JournalId);
LEDGER_DEFINE_STRONG_ID(CategoryId);
LEDGER_DEFINE_STRONG_ID(BudgetId);
LEDGER_DEFINE_STRONG_ID(RuleId);
LEDGER_DEFINE_STRONG_ID(ReportJobId);

#undef LEDGER_DEFINE_STRONG_ID

enum class AccountKind : std::uint8_t { Asset, Expense, Revenue, Liability };
enum class RuleTrigger : std::uint8_t { DescriptionContains };
enum class RuleAction : std::uint8_t { SetCategory };
enum class ReportKind : std::uint8_t { MonthlyStatement, BudgetReport };
enum class ReportStatus : std::uint8_t { Pending, Done, Failed };

}  // namespace ledger

/// @brief Every closed-set enum above, declared with a `glz::meta`/
///        `glz::enumerate` so glaze reflects it (travels as its enumerator
///        name, not its underlying integer) and `morph::forms::schemaJson<A>()`
///        can describe it as a closed set. Without this, `schemaJson<A>()` --
///        called unconditionally by `BRIDGE_REGISTER_ACTION`
///        (`morph::model::detail::buildActionDescription`), independent of
///        whether a rung renders any form -- fails to compile: glaze emits a
///        six-way wildcard type for a metaless enum, and the shipped Qt/QML
///        `DynamicForm` draws that as a checkbox reporting the form `ready`
///        for a value nobody chose (morph#392). Same shape as
///        `kanban::Role`/`kanban::RuleMutationType`
///        (examples/kanban/include/kanban/core/types.hpp), the rungs that
///        established this convention.
template <>
struct glz::meta<ledger::AccountKind> {
    using enum ledger::AccountKind;
    static constexpr auto value = glz::enumerate(Asset, Expense, Revenue, Liability);
};

template <>
struct glz::meta<ledger::RuleTrigger> {
    using enum ledger::RuleTrigger;
    static constexpr auto value = glz::enumerate(DescriptionContains);
};

template <>
struct glz::meta<ledger::RuleAction> {
    using enum ledger::RuleAction;
    static constexpr auto value = glz::enumerate(SetCategory);
};

template <>
struct glz::meta<ledger::ReportKind> {
    using enum ledger::ReportKind;
    static constexpr auto value = glz::enumerate(MonthlyStatement, BudgetReport);
};

template <>
struct glz::meta<ledger::ReportStatus> {
    using enum ledger::ReportStatus;
    static constexpr auto value = glz::enumerate(Pending, Done, Failed);
};

namespace ledger {

/// @brief The service principal `ledger::app::App`'s report runner dispatches
///        `RunReportJob` under, and the only principal
///        `LedgerModel::execute(const RunReportJob&)` accepts.
///
///        Declared here, in the rung's shared core header, rather than in
///        either place that uses it: the model must be able to check the name
///        without including an `app/` header (models know nothing about the
///        App layer), and the App must be able to name it without reaching
///        into the model's implementation file. The reserved `system:` prefix
///        mirrors `bookmarks::auth::kMetadataFetcherPrincipal` -- a namespace
///        no human login occupies, so a user principal cannot collide with it
///        by accident.
///
/// The gate on `RunReportJob` is a genuine authorization boundary:
/// `ledger::app::App` mints a real, signed token for this principal and
/// dispatches through its own `RemoteServer` + `LedgerAuthorizer`
/// (`examples/ledger/include/ledger/app/app.hpp`), and `AuthModel::
/// execute(const Login&)` refuses to mint a token in the reserved `system:`
/// namespace this principal occupies (`ledger::auth::isReservedPrincipal`),
/// so no client can obtain one by logging in. A user-issued action naming
/// this principal is refused the same way any other forged claim is: it
/// never carries a validly signed token for it.
inline constexpr std::string_view kReportRunnerPrincipal = "system:report-runner";

}  // namespace ledger

/// @brief On the wire, each `LEDGER_DEFINE_STRONG_ID` type is its nullable
///        underlying integer -- same rationale and shape as
///        `bookmarks::BookmarkId`'s `glz::meta` specialisation
///        (`examples/bookmarks/include/bookmarks/core/types.hpp`): without
///        this, glaze has no reflection for a type whose only public data
///        member is `std::optional<std::int64_t> value` wrapped in
///        non-aggregate machinery (an explicit constructor, `<=>`), and any
///        `BRIDGE_REGISTER_ACTION` on a DTO carrying one of these fails to
///        compile deep inside glaze's `to`/`from` templates. One
///        specialisation per id type, generated the same way the structs
///        themselves are, then undefined immediately after.
#define LEDGER_DEFINE_STRONG_ID_WIRE(Name)                  \
    template <>                                             \
    struct glz::meta<ledger::Name> {                        \
        static constexpr auto value = &ledger::Name::value; \
        static constexpr std::string_view name = #Name;     \
    }

LEDGER_DEFINE_STRONG_ID_WIRE(LedgerId);
LEDGER_DEFINE_STRONG_ID_WIRE(AccountId);
LEDGER_DEFINE_STRONG_ID_WIRE(JournalId);
LEDGER_DEFINE_STRONG_ID_WIRE(CategoryId);
LEDGER_DEFINE_STRONG_ID_WIRE(BudgetId);
LEDGER_DEFINE_STRONG_ID_WIRE(RuleId);
LEDGER_DEFINE_STRONG_ID_WIRE(ReportJobId);

#undef LEDGER_DEFINE_STRONG_ID_WIRE

/// @brief Stable payload-shape tag for each `LEDGER_DEFINE_STRONG_ID` type, so
///        a journal fingerprint can tell one id from another.
///
/// The `glz::meta` specialisations above are what make these types serialisable
/// at all, and they are also what makes them *opaque* to
/// `morph::model::payloadShape`: a type whose meta names a value rather than an
/// object has no reflected members to decompose, so it renders as the bare `x`
/// and every id in a payload looks like every other one
/// (`morph/core/payload_shape_tag.hpp`; `docs/spec/journal/journal.md`,
/// "Custom-codec types name themselves").
///
/// That matters more here than for most rungs, because this rung's payloads are
/// largely *made of* ids: `UndoTransaction{LedgerId, JournalId}` would
/// fingerprint as two interchangeable `x`s and `SetCategory{AccountId,
/// CategoryId, RuleId, …}` as three, so exchanging two of them -- what an id
/// rename or a copy-paste in a later rung produces -- would leave the
/// fingerprint untouched and `journal::replay()`'s mismatch gate with nothing
/// to fire on, while the recorded integers decoded into the wrong slots. The
/// ids are all `std::optional<std::int64_t>` on the wire, so the JSON is
/// byte-identical across such a swap and no decode, on any path, can catch it:
/// the tag is the only place it is visible at all.
///
/// The tag text is spelled here rather than derived from `glz::name_v`, which
/// is compiler-dependent -- a journal readable only by the build that wrote it
/// is the worse failure. It is part of the on-disk fingerprint of every entry
/// this rung records, so it is an interface: renaming a tag invalidates every
/// retained journal entry carrying that id, exactly as renaming a field does.
///
/// One specialisation per id type, generated the same way the structs and their
/// `glz::meta`s are, then undefined immediately after.
#define LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(Name, Tag)                      \
    template <>                                                           \
    struct morph::model::PayloadShapeTag<ledger::Name> {                  \
        /** @brief This id's stable shape name. @return The tag. */       \
        static constexpr std::string_view name() noexcept { return Tag; } \
    }

LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(LedgerId, "ledger.ledgerId");
LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(AccountId, "ledger.accountId");
LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(JournalId, "ledger.journalId");
LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(CategoryId, "ledger.categoryId");
LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(BudgetId, "ledger.budgetId");
LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(RuleId, "ledger.ruleId");
LEDGER_DEFINE_STRONG_ID_SHAPE_TAG(ReportJobId, "ledger.reportJobId");

#undef LEDGER_DEFINE_STRONG_ID_SHAPE_TAG
