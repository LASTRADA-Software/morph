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

namespace lims::inboxtest {

namespace ui = morph::ui::author;

morph::time::DateTime at(int year, unsigned month, unsigned day, int hour) {
    return lims::test::at(year, month, day, hour);
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> inboxBundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ui::screen(std::string{kInbox}, [](ui::Scope& screen) {
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
