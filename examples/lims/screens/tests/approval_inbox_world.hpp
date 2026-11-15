// SPDX-License-Identifier: Apache-2.0
//
// The lab, its invoice and the one-screen bundle the approval-inbox test drives. They are built in a translation unit
// of their own so that the test need not include the billing, lab, ledger and org models they are made of.
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>

namespace lims::inboxtest {

/// @brief The screen id of the inbox.
inline constexpr std::string_view kInbox = "test.approvalInbox";

/// @brief A UTC instant on a calendar day.
/// @param year The year.
/// @param month The month, 1 to 12.
/// @param day The day.
/// @param hour The hour.
/// @return The instant.
[[nodiscard]] morph::time::DateTime at(int year, unsigned month, unsigned day, int hour);

/// @brief A bundle of one screen: the caller's pending approvals, counted, a button that asks again, and a button that
///        submits invoice 1.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> inboxBundle();

/// @brief A fresh database holding the lab the approval tests run in, destroyed with the object.
class World {
public:
    World();
    World(World const&) = delete;
    World& operator=(World const&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;
    ~World();

    /// @brief Drafts the client's invoice as bea at the seeded site; it is invoice 1.
    void draftInvoice() const;

    /// @brief Submits the drafted invoice as bea at 09:00 on 10 November.
    void submitInvoice() const;

    /// @brief Grants @p principal the billing clerk role in the seeded company.
    /// @param principal The principal.
    static void grantBillingClerk(std::string const& principal);

    /// @brief Inserts a second invoice, submitted by bea and not by an action, so that no open inbox is told of it.
    static void insertSubmittedInvoiceWithoutAnAction();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace lims::inboxtest
