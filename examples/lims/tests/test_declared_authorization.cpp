// SPDX-License-Identifier: Apache-2.0
//
// Declared authorization (lims/org) and its first separation-of-duties pair, lab's capture and verify.
//
// This file measures the split README decision 25 records: `IAuthorizer::authorize` is handed the model and action
// ids and never the payload, so the server edge can check that a principal holds a role somewhere, and only the
// model, which has the payload, can check the scope and the pair. The "Hypothesis 6" cases below show a verifier whose
// role is at another site passing the edge and being refused by the model, on every backend.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "lims/app/declared_requirements.hpp"
#include "lims/app/lims_authorizer.hpp"
#include "lims/lab/authorization/lab_authorization.hpp"
#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/authorization/declared.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "lims-declared-authorization-test-secret-32b";

/// @brief Exact money-like amounts for the value-limit cases.
/// @param units Whole units.
/// @return The amount at two decimal places.
[[nodiscard]] morph::math::Rational amount(std::int64_t units) {
    return morph::math::Rational{units, morph::math::DecimalPlaces{2}};
}

/// @brief Facts for a principal holding @p held, asked for `Verifier`.
/// @param held The roles held in the scope.
/// @return The facts.
[[nodiscard]] lims::AuthorizationFacts verifierFacts(std::vector<lims::LimsRole> held) {
    return lims::AuthorizationFacts{.principal = "bob",
                                    .required = lims::LimsRole::Verifier,
                                    .held = std::move(held),
                                    .firstPerformer = std::nullopt,
                                    .amount = std::nullopt,
                                    .limit = std::nullopt};
}

/// @brief A signed session for @p principal.
/// @param issuer The issuer.
/// @param principal The principal.
/// @return The context, with the token and the principal set.
[[nodiscard]] morph::session::Context signedAs(morph::session::TokenIssuer const& issuer, std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief One sample at the seeded site with one reading captured by alice, awaiting verification, and a second
///        site in the same company.
struct TwoSites {
    lims::SiteId fieldLab;
    lims::SampleId sample;
    lims::ResultId result;
};

/// @brief Builds `TwoSites` through the models directly, as alice.
/// @return The ids.
[[nodiscard]] TwoSites twoSites() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    lims::AnalysisCatalogModel catalog;
    auto const nitrate =
        catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample = bench.execute(
        lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId = client.clientId, .reference = "WW-1"});
    bench.execute(lims::ReceiveSample{});
    bench.execute(lims::StartWork{});
    auto const result = bench.execute(lims::CaptureConcentration{
        .analysisVersionId = nitrate.versionId,
        .value = lims::Concentration{morph::math::Rational{morph::math::Numerator{12}, morph::math::Denominator{5},
                                                           morph::math::DecimalPlaces{3}}}});
    bench.execute(lims::SubmitForVerification{});
    return TwoSites{.fieldLab = fieldLab.id, .sample = sample.id, .result = result.id};
}

}  // namespace

// ── The verdict, with no database ──────────────────────────────────────────

// Mutation: treat any held role as enough (an analyst is allowed to verify).
TEST_CASE("declared authorization: the role is required in the scope", "[lims][authorization]") {
    CHECK(lims::authorizationVerdict(verifierFacts({})) == lims::Verdict::MissingRole);
    CHECK(lims::authorizationVerdict(verifierFacts({lims::LimsRole::Analyst})) == lims::Verdict::MissingRole);
    CHECK(lims::authorizationVerdict(verifierFacts({lims::LimsRole::Analyst, lims::LimsRole::Verifier})) ==
          lims::Verdict::Allowed);
}

// Mutation: compare the first performer case-insensitively or not at all (bob verifies his own capture).
TEST_CASE("declared authorization: one principal may not perform both halves of a pair", "[lims][authorization]") {
    auto facts = verifierFacts({lims::LimsRole::Verifier});
    facts.firstPerformer = "bob";
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::SameActor);
    facts.firstPerformer = "alice";
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::Allowed);
    // Without the role the pair is not even reached: the role is the first question.
    facts = verifierFacts({});
    facts.firstPerformer = "bob";
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::MissingRole);
}

// Mutations: allow an amount when no limit is set (NoLimit reads Allowed); compare with >= (the amount at the
// limit is refused).
TEST_CASE("declared authorization: a value limit caps the amount, and no limit allows nothing",
          "[lims][authorization]") {
    auto facts = verifierFacts({lims::LimsRole::Verifier});
    facts.amount = amount(500);
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::NoLimit);
    facts.limit = amount(500);
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::Allowed);
    facts.limit = amount(499);
    CHECK(lims::authorizationVerdict(facts) == lims::Verdict::OverLimit);
}

TEST_CASE("declared authorization: lab declares capture and verify as a pair", "[lims][authorization]") {
    using Rule = lims::Authorization<lims::VerifyResult>;
    STATIC_REQUIRE(lims::DeclaresScope<lims::VerifyResult>);
    STATIC_REQUIRE(lims::DeclaresSeparation<lims::VerifyResult>);
    STATIC_REQUIRE_FALSE(lims::DeclaresValueLimit<lims::VerifyResult>);
    STATIC_REQUIRE_FALSE(lims::DeclaresRule<lims::CaptureConcentration>);
    CHECK(Rule::role == lims::LimsRole::Verifier);
    CHECK(Rule::separatedFrom == "CaptureConcentration");
    CHECK(Rule::separatedFrom == morph::model::ActionTraits<lims::CaptureConcentration>::typeId());
}

// Mutations: leave a declared action out of declaredRequirements() (its edge check is lost); leave a mutating action
// out of both the table and openMutatingActions().
TEST_CASE("declared authorization: every mutating action is declared or deliberately open", "[lims][authorization]") {
    // Every mutating action of the org, sample and analysis-catalog models. The dispatcher check below fails when a
    // registered mutating action is missing from this list.
    using Mutating =
        std::tuple<lims::CreateCompany, lims::CreateSite, lims::AssignRole, lims::RevokeRole, lims::DefineAnalysis,
                   lims::ReviseAnalysis, lims::RegisterClient, lims::RegisterSample, lims::ReceiveSample,
                   lims::StartWork, lims::SubmitForVerification, lims::ReturnForRework, lims::PublishSample,
                   lims::RejectSample, lims::CaptureConcentration, lims::QueuedCapture, lims::ResolveConflict,
                   lims::VerifyResult, lims::NewSample, lims::LoadSample, lims::SaveSample, lims::DeleteSample,
                   lims::HeaderSection, lims::SamplingSection, lims::TurbiditySection, lims::NitrateSection,
                   lims::ClearOptionalMeasurement, lims::SelectProject>;
    std::set<std::string_view> listed;
    std::set<std::string_view> declared;
    std::apply(
        [&](auto... action) {
            (
                [&] {
                    using A = decltype(action);
                    auto const id = morph::model::ActionTraits<A>::typeId();
                    listed.insert(id);
                    if constexpr (lims::DeclaresRule<A>) {
                        declared.insert(id);
                    }
                }(),
                ...);
        },
        Mutating{});

    std::set<std::string_view> const open{lims::app::openMutatingActions().begin(),
                                          lims::app::openMutatingActions().end()};
    std::set<std::string_view> const inTable = [] {
        std::set<std::string_view> ids;
        for (auto const& requirement : lims::app::declaredRequirements()) {
            ids.insert(requirement.actionType);
        }
        return ids;
    }();

    CHECK(inTable == declared);
    for (auto const id : listed) {
        CAPTURE(id);
        CHECK((declared.contains(id) != open.contains(id)));
    }
    for (auto const id : open) {
        CAPTURE(id);
        CHECK(listed.contains(id));
    }

    // What this proves: every action of the three models that declares an effect other than None is in the list
    // above, and the list holds nothing else but the effect-less actions named here. It does not prove that an
    // action registered without effects is non-mutating; such an action is only caught by this list being written
    // out by hand, which is why RegisterClient, the one effect-less writer, is named.
    std::set<std::string_view> const effectless{"RegisterClient"};
    auto const& dispatcher = morph::model::detail::ActionDispatcher::instance();
    std::set<std::string> withEffects;
    for (std::string_view const model : {"OrgModel", "SampleModel", "AnalysisCatalogModel"}) {
        for (auto const& id : dispatcher.actionsOf(model)) {
            auto const effects = morph::effects::effectsOf(id);
            bool const mutates =
                effects.has_value() && std::ranges::any_of(*effects, [](morph::effects::Effect const& e) {
                    return e.kind != morph::effects::Kind::None;
                });
            if (mutates) {
                withEffects.emplace(id);
            }
        }
    }
    std::set<std::string> expected{listed.begin(), listed.end()};
    for (auto const id : effectless) {
        CAPTURE(id);
        CHECK(listed.contains(id));
        expected.erase(std::string{id});
    }
    CHECK(withEffects == expected);
}

TEST_CASE("declared authorization: the edge's table lists every declared role, and nothing else",
          "[lims][authorization]") {
    CHECK(lims::app::requiredRole("VerifyResult") == lims::LimsRole::Verifier);
    CHECK(lims::app::requiredRole("CreateCompany") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("CreateSite") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("AssignRole") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("RevokeRole") == lims::LimsRole::Supervisor);
    CHECK_FALSE(lims::app::requiredRole("CaptureConcentration").has_value());
    CHECK_FALSE(lims::app::requiredRole("GetSample").has_value());
}

// ── Through the models ─────────────────────────────────────────────────────

// Mutation: check the role anywhere instead of at the sample's site (bob, a verifier at the field lab, verifies).
TEST_CASE("declared authorization: verifying needs the verifier role at the sample's site", "[lims][authorization]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    lims::test::grant("carol", lims::LimsRole::Verifier);
    {
        ScopedPrincipal const bob{"bob"};
        lims::SampleModel verifier;
        CHECK_THROWS_AS(verifier.execute(lims::VerifyResult{.resultId = lab.result}), lims::Forbidden);
    }
    {
        ScopedPrincipal const carol{"carol"};
        lims::SampleModel verifier;
        CHECK(verifier.execute(lims::VerifyResult{.resultId = lab.result}).verifiedBy == "carol");
    }
}

// Mutations: let a site-scoped supervisor create sites in the company (bob creates one); require CreateCompany's
// role at the seeded company (bob, a supervisor only at a site, is refused).
TEST_CASE("declared authorization: a site needs a supervisor at its company, a company a supervisor anywhere",
          "[lims][authorization]") {
    DbFixture fixture;
    lims::test::grant("bob", lims::LimsRole::Supervisor, lims::Scope{lims::test::seededSite()});
    ScopedPrincipal const bob{"bob"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"}),
                    lims::Forbidden);
    // A new company has no scope of its own yet, so CreateCompany declares its role in any scope.
    CHECK_NOTHROW(org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April}));
    ScopedPrincipal const mallory{"mallory"};
    CHECK_THROWS_AS(
        org.execute(lims::CreateCompany{
            .name = "Mallory Ltd", .baseCurrency = lims::CurrencyCode::EUR, .fiscalYearStart = lims::Month::January}),
        lims::Forbidden);
}

// Mutation: drop the same-actor comparison from authorizationVerdict (alice verifies her own capture).
TEST_CASE("declared authorization: the capturing analyst cannot verify, whatever role or scope they hold",
          "[lims][authorization]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("alice", lims::LimsRole::Verifier);
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel verifier;
    CHECK_THROWS_WITH(verifier.execute(lims::VerifyResult{.resultId = lab.result}),
                      Catch::Matchers::ContainsSubstring("captured this result and cannot also verify it"));
}

// Mutation: treat a sample with no site as being at the seeded site (carol, a company verifier, may verify it).
TEST_CASE("declared authorization: a sample with no site cannot be verified by anyone", "[lims][authorization]") {
    DbFixture fixture;
    auto const lab = twoSites();
    {
        Lightweight::DataMapper mapper;
        auto rows = mapper.Query<lims::db::SampleRecord>().All();
        REQUIRE(rows.size() == 1);
        rows.front().siteId = std::nullopt;
        mapper.Update(rows.front());
    }
    lims::test::grant("carol", lims::LimsRole::Verifier);
    lims::test::grant("dave", lims::LimsRole::Verifier, lims::Scope{lims::test::seededSite()});
    for (std::string const who : {"carol", "dave"}) {
        CAPTURE(who);
        ScopedPrincipal const principal{who};
        lims::SampleModel verifier;
        CHECK_THROWS_WITH(verifier.execute(lims::VerifyResult{.resultId = lab.result}),
                          Catch::Matchers::ContainsSubstring("role at no site"));
    }
}

// Mutation: check the role at the sample's company only (dave, a verifier at the sample's own site, is refused).
TEST_CASE("declared authorization: a verifier at the sample's own site may verify it", "[lims][authorization]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("dave", lims::LimsRole::Verifier, lims::Scope{lims::test::seededSite()});
    ScopedPrincipal const dave{"dave"};
    lims::SampleModel verifier;
    CHECK(verifier.execute(lims::VerifyResult{.resultId = lab.result}).verifiedBy == "dave");
}

// Mutation: drop requireAuthorized from CreateCompany (mallory, who holds no role anywhere, creates a company).
TEST_CASE("declared authorization: while no supervisor exists anywhere, no company can be created",
          "[lims][authorization]") {
    DbFixture fixture;
    ScopedPrincipal const mallory{"mallory"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{.name = "Mallory Ltd"}), lims::Forbidden);
    CHECK_FALSE(lims::OrgModel::supervisorExists());
    {
        // Read from the table: mallory holds no role, so no list would show her the seeded company.
        Lightweight::DataMapper mapper;
        CHECK(mapper.Query<lims::db::CompanyRecord>().All().size() == 1);
    }
    // The bootstrap assignment is still the first administrative act, and a company can be created after it.
    CHECK_NOTHROW(org.execute(lims::AssignRole{.principal = "mallory",
                                               .role = lims::LimsRole::Supervisor,
                                               .company = lims::test::seededCompany(),
                                               .site = {}}));
    CHECK_NOTHROW(org.execute(lims::CreateCompany{.name = "Mallory Ltd"}));
}

// Mutation: let any supervisor create a site in any company (carol, a supervisor of the other company, creates one).
TEST_CASE("declared authorization: a site is created only by a supervisor of its company", "[lims][authorization]") {
    DbFixture fixture;
    // Carol supervises only the seeded site, so she may found a company and becomes its supervisor, but she does not
    // administer the seeded company.
    lims::test::grant("carol", lims::LimsRole::Supervisor, lims::Scope{lims::test::seededSite()});
    lims::CompanyId other;
    {
        ScopedPrincipal const carol{"carol"};
        lims::OrgModel org;
        other = org.execute(lims::CreateCompany{.name = "Osaka branch"}).id;
        CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"}),
                        lims::Forbidden);
        CHECK_NOTHROW(org.execute(lims::CreateSite{.companyId = other, .name = "Osaka lab"}));
    }
    ScopedPrincipal const mallory{"mallory"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = other, .name = "Mallory lab"}), lims::Forbidden);
    // Permission is asked before existence: a caller with no role learns nothing about which companies exist.
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::CompanyId{999}, .name = "Nowhere"}),
                    lims::Forbidden);
}

// Mutations: drop requireAuthorized from AssignRole (carol assigns at the seeded company) or from RevokeRole (carol
// revokes dave's assignment there).
TEST_CASE("declared authorization: a role is assigned and revoked only by a supervisor of its scope",
          "[lims][authorization]") {
    DbFixture fixture;
    lims::test::grant("carol", lims::LimsRole::Supervisor, lims::Scope{lims::test::seededSite()});
    auto const dave = lims::test::grant("dave", lims::LimsRole::Analyst);
    ScopedPrincipal const carol{"carol"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{.name = "Osaka branch"});
    // Carol supervises Osaka and the seeded site, and nothing else.
    CHECK_NOTHROW(org.execute(
        lims::AssignRole{.principal = "erin", .role = lims::LimsRole::Analyst, .company = osaka.id, .site = {}}));
    CHECK_NOTHROW(org.execute(lims::AssignRole{
        .principal = "erin", .role = lims::LimsRole::Analyst, .company = {}, .site = lims::test::seededSite()}));
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{
            .principal = "erin", .role = lims::LimsRole::Analyst, .company = lims::test::seededCompany(), .site = {}}),
        lims::Forbidden);
    CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = dave.id}), lims::Forbidden);
}

// ── Hypothesis 6 ───────────────────────────────────────────────────────────

// The edge decides on (principal, model, action) and is right to allow bob: he does hold the verifier role. Only
// the model, which can read the result's sample and its site, can tell that he holds it somewhere else.
TEST_CASE("Hypothesis 6: the server edge allows a verifier whose role is at another site",
          "[lims][authorization][h6]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    lims::auth::LimsAuthorizer const authorizer{
        std::string{kSecret}, [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); }};
    CHECK(authorizer.authorize(signedAs(issuer, "bob"), "SampleModel", "VerifyResult"));
    CHECK_FALSE(authorizer.authorize(signedAs(issuer, "mallory"), "SampleModel", "VerifyResult"));
}

// Mutation: drop requireAuthorized from VerifyResult (bob's verification is recorded in every mode).
TEST_CASE("Hypothesis 6: on every backend, the model refuses what the edge let through", "[lims][authorization][h6]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    lims::test::grant("carol", lims::LimsRole::Verifier);

    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1,
                   std::make_shared<lims::auth::LimsAuthorizer>(std::string{kSecret}, [](std::string_view principal) {
                       return lims::OrgModel::rolesAnywhere(principal);
                   })};
    rig.bridge(0).setDefaultSession(signedAs(issuer, "bob"));
    auto verifier = rig.client<lims::SampleModel>(0);
    // The refusal's own words prove the model refused: any other failure of the call would not contain them.
    CHECK_THROWS_WITH(awaitQt(verifier.execute(lims::VerifyResult{.resultId = lab.result})),
                      Catch::Matchers::ContainsSubstring("does not hold the verifier role at site"));
    {
        ScopedPrincipal const alice{"alice"};
        lims::SampleModel reader;
        reader.execute(lims::OpenSample{.sampleId = lab.sample});
        CHECK(reader.execute(lims::ListVerifications{}).verifications.empty());
    }

    // The control: the same call by a verifier at the sample's company goes through the same edge and the same model.
    rig.bridge(0).setDefaultSession(signedAs(issuer, "carol"));
    auto const recorded = awaitQt(verifier.execute(lims::VerifyResult{.resultId = lab.result}));
    CHECK(recorded.verifiedBy == "carol");
}
