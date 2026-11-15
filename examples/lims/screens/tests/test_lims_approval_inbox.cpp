// SPDX-License-Identifier: Apache-2.0
//
// What an approver's inbox is told. Escalation is a deadline the approval rule reads, and no action runs when it
// passes, so the model reports the instant and the server tells the inbox. A submission is an action, and its declared
// effect decides whether an open inbox learns of the invoice it gained.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <testkit/screen_harness.hpp>

#include "approval_inbox_world.hpp"
#include "clock.hpp"
#include "lims/app/lims_authorizer.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"

namespace {

using lims::inboxtest::at;
using lims::inboxtest::inboxBundle;
using lims::inboxtest::kInbox;
using lims::inboxtest::kSignIn;
using lims::inboxtest::signInBundle;
using lims::inboxtest::World;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::ScopedClockOverride;

}  // namespace

// Past the deadline, paula's inbox is told and drops the invoice she can no longer approve. The server learns the
// instant from `NextApprovalDeadline`, owns the clock, and tells every watcher of the invoice record when the clock
// reaches it; nothing polls and the harness's manual scheduler is never advanced. The screen runs behind a server that
// authenticates, so paula signs in. Mutations: judge the approval scope at the site whatever the time (the count
// never reads "1 pending" before the deadline's refetch shows "0 pending", so the control after the wait fails); drop
// the server's fan-out at the reported instant (the count stays "1 pending").
TEST_CASE("approval inbox: a site approver's inbox is told when an approval's deadline passes",
          "[lims-screens][billing][h4]") {
    World const world;
    world.draftInvoice();
    world.submitInvoice();
    std::optional<ScopedClockOverride> clock;
    clock.emplace(at(2026, 11, 12, 8));
    morph::client::LocalApp app{.name = "lims-approval-inbox", .bundle = &signInBundle, .setUp = {}};
    app.setUp = [](morph::client::LocalSetup const& setup) { lims::screens::prepareLocal(setup.user); };
    app.serverAuthorizer = [] {
        return std::make_shared<lims::auth::LimsAuthorizer>(
            "lims-local-development", morph::session::hmacSha256,
            [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
    };
    ScreenHarness harness{app, std::string{kSignIn}, RigMode::Simulated};
    harness.edit("username", "paula");
    harness.click("signIn");
    REQUIRE(harness.waitForText("pendingCount", "1 pending"));

    clock.reset();
    clock.emplace(at(2026, 11, 12, 10));  // the deadline was 09:00
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
    World::grantBillingClerk("carl");
    ScopedClockOverride const clock{at(2026, 11, 10, 9)};
    ScreenHarness harness{morph::client::LocalApp{.name = "lims-approval-inbox", .bundle = &inboxBundle, .setUp = {}},
                          std::string{kInbox},
                          RigMode::Local,
                          {},
                          morph::client::LocalSetup{.db = {}, .user = "carl"}};
    REQUIRE(harness.waitForText("pendingCount", "0 pending"));

    // Another invoice, submitted by bea, which carl could approve; no action ran, so his inbox does not show it.
    World::insertSubmittedInvoiceWithoutAnAction();
    REQUIRE_FALSE(
        harness.waitFor([&harness] { return harness.text("pendingCount") == std::optional<std::string>{"1 pending"}; },
                        std::chrono::milliseconds{300}));

    harness.click("submit");
    CHECK(harness.waitForText("pendingCount", "1 pending"));
    CHECK(harness.problems().empty());
}
