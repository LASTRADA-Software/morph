// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <morph/core/registry.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lims/inventory/dto/order_dto.hpp"
#include "lims/inventory/dto/receipt_dto.hpp"
#include "lims/inventory/dto/requisition_dto.hpp"
#include "lims/inventory/dto/stock_dto.hpp"
#include "lims/org/authorization/declared.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// The inventory module's declared authorization: a company's inventory settings are a supervisor's; its items,
/// locations and opening lots are a stock keeper's, at the company or at the site of the location; a requisition is
/// raised, filled and submitted by anyone holding a role at its site, and approved by a purchase approver within a
/// value limit, who is not the one who raised or submitted it; a supplier is registered, and an approved requisition
/// ordered, by a purchaser at the company; goods are received against an order by a stock keeper at the company.

namespace lims {

/// @brief A company's inventory settings are a supervisor's.
template <>
struct Authorization<SetInventorySettings> : CompanyRule<SetInventorySettings, LimsRole::Supervisor> {};

/// @brief An item is created by a stock keeper at its company.
template <>
struct Authorization<CreateItem> : CompanyRule<CreateItem, LimsRole::StockKeeper> {};

/// @brief A location is created by a stock keeper at its site.
template <>
struct Authorization<CreateLocation> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;
    /// @brief The scope is spelled out in the payload.
    static constexpr bool scopeInPayload = true;

    /// @brief The site the payload names.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(CreateLocation const& action) { return Scope{action.site}; }
};

/// @brief An opening lot is recorded by a stock keeper at the location's site.
template <>
struct Authorization<RecordOpeningLot> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;

    /// @brief The site of the location the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such location.
    [[nodiscard]] static Scope scopeOf(RecordOpeningLot const& action);
};

/// @brief A requisition is raised by anyone holding a role at its site.
template <>
struct Authorization<CreateRequisition> {
    /// @brief Some role, whichever.
    static constexpr bool anyRole = true;
    /// @brief The scope is spelled out in the payload.
    static constexpr bool scopeInPayload = true;

    /// @brief The site the payload names.
    /// @param action The action.
    /// @return It.
    [[nodiscard]] static Scope scopeOf(CreateRequisition const& action) { return Scope{action.site}; }
};

/// @brief A line is added by anyone holding a role at the requisition's site; the model adds that it is its requester.
template <>
struct Authorization<AddRequisitionLine> {
    /// @brief Some role, whichever.
    static constexpr bool anyRole = true;

    /// @brief The site of the requisition the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(AddRequisitionLine const& action);
};

/// @brief A requisition is submitted by anyone holding a role at its site; the model adds that it is its requester.
template <>
struct Authorization<SubmitRequisition> {
    /// @brief Some role, whichever.
    static constexpr bool anyRole = true;

    /// @brief The site of the requisition the payload names.
    /// @param action The action.
    /// @return It; an empty site when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(SubmitRequisition const& action);
};

/// @brief A requisition is approved by a purchase approver who neither raised nor submitted it and whose limit for
///        that role covers its total: at its site before its deadline, at its company from the deadline on. The scope
///        moving from site to company at the deadline is the escalation.
template <>
struct Authorization<ApproveRequisition> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::PurchaseApprover;

    /// @brief The pairs: whoever raised the requisition, and whoever submitted it, may not also approve it. The
    ///        actions are named by their registered ids, spelled out because this header may be read before the models
    ///        that register them; the declared-authorization suite checks each is registered.
    static constexpr std::array<SeparatedFrom, 2> separations{
        SeparatedFrom{
            .action = "CreateRequisition",
            .refusal = "requested this requisition and cannot also approve it",
        },
        SeparatedFrom{
            .action = "SubmitRequisition",
            .refusal = "submitted this requisition and cannot also approve it",
        },
    };

    /// @brief The requisition's site before its deadline, its company from the deadline on.
    /// @param action The action.
    /// @return The scope; an empty company when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(ApproveRequisition const& action);

    /// @brief Refuses a requisition that is not submitted. Runs once the caller holds the role where the requisition
    ///        is visible and before its value or its requester is read, so approving a draft or an approved
    ///        requisition is an `IllegalTransition` for every approver and never a refusal naming who raised it.
    /// @param action The action.
    /// @throws IllegalTransition when the requisition is not submitted.
    static void checkState(ApproveRequisition const& action);

    /// @brief The requisition's total in its company's currency, as submitted.
    /// @param action The action.
    /// @return The amount; zero when there is no such requisition.
    [[nodiscard]] static ::morph::math::Rational amountOf(ApproveRequisition const& action);

    /// @brief @p principal's limit for this role at the company of @p scope.
    /// @param principal The principal.
    /// @param scope The scope `scopeOf` gave.
    /// @return The limit, or nothing.
    [[nodiscard]] static std::optional<::morph::math::Rational> limitOf(std::string_view principal,
                                                                        Scope const& scope);

    /// @brief Who raised the requisition and who submitted it, in `separations`' order.
    /// @param action The action.
    /// @return The two; each empty when there is no such requisition or no submission.
    [[nodiscard]] static std::array<std::optional<std::string>, 2> firstPerformersOf(ApproveRequisition const& action);
};

/// @brief A supplier is registered by a purchaser at its company.
template <>
struct Authorization<CreateSupplier> : CompanyRule<CreateSupplier, LimsRole::Purchaser> {};

/// @brief A purchase order is placed by a purchaser at the requisition's company.
template <>
struct Authorization<CreatePurchaseOrder> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Purchaser;

    /// @brief The company of the requisition the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such requisition.
    [[nodiscard]] static Scope scopeOf(CreatePurchaseOrder const& action);
};

/// @brief Goods are received against an order by a stock keeper at the order's company.
template <>
struct Authorization<ReceiveGoods> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::StockKeeper;

    /// @brief The company of the order the payload names.
    /// @param action The action.
    /// @return It; an empty company when there is no such order.
    [[nodiscard]] static Scope scopeOf(ReceiveGoods const& action);
};

}  // namespace lims

// The models are included last, after every declaration their registrations need.
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
