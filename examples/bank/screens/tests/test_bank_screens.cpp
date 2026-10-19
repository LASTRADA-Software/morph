// SPDX-License-Identifier: Apache-2.0
//
// bank's screens on the screen harness (spec 4 §7): every screen validates; registering signs the new customer in and
// opens the accounts; the account chosen on Move money stays chosen, and receives the deposit, across the refetch
// the deposit causes (the A/B ordering); signing out returns to the login; a loan's schedule is the model's. The
// models run in process over the bank's test database.

#include <app/app_source.hpp>
#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/bundle_warnings.hpp>
#include <testkit/screen_harness.hpp>
#include <vector>

#include "bank_screens.hpp"
#include "bank_test_support.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

LocalApp bankApp() {
    return LocalApp{.name = "bank", .bundle = &bank::screens::bundle, .setUp = [](LocalSetup const&) {
                        bank::testing::ensureDatabase();
                    }};
}

/// A principal no other test uses: the tests share one database.
std::string uniqueUser(std::string_view stem) {
    static std::atomic<int> counter{0};
    return std::string{stem} + "-" + std::to_string(++counter);
}

/// The harness on @p screenId, signed in as @p user (empty: signed out).
ScreenHarness open(std::string_view screenId, std::string const& user) {
    return ScreenHarness{bankApp(), std::string{screenId}, RigMode::Local, {}, LocalSetup{.db = {}, .user = user}};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// The number of `Text` widgets under @p root.
std::size_t textsUnder(ScreenHarness& harness, int root) {
    auto const& backend = harness.backend();
    std::size_t count = 0;
    std::vector<int> pending{root};
    while (!pending.empty()) {
        int const widget = pending.back();
        pending.pop_back();
        count += backend.kindOf(widget) == "Text" ? 1U : 0U;
        for (int const child : backend.children(widget)) {
            pending.push_back(child);
        }
    }
    return count;
}

/// The keys of a select's options, as the recording backend lists them (`[5:label,6:label]`).
std::vector<std::int64_t> optionKeys(ScreenHarness& harness, std::string_view testId) {
    auto const select = harness.find("Select", testId);
    std::vector<std::int64_t> keys;
    if (!select) {
        return keys;
    }
    std::string const options = harness.backend().prop(*select, "options");
    std::size_t position = 1;
    while (position < options.size()) {
        std::size_t const colon = options.find(':', position);
        if (colon == std::string::npos) {
            break;
        }
        keys.push_back(std::stoll(options.substr(position, colon - position)));
        std::size_t const comma = options.find(',', colon);
        position = comma == std::string::npos ? options.size() : comma + 1;
    }
    return keys;
}

/// Opens one account through the accounts screen's form, and waits for the list to show @p count accounts.
void openAccount(ScreenHarness& harness, std::size_t count) {
    harness.editField("openForm", "kind", "0");
    harness.editField("openForm", "currency", "0");
    harness.editField("openForm", "overdraftMinor", "0");
    REQUIRE(harness.submitForm("openForm"));
    REQUIRE(harness.waitFor([&harness, count] { return optionKeys(harness, "historyAccount").size() == count; }));
}

}  // namespace

TEST_CASE("bank screens: every screen validates, and the bundle builds", "[bank-screens]") {
    auto const bundle = bank::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Every document loads against the catalog the server serves with no refusal and no warning: each mutation's action
// declares its effects, or `none`.
TEST_CASE("bank_bundle_loads_without_warnings", "[bank-screens]") {
    auto const bundle = bank::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
    std::vector<std::string> const findings = morph::examples::testing::bundleLoadFindings(**bundle);
    for (std::string const& finding : findings) {
        UNSCOPED_INFO(finding);
    }
    CHECK(findings.empty());
}

// Mutation: navigate without signing in (the accounts screen's calls carry no principal and fail).
TEST_CASE("bank screens: registering signs the customer in and opens their accounts", "[bank-screens]") {
    ScreenHarness harness = open(bank::screens::kLogin, {});
    std::string const user = uniqueUser("reg");
    harness.edit("username", user);
    harness.edit("password", "secret-123");
    harness.edit("displayName", "Reg User");
    harness.click("register");
    REQUIRE(harness.waitFor([&harness] { return harness.mounted().size() == 2; }));
    CHECK(harness.mounted().back() == bank::screens::kAccounts);
    REQUIRE(harness.waitFor([&harness] { return harness.find("Column", "openForm").has_value(); }));
    openAccount(harness, 1);
    reportProblems(harness);
    CHECK(harness.text("error").value_or("").empty());
    CHECK(harness.problems().empty());

    // Signing out, from the client's frame, returns to the login.
    harness.click("shell_signOut");
    REQUIRE(harness.waitFor([&harness] { return harness.mounted().size() == 3; }));
    CHECK(harness.mounted().back() == bank::screens::kLogin);
}

// Mutation: a wrong password still signs in (no refusal is shown, the accounts open).
TEST_CASE("bank screens: a wrong password is refused with the model's message", "[bank-screens]") {
    std::string const user = uniqueUser("pw");
    {
        ScreenHarness first = open(bank::screens::kLogin, {});
        first.edit("username", user);
        first.edit("password", "right-123");
        first.edit("displayName", "Pw User");
        first.click("register");
        REQUIRE(first.waitFor([&first] { return first.mounted().size() == 2; }));
    }
    ScreenHarness harness = open(bank::screens::kLogin, {});
    harness.edit("username", user);
    harness.edit("password", "wrong-123");
    harness.click("signIn");
    REQUIRE(harness.waitFor([&harness] { return !harness.text("loginError").value_or("").empty(); }));
    CHECK(harness.mounted().size() == 1);
}

// Mutation: the deposit form's account is not the select's (the deposit lands in A, B's balance is unchanged).
TEST_CASE("bank screens: the account chosen on Move money keeps naming the account the next deposit lands in",
          "[bank-screens]") {
    std::string const user = uniqueUser("ab");
    bank::testing::ensurePrincipal(user);
    {
        ScreenHarness accounts = open(bank::screens::kAccounts, user);
        REQUIRE(accounts.waitFor([&accounts] { return accounts.find("Column", "openForm").has_value(); }));
        openAccount(accounts, 1);
        openAccount(accounts, 2);
    }
    ScreenHarness harness = open(bank::screens::kMove, user);
    REQUIRE(harness.waitFor([&harness] { return optionKeys(harness, "account").size() == 2; }));
    std::vector<std::int64_t> const keys = optionKeys(harness, "account");
    std::int64_t const second = keys.at(1);
    harness.backend().choose(harness.find("Select", "account").value(), morph::ui::Key{second});
    REQUIRE(harness.waitForText("balance", "Balance 0.00"));

    harness.editField("depositForm", "amountMinor", "1250");
    REQUIRE(harness.submitForm("depositForm"));
    bool const deposited = harness.waitForText("balance", "Balance 12.50");
    reportProblems(harness);
    CHECK(deposited);
    // Still B, after the refetch the deposit caused.
    CHECK(harness.backend().prop(harness.find("Select", "account").value(), "selected") == std::to_string(second));
    CHECK(harness.problems().empty());
}

// Mutation: the schedule shows the loan list instead (no installment rows).
TEST_CASE("bank screens: a loan's schedule is the model's, through the schedule component's fallback",
          "[bank-screens]") {
    std::string const user = uniqueUser("loan");
    bank::testing::ensurePrincipal(user);
    std::int64_t accountId = 0;
    {
        ScreenHarness accounts = open(bank::screens::kAccounts, user);
        REQUIRE(accounts.waitFor([&accounts] { return accounts.find("Column", "openForm").has_value(); }));
        openAccount(accounts, 1);
        accountId = optionKeys(accounts, "historyAccount").at(0);
    }
    ScreenHarness harness = open(bank::screens::kLoans, user);
    REQUIRE(harness.waitFor([&harness] { return harness.find("Column", "applyForm").has_value(); }));
    harness.editField("applyForm", "accountId", std::to_string(accountId));
    harness.editField("applyForm", "principalMinor", "120000");
    harness.editField("applyForm", "rateBps", "600");
    harness.editField("applyForm", "termMonths", "12");
    REQUIRE(harness.submitForm("applyForm"));
    REQUIRE(harness.waitFor([&harness] { return optionKeys(harness, "scheduleLoan").size() == 1; }));
    harness.backend().choose(harness.find("Select", "scheduleLoan").value(),
                             morph::ui::Key{optionKeys(harness, "scheduleLoan").at(0)});
    bool const shown = harness.waitFor([&harness] {
        auto const monthly = harness.text("monthly");
        return monthly && monthly->starts_with("Monthly payment ");
    });
    reportProblems(harness);
    CHECK(shown);
    // Twelve installments, three cells each, drawn by the component's fallback.
    auto const table = harness.find("Table", "scheduleFallback");
    REQUIRE(table.has_value());
    CHECK(textsUnder(harness, table.value()) == 36U);
    CHECK(harness.problems().empty());
}

// Mutation: send the explorer's Try it form to Deposit instead (B's balance is unchanged).
TEST_CASE("bank screens: the explorer lists the bank's models, and its Try it for Transfer moves money",
          "[bank-screens][explorer]") {
    std::string const user = uniqueUser("explore");
    bank::testing::ensurePrincipal(user);
    std::vector<std::int64_t> keys;
    {
        ScreenHarness accounts = open(bank::screens::kAccounts, user);
        REQUIRE(accounts.waitFor([&accounts] { return accounts.find("Column", "openForm").has_value(); }));
        openAccount(accounts, 1);
        openAccount(accounts, 2);
        keys = optionKeys(accounts, "historyAccount");
    }
    REQUIRE(keys.size() == 2U);
    {
        ScreenHarness move = open(bank::screens::kMove, user);
        REQUIRE(move.waitFor([&move] { return optionKeys(move, "account").size() == 2; }));
        move.backend().choose(move.find("Select", "account").value(), morph::ui::Key{keys.at(0)});
        REQUIRE(move.waitForText("balance", "Balance 0.00"));
        move.editField("depositForm", "amountMinor", "1250");
        REQUIRE(move.submitForm("depositForm"));
        REQUIRE(move.waitForText("balance", "Balance 12.50"));
    }

    ScreenHarness harness = open("explorer", user);
    for (std::string_view const model : {"AccountModel", "TransactionModel", "Bank_AuthModel", "LoanModel"}) {
        CHECK(harness.find("Collapsible", "explorer_model_" + std::string{model}).has_value());
    }
    harness.click("explorer_action_Transfer");
    REQUIRE(harness.waitFor([&harness] { return harness.find("Panel", "explorer_fields").has_value(); }));
    CHECK(harness.backend().find("Text", "text", "fromAccountId").has_value());
    CHECK(harness.backend().find("Text", "text", "permitted").has_value());
    harness.editField("tryit", "fromAccountId", std::to_string(keys.at(0)));
    harness.editField("tryit", "toAccountId", std::to_string(keys.at(1)));
    harness.editField("tryit", "amountMinor", "500");
    REQUIRE(harness.submitForm("tryit"));
    bool const replied = harness.waitFor([&harness] { return !harness.text("tryit_result").value_or("").empty(); });
    reportProblems(harness);
    CHECK(replied);
    CHECK(harness.text("tryit_error").value_or("").empty());

    // The balances read back from the move screen.
    ScreenHarness move = open(bank::screens::kMove, user);
    REQUIRE(move.waitFor([&move] { return optionKeys(move, "account").size() == 2; }));
    move.backend().choose(move.find("Select", "account").value(), morph::ui::Key{keys.at(1)});
    CHECK(move.waitForText("balance", "Balance 5.00"));
    move.backend().choose(move.find("Select", "account").value(), morph::ui::Key{keys.at(0)});
    CHECK(move.waitForText("balance", "Balance 7.50"));
}
