// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
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
/// @warning This rung installs no authorizer, so nothing verifies a claimed
///          principal. The gate on `RunReportJob` is therefore a *layering*
///          check -- it keeps a user-issued action from silently completing a
///          job the runner owns -- not an authorization boundary. It becomes
///          one the moment this rung grows a signing authorizer, exactly as
///          bookmarks' did, and not before. See
///          `examples/ledger/include/ledger/app/app.hpp`.
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
