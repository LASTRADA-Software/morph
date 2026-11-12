// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/inventory/core/types.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Requisitions: a request to buy, raised at a site, submitted by its requester and approved by a purchase approver
/// whose value limit covers its total, as the wire sees them.

namespace lims {

/// @brief Raises a requisition for a site. It starts as a draft with no line.
struct CreateRequisition {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company; the site must be one of its sites.
    CompanyId company;
    /// @brief The site whose approvers judge it.
    SiteId site;
    /// @brief Why it is needed.
    std::string reason;

    /// @brief Whether the requisition is well-formed.
    /// @return `true` when a company, a site and a reason of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && site.hasValue() && !reason.empty() && reason.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Adds a line to a draft requisition.
///
/// The quantity may be in any unit of the item's dimension; it is stored in the item's unit.
struct AddRequisitionLine {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The requisition.
    RequisitionId requisition;
    /// @brief The item, of the requisition's company.
    ItemId item;
    /// @brief How much, in @p unit.
    Measure quantity;
    /// @brief The unit @p quantity is in.
    StockUnit unit = StockUnit::piece;
    /// @brief The estimated total of the line, in the company's currency.
    Amount estimate;

    /// @brief Whether the line is well-formed.
    /// @return `true` when a requisition, an item, a quantity and an estimate are named.
    [[nodiscard]] bool validate() const noexcept {
        return requisition.hasValue() && item.hasValue() && quantity.hasValue() && estimate.hasValue();
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Submits a draft requisition for approval, recording the deadline after which its approval escalates.
struct SubmitRequisition {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The requisition.
    RequisitionId requisitionId;

    /// @brief Whether a requisition is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return requisitionId.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief Approves a submitted requisition.
struct ApproveRequisition {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The requisition.
    RequisitionId requisitionId;

    /// @brief Whether a requisition is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return requisitionId.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One line of a requisition.
struct RequisitionLineView {
    /// @brief Its id.
    RequisitionLineId id;
    /// @brief The item.
    ItemId item;
    /// @brief The item's code.
    std::string itemCode;
    /// @brief How much, in @p unit.
    Measure quantity;
    /// @brief The item's unit.
    StockUnit unit = StockUnit::piece;
    /// @brief The estimated total of the line, in the requisition's currency.
    Amount estimate;
};

/// @brief A requisition.
struct RequisitionView {
    /// @brief Its id.
    RequisitionId id;
    /// @brief The company.
    CompanyId company;
    /// @brief The site whose approvers judge it before its deadline.
    SiteId site;
    /// @brief Why it is needed.
    std::string reason;
    /// @brief Where it stands.
    RequisitionState state = RequisitionState::Draft;
    /// @brief Its lines, by id.
    std::vector<RequisitionLineView> lines;
    /// @brief The lines' exact sum, which an approver's limit is judged against.
    Amount total;
    /// @brief The currency of every amount on it.
    CurrencyCode currency = CurrencyCode::EUR;
    /// @brief Who raised it.
    std::string requestedBy;
    /// @brief Who submitted it; empty until submitted.
    std::string submittedBy;
    /// @brief Who approved it; empty until approved.
    std::string approvedBy;
    /// @brief When it was submitted; empty until then.
    ::morph::time::Timestamp submittedAt;
    /// @brief When its approval escalates to the company; empty until submitted.
    ::morph::time::Timestamp approvalDeadline;
    /// @brief Who must approve it: `Company` once the deadline has passed.
    RequisitionLevel level = RequisitionLevel::Site;
    /// @brief The purchase order it became; empty until ordered.
    PurchaseOrderId order;
};

/// @brief Reads a requisition.
struct GetRequisition {
    /// @brief The requisition.
    RequisitionId requisitionId;

    /// @brief Whether a requisition is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return requisitionId.hasValue(); }
};

/// @brief Lists a company's requisitions.
struct ListRequisitions {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief Lists the submitted requisitions the caller could approve now.
struct ListPendingRequisitions {
    /// @brief Whether the query is well-formed.
    /// @return Always `true`: it names nothing.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief A list of requisitions, by id.
struct ListRequisitionsResult {
    /// @brief The requisitions.
    std::vector<RequisitionView> requisitions;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::RequisitionView, "Inventory_Requisition")
