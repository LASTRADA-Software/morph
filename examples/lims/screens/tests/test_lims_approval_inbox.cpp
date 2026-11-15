// SPDX-License-Identifier: Apache-2.0
//
// What an approver's inbox is told. Escalation is a deadline the approval rule reads, and no action runs when it
// passes, so nothing tells the inbox. A submission is an action, and its declared effect decides whether an open
// inbox learns of the invoice it gained.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <testkit/screen_harness.hpp>

#include "approval_inbox_world.hpp"
#include "clock.hpp"

namespace {

using lims::inboxtest::at;
using lims::inboxtest::inboxBundle;
using lims::inboxtest::kInbox;
using lims::inboxtest::World;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::ScopedClockOverride;

}  // namespace

// The expected outcome: past the deadline, paula's inbox still lists the invoice she can no longer approve, until it
// asks again. The check sees a refresh the harness's manual scheduler can fire, such as a poll; a refresh driven by a
// real timer is outside the window. Mutations: judge the approval scope at the site whatever the time (the control,
// the refetch showing "0 pending", fails); poll the inbox every second (the check that nothing told the inbox
// fails).
TEST_CASE("approval inbox: a site approver's inbox is not told when an approval's deadline passes",
          "[lims-screens][billing][h4]") {
    World const world;
    world.draftInvoice();
    world.submitInvoice();
    std::optional<ScopedClockOverride> clock;
    clock.emplace(at(2026, 11, 12, 8));
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-approval-inbox", .bundle = &inboxBundle, .setUp = {}},
                          std::string{kInbox},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "paula"}};
    REQUIRE(harness.waitForText("pendingCount", "1 pending"));

    clock.reset();
    clock.emplace(at(2026, 11, 12, 10));        // the deadline was 09:00
    harness.advance(std::chrono::minutes{10});  // the harness's scheduler is manual: a poll fires only here
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
    World const world;
    world.draftInvoice();
    world.grantBillingClerk("carl");
    ScopedClockOverride const clock{at(2026, 11, 10, 9)};
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-approval-inbox", .bundle = &inboxBundle, .setUp = {}},
                          std::string{kInbox},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "carl"}};
    REQUIRE(harness.waitForText("pendingCount", "0 pending"));

    // Another invoice, submitted by bea, which carl could approve; no action ran, so his inbox does not show it.
    world.insertSubmittedInvoiceWithoutAnAction();
    REQUIRE_FALSE(
        harness.waitFor([&harness] { return harness.text("pendingCount") == std::optional<std::string>{"1 pending"}; },
                        std::chrono::milliseconds{300}));

    harness.click("submit");
    CHECK(harness.waitForText("pendingCount", "1 pending"));
    CHECK(harness.problems().empty());
}
