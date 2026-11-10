// SPDX-License-Identifier: Apache-2.0
//
// Escalation is a deadline the approval rule reads, and no action runs when it passes. What that leaves on a screen:
// an approver's inbox is not told.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/models/invoice_model.hpp"

namespace {

namespace ui = morph::ui::author;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::ScopedClockOverride;

constexpr std::string_view kInbox = "test.approvalInbox";

/// @brief A bundle of one screen: the caller's pending approvals, counted, and a button that asks again.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> inboxBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen(std::string{kInbox}, [](ui::Scope& screen) {
        auto const invoices = screen.model<lims::InvoiceModel>("invoices");
        auto const pending = screen.query<lims::ListPendingApprovals>("pending", invoices);
        auto const rows = pending.value().at(&lims::ListInvoicesResult::invoices);
        return ui::column({ui::text({.text = ui::format("{0} pending", {ui::count(rows)})}).testId("pendingCount"),
                           ui::button({.label = "Refresh", .onClick = ui::refetch(pending)}).testId("refresh")});
    }));
    if (!registered) {
        return std::unexpected(registered.error());
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-approval-inbox-test", .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kInbox};
    entry.title = "Approvals";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace

// The expected outcome: past the deadline, paula's inbox still lists the invoice she can no longer approve, until it
// asks again. Mutation: judge the approval scope at the site whatever the time (the control, the refetch showing
// "0 pending", fails).
TEST_CASE("approval inbox: a site approver's inbox is not told when an approval's deadline passes",
          "[lims-screens][billing][h4]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    std::optional<ScopedClockOverride> clock;
    clock.emplace(lims::test::at(2026, 11, 12, 8));
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-approval-inbox", .bundle = &inboxBundle, .setUp = {}},
                          std::string{kInbox},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "paula"}};
    REQUIRE(harness.waitForText("pendingCount", "1 pending"));

    clock.reset();
    clock.emplace(lims::test::at(2026, 11, 12, 10));  // the deadline was 09:00
    bool const told =
        harness.waitFor([&harness] { return harness.text("pendingCount") == std::optional<std::string>{"0 pending"}; },
                        std::chrono::milliseconds{1500});
    CHECK_FALSE(told);

    // The control: asked again, the same panel shows the escalation, so the panel can show it.
    harness.click("refresh");
    CHECK(harness.waitForText("pendingCount", "0 pending"));
    CHECK(harness.problems().empty());
}
