// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "lims/app/lims_authorizer.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

/// @file
/// The scaffolding every lims test needs: a scoped authenticated principal.
///
/// Models in this rung refuse to mutate anything without one
/// (`lims::requirePrincipal`), which is deliberate — the README names
/// empty-principal audit entries as disqualifying — so almost every test
/// declares one.

namespace lims::test {

/// @brief Builds a session context naming @p principal.
/// @param principal The authenticated principal's name.
/// @return The context.
[[nodiscard]] inline morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

/// @brief Installs @p principal as the authenticated principal for this scope.
///
/// `_ctx` is declared before `_scope` on purpose: `ScopedContext` holds a
/// reference to it, so the member order *is* the lifetime guarantee.
class ScopedPrincipal {
public:
    /// @param principal The name every action in this scope executes as.
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief The site the first org migration seeds, where every sample that predates sites is.
/// @return Its id.
[[nodiscard]] inline lims::SiteId seededSite() { return lims::SiteId{lims::db::kSeededSiteId}; }

/// @brief The company the first org migration seeds, which owns the seeded site.
/// @return Its id.
[[nodiscard]] inline lims::CompanyId seededCompany() { return lims::CompanyId{lims::db::kSeededCompanyId}; }

/// @brief Assigns @p role to @p principal in @p scope.
///
/// Made by `lab-supervisor`, a supervisor at the seeded company the helper appoints itself while the lab has none. A
/// test that appoints its own supervisor first assigns through `OrgModel` as that supervisor instead.
/// @param principal Who gets the role.
/// @param role The role.
/// @param scope Where; the seeded company unless said.
/// @return The assignment.
inline lims::RoleAssignmentView grant(std::string principal, lims::LimsRole role,
                                      lims::Scope scope = lims::Scope{seededCompany()}) {
    constexpr std::string_view kGranter = "lab-supervisor";
    ScopedPrincipal const granter{std::string{kGranter}};
    lims::OrgModel org;
    auto const held = lims::OrgModel::rolesIn(kGranter, lims::Scope{seededCompany()});
    if (std::ranges::find(held, lims::LimsRole::Supervisor) == held.end()) {
        ::morph::model::executeChecked(org, lims::AssignRole{.principal = std::string{kGranter},
                                                             .role = lims::LimsRole::Supervisor,
                                                             .company = seededCompany(),
                                                             .site = {}});
    }
    auto const* company = std::get_if<lims::CompanyId>(&scope);
    return ::morph::model::executeChecked(
        org, lims::AssignRole{.principal = std::move(principal),
                              .role = role,
                              .company = company != nullptr ? *company : lims::CompanyId{},
                              .site = company != nullptr ? lims::SiteId{} : std::get<lims::SiteId>(scope)});
}

/// @brief A fresh database in which the lab's bench staff, `alice`, `fiona`, `gerard` and `tech`, are analysts at the
///        seeded company, which holds at every one of its sites.
///
/// Every write on a sample needs some role at the sample's site, so a test that registers, transitions or captures as
/// one of them needs these grants as setup. A test about what a principal without a role may do uses a plain
/// `DbFixture`.
class LabFixture : public ::morph::ladder::testkit::DbFixture {
public:
    LabFixture() {
        for (std::string const staff : {"alice", "fiona", "gerard", "tech"}) {
            static_cast<void>(grant(staff, lims::LimsRole::Analyst));
        }
    }
};

/// @brief A UTC instant on a calendar day.
/// @param year The year.
/// @param month The month, 1 to 12.
/// @param day The day.
/// @param hour The hour.
/// @return The instant.
[[nodiscard]] inline morph::time::DateTime at(int year, unsigned month, unsigned day, int hour = 9) {
    return morph::time::DateTime{std::chrono::year{year},  std::chrono::month{month}, std::chrono::day{day},
                                 std::chrono::hours{hour}, std::chrono::minutes{0},   std::chrono::seconds{0}};
}

/// @brief The whole UTC second of @p at as ISO 8601 text, the form the table engine's date parser reads; an operand
///        built from it falls exactly on the second the engine floors the instant to.
/// @param at The instant.
/// @return For example `2026-11-10T09:00:00Z`.
[[nodiscard]] inline std::string isoSecond(morph::time::Timestamp const& at) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>((*at).value));
}

/// @brief A signed session for @p principal.
/// @param issuer The issuer.
/// @param principal The principal.
/// @return The context, with the token and the principal set.
[[nodiscard]] inline morph::session::Context signedAs(morph::session::TokenIssuer const& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief The server's authorizer, reading roles where production does.
/// @param secret The token secret.
/// @return The authorizer.
[[nodiscard]] inline std::shared_ptr<lims::auth::LimsAuthorizer> limsAuthorizer(std::string_view secret) {
    return std::make_shared<lims::auth::LimsAuthorizer>(
        std::string{secret}, [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
}

/// @brief The secret the test servers' authorizer and the service tokens share.
inline constexpr std::string_view kServiceSecret = "lims-service-test-secret-at-least-32-bytes";

/// @brief Runs @p action on a fresh @p Model as @p principal through an in-process bridge, as a client's call runs.
///
/// A handler that is a coroutine (a Task handler) is only built by a direct `execute`; a dispatch drives it, so a
/// suite sends such an action this way.
/// @tparam Model The model.
/// @tparam Action The action.
/// @param principal Who sends it.
/// @param action The action.
/// @param mode The backend mode the rig runs in.
/// @return Its result.
/// @throws std::exception the action's failure.
template <typename Model, typename Action>
auto dispatchAs(std::string const& principal, Action action,
                ::morph::ladder::testkit::Mode mode = ::morph::ladder::testkit::Mode::Local) {
    ::morph::ladder::testkit::BackendRig rig{mode, 1, limsAuthorizer(kServiceSecret)};
    ::morph::session::TokenIssuer const issuer{std::string{kServiceSecret}, ::morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(signedAs(issuer, principal));
    auto handler = rig.client<Model>(0);
    return ::morph::ladder::testkit::awaitQt(handler.execute(std::move(action)));
}

/// @brief Runs one SQL statement on a fresh connection to the fixture's database.
/// @param text The statement.
inline void runSql(std::string const& text) {
    Lightweight::SqlStatement statement;
    static_cast<void>(statement.ExecuteDirect(text));
}

/// @brief What a busy database says.
inline constexpr std::string_view kBusyMessage = "database is locked";

/// @brief Makes every statement of one kind on one table fail with a chosen message, until it goes out of scope.
///
/// The statement the model issues is the real one; a trigger refuses it, so the model's own handling of a database
/// failure runs. A message containing "database is locked" reads as a busy database (`isBusy`), anything else as a
/// failure that is not.
class FailingStatements {
public:
    /// @param table The table.
    /// @param event `INSERT`, `UPDATE` or `DELETE`.
    /// @param message What the database says.
    /// @param when An optional condition over `NEW` or `OLD`, as SQL; empty fails every statement.
    FailingStatements(std::string const& table, std::string const& event, std::string const& message,
                      std::string const& when = {})
        : _name{"lims_test_fail_" + table + "_" + event} {
        runSql("CREATE TRIGGER " + _name + " BEFORE " + event + " ON " + table +
               (when.empty() ? "" : " WHEN " + when) + " BEGIN SELECT RAISE(ABORT, '" + message + "'); END");
    }

    FailingStatements(FailingStatements const&) = delete;
    FailingStatements& operator=(FailingStatements const&) = delete;

    ~FailingStatements() {
        try {
            runSql("DROP TRIGGER IF EXISTS " + _name);
        } catch (...) {
            // The fixture's database is discarded with the test, and the trigger with it.
        }
    }

private:
    std::string _name;
};

/// @brief Makes every statement of one kind on one table change no row, until it goes out of scope.
///
/// A row-level `RAISE(IGNORE)` skips the row the statement targets, so an `UPDATE` that a model guards with "exactly
/// one row changed" sees zero, as it does when another writer changed the row between the model's read and its write.
class SkippedStatements {
public:
    /// @param table The table.
    /// @param event `UPDATE` or `DELETE`.
    /// @param when An optional condition over `NEW` or `OLD`, as SQL; empty skips every row.
    SkippedStatements(std::string const& table, std::string const& event, std::string const& when = {})
        : _name{"lims_test_skip_" + table + "_" + event} {
        runSql("CREATE TRIGGER " + _name + " BEFORE " + event + " ON " + table +
               (when.empty() ? "" : " WHEN " + when) + " BEGIN SELECT RAISE(IGNORE); END");
    }

    SkippedStatements(SkippedStatements const&) = delete;
    SkippedStatements& operator=(SkippedStatements const&) = delete;

    ~SkippedStatements() {
        try {
            runSql("DROP TRIGGER IF EXISTS " + _name);
        } catch (...) {
            // The fixture's database is discarded with the test, and the trigger with it.
        }
    }

private:
    std::string _name;
};

/// @brief A trigger the test installs by its own text, dropped when it goes out of scope.
///
/// For the arm that reads a row a moment after the model wrote one: an `AFTER` trigger that deletes the row it just
/// saw written stands in for another writer that removed it between the two statements.
class ScopedTrigger {
public:
    /// @param when The part after the trigger's name, for example `AFTER UPDATE ON billing_invoices WHEN NEW.state =
    /// 3`.
    /// @param body The statements to run, each ending in a semicolon.
    ScopedTrigger(std::string const& when, std::string const& body)
        : _name{"lims_test_trigger_" + std::to_string(++_made)} {
        runSql("CREATE TRIGGER " + _name + " " + when + " BEGIN " + body + " END");
    }

    ScopedTrigger(ScopedTrigger const&) = delete;
    ScopedTrigger& operator=(ScopedTrigger const&) = delete;

    ~ScopedTrigger() {
        try {
            runSql("DROP TRIGGER IF EXISTS " + _name);
        } catch (...) {
            // The fixture's database is discarded with the test, and the trigger with it.
        }
    }

private:
    static inline int _made = 0;
    std::string _name;
};

}  // namespace lims::test
