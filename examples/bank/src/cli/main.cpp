// SPDX-License-Identifier: Apache-2.0
//
// A scripted, headless tour of the bank example. The same scenario function is
// run twice — once over a LocalBackend and once over a SimulatedRemoteBackend —
// to show that the model code and call sites are identical regardless of where
// the models actually execute.

#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>

#include "bank/core/money.hpp"
#include "bank/core/types.hpp"
#include "bank/db/database.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/auth_dto.hpp"
#include "bank/dto/budget_dto.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank/dto/loan_dto.hpp"
#include "bank/dto/notification_dto.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/dto/payment_dto.hpp"
#include "bank/dto/statement_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/customer_model.hpp"
#include "bank/models/auth_model.hpp"
#include "bank/models/budget_model.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/loan_model.hpp"
#include "bank/models/notification_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"
#include "bank/models/statement_model.hpp"
#include "bank/models/transaction_model.hpp"

namespace {

/// Pumps @p gui until @p completion resolves, returning its value or throwing.
template <typename T>
T await(morph::async::Completion<T> completion, morph::exec::MainThreadExecutor& gui) {
    std::atomic<bool> done{false};
    std::optional<T> value;
    std::exception_ptr error;
    completion.then([&](T resolved) { value = std::move(resolved); done.store(true); })
        .onError([&](const std::exception_ptr& err) { error = err; done.store(true); });
    while (!done.load()) {
        gui.runFor(std::chrono::milliseconds{10});
    }
    if (error) {
        std::rethrow_exception(error);
    }
    return std::move(*value);
}

bank::Money usd(std::int64_t minor) { return bank::Money{.minor = minor, .currency = bank::Currency::USD}; }

/// Runs the full scenario against an already-wired bridge + gui executor.
///
/// No per-handler wiring is needed for auditing: `morph::journal::setActionLog`
/// was called once in `main()`, so every handler constructed below — local or
/// (simulated) remote — automatically records to it. `session::current()`'s
/// principal is captured on each entry automatically too, from the login below.
void runScenario(morph::bridge::Bridge& bridge, morph::exec::MainThreadExecutor& gui, const char* label,
                 const std::string& principal) {
    std::println("\n========== {} ==========", label);

    morph::bridge::BridgeHandler<bank::AuthModel> auth{bridge, &gui};
    morph::bridge::BridgeHandler<bank::CustomerModel> accounts{bridge, &gui};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{bridge, &gui};
    morph::bridge::BridgeHandler<bank::PayeeModel> payees{bridge, &gui};
    morph::bridge::BridgeHandler<bank::PaymentModel> payments{bridge, &gui};
    morph::bridge::BridgeHandler<bank::CardModel> cards{bridge, &gui};
    morph::bridge::BridgeHandler<bank::LoanModel> loans{bridge, &gui};
    morph::bridge::BridgeHandler<bank::BudgetModel> budgets{bridge, &gui};
    morph::bridge::BridgeHandler<bank::NotificationModel> notes{bridge, &gui};
    morph::bridge::BridgeHandler<bank::StatementModel> statements{bridge, &gui};

    // Register + login. After login the bridge carries the principal on every call.
    await(auth.execute(bank::dto::RegisterUser{.username = principal, .password = "s3cret",
                                               .displayName = "Demo User"}),
          gui);
    auto login = await(auth.execute(bank::dto::LoginRequest{.username = principal, .password = "s3cret"}), gui);
    morph::session::Context ctx;
    ctx.principal = login.principal;
    bridge.setDefaultSession(ctx);
    std::println("logged in as {} ({})", login.principal, login.displayName);

    // Open two accounts.
    auto checking = await(accounts.execute(bank::dto::OpenAccount{
                              .kind = static_cast<int>(bank::AccountKind::Checking),
                              .currency = static_cast<int>(bank::Currency::USD),
                              .overdraftMinor = 20000}),
                          gui);
    auto savings = await(accounts.execute(bank::dto::OpenAccount{
                             .kind = static_cast<int>(bank::AccountKind::Savings),
                             .currency = static_cast<int>(bank::Currency::USD)}),
                         gui);
    std::println("opened checking {} and savings {}", checking.number, savings.number);

    // Deposit + transfer.
    await(txns.execute(bank::dto::Deposit{.accountId = checking.id, .amountMinor = 100000,
                                          .description = "opening deposit"}),
          gui);
    auto transfer = await(txns.execute(bank::dto::Transfer{.fromAccountId = checking.id,
                                                           .toAccountId = savings.id,
                                                           .amountMinor = 25000,
                                                           .description = "to savings"}),
                          gui);
    std::println("after transfer: checking {} / savings {}", bank::format(usd(transfer.fromBalanceMinor)),
                 bank::format(usd(transfer.toBalanceMinor)));

    // Payee + bill payment.
    auto payee = await(payees.execute(bank::dto::AddPayee{.name = "City Power",
                                                          .iban = "DE89370400440532013000",
                                                          .bankName = "Stadtbank"}),
                       gui);
    await(payments.execute(bank::dto::PayBill{.fromAccountId = checking.id, .payeeId = payee.id,
                                              .amountMinor = 7500, .description = "electricity"}),
          gui);
    std::println("paid {} to {}", bank::format(usd(7500)), payee.name);

    // Card lifecycle.
    auto card = await(cards.execute(bank::dto::IssueCard{.accountId = checking.id,
                                                         .kind = static_cast<int>(bank::CardKind::Debit),
                                                         .dailyLimitMinor = 50000}),
                      gui);
    await(cards.execute(bank::dto::FreezeCard{.id = card.id}), gui);
    await(cards.execute(bank::dto::UnfreezeCard{.id = card.id}), gui);
    std::println("issued debit card ****{} (frozen, then unfrozen)", card.panLast4);

    // Loan: apply, show first 3 schedule rows, repay.
    auto loan = await(loans.execute(bank::dto::ApplyLoan{.accountId = checking.id, .principalMinor = 1200000,
                                                         .rateBps = 600, .termMonths = 12}),
                      gui);
    auto schedule = await(loans.execute(bank::dto::LoanScheduleRequest{.loanId = loan.id}), gui);
    std::println("loan {} disbursed; monthly payment {}", loan.id,
                 bank::format(usd(schedule.monthlyPaymentMinor)));
    for (int idx = 0; idx < 3 && idx < static_cast<int>(schedule.installments.size()); ++idx) {
        const auto& inst = schedule.installments[static_cast<std::size_t>(idx)];
        std::println("  month {}: principal {} interest {} remaining {}", inst.month,
                     bank::format(usd(inst.principalMinor)), bank::format(usd(inst.interestMinor)),
                     bank::format(usd(inst.remainingMinor)));
    }
    auto repaid = await(loans.execute(bank::dto::RepayLoan{.loanId = loan.id, .fromAccountId = checking.id,
                                                           .amountMinor = 20000}),
                        gui);
    std::println("after repayment, loan outstanding {}", bank::format(usd(repaid.outstandingMinor)));

    // Budget + spending analytics.
    await(budgets.execute(bank::dto::SetBudget{.category = "utilities", .monthlyLimitMinor = 30000,
                                               .currency = 0}),
          gui);
    auto spend = await(budgets.execute(bank::dto::SpendingByKind{.accountId = checking.id}), gui);
    std::println("total debits on checking: {}", bank::format(usd(spend.totalDebitsMinor)));

    // Notifications.
    await(notes.execute(bank::dto::Notify{.message = "Welcome to morph bank!", .severity = 0}), gui);
    auto noteList = await(notes.execute(bank::dto::ListNotifications{}), gui);
    std::println("notifications: {} ({} unread)", noteList.notifications.size(), noteList.unreadCount);

    // Statement across accounts.
    auto statement = await(statements.execute(bank::dto::GenerateStatement{}), gui);
    std::println("statement: {} accounts, credits {} debits {}", statement.lines.size(),
                 bank::format(usd(statement.totalCreditsMinor)),
                 bank::format(usd(statement.totalDebitsMinor)));
}

}  // namespace

int main() {
    const auto dbPath = std::filesystem::temp_directory_path() / "morph_bank_cli.db";
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    bank::db::setup("DRIVER=SQLite3;Database=" + dbPath.string());

    // Audit trail: set once, here, and every model created from this point on
    // — in either scenario below, local or (simulated) remote — automatically
    // records to it. No per-handler wiring needed; see morph::journal::setActionLog.
    const auto auditPath = std::filesystem::temp_directory_path() / "morph_bank_cli_audit.ndjson";
    std::filesystem::remove(auditPath, ec);
    auto auditLog = std::make_shared<morph::journal::FileActionLog>(auditPath);
    morph::journal::setActionLog(auditLog);

    morph::exec::ThreadPoolExecutor workerPool{4};
    morph::exec::MainThreadExecutor gui;

    // 1) Local backend: models run in this process on the worker pool.
    {
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(workerPool)};
        runScenario(bridge, gui, "LocalBackend", "demo-local");
    }

    // 2) Remote backend (simulated): identical scenario, identical code. The
    // audited instances RemoteServer creates for this backend go through the
    // very same ModelFactory::create<Model>() as the local ones above, so the
    // audit log reaches them too with no extra wiring.
    {
        morph::exec::ThreadPoolExecutor serverPool{4};
        auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
        runScenario(bridge, gui, "SimulatedRemoteBackend", "demo-remote");
    }

    auditLog->flush();
    std::println("\n========== Audit trail ({}) ==========", auditPath.string());
    for (const auto& entry : auditLog->entries()) {
        std::println("[{}] {}::{} payload={} result={}", entry.principal, entry.modelType, entry.actionType,
                     entry.payload, entry.result);
    }

    std::println("\nDone.");
    return 0;
}
