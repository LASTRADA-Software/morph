// SPDX-License-Identifier: Apache-2.0
#include "approval_inbox_world.hpp"

#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <utility>

#include "billing_test_support.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/org/dto/auth_dto.hpp"
#include "lims/org/models/auth_model.hpp"

namespace lims::inboxtest {

namespace ui = morph::ui::author;

morph::time::DateTime at(int year, unsigned month, unsigned day, int hour) {
    return lims::test::at(year, month, day, hour);
}

namespace {

/// @brief The inbox screen.
/// @return The screen.
ui::Screen inboxScreen() {
    return ui::screen(std::string{kInbox}, [](ui::Scope& screen) {
        auto const invoices = screen.model<lims::InvoiceModel>("invoices");
        auto const pending = screen.query<lims::ListPendingApprovals>("pending", invoices);
        auto const rows = pending.value().at(&lims::ListInvoicesResult::invoices);
        auto const submit = screen.mutation<lims::SubmitInvoice>("submit", invoices);
        return ui::column({
            ui::text({.text = ui::format("{0} pending", {ui::count(rows)})}).testId("pendingCount"),
            ui::button({.label = "Refresh", .onClick = ui::refetch(pending)}).testId("refresh"),
            ui::button({.label = "Submit", .onClick = ui::run(submit, {{&lims::SubmitInvoice::invoiceId, 1}})})
                .testId("submit"),
        });
    });
}

/// @brief The sign-in screen: any username, then the inbox.
/// @return The screen.
ui::Screen signInScreen() {
    return ui::screen(std::string{kSignIn}, [](ui::Scope& screen) {
        auto const auth = screen.model<lims::LimsAuthModel>("auth", {.instance = ui::Instance::Private});
        auto const username = ui::element("username");
        auto const login =
            screen.mutation<lims::LimsLogin>("login", auth,
                                             {
                                                 .body = {{&lims::LimsLogin::username, username.value()}},
                                                 .onSuccess =
                                                     {
                                                         ui::signIn(ui::MutationRef{"login"}),
                                                         ui::navigate(std::string{kInbox}),
                                                     },
                                             });
        return ui::column({
            ui::textInput({.label = "Username"}).id(username).testId("username"),
            ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn"),
        });
    });
}

/// @brief A bundle of the inbox, and of the sign-in before it when asked.
/// @param withSignIn Whether the bundle opens with the sign-in.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundleOf(bool withSignIn) {
    ui::ScreenRegistry screens;
    auto inbox = screens.add(inboxScreen());
    if (!inbox) {
        return std::unexpected(inbox.error());
    }
    ui::RegisteredScreen const* signIn = nullptr;
    if (withSignIn) {
        auto registered = screens.add(signInScreen());
        if (!registered) {
            return std::unexpected(registered.error());
        }
        signIn = *registered;
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "lims-approval-inbox-test", .version = "1.0"},
                                 screens.catalog()};
    if (signIn != nullptr) {
        morph::ui::CatalogEntry entry;
        entry.id = std::string{kSignIn};
        entry.title = "Sign in";
        entry.isPublic = true;
        if (auto added = served.addScreen(std::move(entry), signIn->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kInbox};
    entry.title = "Approvals";
    if (auto added = served.addScreen(std::move(entry), (*inbox)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> inboxBundle() {
    return bundleOf(false);
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> signInBundle() {
    return bundleOf(true);
}

struct World::Impl {
    lims::test::LabFixture fixture;
    std::optional<lims::InvoiceView> invoice;
};

World::World() : _impl{std::make_unique<Impl>()} {}

World::~World() = default;

void World::draftInvoice() const {
    _impl->invoice = lims::test::draftedInvoice();
    REQUIRE(*_impl->invoice->id == 1);
}

void World::submitInvoice() const { lims::test::submit(*_impl->invoice); }

void World::grantBillingClerk(std::string const& principal) {
    lims::test::grant(principal, lims::LimsRole::BillingClerk);
}

void World::insertSubmittedInvoiceWithoutAnAction() {
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
}

}  // namespace lims::inboxtest
