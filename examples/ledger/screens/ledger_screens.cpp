// SPDX-License-Identifier: Apache-2.0

#include "ledger_screens.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <utility>

#include "ledger/auth/ledger_authorizer.hpp"
#include "ledger/dto/account_dto.hpp"
#include "ledger/dto/auth_dto.hpp"
#include "ledger/dto/report_dto.hpp"
#include "ledger/models/auth_model.hpp"
#include "ledger/models/ledger_model.hpp"

namespace ledger::screens {

namespace ui = morph::ui::author;

namespace {

// The statement job: requesting it submits the job, which the server's report runner runs; the screen polls its status
// with `refreshEvery`, which skips a tick while a poll is in flight, and stops once the job is done or failed: the
// poll's gate is the job being submitted and not yet finished.
ui::Screen ledgerScreen() {
    return ui::screen(std::string{kLedger}, [](ui::Scope& screen) {
        screen.title("Ledger");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const books = screen.model<LedgerModel>("books", {.instance = ui::Instance::Private});
        auto const signedIn = screen.state("signedIn", false);
        auto const ledgerId = screen.state<std::int64_t>("ledgerId");
        auto const finished = screen.state("finished", false);
        auto const state = screen.state<std::string>("jobState");
        auto const username = ui::element("username");
        auto const login =
            screen.mutation<Login>("login", auth,
                                   {.body = {{&Login::username, username.value()}},
                                    .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::set(signedIn, true)}});
        auto const accounts = screen.query<GetLedger>("accounts", books, {{&GetLedger::ledgerId, ledgerId}},
                                                      {.when = ui::engaged(ledgerId)});
        auto const submit = screen.mutation<SubmitReport>(
            "submit", books,
            {.body = {{&SubmitReport::ledgerId, ledgerId},
                      {&SubmitReport::kind, ui::AnyHandle{std::string{"MonthlyStatement"}}},
                      {&SubmitReport::params, std::string{"{}"}}},
             .onSuccess = {ui::set(finished, false), ui::reset(state)}});
        auto const jobId = screen.let("jobId", submit.result());
        auto const status = screen.query<GetReportStatus>(
            "status", books, {{&GetReportStatus::jobId, jobId}},
            {.when = ui::engaged(jobId) && !finished, .refreshEvery = std::chrono::milliseconds{200}});
        // The poll's value goes when its gate closes, so the last status is kept in a state of its own.
        auto const polled = screen.let("polled", status.value().at(&GetReportStatusResult::status));
        screen.watch(polled,
                     ui::ifThen(ui::engaged(polled),
                                {ui::set(state, polled), ui::ifThen(ui::eq(polled, "Done") || ui::eq(polled, "Failed"),
                                                                    ui::set(finished, true))}));
        return ui::column(
            {ui::row({ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
                      ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn")},
                     {.gap = 8})
                 .testId("signInRow")
                 .visible(!signedIn),
             ui::form<CreateLedger>(books,
                                    {.submit = "explicit", .onSuccess = {ui::set(ledgerId, ui::ref("event.id"))}})
                 .id(ui::element("ledgerForm"))
                 .testId("ledgerForm")
                 .visible(signedIn),
             ui::text({.text = ui::format("Ledger {0}", {ledgerId})}).testId("ledger").visible(ui::engaged(ledgerId)),
             ui::form<OpenAccount>(books, {.prefill = ui::obj({{"ledgerId", ledgerId}}),
                                           .submit = "explicit",
                                           .fields = {{&OpenAccount::ledgerId, {.hidden = true}}},
                                           .onSuccess = {ui::refetch(accounts)}})
                 .id(ui::element("accountForm"))
                 .testId("accountForm")
                 .visible(ui::engaged(ledgerId)),
             ui::table(
                 accounts.value().at(&GetLedgerResult::accounts),
                 {.key = ui::row("id"),
                  .columns = {{.name = "name", .label = "Account", .cell = ui::text({.text = ui::row("name")})}}})
                 .a11y("Accounts")
                 .testId("accounts")
                 .visible(ui::engaged(ledgerId)),
             ui::button({.label = "Request statement", .onClick = ui::run(submit)})
                 .testId("requestStatement")
                 .enabled(ui::engaged(ledgerId)),
             ui::text({.text = "Checking the statement…"}).testId("polling").visible(ui::engaged(jobId) && !finished),
             ui::text({.text = ui::format("Statement {0}: {1}", {jobId, state})})
                 .testId("jobStatus")
                 .visible(ui::engaged(state)),
             ui::text({.text = ui::coalesce({login.error(), submit.error(), status.error()})}).testId("error")},
            {.gap = 8});
    });
}

}  // namespace

void prepareLocal() {
    // A development secret: local mode signs nothing that leaves the process.
    auth::setTokenIssuer(
        std::make_shared<morph::session::TokenIssuer>("ledger-local-development", morph::session::hmacSha256));
}

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    auto registered = screens.add(ledgerScreen());
    if (!registered) {
        return std::unexpected(std::move(registered.error()));
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    morph::ui::CatalogEntry entry;
    entry.id = std::string{kLedger};
    entry.title = "Ledger";
    if (auto added = served.addScreen(std::move(entry), (*registered)->text); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

}  // namespace ledger::screens
