// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace ledger {

/// @brief Macro-free strong id boilerplate, one struct per identity role —
///        matches kanban's `ProjectId` shape
///        (docs/superpowers/specs/2026-08-16-kanban-rung4-design.md §7):
///        `std::optional<std::int64_t>` payload, `hasValue()`,
///        `fromOptional()`, `operator*()`, total ordering.
#define LEDGER_DEFINE_STRONG_ID(Name)                                                            \
    struct Name {                                                                                \
        std::optional<std::int64_t> value{};                                                     \
        Name() = default;                                                                        \
        explicit Name(std::int64_t v) : value{v} {}                                              \
        [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }               \
        [[nodiscard]] std::int64_t operator*() const { return *value; }                          \
        static Name fromOptional(std::optional<std::int64_t> v) {                                \
            Name id;                                                                             \
            id.value = v;                                                                        \
            return id;                                                                           \
        }                                                                                         \
        auto operator<=>(const Name&) const = default;                                           \
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
