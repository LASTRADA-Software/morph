// SPDX-License-Identifier: Apache-2.0

#include "ledger_screens.hpp"

#include <chrono>
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
#include "ledger/dto/budget_dto.hpp"
#include "ledger/dto/report_dto.hpp"
#include "ledger/dto/rule_dto.hpp"
#include "ledger/dto/transaction_dto.hpp"
#include "ledger/models/auth_model.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"

namespace ledger::screens {

namespace ui = morph::ui::author;

namespace {

// An amount as ledger stores it, a whole number of the currency's minor units, shown as money: exact, at the
// currency's places, with its code.
ui::Handle<std::string> money(ui::AnyHandle const& minor, ui::AnyHandle const& currency) {
    auto const wholeUnits = ui::eq(currency, "JPY") || ui::eq(currency, "KRW");
    auto const perUnit = ui::cond(wholeUnits, ui::Handle<std::int64_t>{std::int64_t{1}},
                                  ui::Handle<std::int64_t>{std::int64_t{100}});
    return ui::call<std::string>("money", {minor / perUnit, currency});
}

// The ledger tab: open accounts, store a transaction between them, list a month's entries and undo one. Amounts are
// exact `Rational`s end to end: whole minor units on the wire, shown as money.
ui::Nodes ledgerTab(ui::Scope& screen, ui::ModelHandle<LedgerModel> const& books, ui::AnyHandle const& ledgerId) {
    auto const accounts = screen.query<GetLedger>("accounts", books, {{&GetLedger::ledgerId, ledgerId}},
                                                  {.when = ui::engaged(ledgerId)});
    auto const month = ui::element("month");
    auto const entries = screen.query<ListTransactions>(
        "entries", books, {{&ListTransactions::ledgerId, ledgerId}, {&ListTransactions::month, month.value()}},
        {.when = ui::engaged(ledgerId) && !ui::isEmpty(month.value())});
    auto const undo = screen.mutation<UndoTransaction>("undo", books, {.invalidates = {accounts, entries}});
    return {
        ui::form<OpenAccount>(books, {.prefill = ui::obj({{"ledgerId", ledgerId}}),
                                      .submit = "explicit",
                                      .fields = {{&OpenAccount::ledgerId, {.hidden = true}}},
                                      .onSuccess = {ui::refetch(accounts)}})
            .id(ui::element("accountForm"))
            .testId("accountForm"),
        ui::table(accounts.value().at(&GetLedgerResult::accounts),
                  {.key = ui::row("id"),
                   .columns = {{.name = "id", .label = "Id", .cell = ui::text({.text = ui::row("id")}).testId("accountId")},
                               {.name = "name", .label = "Account", .cell = ui::text({.text = ui::row("name")})},
                               {.name = "balance",
                                .label = "Balance",
                                .cell = ui::text({.text = money(ui::row("balance"), ui::row("currency"))}).testId("balance")}}})
            .a11y("Accounts")
            .testId("accounts"),
        ui::form<StoreTransaction>(books, {.prefill = ui::obj({{"ledgerId", ledgerId}}),
                                           .submit = "explicit",
                                           .fields = {{&StoreTransaction::ledgerId, {.hidden = true}},
                                                      // Entered by hand, a transaction is not replayed:
                                                      // without an op id it skips the exactly-once ledger.
                                                      {&StoreTransaction::opId, {.omit = true}}},
                                           .onSuccess = {ui::refetch(accounts), ui::refetch(entries)}})
            .id(ui::element("transactionForm"))
            .testId("transactionForm"),
        ui::textInput({.label = "Month", .placeholder = "YYYY-MM"}).id(month).testId("month"),
        ui::table(entries.value().at(&ListTransactionsResult::entries),
                  {.key = ui::row("id"),
                   .columns = {{.name = "description",
                                .label = "Entry",
                                .cell = ui::text({.text = ui::row("description")}).testId("entry")},
                               {.name = "undo",
                                .label = "",
                                .cell = ui::button({.label = "Undo",
                                                    .onClick = ui::run(undo, {{&UndoTransaction::ledgerId, ledgerId},
                                                                              {&UndoTransaction::journalId,
                                                                               ui::row("id")}})})
                                            .testId("undo")}}})
            .a11y("Entries")
            .testId("entries"),
        ui::text({.text = ui::coalesce({accounts.error(), entries.error(), undo.error()})}).testId("ledgerError")};
}

// The budgets tab: a category, an account linked to it, a budget over it with a month's limit, and the month's report
// of limit and spend, both exact.
ui::Nodes budgetsTab(ui::Scope& screen, ui::AnyHandle const& ledgerId) {
    auto const budgets = screen.model<BudgetModel>("budgets", {.instance = ui::Instance::Private});
    auto const categoryId = screen.state<std::int64_t>("categoryId");
    auto const budgetId = screen.state<std::int64_t>("budgetId");
    auto const month = ui::element("budgetMonth");
    auto const report = screen.query<GetBudgetReport>(
        "report", budgets, {{&GetBudgetReport::budgetId, budgetId}, {&GetBudgetReport::month, month.value()}},
        {.when = ui::engaged(budgetId) && !ui::isEmpty(month.value())});
    return {
        ui::form<CreateCategory>(budgets, {.prefill = ui::obj({{"ledgerId", ledgerId}}),
                                           .submit = "explicit",
                                           .fields = {{&CreateCategory::ledgerId, {.hidden = true}}},
                                           .onSuccess = {ui::set(categoryId, ui::ref("event"))}})
            .id(ui::element("categoryForm"))
            .testId("categoryForm"),
        ui::form<LinkAccountToCategory>(
            budgets, {.prefill = ui::obj({{"categoryId", categoryId}}),
                      .submit = "explicit",
                      .fields = {{&LinkAccountToCategory::categoryId, {.hidden = true}}}})
            .id(ui::element("linkForm"))
            .testId("linkForm")
            .visible(ui::engaged(categoryId)),
        ui::form<CreateBudget>(budgets, {.prefill = ui::obj({{"ledgerId", ledgerId}, {"categoryId", categoryId}}),
                                         .submit = "explicit",
                                         .fields = {{&CreateBudget::ledgerId, {.hidden = true}},
                                                    {&CreateBudget::categoryId, {.hidden = true}}},
                                         .onSuccess = {ui::set(budgetId, ui::ref("event"))}})
            .id(ui::element("budgetForm"))
            .testId("budgetForm")
            .visible(ui::engaged(categoryId)),
        ui::form<SetBudgetLimit>(budgets, {.prefill = ui::obj({{"budgetId", budgetId}}),
                                           .submit = "explicit",
                                           .fields = {{&SetBudgetLimit::budgetId, {.hidden = true}}},
                                           .onSuccess = {ui::refetch(report)}})
            .id(ui::element("limitForm"))
            .testId("limitForm")
            .visible(ui::engaged(budgetId)),
        ui::textInput({.label = "Report month", .placeholder = "YYYY-MM"})
            .id(month)
            .testId("budgetMonth")
            .visible(ui::engaged(budgetId)),
        ui::text({.text = ui::format("Limit {0}, spent {1}",
                                     {money(report.value().at(&GetBudgetReportResult::limit),
                                            report.value().at(&GetBudgetReportResult::currency)),
                                      money(report.value().at(&GetBudgetReportResult::spent),
                                            report.value().at(&GetBudgetReportResult::currency))})})
            .testId("budgetReport")
            .visible(ui::engaged(report.value())),
        ui::text({.text = report.error()}).testId("budgetError")};
}

// The rules tab: create a rule, then edit it; the edit answers the rule with its bumped version.
ui::Nodes rulesTab(ui::Scope& screen, ui::AnyHandle const& ledgerId) {
    auto const rules = screen.model<RuleModel>("rules", {.instance = ui::Instance::Private});
    auto const ruleId = screen.state<std::int64_t>("ruleId");
    auto const version = screen.state<std::int64_t>("ruleVersion");
    return {ui::form<CreateRule>(rules, {.prefill = ui::obj({{"ledgerId", ledgerId}}),
                                         .submit = "explicit",
                                         .fields = {{&CreateRule::ledgerId, {.hidden = true}}},
                                         .onSuccess = {ui::set(ruleId, ui::ref("event")), ui::reset(version)}})
                .id(ui::element("ruleForm"))
                .testId("ruleForm"),
            ui::form<UpdateRule>(rules, {.prefill = ui::obj({{"ruleId", ruleId}}),
                                         .submit = "explicit",
                                         .fields = {{&UpdateRule::ruleId, {.hidden = true}},
                                                    {&UpdateRule::expectedVersion, {.hidden = true}}},
                                         .onSuccess = {ui::set(version, ui::ref("event.version"))}})
                .id(ui::element("ruleEditForm"))
                .testId("ruleEditForm")
                .visible(ui::engaged(ruleId)),
            ui::text({.text = ui::format("Rule {0}", {ruleId})}).testId("rule").visible(ui::engaged(ruleId)),
            ui::text({.text = ui::format("Rule {0}, version {1}", {ruleId, version})})
                .testId("ruleVersion")
                .visible(ui::engaged(version))};
}

// The statement tab: requesting it submits the job, which the server's report runner runs; the screen polls its status
// with `refreshEvery`, which skips a tick while a poll is in flight, and stops once the job is done or failed: the
// poll's gate is the job being submitted and not yet finished.
ui::Nodes statementTab(ui::Scope& screen, ui::ModelHandle<LedgerModel> const& books, ui::AnyHandle const& ledgerId) {
    auto const finished = screen.state("finished", false);
    auto const state = screen.state<std::string>("jobState");
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
    return {ui::button({.label = "Request statement", .onClick = ui::run(submit)}).testId("requestStatement"),
            ui::text({.text = "Checking the statement…"}).testId("polling").visible(ui::engaged(jobId) && !finished),
            ui::text({.text = ui::format("Statement {0}: {1}", {jobId, state})})
                .testId("jobStatus")
                .visible(ui::engaged(state)),
            ui::text({.text = ui::coalesce({submit.error(), status.error()})}).testId("statementError")};
}

// Sign-in and a ledger, then the ledger's tabs: its accounts and entries, its budgets, its rules and its statement.
ui::Screen ledgerScreen() {
    return ui::screen(std::string{kLedger}, [](ui::Scope& screen) {
        screen.title("Ledger");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const books = screen.model<LedgerModel>("books", {.instance = ui::Instance::Private});
        auto const signedIn = screen.state("signedIn", false);
        auto const ledgerId = screen.state<std::int64_t>("ledgerId");
        auto const username = ui::element("username");
        auto const login =
            screen.mutation<Login>("login", auth,
                                   {.body = {{&Login::username, username.value()}},
                                    .onSuccess = {ui::signIn(ui::MutationRef{"login"}), ui::set(signedIn, true)}});
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
             ui::column({ui::tabs({{.label = "Ledger", .children = ledgerTab(screen, books, ledgerId)},
                                   {.label = "Budgets", .children = budgetsTab(screen, ledgerId)},
                                   {.label = "Rules", .children = rulesTab(screen, ledgerId)},
                                   {.label = "Statement", .children = statementTab(screen, books, ledgerId)}})
                             .testId("sections")})
                 .visible(ui::engaged(ledgerId)),
             ui::text({.text = login.error()}).testId("error")},
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
