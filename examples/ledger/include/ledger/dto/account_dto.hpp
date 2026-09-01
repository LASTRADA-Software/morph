// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

namespace ledger {

/// @brief Longest book name this rung accepts, in bytes -- exactly
///        `db::LedgerRecord::name`'s `Light::SqlAnsiString<128>` capacity.
///        `src/models/ledger_model.cpp` carries the `static_assert` tying the
///        two together, the same guard `kanban::kMaxProjectNameBytes` and
///        `polls`' own title bound each carry: `Light::SqlFixedString`'s
///        constructor is `noexcept` and truncates rather than throwing, so a
///        bound that drifts above the column's capacity means `CreateLedger`
///        answers `ok` with an id whose stored name is not the one the caller
///        sent.
inline constexpr std::size_t kMaxLedgerNameBytes = 128;

/// @brief Creates a book -- the root entity every other ledger action keys
///        off (morph#361).
///
/// Lives here, beside `OpenAccount`/`GetLedger`, because those are the other
/// two actions whose subject is the book itself rather than what is posted
/// into it.
///
/// **Who may create one.** Any authenticated principal.
/// `LedgerModel::execute(const CreateLedger&)` refuses an empty principal
/// (`EmptyPrincipalError`, design spec §11) exactly as every other mutating
/// action on this model does, and `LedgerAuthorizer` already requires a
/// validly signed token for everything but `AuthModel`/`Login`, so a
/// tokenless caller never reaches the model at all. Beyond that there is no
/// gate, and deliberately so: unlike `kanban::CreateProject` -- whose caller
/// becomes the new project's first `Manager` in the same transaction -- this
/// rung ships no per-book roles at all, no table to write one into, and no
/// action that reads one. Recording an owner here would be the first half of
/// an authorization scheme whose second half does not exist, which reads as a
/// guarantee the rung cannot keep. What this rung *does* enforce is
/// documented in `ledger/auth/ledger_authorizer.hpp`'s file comment. Filed as
/// morph#382.
struct CreateLedger {
    std::string name;

    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= kMaxLedgerNameBytes; }
};

/// @brief The new book's id -- the value every subsequent action's
///        `ledgerId` carries.
///
/// A struct rather than a bare `LedgerId`, following
/// `kanban::CreateProjectResult`: a client reads `{"id": 7}` and can be given
/// more about the book later without changing the shape of a reply it already
/// parses. (`BudgetModel::execute(const CreateCategory&)` and friends return
/// a bare id instead; both shapes are live in this rung, and this one is the
/// one the sibling root-creating action across the ladder uses.)
struct CreateLedgerResult {
    LedgerId id;
};

struct OpenAccount {
    LedgerId ledgerId;
    std::string name;
    AccountKind kind;
    Currency currency;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !name.empty(); }
};

struct GetLedger {
    LedgerId ledgerId;

    [[nodiscard]] bool validate() const noexcept { return morph::forms::allRequiredEngaged(*this); }
};

struct AccountInfo {
    AccountId id;
    std::string name;
    AccountKind kind;
    Currency currency;
    morph::math::Rational balance;  // plain Rational -- real currency is the sibling `currency` field above,
                                    // never a Quantity's compile-time unit parameter (design spec §2)
};

struct GetLedgerResult {
    std::vector<AccountInfo> accounts;
};

}  // namespace ledger
