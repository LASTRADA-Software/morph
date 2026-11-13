// SPDX-License-Identifier: Apache-2.0
//
// What an approver's inbox is told. Escalation is a deadline the approval rule reads, and no action runs when it
// passes, so nothing tells the inbox. A submission is an action, and its declared effect decides whether an open
// inbox learns of the invoice it gained.

#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "shell_models.hpp"

namespace {

namespace ui = morph::ui::author;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::ScopedClockOverride;

constexpr std::string_view kInbox = "test.approvalInbox";

/// @brief A bundle of one screen: the caller's pending approvals, counted, a button that asks again, and a button
///        that submits invoice 1.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> inboxBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen(std::string{kInbox}, [](ui::Scope& screen) {
        auto const invoices = screen.model<lims::InvoiceModel>("invoices");
        auto const pending = screen.query<lims::ListPendingApprovals>("pending", invoices);
        auto const rows = pending.value().at(&lims::ListInvoicesResult::invoices);
        auto const submit = screen.mutation<lims::SubmitInvoice>("submit", invoices);
        return ui::column(
            {ui::text({.text = ui::format("{0} pending", {ui::count(rows)})}).testId("pendingCount"),
             ui::button({.label = "Refresh", .onClick = ui::refetch(pending)}).testId("refresh"),
             ui::button({.label = "Submit", .onClick = ui::run(submit, {{&lims::SubmitInvoice::invoiceId, 1}})})
                 .testId("submit")});
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
// asks again. The check sees a refresh the harness's manual scheduler can fire, such as a poll; a refresh driven by a
// real timer is outside the window. Mutations: judge the approval scope at the site whatever the time (the control,
// the refetch showing "0 pending", fails); poll the inbox every second (the check that nothing told the inbox
// fails).
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
    harness.advance(std::chrono::minutes{10});        // the harness's scheduler is manual: a poll fires only here
    bool const told =
        harness.waitFor([&harness] { return harness.text("pendingCount") == std::optional<std::string>{"0 pending"}; },
                        std::chrono::milliseconds{1500});
    CHECK_FALSE(told);

    // The control: asked again, the same panel shows the escalation, so the panel can show it.
    harness.click("refresh");
    CHECK(harness.waitForText("pendingCount", "0 pending"));
    CHECK(harness.problems().empty());
}

// A submission moves an invoice into the inbox's filtered list, a list that did not hold it as a draft, so the
// action declares `changes`: `updates` refetches only the entries that already hold the changed key. One client
// drives the screen, so the submitter's own inbox stands for every approver's: both are told by the same effect. A
// second submitted invoice that the open inbox has not been told of makes the refetch visible, because the
// submitter cannot approve the invoice she submitted and her count would otherwise stay 0.
// Mutation: declare `updates<InvoiceView>` for SubmitInvoice (the count stays at "0 pending").
TEST_CASE("approval inbox: submitting an invoice refreshes an inbox that did not hold it", "[lims-screens][billing]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    REQUIRE(*invoice.id == 1);
    lims::test::grant("carl", lims::LimsRole::BillingClerk);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 10)};
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-approval-inbox", .bundle = &inboxBundle, .setUp = {}},
                          std::string{kInbox},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "carl"}};
    REQUIRE(harness.waitForText("pendingCount", "0 pending"));

    // Another invoice, submitted by bea, which carl could approve; no action ran, so his inbox does not show it.
    Lightweight::SqlStatement statement;
    for (char const* sql : {
             "CREATE TEMP TABLE other AS SELECT * FROM billing_invoices WHERE id = 1",
             "UPDATE other SET id = 2, number = 'other', state = 1, submitted_by = 'bea', submitted_at = 1, "
             "deadline_at = 9999999999999, gross_company_num = gross_num, gross_company_den = gross_den",
             "INSERT INTO billing_invoices SELECT * FROM other",
             "DROP TABLE other",
         }) {
        (void)statement.ExecuteDirect(sql);
    }
    REQUIRE_FALSE(
        harness.waitFor([&harness] { return harness.text("pendingCount") == std::optional<std::string>{"1 pending"}; },
                        std::chrono::milliseconds{300}));

    harness.click("submit");
    CHECK(harness.waitForText("pendingCount", "1 pending"));
    CHECK(harness.problems().empty());
}
