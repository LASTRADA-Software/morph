// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/billing/core/types.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/references.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Billable lines: what a published result becomes in billing.

namespace lims {

/// @brief Bills one published result: what lab's `lab.AnalysisPublished` becomes in billing.
///
/// Sent only by the app's event relay, as a service principal; a redelivered event id answers the line it made.
struct RecordBillableLine {
    /// @brief The event's id, `<module>.<event>:<aggregate id>:<aggregate version>`; the same for a redelivery.
    std::string eventId;
    /// @brief The published result.
    ResultId result;
    /// @brief Its sample.
    SampleId sample;
    /// @brief The analysis definition it was captured under.
    AnalysisId analysis;
    /// @brief The sample's client.
    ClientId client;
    /// @brief The sample's site; it names the company that bills.
    SiteId site;
    /// @brief When the sample was published; the date the price is looked up on.
    ::morph::time::Timestamp publishedAt;

    /// @brief Whether the event names everything a line needs.
    /// @return `true` when it has an event id of at most 128 bytes and every id and the date.
    [[nodiscard]] bool validate() const noexcept {
        return !eventId.empty() && eventId.size() <= 128 && result.hasValue() && sample.hasValue() &&
               analysis.hasValue() && client.hasValue() && site.hasValue() && publishedAt.hasValue();
    }
};

/// @brief One published result, to be billed.
struct BillableLineView {
    /// @brief Its id.
    BillableLineId id;
    /// @brief The company that bills it: the sample's site's company.
    CompanyId company;
    /// @brief The sample's site.
    SiteId site;
    /// @brief The sample's client.
    ClientId client;
    /// @brief The sample.
    SampleId sample;
    /// @brief The result.
    ResultId result;
    /// @brief The analysis definition.
    AnalysisId analysis;
    /// @brief When the sample was published.
    ::morph::time::Timestamp publishedAt;
    /// @brief Where the line stands.
    BillableLineState state = BillableLineState::Unbilled;
    /// @brief The price; empty for an `Unpriced` line.
    Amount amount;
    /// @brief The currency of @p amount; meaningful only when priced.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief The invoice it is on; empty until invoiced.
    InvoiceId invoice;
    /// @brief The id of the event that made it.
    std::string eventId;
};

/// @brief Lists the billable lines of the companies the caller can see.
struct ListBillableLines {
    /// @brief Only this company's lines; empty for every company.
    CompanyId company;
    /// @brief Only this client's lines; empty for every client.
    ClientId client;
    /// @brief Only this sample's lines; empty for every sample.
    SampleId sample;

    /// @brief Always ready: an empty field matches every line.
    /// @return `true`.
    [[nodiscard]] static bool validate() noexcept { return true; }
};

/// @brief The billable lines, by id.
struct ListBillableLinesResult {
    /// @brief The lines.
    std::vector<BillableLineView> lines;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::BillableLineView, "Billing_Line")
