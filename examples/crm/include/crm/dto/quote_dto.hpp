// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "crm/core/types.hpp"

/// @file
/// Quote/pricing DTOs (README build order §4, "Tryton semantics": exact
/// line items, discounts, tax, and a recomputed total). `morph::math::Rational`
/// for every money value — matching `ledger::TransactionLeg::amount`'s
/// precedent, not a floating-point type.
///
/// Design decision, in writing: a line's own `total = qty * unitPrice -
/// discount` uses the framework's real `computed`/`computeList` mechanism
/// (precedented and working, `tests/test_computed_fields.cpp`). The quote's
/// *grand* total (sum across every line, plus tax) has **no** declarative
/// path — `docs/spec/forms/forms.md` states computed fields are "top-level
/// only... regardless of nesting depth", so a `computedFields` entry cannot
/// fold over a `std::vector<QuoteLine>` member. `QuoteModel::execute()`
/// therefore computes the grand total imperatively, in a plain loop over
/// `lines`, matching `ledger::checkZeroSumByCurrency`'s/`buildLedgerState`'s
/// identical hand-written-accumulation precedent — this is the rung's own
/// resolution of the "[framework gap]" crm/README.md's Expected Strain
/// Points names for nested line items.

namespace crm {

/// @brief One priced line of a quote.
///
/// `total` is declared computed (`qty * unitPrice - discount`, at the
/// currency's own decimal precision) so a submitted line's total can never
/// drift from its inputs — the same guarantee `docs/spec/forms/forms.md`'s
/// own worked example gives a flat action, applied here per-element. Because
/// `computedFields` is top-level-only, this recomputation runs only when
/// `QuoteLine` itself is the type `recomputeAll`/`schemaJson` is
/// instantiated over — which happens nowhere in the dispatch path today
/// (`CreateQuote`/`UpdateQuote` are the dispatched actions, not `QuoteLine`),
/// so `QuoteModel` calls `morph::forms::recomputeAll<QuoteLine>` on each
/// line by hand before validating/persisting it. See that model's doc
/// comment for why.
struct QuoteLine {
    std::string productName;
    morph::math::Rational quantity;
    morph::math::Rational unitPrice;
    /// @brief Absolute discount subtracted from `quantity * unitPrice`
    ///        before tax — a percentage discount is `unitPrice`-relative and
    ///        out of scope for this rung (README names no discount-kind
    ///        requirement beyond "discounts").
    morph::math::Rational discount;
    morph::math::Rational total;

    static constexpr auto computedFields = ::morph::forms::computeList(
        ::morph::forms::computed<&QuoteLine::total, &QuoteLine::quantity, &QuoteLine::unitPrice, &QuoteLine::discount>(
            [](const auto& line) { return line.quantity * line.unitPrice - line.discount; }));

    /// @brief Whether this line's own fields are individually well-formed.
    ///
    /// The per-element half of the "app-level recursive validator" crm's
    /// Expected Strain Points names — `allRequiredEngaged` stops at the
    /// top-level action per `docs/spec/forms/forms.md`, so `QuoteModel`
    /// calls this once per element of `lines` rather than relying on any
    /// framework-provided nested enforcement.
    [[nodiscard]] bool validate() const noexcept {
        return !productName.empty() && quantity.numerator > 0 && unitPrice.numerator >= 0 && discount.numerator >= 0;
    }
};

enum class QuoteStatus : std::uint8_t {
    Draft,     ///< Being edited; not yet sent.
    Sent,      ///< Sent to the customer.
    Accepted,  ///< Customer accepted. Terminal.
    Rejected,  ///< Customer declined. Terminal.
};

struct QuoteView {
    QuoteId id;
    OpportunityId opportunityId;
    QuoteStatus status = QuoteStatus::Draft;
    std::vector<QuoteLine> lines;
    /// @brief Percentage tax rate applied to the sum of every line's `total`
    ///        (e.g. `Rational{7, 100, dp=4}` for 7%) — a single rate for the
    ///        whole quote, matching Tryton's document-level tax line rather
    ///        than a per-line tax rate (out of scope: this rung names no
    ///        per-line-tax requirement).
    morph::math::Rational taxRate;
    /// @brief Sum of every line's `total`, plus tax — computed imperatively
    ///        by `QuoteModel`, not by the forms framework (see this file's
    ///        header comment). Never trust a submitted value for this field;
    ///        it is always server-recomputed before storage.
    morph::math::Rational grandTotal;
    std::int32_t version = 0;
};

struct CreateQuote {
    OpportunityId opportunityId;
    std::vector<QuoteLine> lines;
    morph::math::Rational taxRate;

    [[nodiscard]] bool validate() const noexcept {
        if (!opportunityId.hasValue() || lines.empty()) {
            return false;
        }
        for (const auto& line : lines) {
            if (!line.validate()) {
                return false;
            }
        }
        return taxRate.numerator >= 0;
    }
};

struct CreateQuoteResult {
    QuoteView quote;
};

/// @brief Replaces a quote's lines/tax rate wholesale. Only legal while the
///        quote is still `Draft` — `Sent`/`Accepted`/`Rejected` are terminal
///        for editing, matching `LeadModel`/`OpportunityModel`'s identical
///        guarded-state-machine convention.
struct UpdateQuote {
    QuoteId quoteId;
    std::vector<QuoteLine> lines;
    morph::math::Rational taxRate;
    std::int32_t expectedVersion = 0;

    [[nodiscard]] bool validate() const noexcept {
        if (!quoteId.hasValue() || lines.empty()) {
            return false;
        }
        for (const auto& line : lines) {
            if (!line.validate()) {
                return false;
            }
        }
        return taxRate.numerator >= 0;
    }
};

struct UpdateQuoteResult {
    QuoteView quote;
};

/// @brief Sends a Draft quote (Draft -> Sent). No content change.
struct SendQuote {
    QuoteId quoteId;

    [[nodiscard]] bool validate() const noexcept { return quoteId.hasValue(); }
};

/// @brief Records the customer's decision on a Sent quote (Sent -> Accepted
///        or Sent -> Rejected).
struct DecideQuote {
    QuoteId quoteId;
    bool accepted = false;

    [[nodiscard]] bool validate() const noexcept { return quoteId.hasValue(); }
};

struct GetQuote {
    QuoteId quoteId;

    [[nodiscard]] bool validate() const noexcept { return quoteId.hasValue(); }
};

struct ListQuotes {
    std::optional<OpportunityId> opportunityId;

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ListQuotesResult {
    std::vector<QuoteView> quotes;
};

}  // namespace crm

template <>
struct glz::meta<crm::QuoteStatus> {
    using enum crm::QuoteStatus;
    static constexpr auto value = glz::enumerate(Draft, Sent, Accepted, Rejected);
};
