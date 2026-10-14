// SPDX-License-Identifier: Apache-2.0
//
// ledger's screen on the screen harness (spec 4 §7): the screen validates; a signed-in person creates a ledger, opens
// accounts, stores a transaction between them and undoes it, with balances exact throughout; sets a budget's limit and
// reads the month's spend exactly; creates and edits a rule, whose version the edit bumps; and requests a statement,
// polled until it is done, and until it fails. Refusals are shown on the form that caused them. The statement test
// runs the job as the server's report runner does, under the runner's principal.

#include <algorithm>
#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <morph/session/session.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <vector>

#include "ledger/core/types.hpp"
#include "ledger/dto/report_dto.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp ledgerApp() {
    return LocalApp{.name = "ledger", .bundle = &ledger::screens::bundle, .setUp = [](LocalSetup const&) {
                        ledger::screens::prepareLocal();
                    }};
}

ScreenHarness open() {
    return ScreenHarness{
        ledgerApp(), std::string{ledger::screens::kLedger}, RigMode::Local, {}, LocalSetup{.db = {}, .user = {}}};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// The number after @p prefix in the text with @p testId.
std::int64_t numberIn(ScreenHarness& harness, std::string const& testId, std::string const& prefix) {
    std::string const text = harness.text(testId).value_or("");
    REQUIRE(text.starts_with(prefix));
    return std::stoll(text.substr(prefix.size()));
}

/// Waits for the job's status to end with @p suffix, advancing the clock so the status query polls.
bool waitForJob(ScreenHarness& harness, std::string const& suffix) {
    return harness.waitFor([&harness, &suffix] {
        harness.advance(std::chrono::milliseconds{200});
        return harness.text("jobStatus").value_or("").ends_with(suffix);
    });
}

/// Signs in when needed, creates a ledger named @p name, and answers its id.
std::int64_t createLedger(ScreenHarness& harness, std::string const& name) {
    if (harness.find("TextInput", "username")) {
        harness.edit("username", "alice");
        harness.click("signIn");
        // Typing into the ledger form before the sign-in lands would be lost with it.
        REQUIRE(harness.waitFor([&harness] {
            return harness.backend().prop(harness.find("Row", "signInRow").value(), "visible") == "false";
        }));
    }
    std::string const before = harness.text("ledger").value_or("");
    harness.editField("ledgerForm", "name", name);
    REQUIRE(harness.submitForm("ledgerForm"));
    REQUIRE(harness.waitFor([&harness, &before] {
        auto const shown = harness.text("ledger");
        return shown && shown->starts_with("Ledger ") && *shown != before;
    }));
    return numberIn(harness, "ledger", "Ledger ");
}

// The tabs, in the order the screen declares them.
constexpr std::size_t kLedgerTab = 0;
constexpr std::size_t kBudgetsTab = 1;
constexpr std::size_t kRulesTab = 2;
constexpr std::size_t kStatementTab = 3;

/// The texts of every widget with @p testId under the widget with @p rootTestId of kind @p rootKind, such as a table
/// column's cells.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the root's kind and test id, then the widgets' test id.
std::vector<std::string> textsIn(ScreenHarness& harness, std::string const& rootKind, std::string const& rootTestId,
                                 std::string const& testId) {
    std::vector<std::string> texts;
    auto const root = harness.find(rootKind, rootTestId);
    if (!root) {
        return texts;
    }
    std::vector<int> pending{*root};
    while (!pending.empty()) {
        int const widget = pending.back();
        pending.pop_back();
        if (harness.backend().prop(widget, "testId") == testId) {
            texts.push_back(harness.backend().prop(widget, "text"));
        }
        for (int const child : harness.backend().children(widget)) {
            pending.push_back(child);
        }
    }
    std::ranges::sort(texts);
    return texts;
}

/// The text of the reply or error line of the form node with @p formTestId.
std::string formResult(ScreenHarness& harness, std::string const& formTestId) {
    auto const line = harness.findIn(formTestId, "Text", "form_result");
    return line ? harness.backend().prop(*line, "text") : std::string{};
}

/// Opens an account named @p name of @p kind in euros, and answers its id once the accounts table lists it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the name, then the kind, as the form asks for them.
std::int64_t openAccount(ScreenHarness& harness, std::string const& name, std::string const& kind) {
    std::size_t const before = textsIn(harness, "Table", "accounts", "accountId").size();
    harness.editField("accountForm", "name", name);
    harness.chooseField("accountForm", "kind", "\"" + kind + "\"");
    harness.chooseField("accountForm", "currency", "\"EUR\"");
    REQUIRE(harness.submitForm("accountForm"));
    REQUIRE(harness.waitFor(
        [&harness, before] { return textsIn(harness, "Table", "accounts", "accountId").size() == before + 1; }));
    std::int64_t newest = 0;
    for (std::string const& shown : textsIn(harness, "Table", "accounts", "accountId")) {
        newest = std::max<std::int64_t>(newest, std::stoll(shown));
    }
    return newest;
}

/// Types the exact decimal @p decimal, such as `-12.34`, into the `Rational` field at @p path of the form with
/// @p formTestId, as ledger takes money: a whole number of minor units, so `-12.34` is -1234 at two places. A bare
/// `Rational` is entered as its parts.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the form, the field's path, then the amount.
void editAmount(ScreenHarness& harness, std::string const& formTestId, std::string const& path,
                std::string const& decimal) {
    auto const point = decimal.find('.');
    std::size_t const places = point == std::string::npos ? 0 : decimal.size() - point - 1;
    std::string digits = decimal;
    if (point != std::string::npos) {
        digits.erase(point, 1);
    }
    harness.editField(formTestId, path + ".num", std::to_string(std::stoll(digits)));
    harness.editField(formTestId, path + ".den", "1");
    harness.editField(formTestId, path + ".dp", std::to_string(places));
}

/// Selects tab @p index of the ledger's tabs and waits for its page to mount, which it does the first time, once the
/// selection has settled: @p testId names a widget of kind @p kind on that page.
void showTab(ScreenHarness& harness, std::size_t index, std::string const& kind, std::string const& testId) {
    harness.selectTab("sections", index);
    REQUIRE(harness.waitFor([&harness, &kind, &testId] { return harness.find(kind, testId).has_value(); }));
}

/// Fills the transaction form with two legs, @p fromAmount on @p fromAccount and @p toAmount on @p toAccount, dated
/// mid-October 2026, and submits it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the accounts, then the amounts, in leg order.
void storeTransaction(ScreenHarness& harness, std::int64_t fromAccount, std::int64_t toAccount,
                      std::string const& fromAmount, std::string const& toAmount) {
    if (!harness.findIn("transactionForm", "TextInput", "field_legs#2.accountId")) {
        harness.clickIn("transactionForm", "add_legs");
        harness.clickIn("transactionForm", "add_legs");
    }
    REQUIRE(harness.waitFor(
        [&harness] { return harness.findIn("transactionForm", "TextInput", "field_legs#2.accountId").has_value(); }));
    harness.editField("transactionForm", "description", "Weekly shop");
    harness.setDateField("transactionForm", "date",
                         morph::time::Timestamp{morph::time::DateTime{
                             std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{14},
                             std::chrono::hours{12}, std::chrono::minutes{0}, std::chrono::seconds{0}}});
    harness.editField("transactionForm", "legs#1.accountId", std::to_string(fromAccount));
    editAmount(harness, "transactionForm", "legs#1.amount", fromAmount);
    harness.editField("transactionForm", "legs#2.accountId", std::to_string(toAccount));
    editAmount(harness, "transactionForm", "legs#2.amount", toAmount);
    bool const submitted = harness.submitForm("transactionForm");
    reportProblems(harness);
    REQUIRE(submitted);
}

/// The balances the accounts table shows, sorted.
std::vector<std::string> balances(ScreenHarness& harness) { return textsIn(harness, "Table", "accounts", "balance"); }

/// Runs a report job as the server's report runner does.
void runAsRunner(std::int64_t jobId, std::int64_t ledgerId) {
    morph::session::Context context;
    context.principal = std::string{ledger::kReportRunnerPrincipal};
    morph::session::detail::ScopedContext const scope{context};
    ledger::LedgerModel model;
    try {
        static_cast<void>(model.execute(
            ledger::RunReportJob{.jobId = ledger::ReportJobId{jobId}, .ledgerId = ledger::LedgerId{ledgerId}}));
    } catch (std::exception const& error) {
        // A refused run is settled Failed, which the screen is expected to show.
        UNSCOPED_INFO("the runner's run was refused: " << error.what());
    }
}

}  // namespace

TEST_CASE("ledger screens: the screen validates, and the bundle builds", "[ledger-screens]") {
    auto const bundle = ledger::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: show the status once and never poll (it stays Pending).
TEST_CASE("ledger screens: the statement job is polled until it is done", "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    std::int64_t const ledgerId = createLedger(harness, "Household");
    showTab(harness, kStatementTab, "Button", "requestStatement");
    harness.click("requestStatement");
    REQUIRE(harness.waitFor([&harness] { return harness.text("jobStatus").value_or("").ends_with(": Pending"); }));
    runAsRunner(numberIn(harness, "jobStatus", "Statement "), ledgerId);
    bool const done = waitForJob(harness, ": Done");
    reportProblems(harness);
    // Polling stopped: the gate closed with the job.
    CHECK(harness.backend().prop(harness.find("Text", "polling").value(), "visible") == "false");
    CHECK(done);
    CHECK(harness.problems().empty());
}

// Mutation: stop polling on Done only (a failed job is polled forever, and the gate never closes).
TEST_CASE("ledger screens: a statement job run against another ledger is polled until it fails", "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    std::int64_t const first = createLedger(harness, "First");
    showTab(harness, kStatementTab, "Button", "requestStatement");
    harness.click("requestStatement");
    REQUIRE(harness.waitFor([&harness] { return harness.text("jobStatus").value_or("").ends_with(": Pending"); }));
    std::int64_t const second = createLedger(harness, "Second");
    REQUIRE(second != first);
    runAsRunner(numberIn(harness, "jobStatus", "Statement "), second);
    bool const failed = waitForJob(harness, ": Failed");
    reportProblems(harness);
    CHECK(harness.backend().prop(harness.find("Text", "polling").value(), "visible") == "false");
    CHECK(failed);
}

// Mutation: show the balance as a rounded float, or skip the accounts' refetch after the transaction.
TEST_CASE("ledger screens: a transaction moves an exact amount between accounts, and undo reverses it",
          "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    static_cast<void>(createLedger(harness, "Household"));
    showTab(harness, kLedgerTab, "Column", "accountForm");
    std::int64_t const bank = openAccount(harness, "Bank", "Asset");
    std::int64_t const food = openAccount(harness, "Groceries", "Expense");
    storeTransaction(harness, bank, food, "-12.34", "12.34");
    bool const moved = harness.waitFor(
        [&harness] { return balances(harness) == std::vector<std::string>{"-12.34 EUR", "12.34 EUR"}; });
    reportProblems(harness);
    for (std::string const& balance : balances(harness)) {
        UNSCOPED_INFO("balance " << balance);
    }
    REQUIRE(moved);

    harness.edit("month", "2026-10");
    REQUIRE(harness.waitFor([&harness] {
        return textsIn(harness, "Table", "entries", "entry") == std::vector<std::string>{"Weekly shop"};
    }));
    harness.click("undo");
    CHECK(
        harness.waitFor([&harness] { return balances(harness) == std::vector<std::string>{"0.00 EUR", "0.00 EUR"}; }));
    // Undo appends a compensating entry, which the month lists beside the one it reverses.
    CHECK(harness.waitFor([&harness] { return textsIn(harness, "Table", "entries", "entry").size() == 2; }));
    CHECK(harness.problems().empty());
}

// Mutation: drop the form's reply line (the refusal is then shown nowhere).
TEST_CASE("ledger screens: an unbalanced transaction is refused, shown on its form, and stores nothing",
          "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    static_cast<void>(createLedger(harness, "Household"));
    showTab(harness, kLedgerTab, "Column", "accountForm");
    std::int64_t const bank = openAccount(harness, "Bank", "Asset");
    std::int64_t const food = openAccount(harness, "Groceries", "Expense");
    storeTransaction(harness, bank, food, "-12.34", "10");
    CHECK(harness.waitFor([&harness] { return !formResult(harness, "transactionForm").empty(); }));
    INFO("form result: " << formResult(harness, "transactionForm"));
    CHECK(balances(harness) == std::vector<std::string>{"0.00 EUR", "0.00 EUR"});
}

// Mutation: show the report's spend from the limit field, or round either to whole units.
TEST_CASE("ledger screens: a budget's month shows its limit and the linked account's spend exactly",
          "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    static_cast<void>(createLedger(harness, "Household"));
    showTab(harness, kLedgerTab, "Column", "accountForm");
    std::int64_t const bank = openAccount(harness, "Bank", "Asset");
    std::int64_t const food = openAccount(harness, "Groceries", "Expense");
    storeTransaction(harness, bank, food, "-12.34", "12.34");
    REQUIRE(harness.waitFor(
        [&harness] { return balances(harness).size() == 2 && balances(harness).front() == "-12.34 EUR"; }));

    showTab(harness, kBudgetsTab, "Column", "categoryForm");
    harness.editField("categoryForm", "name", "Food");
    REQUIRE(harness.submitForm("categoryForm"));
    REQUIRE(harness.waitFor([&harness] {
        return harness.backend().prop(harness.find("Column", "linkForm").value(), "visible") != "false";
    }));
    harness.editField("linkForm", "accountId", std::to_string(food));
    REQUIRE(harness.submitForm("linkForm"));
    REQUIRE(harness.waitFor([&harness] { return !formResult(harness, "linkForm").empty(); }));
    harness.editField("budgetForm", "name", "Food budget");
    REQUIRE(harness.submitForm("budgetForm"));
    REQUIRE(harness.waitFor([&harness] {
        return harness.backend().prop(harness.find("Column", "limitForm").value(), "visible") != "false";
    }));
    harness.editField("limitForm", "month", "2026-10");
    editAmount(harness, "limitForm", "limit", "100.50");
    harness.chooseField("limitForm", "currency", "\"EUR\"");
    REQUIRE(harness.submitForm("limitForm"));
    REQUIRE(harness.waitFor([&harness] { return !formResult(harness, "limitForm").empty(); }));
    harness.edit("budgetMonth", "2026-10");
    bool const shown = harness.waitForText("budgetReport", "Limit 100.50 EUR, spent 12.34 EUR");
    reportProblems(harness);
    INFO("report: " << harness.text("budgetReport").value_or("") << " / " << harness.text("budgetError").value_or(""));
    CHECK(shown);
}

// Mutation: send the limit with the budget id left out (every limit is then refused, the valid one too).
TEST_CASE("ledger screens: a limit for a month that does not exist is refused on its form", "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    static_cast<void>(createLedger(harness, "Household"));
    showTab(harness, kBudgetsTab, "Column", "categoryForm");
    harness.editField("categoryForm", "name", "Food");
    REQUIRE(harness.submitForm("categoryForm"));
    REQUIRE(harness.waitFor([&harness] {
        return harness.backend().prop(harness.find("Column", "budgetForm").value(), "visible") != "false";
    }));
    harness.editField("budgetForm", "name", "Food budget");
    REQUIRE(harness.submitForm("budgetForm"));
    REQUIRE(harness.waitFor([&harness] {
        return harness.backend().prop(harness.find("Column", "limitForm").value(), "visible") != "false";
    }));
    harness.editField("limitForm", "month", "2026-13");
    editAmount(harness, "limitForm", "limit", "100");
    harness.chooseField("limitForm", "currency", "\"EUR\"");
    REQUIRE(harness.submitForm("limitForm"));
    CHECK(harness.waitFor([&harness] { return !formResult(harness, "limitForm").empty(); }));
    std::string const refused = formResult(harness, "limitForm");
    INFO("form result: " << refused);
    harness.editField("limitForm", "month", "2026-10");
    REQUIRE(harness.submitForm("limitForm"));
    CHECK(harness.waitFor([&harness, &refused] { return formResult(harness, "limitForm") != refused; }));
}

// Mutation: show the version the rule was created with rather than the edit's answer.
TEST_CASE("ledger screens: a rule's edit answers it with its bumped version", "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    static_cast<void>(createLedger(harness, "Household"));
    showTab(harness, kRulesTab, "Column", "ruleForm");
    harness.chooseField("ruleForm", "trigger", "\"DescriptionContains\"");
    harness.chooseField("ruleForm", "action", "\"SetCategory\"");
    harness.editField("ruleForm", "matchText", "shop");
    harness.editField("ruleForm", "actionValue", "1");
    bool const created = harness.submitForm("ruleForm");
    reportProblems(harness);
    REQUIRE(created);
    REQUIRE(harness.waitFor([&harness] { return harness.text("rule").value_or("").starts_with("Rule "); }));
    std::int64_t const ruleId = numberIn(harness, "rule", "Rule ");
    harness.editField("ruleEditForm", "matchText", "market");
    harness.editField("ruleEditForm", "actionValue", "1");
    REQUIRE(harness.submitForm("ruleEditForm"));
    bool const bumped = harness.waitForText("ruleVersion", "Rule " + std::to_string(ruleId) + ", version 2");
    reportProblems(harness);
    INFO("shown: " << harness.text("ruleVersion").value_or("") << " / " << formResult(harness, "ruleEditForm"));
    CHECK(bumped);
}
