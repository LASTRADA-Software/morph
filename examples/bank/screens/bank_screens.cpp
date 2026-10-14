// SPDX-License-Identifier: Apache-2.0

#include "bank_screens.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bank/dto/account_dto.hpp"
#include "bank/dto/auth_dto.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank/dto/loan_dto.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/auth_model.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/loan_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/transaction_model.hpp"

namespace bank::screens {

namespace ui = morph::ui::author;
namespace dto = bank::dto;

namespace {

// An amount in minor units, shown with two places.
ui::Handle<std::string> money(ui::AnyHandle const& minor) {
    return ui::call<std::string>("decimal", {minor / std::int64_t{100}, std::int64_t{2}});
}

// An account as a picker option: its id, and its number with its balance.
ui::AnyHandle accountOptions(ui::AnyHandle const& accounts) {
    return ui::map(accounts, "a", [](ui::Handle<ui::Any> const& account) {
        return ui::obj(
            {{"id", account.at("id")},
             {"label", ui::format("{0} ({1})", {account.at("number"), money(account.at("balanceMinor"))})}});
    });
}

// The sidebar every signed-in screen shows: the other screens, and signing out.
ui::Node sidebar(std::string_view current) {
    struct Entry {
        std::string_view screen;
        std::string label;
        std::string testId;
    };
    std::vector<Entry> const entries{{.screen = kAccounts, .label = "Accounts", .testId = "nav-accounts"},
                                     {.screen = kMove, .label = "Move money", .testId = "nav-move"},
                                     {.screen = kCards, .label = "Cards", .testId = "nav-cards"},
                                     {.screen = kPayees, .label = "Payees", .testId = "nav-payees"},
                                     {.screen = kLoans, .label = "Loans", .testId = "nav-loans"}};
    std::vector<ui::Node> items;
    items.reserve(entries.size() + 1);
    for (Entry const& entry : entries) {
        items.push_back(ui::button({.label = entry.label, .onClick = ui::navigate(std::string{entry.screen})})
                            .testId(entry.testId)
                            .enabled(entry.screen != current));
    }
    items.push_back(ui::button({.label = "Sign out", .onClick = {ui::signOut(), ui::navigate(std::string{kLogin})}})
                        .testId("signOut"));
    return ui::column(items, {.gap = 4});
}

// A signed-in screen: the sidebar, then its content.
ui::Node page(std::string_view current, ui::Nodes const& content) {
    return ui::row({sidebar(current), ui::column(content, {.gap = 8})}, {.gap = 16});
}

// Sign-in: a successful login or registration installs the principal it answered and opens the accounts.
ui::Screen loginScreen() {
    return ui::screen(std::string{kLogin}, [](ui::Scope& screen) {
        screen.title("Sign in");
        auto const auth = screen.model<AuthModel>("auth", {.instance = ui::Instance::Private});
        auto const username = ui::element("username");
        auto const password = ui::element("password");
        auto const displayName = ui::element("displayName");
        auto const signedIn = [](ui::MutationRef const& mutation, ui::AnyHandle const& result) {
            return ui::Commands{ui::ifThen(ui::eq(result.at("ok"), true),
                                           {ui::signIn(mutation), ui::navigate(std::string{kAccounts})})};
        };
        auto const login =
            screen.mutation<dto::LoginRequest>("login", auth,
                                               {.body = {{&dto::LoginRequest::username, username.value()},
                                                         {&dto::LoginRequest::password, password.value()}}});
        auto const registered =
            screen.mutation<dto::RegisterUser>("register", auth,
                                               {.body = {{&dto::RegisterUser::username, username.value()},
                                                         {&dto::RegisterUser::password, password.value()},
                                                         {&dto::RegisterUser::displayName, displayName.value()}}});
        auto const loggedIn = screen.let("loggedIn", login.result());
        auto const created = screen.let("created", registered.result());
        screen.watch(loggedIn, signedIn(login, loggedIn));
        screen.watch(created, signedIn(registered, created));
        auto const refusal = ui::coalesce({loggedIn.at("message"), created.at("message")});
        return ui::column(
            {ui::text({.text = "Morph Bank"}).testId("brand"),
             ui::textInput({.label = "Username", .placeholder = "Username"}).id(username).testId("username"),
             ui::textInput({.label = "Password", .placeholder = "Password", .mode = std::string{"password"}})
                 .id(password)
                 .testId("password"),
             ui::textInput({.label = "Display name", .placeholder = "Display name (for new accounts)"})
                 .id(displayName)
                 .testId("displayName"),
             ui::row({ui::button({.label = "Sign in", .onClick = ui::run(login)}).testId("signIn"),
                      ui::button({.label = "Create account", .onClick = ui::run(registered)}).testId("register")},
                     {.gap = 8}),
             ui::text({.text = ui::coalesce({refusal, login.error(), registered.error()})}).testId("loginError")},
            {.gap = 8});
    });
}

// The accounts: the cards of every account, opening one, closing one, and the history of the one chosen.
ui::Screen accountsScreen() {
    return ui::screen(std::string{kAccounts}, [](ui::Scope& screen) {
        screen.title("Accounts");
        auto const customer = screen.model<CustomerModel>("customer", {.instance = ui::Instance::Private});
        auto const account = screen.model<AccountModel>("account", {.instance = ui::Instance::Private});
        auto const transactions = screen.model<TransactionModel>("transactions", {.instance = ui::Instance::Private});
        auto const accounts = screen.query<dto::ListAccounts>("accounts", customer);
        auto const rows = accounts.value().at(&dto::AccountList::accounts);
        auto const chosen = ui::element("historyAccount");
        auto const history =
            screen.query<dto::History>("history", transactions, {{&dto::History::accountId, chosen.selected()}},
                                       {.when = ui::engaged(chosen.selected())});
        auto const close = screen.mutation<dto::CloseAccount>("close", account, {.invalidates = {accounts}});
        auto const cards = screen.component(
            "AccountCards",
            {.host = "AccountCards",
             .version = 1,
             .props = {{"accounts", ui::PropType::Json}},
             .fallback = ui::table(ui::prop("accounts"),
                                   {.key = ui::row("id"),
                                    .columns = {{.name = "number",
                                                 .label = "Account",
                                                 .cell = ui::text({.text = ui::row("number")})},
                                                {.name = "balance",
                                                 .label = "Balance",
                                                 .cell = ui::text({.text = money(ui::row("balanceMinor"))})}}})
                             .a11y("Accounts")
                             .testId("accountCardsFallback")});
        return page(
            kAccounts,
            {ui::custom(cards, {.props = {{"accounts", rows}}}).a11y("Account cards").testId("accountCards"),
             ui::table(
                 rows,
                 {.key = ui::row("id"),
                  .columns = {{.name = "number", .label = "Account", .cell = ui::text({.text = ui::row("number")})},
                              {.name = "close",
                               .label = "",
                               .cell = ui::button(
                                   {.label = "Close",
                                    .onClick = ui::run(close, {{&dto::CloseAccount::id, ui::row("id")}})})}}})
                 .a11y("Accounts")
                 .testId("accounts"),
             ui::form<dto::OpenAccount>(customer, {.submit = "explicit",
                                                   .fields = {{&dto::OpenAccount::owner, {.omit = true}},
                                                              {&dto::OpenAccount::overdraftMinor,
                                                               {.label = std::string{"Overdraft (cents)"}}}},
                                                   .onSuccess = {ui::refetch(accounts)}})
                 .id(ui::element("openForm"))
                 .testId("openForm"),
             ui::select({.options = accountOptions(rows), .label = "History of"}).id(chosen).testId("historyAccount"),
             ui::table(history.value().at(&dto::HistoryPage::entries),
                       {.key = ui::row("id"),
                        .columns = {{.name = "description",
                                     .label = "Description",
                                     .cell = ui::text({.text = ui::row("description")})},
                                    {.name = "amount",
                                     .label = "Amount",
                                     .cell = ui::text({.text = money(ui::row("amountMinor"))})}}})
                 .a11y("History")
                 .testId("history"),
             ui::text({.text = ui::coalesce({accounts.error(), close.error(), history.error()})}).testId("error")});
    });
}

// Moving money: the account chosen stays chosen across the refetch every deposit, withdrawal and transfer causes.
ui::Screen moveScreen() {
    return ui::screen(std::string{kMove}, [](ui::Scope& screen) {
        screen.title("Move money");
        auto const customer = screen.model<CustomerModel>("customer", {.instance = ui::Instance::Private});
        auto const transactions = screen.model<TransactionModel>("transactions", {.instance = ui::Instance::Private});
        auto const accounts = screen.query<dto::ListAccounts>("accounts", customer);
        auto const rows = accounts.value().at(&dto::AccountList::accounts);
        auto const from = ui::element("account");
        auto const target = ui::element("target");
        auto const balance = screen.let(
            "chosenAccount", ui::call("first", {ui::filter(rows, "a", [&from](ui::Handle<ui::Any> const& row) {
                                          return ui::eq(row.at("id"), from.selected());
                                      })}));
        auto const flow = screen.component(
            "TransferFlow",
            {.host = "TransferFlow",
             .version = 1,
             .props = {{"from", ui::PropType::Json}, {"to", ui::PropType::Json}},
             .fallback =
                 ui::text({.text = ui::format("From {0} to {1}", {ui::prop("from.label"), ui::prop("to.label")})})
                     .testId("transferFlowFallback")});
        auto const moved = ui::Commands{ui::refetch(accounts)};
        auto const hidden = ui::FieldSettings{.hidden = true};
        return page(
            kMove,
            {ui::select({.options = accountOptions(rows), .label = "Account"}).id(from).testId("account"),
             ui::text({.text = ui::format("Balance {0}", {money(balance.at("balanceMinor"))})})
                 .testId("balance")
                 .visible(ui::engaged(balance)),
             ui::form<dto::Deposit>(transactions,
                                    {.prefill = ui::obj({{"accountId", from.selected()}}),
                                     .submit = "explicit",
                                     .fields = {{&dto::Deposit::accountId, hidden},
                                                {&dto::Deposit::amountMinor, {.label = std::string{"Amount (cents)"}}},
                                                {&dto::Deposit::description, {.blankAs = std::string{"empty"}}}},
                                     .onSuccess = moved})
                 .id(ui::element("depositForm"))
                 .testId("depositForm"),
             ui::form<dto::Withdraw>(
                 transactions, {.prefill = ui::obj({{"accountId", from.selected()}}),
                                .submit = "explicit",
                                .fields = {{&dto::Withdraw::accountId, hidden},
                                           {&dto::Withdraw::amountMinor, {.label = std::string{"Amount (cents)"}}},
                                           {&dto::Withdraw::description, {.blankAs = std::string{"empty"}}}},
                                .onSuccess = moved})
                 .id(ui::element("withdrawForm"))
                 .testId("withdrawForm"),
             ui::select({.options = accountOptions(rows), .label = "Transfer to"}).id(target).testId("target"),
             ui::custom(flow, {.props = {{"from", ui::ref("account.selectedOption")},
                                         {"to", ui::ref("target.selectedOption")}}})
                 .a11y("Transfer")
                 .testId("transferFlow"),
             ui::form<dto::Transfer>(
                 transactions,
                 {.prefill = ui::obj({{"fromAccountId", from.selected()}, {"toAccountId", target.selected()}}),
                  .submit = "explicit",
                  .fields = {{&dto::Transfer::fromAccountId, hidden},
                             {&dto::Transfer::toAccountId, hidden},
                             {&dto::Transfer::amountMinor, {.label = std::string{"Amount (cents)"}}},
                             {&dto::Transfer::description, {.blankAs = std::string{"empty"}}}},
                  .onSuccess = moved})
                 .id(ui::element("transferForm"))
                 .testId("transferForm"),
             ui::text({.text = accounts.error()}).testId("error")});
    });
}

// The cards: issue one on an account, and freeze, unfreeze or cancel each.
ui::Screen cardsScreen() {
    return ui::screen(std::string{kCards}, [](ui::Scope& screen) {
        screen.title("Cards");
        auto const cardModel = screen.model<CardModel>("card", {.instance = ui::Instance::Private});
        auto const cards = screen.query<dto::ListCards>("cards", cardModel);
        auto const freeze = screen.mutation<dto::FreezeCard>("freeze", cardModel, {.invalidates = {cards}});
        auto const unfreeze = screen.mutation<dto::UnfreezeCard>("unfreeze", cardModel, {.invalidates = {cards}});
        auto const cancel = screen.mutation<dto::CancelCard>("cancel", cardModel, {.invalidates = {cards}});
        auto const action = [](auto const& mutation, auto member, std::string const& label) {
            return ui::button({.label = label, .onClick = ui::run(mutation, {{member, ui::row("id")}})});
        };
        return page(
            kCards,
            {ui::table(
                 cards.value().at(&dto::CardList::cards),
                 {.key = ui::row("id"),
                  .columns =
                      {{.name = "card",
                        .label = "Card",
                        .cell = ui::text({.text = ui::format("•••• {0}", {ui::row("panLast4")})})},
                       {.name = "status", .label = "Status", .cell = ui::text({.text = ui::row("status")})},
                       {.name = "freeze", .label = "", .cell = action(freeze, &dto::FreezeCard::id, "Freeze")},
                       {.name = "unfreeze", .label = "", .cell = action(unfreeze, &dto::UnfreezeCard::id, "Unfreeze")},
                       {.name = "cancel", .label = "", .cell = action(cancel, &dto::CancelCard::id, "Cancel")}}})
                 .a11y("Cards")
                 .testId("cards"),
             ui::form<dto::IssueCard>(cardModel, {.submit = "explicit",
                                                  .fields = {{&dto::IssueCard::dailyLimitMinor,
                                                              {.label = std::string{"Daily limit (cents)"}}}},
                                                  .onSuccess = {ui::refetch(cards)}})
                 .id(ui::element("issueForm"))
                 .testId("issueForm"),
             ui::text({.text = ui::coalesce({cards.error(), freeze.error(), unfreeze.error(), cancel.error()})})
                 .testId("error")});
    });
}

// The payees: add one, remove each.
ui::Screen payeesScreen() {
    return ui::screen(std::string{kPayees}, [](ui::Scope& screen) {
        screen.title("Payees");
        auto const payeeModel = screen.model<PayeeModel>("payee", {.instance = ui::Instance::Private});
        auto const payees = screen.query<dto::ListPayees>("payees", payeeModel);
        auto const remove = screen.mutation<dto::RemovePayee>("remove", payeeModel, {.invalidates = {payees}});
        return page(
            kPayees,
            {ui::table(payees.value().at(&dto::PayeeList::payees),
                       {.key = ui::row("id"),
                        .columns = {{.name = "name", .label = "Name", .cell = ui::text({.text = ui::row("name")})},
                                    {.name = "iban", .label = "IBAN", .cell = ui::text({.text = ui::row("iban")})},
                                    {.name = "remove",
                                     .label = "",
                                     .cell = ui::button(
                                         {.label = "Remove",
                                          .onClick = ui::run(remove, {{&dto::RemovePayee::id, ui::row("id")}})})}}})
                 .a11y("Payees")
                 .testId("payees"),
             ui::form<dto::AddPayee>(payeeModel, {.submit = "explicit", .onSuccess = {ui::refetch(payees)}})
                 .id(ui::element("addForm"))
                 .testId("addForm"),
             ui::text({.text = ui::coalesce({payees.error(), remove.error()})}).testId("error")});
    });
}

// The loans: apply for one, repay one, and the schedule of the one chosen, which the model computes.
ui::Screen loansScreen() {
    return ui::screen(std::string{kLoans}, [](ui::Scope& screen) {
        screen.title("Loans");
        auto const loanModel = screen.model<LoanModel>("loan", {.instance = ui::Instance::Private});
        auto const loans = screen.query<dto::ListLoans>("loans", loanModel);
        auto const rows = loans.value().at(&dto::LoanList::loans);
        auto const chosen = ui::element("scheduleLoan");
        auto const schedule = screen.query<dto::LoanScheduleRequest>(
            "schedule", loanModel, {{&dto::LoanScheduleRequest::loanId, chosen.selected()}},
            {.when = ui::engaged(chosen.selected())});
        auto const installments = schedule.value().at(&dto::LoanScheduleResult::installments);
        auto const scheduleView = screen.component(
            "LoanSchedule",
            {.host = "LoanSchedule",
             .version = 1,
             .props = {{"installments", ui::PropType::Json}},
             .fallback =
                 ui::table(
                     ui::prop("installments"),
                     {.key = ui::row("month"),
                      .columns = {{.name = "month", .label = "Month", .cell = ui::text({.text = ui::row("month")})},
                                  {.name = "payment",
                                   .label = "Payment",
                                   .cell = ui::text({.text = money(ui::row("paymentMinor"))})},
                                  {.name = "remaining",
                                   .label = "Remaining",
                                   .cell = ui::text({.text = money(ui::row("remainingMinor"))})}}})
                     .a11y("Schedule")
                     .testId("scheduleFallback")});
        auto const options = ui::map(rows, "l", [](ui::Handle<ui::Any> const& loan) {
            return ui::obj({{"id", loan.at("id")},
                            {"label", ui::format("Loan {0}: {1} outstanding",
                                                 {loan.at("id"), money(loan.at("outstandingMinor"))})}});
        });
        return page(
            kLoans,
            {ui::table(rows, {.key = ui::row("id"),
                              .columns = {{.name = "loan", .label = "Loan", .cell = ui::text({.text = ui::row("id")})},
                                          {.name = "outstanding",
                                           .label = "Outstanding",
                                           .cell = ui::text({.text = money(ui::row("outstandingMinor"))})}}})
                 .a11y("Loans")
                 .testId("loans"),
             ui::form<dto::ApplyLoan>(loanModel, {.submit = "explicit",
                                                  .fields = {{&dto::ApplyLoan::principalMinor,
                                                              {.label = std::string{"Principal (cents)"}}}},
                                                  .onSuccess = {ui::refetch(loans)}})
                 .id(ui::element("applyForm"))
                 .testId("applyForm"),
             ui::form<dto::RepayLoan>(
                 loanModel, {.submit = "explicit",
                             .fields = {{&dto::RepayLoan::amountMinor, {.label = std::string{"Amount (cents)"}}}},
                             .onSuccess = {ui::refetch(loans)}})
                 .id(ui::element("repayForm"))
                 .testId("repayForm"),
             ui::select({.options = options, .label = "Schedule of"}).id(chosen).testId("scheduleLoan"),
             ui::custom(scheduleView, {.props = {{"installments", installments}}})
                 .a11y("Loan schedule")
                 .testId("schedule"),
             ui::text(
                 {.text = ui::format("Monthly payment {0}",
                                     {money(schedule.value().at(&dto::LoanScheduleResult::monthlyPaymentMinor))})})
                 .testId("monthly")
                 .visible(ui::engaged(schedule.value())),
             ui::text({.text = ui::coalesce({loans.error(), schedule.error()})}).testId("error")});
    });
}

}  // namespace

std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle() {
    ui::ScreenRegistry screens;
    struct Listed {
        std::string id;
        std::string title;
        decltype(screens.add(loginScreen())) registered;
    };
    std::vector<Listed> listed;
    listed.push_back({.id = std::string{kLogin}, .title = "Sign in", .registered = screens.add(loginScreen())});
    listed.push_back({.id = std::string{kAccounts}, .title = "Accounts", .registered = screens.add(accountsScreen())});
    listed.push_back({.id = std::string{kMove}, .title = "Move money", .registered = screens.add(moveScreen())});
    listed.push_back({.id = std::string{kCards}, .title = "Cards", .registered = screens.add(cardsScreen())});
    listed.push_back({.id = std::string{kPayees}, .title = "Payees", .registered = screens.add(payeesScreen())});
    listed.push_back({.id = std::string{kLoans}, .title = "Loans", .registered = screens.add(loansScreen())});
    for (Listed const& screen : listed) {
        if (!screen.registered) {
            return std::unexpected(screen.registered.error());
        }
    }
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = std::string{kApplicationId}, .version = "1.0"},
                                 screens.catalog()};
    for (Listed const& screen : listed) {
        morph::ui::CatalogEntry entry;
        entry.id = screen.id;
        entry.title = screen.title;
        entry.isPublic = screen.id == kLogin;
        if (auto added = served.addScreen(std::move(entry), (*screen.registered)->text); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace bank::screens
