// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <morph/core/records.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/role_dto.hpp"

/// @file
/// What the application shell reads: the scopes a person can choose between, and the shapes every module answers its
/// open-work count and its search with. The shapes are defined once here; each module's own actions answer them, so a
/// module's count sits beside the records it counts and refreshes through that module's effects.

namespace lims {

/// @brief The arguments of a scope's detail line.
struct ScopeDetailArgs {
    /// @brief The company's base currency code.
    std::string currency;
    /// @brief How many of the company's sites the person can see, as text.
    std::string count;
};

/// @brief A scope's detail line as a message reference, which the client resolves through the catalogue in the
///        session's language: `{"t": key, "fallback": pattern, "args": {...}}`.
struct ScopeDetail {
    /// @brief The catalogue key of the pattern.
    std::string t;
    /// @brief The English pattern shown when the catalogue has none.
    std::string fallback;
    /// @brief The values the pattern's `{$name}` tokens stand for.
    ScopeDetailArgs args;
};

/// @brief One scope the person can work in: a company they can see.
///
/// The scope's value, the company's id, is what the shell hands every screen as its context. A scope is a company and
/// not a company-and-site pair because a context is a single string or integer, and every list a screen reads is keyed
/// by company; the detail line is the company's base currency and how many of its sites the person can see.
struct ScopeOption {
    /// @brief The company; the option's value.
    CompanyId company;
    /// @brief The company's name.
    std::string label;
    /// @brief The company's base currency code and, when the person can see any, the number of its sites that they can
    ///        see, worded in the session's language: `EUR · 2 sites`.
    ScopeDetail detail;
    /// @brief The company's base currency.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The decimals of that currency.
    Count decimals;
};

/// @brief Lists the scopes the caller can choose between.
struct ListMyScopes {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The scopes, by company.
struct ListMyScopesResult {
    /// @brief The scopes: every company the caller can see, in id order.
    std::vector<ScopeOption> scopes;
};

/// @brief How much of a module's work is open at a company, for the shell's badge.
///
/// The shell reads the member as a whole quantity, which is how a `Count` is written.
/// One template, instantiated per module with the record the count is derived from, so that each module's count is its
/// own record type and refreshes when that module's records change, not when another module's do.
/// @tparam Source The record whose change changes the count.
template <typename Source>
struct OpenWork {
    /// @brief The number of open items.
    Count count;
};

/// @brief A whole number of items.
/// @param value The number; not negative.
/// @return It as a `Count`.
[[nodiscard]] inline Count wholeCount(std::int64_t value) {
    return Count{::morph::math::Rational{value, ::morph::math::DecimalPlaces{0}}};
}

/// @brief A module's open work.
/// @tparam Source The record whose change changes the count.
/// @param open How many items are open.
/// @return The reply.
template <typename Source>
[[nodiscard]] OpenWork<Source> openWork(std::int64_t open) {
    return OpenWork<Source>{.count = wholeCount(open)};
}

/// @brief The most hits one search answers.
inline constexpr std::size_t kSearchHitLimit = 8;

/// @brief One record a search found, shaped as the shell's search panel reads a hit.
struct SearchHit {
    /// @brief The record's first line.
    std::string title;
    /// @brief The record's second line.
    std::string detail;
    /// @brief The screen that opens the record.
    std::string screen;
    /// @brief The screen's params.
    std::map<std::string, std::int64_t> params;
};

/// @brief What a search found at one company.
/// @tparam Source The record whose change changes the hits.
template <typename Source>
struct SearchHits {
    /// @brief The first hits, at most `kSearchHitLimit`, in the order of the module's own list.
    std::vector<SearchHit> hits;
    /// @brief How many records matched in all.
    Count total;
};

/// @brief The screens a hit opens, with the param each takes. The module's screens take these ids.
namespace hit_screens {
/// @brief A sample, param `sampleId`.
inline constexpr std::string_view kSample = "lims.lab.sample";
/// @brief An invoice, param `invoiceId`.
inline constexpr std::string_view kInvoice = "lims.billing.invoice";
/// @brief The chart of accounts at an account, param `accountId`.
inline constexpr std::string_view kAccount = "lims.ledger.accounts";
/// @brief The stock of an item, param `itemId`.
inline constexpr std::string_view kStock = "lims.inventory.stock";
}  // namespace hit_screens

/// @brief Whether @p text starts with @p prefix, ignoring ASCII case.
/// @param text The text.
/// @param prefix The prefix; empty matches nothing.
/// @return Whether it does.
[[nodiscard]] inline bool startsWithFolded(std::string_view text, std::string_view prefix) noexcept {
    if (prefix.empty() || text.size() < prefix.size()) {
        return false;
    }
    return std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    });
}

/// @brief Whether @p text contains @p needle, ignoring ASCII case.
/// @param text The text.
/// @param needle The needle; empty matches nothing.
/// @return Whether it does.
[[nodiscard]] inline bool containsFolded(std::string_view text, std::string_view needle) noexcept {
    if (needle.empty()) {
        return false;
    }
    return !std::ranges::search(text, needle, [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left)) ==
                       std::tolower(static_cast<unsigned char>(right));
            }).empty();
}

/// @brief The reply of a search.
/// @tparam Source The record whose change changes the hits.
/// @param found The matches the module read, in the order it lists them; at most `kSearchHitLimit` are kept.
/// @param total How many records matched in all, which may be more than were read.
/// @return The first `kSearchHitLimit` of @p found and the total.
template <typename Source>
[[nodiscard]] SearchHits<Source> hitsOf(std::vector<SearchHit> found, std::int64_t total) {
    SearchHits<Source> out;
    out.total = wholeCount(total);
    if (found.size() > kSearchHitLimit) {
        found.resize(kSearchHitLimit);
    }
    out.hits = std::move(found);
    return out;
}

/// @brief The reply of a search that read every match.
/// @tparam Source The record whose change changes the hits.
/// @param found Every match, in the order the module lists them.
/// @return The first `kSearchHitLimit` of them and the number found.
template <typename Source>
[[nodiscard]] SearchHits<Source> hitsOf(std::vector<SearchHit> found) {
    auto const total = static_cast<std::int64_t>(found.size());
    return hitsOf<Source>(std::move(found), total);
}

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::ScopeOption, "Org_Scope", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::CompanyView, lims::SiteView, lims::RoleAssignmentView>)
