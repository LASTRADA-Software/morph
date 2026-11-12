// SPDX-License-Identifier: Apache-2.0
//
// Declared authorization (lims/org) and its first separation-of-duties pair, lab's capture and verify.
//
// `IAuthorizer::authorize` is handed the model and action ids and never the payload, so the server edge can check
// that a principal holds a role somewhere, and only the action's guard, which sees the decoded action, can check the
// scope and the pair. The "Hypothesis 6" cases below show a verifier whose role is at another site passing the edge
// and being refused by the guard, on every backend.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
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
#include "lims/lab/models/ignition_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/authorization/ledger_authorization.hpp"
#include "lims/org/authorization/declared.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"
#include "testkit/storage_owner.hpp"

using lims::test::ScopedPrincipal;
using morph::bridge::AllowShared;
using morph::bridge::BridgeHandler;
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

/// @brief A second company with its own site, and a principal who works only there.
struct Rival {
    lims::CompanyId company;
    lims::SiteId site;
};

/// @brief Creates the rival company as alice, who becomes its supervisor, and appoints `eve` an analyst at it.
/// @return The rival company and its site.
[[nodiscard]] Rival rivalWithEve() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Rival Labs", .baseCurrency = lims::CurrencyCode::EUR, .fiscalYearStart = lims::Month::January});
    auto const site = org.execute(lims::CreateSite{.companyId = company.id, .name = "Rival bench"});
    org.execute(lims::AssignRole{
        .principal = "eve", .role = lims::LimsRole::Analyst, .company = company.id, .site = lims::SiteId{}});
    return Rival{.company = company.id, .site = site.id};
}

/// @brief A sample at the seeded site, the client it belongs to and the analysis version a reading can name.
struct Bench {
    lims::ClientId client;
    lims::SampleId sample;
    lims::AnalysisVersionId analysisVersion;
};

/// @brief Registers a sample at the seeded site as alice, an analyst at the seeded company, and walks it to @p state.
/// @param state The state to leave it in: Registered, Received, InProgress or ToBeVerified.
/// @return The bench.
[[nodiscard]] Bench benchIn(lims::SampleState state) {
    lims::test::grant("alice", lims::LimsRole::Analyst);
    ScopedPrincipal const alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    auto const nitrate =
        catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample = bench.execute(
        lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId = client.clientId, .reference = "WW-1"});
    if (state != lims::SampleState::Registered) {
        bench.execute(lims::ReceiveSample{});
    }
    if (state == lims::SampleState::InProgress || state == lims::SampleState::ToBeVerified) {
        bench.execute(lims::StartWork{});
    }
    if (state == lims::SampleState::ToBeVerified) {
        bench.execute(lims::SubmitForVerification{});
    }
    return Bench{.client = client.clientId, .sample = sample.id, .analysisVersion = nitrate.versionId};
}

/// @brief A reading of the nitrate version.
/// @param bench The bench.
/// @return The capture.
[[nodiscard]] lims::CaptureConcentration reading(Bench const& bench) {
    return lims::CaptureConcentration{
        .analysisVersionId = bench.analysisVersion,
        .value = lims::Concentration{morph::math::Rational{morph::math::Numerator{12}, morph::math::Denominator{5},
                                                           morph::math::DecimalPlaces{3}}}};
}

/// @brief One lab write, as the principal currently in scope, on a handler attached to the bench's sample.
struct LabWrite {
    /// @brief The action's id.
    std::string_view name;
    /// @brief The state the sample is in when the write is attempted.
    lims::SampleState from;
    /// @brief Performs the write.
    std::function<void(lims::SampleModel&, Bench const&, std::string const&)> run;
};

/// @brief Flags an open conflict on the bench's sample, as the offline replay does.
/// @param bench The bench.
/// @return The conflict's id.
[[nodiscard]] lims::ConflictId flagConflict(Bench const& bench) {
    Lightweight::DataMapper mapper;
    lims::db::OfflineConflictRecord row;
    row.sample = static_cast<std::uint64_t>(*bench.sample);
    row.baseVersion = 1;
    row.serverVersion = 2;
    row.reason = static_cast<int>(lims::ConflictReason::StaleBase);
    row.status = static_cast<int>(lims::ConflictStatus::Open);
    row.payload = Lightweight::SqlDynamicAnsiString<4096>{std::string{"{}"}};
    row.detectedBy = Lightweight::SqlAnsiString<64>{std::string{"fiona"}};
    row.detectedAt = 1;
    row.resolvedBy = Lightweight::SqlAnsiString<64>{std::string{}};
    row.resolutionNote = Lightweight::SqlAnsiString<255>{std::string{}};
    mapper.Create(row);
    return lims::ConflictId{static_cast<std::int64_t>(row.id.Value())};
}

/// @brief The status of every conflict row.
/// @return The statuses, in id order.
[[nodiscard]] std::vector<int> conflictStatuses() {
    Lightweight::DataMapper mapper;
    std::vector<int> statuses;
    for (auto const& row : mapper.Query<lims::db::OfflineConflictRecord>().All()) {
        statuses.push_back(row.status.Value());
    }
    return statuses;
}

/// @brief Every write on a sample, one per action: the registration, each lifecycle edge, a capture and a replay.
/// @return The table.
[[nodiscard]] std::vector<LabWrite> labWrites() {
    using lims::SampleState;
    return {
        {"RegisterSample", SampleState::Registered,
         [](lims::SampleModel& model, Bench const& bench, std::string const&) {
             ::morph::model::executeChecked(
                 model, lims::RegisterSample{
                            .siteId = lims::test::seededSite(), .clientId = bench.client, .reference = "WW-2"});
         }},
        {"ReceiveSample", SampleState::Registered,
         [](lims::SampleModel& model, Bench const&, std::string const&) {
             ::morph::model::executeChecked(model, lims::ReceiveSample{});
         }},
        {"StartWork", SampleState::Received,
         [](lims::SampleModel& model, Bench const&, std::string const&) {
             ::morph::model::executeChecked(model, lims::StartWork{});
         }},
        {"SubmitForVerification", SampleState::InProgress,
         [](lims::SampleModel& model, Bench const&, std::string const&) {
             ::morph::model::executeChecked(model, lims::SubmitForVerification{});
         }},
        {"ReturnForRework", SampleState::ToBeVerified,
         [](lims::SampleModel& model, Bench const&, std::string const&) {
             ::morph::model::executeChecked(model, lims::ReturnForRework{.reason = "recheck"});
         }},
        {"PublishSample", SampleState::ToBeVerified,
         [](lims::SampleModel& model, Bench const&, std::string const&) {
             ::morph::model::executeChecked(model, lims::PublishSample{});
         }},
        {"RejectSample", SampleState::Registered,
         [](lims::SampleModel& model, Bench const&, std::string const&) {
             ::morph::model::executeChecked(model, lims::RejectSample{.reason = "leaked"});
         }},
        {"CaptureConcentration", SampleState::InProgress,
         [](lims::SampleModel& model, Bench const& bench, std::string const&) {
             ::morph::model::executeChecked(model, reading(bench));
         }},
        {"QueuedCapture", SampleState::InProgress,
         [](lims::SampleModel& model, Bench const& bench, std::string const& who) {
             model.execute(lims::QueuedCapture{.sampleId = bench.sample,
                                               .baseVersion = lims::SampleVersion{3},
                                               .capturedBy = who,
                                               .operationKey = lims::OperationKey{"op-" + who},
                                               .capture = reading(bench)});
         }},
        {"ResolveConflict", SampleState::Registered,
         [](lims::SampleModel& model, Bench const& bench, std::string const&) {
             ::morph::model::executeChecked(model,
                                            lims::ResolveConflict{.conflictId = flagConflict(bench),
                                                                  .resolution = lims::ConflictResolution::DiscardStale,
                                                                  .note = "duplicate"});
         }},
    };
}

/// @brief What the sample tables hold, to show a refused write changed nothing.
/// @return Each sample's id, state and version, and the number of results.
[[nodiscard]] std::vector<std::int64_t> sampleTables() {
    Lightweight::DataMapper mapper;
    std::vector<std::int64_t> snapshot;
    for (auto const& row : mapper.Query<lims::db::SampleRecord>().All()) {
        snapshot.push_back(static_cast<std::int64_t>(row.id.Value()));
        snapshot.push_back(row.state.Value());
        snapshot.push_back(row.version.Value());
    }
    snapshot.push_back(static_cast<std::int64_t>(mapper.Query<lims::db::ResultRecord>().All().size()));
    return snapshot;
}

/// @brief Attaches a fresh handler to the bench's sample, as the principal in scope, and runs @p write.
/// @param write The write.
/// @param bench The bench.
/// @param who The principal in scope.
void runWrite(LabWrite const& write, Bench const& bench, std::string const& who) {
    lims::SampleModel model;
    model.execute(lims::OpenSample{.sampleId = bench.sample});
    write.run(model, bench, who);
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
    STATIC_REQUIRE(lims::DeclaresAnyRole<lims::CaptureConcentration>);
    STATIC_REQUIRE_FALSE(lims::DeclaresRole<lims::CaptureConcentration>);
    STATIC_REQUIRE_FALSE(lims::DeclaresSeparation<lims::CaptureConcentration>);
    CHECK(Rule::role == lims::LimsRole::Verifier);
    CHECK(Rule::separatedFrom == "CaptureConcentration");
    CHECK(Rule::separatedFrom == morph::model::ActionTraits<lims::CaptureConcentration>::typeId());
}

// Mutation: name an action that is not registered in one of ApproveInvoice's or ApproveRequisition's `separations`.
TEST_CASE("declared authorization: every several-pairs separation names a registered action",
          "[lims][authorization]") {
    STATIC_REQUIRE(lims::DeclaresSeparations<lims::ApproveInvoice>);
    STATIC_REQUIRE_FALSE(lims::DeclaresSeparation<lims::ApproveInvoice>);
    STATIC_REQUIRE(lims::DeclaresSeparations<lims::ApproveRequisition>);
    STATIC_REQUIRE_FALSE(lims::DeclaresSeparation<lims::ApproveRequisition>);
    auto const& dispatcher = morph::model::detail::ActionDispatcher::instance();
    std::set<std::string> registered;
    for (std::string_view const model :
         {"OrgModel", "SampleModel", "LedgerModel", "PriceListModel", "InvoiceModel", "ProcurementModel"}) {
        auto const actions = dispatcher.actionsOf(model);
        registered.insert(actions.begin(), actions.end());
    }
    for (auto const& pair : lims::Authorization<lims::ApproveInvoice>::separations) {
        CAPTURE(pair.action);
        CHECK(registered.contains(std::string{pair.action}));
    }
    for (auto const& pair : lims::Authorization<lims::ApproveRequisition>::separations) {
        CAPTURE(pair.action);
        CHECK(registered.contains(std::string{pair.action}));
    }
}

static_assert(lims::DeclaresCheckedInHandler<lims::QueuedCapture>);
static_assert(!morph::model::DeclaresGuard<lims::QueuedCapture>);
static_assert(lims::DeclaresModelScope<lims::SaveSample> && morph::model::DeclaresGuard<lims::SaveSample>);

// Mutations: remove `using guard` from one declared action (it is neither guarded, open nor a declared read); register
// a read without `Loggable::No` (it matches no class); leave a declared action out of declaredRequirements() (the
// table and the guarded set disagree); remove one model from the `listed` set (its declared actions fail the
// cross-check). `LimsAuth` is deliberately not listed: its actions are sign-in, which the edge exempts and which
// declare no rule. If the cross-check names it, that is the reason; it is excepted by name, never by loosening the
// check.
TEST_CASE("declared authorization: every lims action is guarded, deliberately open, or a declared read",
          "[lims][authorization]") {
    std::set<std::string_view> const open{lims::app::openMutatingActions().begin(),
                                          lims::app::openMutatingActions().end()};
    std::set<std::string_view> const checkedInHandler{"QueuedCapture"};
    std::set<std::string_view> inTable;
    for (auto const& requirement : lims::app::declaredRequirements()) {
        inTable.insert(requirement.actionType);
    }
    std::set<std::string> registered;
    auto const& dispatcher = morph::model::detail::defaultDispatcher();
    for (std::string_view const model :
         {"OrgModel", "SampleModel", "AnalysisCatalogModel", "IgnitionModel", "LedgerModel", "LedgerReportModel",
          "PriceListModel", "InvoiceModel", "InvoiceEditModel", "BillingRunModel", "StockModel", "ProcurementModel"}) {
        for (auto const& actionId : dispatcher.actionsOf(model)) {
            CAPTURE(model, actionId);
            registered.insert(actionId);
            bool const guarded = morph::model::isGuarded(model, actionId);
            bool const isOpen = open.contains(actionId);
            bool const inHandler = checkedInHandler.contains(actionId);
            // A read is registered `Loggable::No` and declares no change: a query of a ui document must declare no
            // effect, since a declaration, `none` included, makes the action a mutation there. A writer is loggable
            // unless it says otherwise, so an undeclared writer matches no class here and fails.
            auto const effects = morph::effects::effectsOf(actionId);
            bool const declaredRead =
                !dispatcher.isLoggable(model, actionId) &&
                (!effects.has_value() || std::ranges::all_of(*effects, [](morph::effects::Effect const& effect) {
                    return effect.kind == morph::effects::Kind::None;
                }));
            CHECK(int{guarded} + int{isOpen} + int{inHandler} + int{declaredRead} == 1);
            CHECK(inTable.contains(actionId) == (guarded || inHandler));
        }
    }
    for (auto const actionId : open) {
        CAPTURE(actionId);
        CHECK(registered.contains(std::string{actionId}));
    }
    // The model list above is by hand, and the framework cannot enumerate models: a module added later (procurement)
    // would escape the loop. Every id the application declares must therefore be
    // registered under one of the listed models, so an unlisted model that declares a rule fails here.
    std::set<std::string_view> const listed{"OrgModel",        "SampleModel",  "AnalysisCatalogModel",
                                            "IgnitionModel",   "LedgerModel",  "LedgerReportModel",
                                            "PriceListModel",  "InvoiceModel", "InvoiceEditModel",
                                            "BillingRunModel", "StockModel",   "ProcurementModel"};
    auto const underListedModel = [&](std::string_view actionId) {
        auto const models = dispatcher.modelsOf(actionId);
        return std::ranges::any_of(models, [&](std::string const& model) { return listed.contains(model); });
    };
    for (auto const actionId : inTable) {
        CAPTURE(actionId);
        CHECK(underListedModel(actionId));
    }
    for (auto const actionId : open) {
        CAPTURE(actionId);
        CHECK(underListedModel(actionId));
    }
}

TEST_CASE("declared authorization: the edge's table lists every declared role, and nothing else",
          "[lims][authorization]") {
    CHECK(lims::app::requiredRole("VerifyResult") == lims::LimsRole::Verifier);
    CHECK(lims::app::requiredRole("CreateCompany") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("CreateSite") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("AssignRole") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("RevokeRole") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("SetValueLimit") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("CreateDraftEntry") == lims::LimsRole::Accountant);
    CHECK(lims::app::requiredRole("PostEntry") == lims::LimsRole::Accountant);
    CHECK(lims::app::requiredRole("ReverseEntry") == lims::LimsRole::Accountant);
    CHECK(lims::app::requiredRole("ClosePeriod") == lims::LimsRole::Accountant);
    CHECK(lims::app::requiredRole("SetExchangeRate") == lims::LimsRole::Accountant);
    CHECK(lims::app::requiredRole("SetBillingSettings") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("RegisterBillingClient") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("CreatePriceList") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("SetPrice") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("RecordBillableLine") == lims::LimsRole::Service);
    CHECK(lims::app::requiredRole("CreateInvoice") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("LeaseInvoice") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("SubmitInvoice") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("ApproveInvoice") == lims::LimsRole::InvoiceApprover);
    CHECK(lims::app::requiredRole("PostInvoice") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("RecordPosting") == lims::LimsRole::Service);
    CHECK(lims::app::requiredRole("StartBillingRun") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("AdvanceBillingRun") == lims::LimsRole::BillingClerk);
    CHECK(lims::app::requiredRole("RunBillingSchedule") == lims::LimsRole::Service);
    CHECK(lims::app::requiredRole("SetInventorySettings") == lims::LimsRole::Supervisor);
    CHECK(lims::app::requiredRole("CreateItem") == lims::LimsRole::StockKeeper);
    CHECK(lims::app::requiredRole("CreateLocation") == lims::LimsRole::StockKeeper);
    CHECK(lims::app::requiredRole("RecordOpeningLot") == lims::LimsRole::StockKeeper);
    CHECK(lims::app::requiredRole("ApproveRequisition") == lims::LimsRole::PurchaseApprover);
    // The raising, filling and submitting of a requisition declare some role at its site, whichever.
    for (std::string_view const actionId : {"CreateRequisition", "AddRequisitionLine", "SubmitRequisition"}) {
        CAPTURE(actionId);
        CHECK(lims::app::requirementFor(actionId).has_value());
        CHECK_FALSE(lims::app::requiredRole(actionId).has_value());
    }
    // Some role, whichever: declared, but no specific role.
    CHECK(lims::app::requirementFor("CaptureConcentration").has_value());
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
        CHECK_THROWS_AS(::morph::model::executeChecked(verifier, lims::VerifyResult{.resultId = lab.result}),
                        lims::Forbidden);
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
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        org, lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"}),
                    lims::Forbidden);
    // A new company has no scope of its own yet, so CreateCompany declares its role in any scope.
    CHECK_NOTHROW(org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April}));
    ScopedPrincipal const mallory{"mallory"};
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::CreateCompany{.name = "Mallory Ltd",
                                                                            .baseCurrency = lims::CurrencyCode::EUR,
                                                                            .fiscalYearStart = lims::Month::January}),
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
    CHECK_THROWS_WITH(::morph::model::executeChecked(verifier, lims::VerifyResult{.resultId = lab.result}),
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
        CHECK_THROWS_WITH(::morph::model::executeChecked(verifier, lims::VerifyResult{.resultId = lab.result}),
                          Catch::Matchers::ContainsSubstring("role where this applies"));
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
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::CreateCompany{.name = "Mallory Ltd"}), lims::Forbidden);
    CHECK_FALSE(lims::OrgModel::supervisorExists());
    {
        // Read from the table: mallory holds no role, so no list would show her the seeded company.
        Lightweight::DataMapper mapper;
        CHECK(mapper.Query<lims::db::CompanyRecord>().All().size() == 1);
    }
    // The bootstrap assignment is still the first administrative act, and a company can be created after it.
    CHECK_NOTHROW(::morph::model::executeChecked(org, lims::AssignRole{.principal = "mallory",
                                                                       .role = lims::LimsRole::Supervisor,
                                                                       .company = lims::test::seededCompany(),
                                                                       .site = {}}));
    CHECK_NOTHROW(::morph::model::executeChecked(org, lims::CreateCompany{.name = "Mallory Ltd"}));
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
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            org, lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"}),
                        lims::Forbidden);
        CHECK_NOTHROW(org.execute(lims::CreateSite{.companyId = other, .name = "Osaka lab"}));
    }
    ScopedPrincipal const mallory{"mallory"};
    lims::OrgModel org;
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::CreateSite{.companyId = other, .name = "Mallory lab"}),
                    lims::Forbidden);
    // Permission is asked before existence: a caller with no role learns nothing about which companies exist.
    CHECK_THROWS_AS(
        ::morph::model::executeChecked(org, lims::CreateSite{.companyId = lims::CompanyId{999}, .name = "Nowhere"}),
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
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "erin",
                                                                         .role = lims::LimsRole::Analyst,
                                                                         .company = lims::test::seededCompany(),
                                                                         .site = {}}),
                    lims::Forbidden);
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = dave.id}), lims::Forbidden);
}

// ── Lab writes are scoped to the sample's site ─────────────────────────────

// Mutations: drop the requirement from one write (eve, an analyst of another company, performs it and the tables
// change); check the role anywhere instead of at the site (eve's role at her own company's site satisfies it).
TEST_CASE("declared authorization: a principal of another company is refused every write on a sample",
          "[lims][authorization][lab]") {
    auto const writes = labWrites();
    auto const index = GENERATE_COPY(Catch::Generators::range(std::size_t{0}, writes.size()));
    auto const& write = writes[index];
    CAPTURE(write.name);
    DbFixture fixture;
    auto const rival = rivalWithEve();
    REQUIRE(rival.company != lims::test::seededCompany());
    auto const bench = benchIn(write.from);
    auto const before = sampleTables();

    ScopedPrincipal const eve{"eve"};
    // RegisterSample's payload names its site, so its refusal says which; the others resolve it from the sample.
    CHECK_THROWS_WITH(runWrite(write, bench, "eve"),
                      Catch::Matchers::ContainsSubstring("eve' does not hold any role where this applies") ||
                          Catch::Matchers::ContainsSubstring("eve' does not hold any role at site"));
    CHECK(sampleTables() == before);
}

// Mutation: check the role at the company only (ann, an analyst at the site, is refused); check the site only (cara,
// an analyst at the company, is refused).
TEST_CASE("declared authorization: a role at the sample's site or at its company allows every write on it",
          "[lims][authorization][lab]") {
    auto const writes = labWrites();
    auto const index = GENERATE_COPY(Catch::Generators::range(std::size_t{0}, writes.size()));
    auto const& write = writes[index];
    auto const holder = GENERATE(std::pair{"ann", false}, std::pair{"cara", true});
    CAPTURE(write.name, holder.first);
    DbFixture fixture;
    auto const bench = benchIn(write.from);
    lims::test::grant(
        holder.first, lims::LimsRole::Analyst,
        holder.second ? lims::Scope{lims::test::seededCompany()} : lims::Scope{lims::test::seededSite()});

    ScopedPrincipal const principal{holder.first};
    CHECK_NOTHROW(runWrite(write, bench, holder.first));
}

/// @brief Calls every lab write that names a site, sample or conflict by id with an id that names nothing, as the
///        principal in scope.
/// @tparam Exception What each guarded call must throw.
/// @tparam ReplayException What the replay, which judges its own rule, must throw.
/// @param bench The bench, for a client and a reading.
/// @param who The principal in scope.
template <typename Exception, typename ReplayException>
void checkGhostWrites(Bench const& bench, std::string const& who) {
    lims::SampleModel model;
    CHECK_THROWS_AS(
        ::morph::model::executeChecked(
            model, lims::RegisterSample{.siteId = lims::SiteId{999}, .clientId = bench.client, .reference = "WW-9"}),
        Exception);
    CHECK_THROWS_AS(model.execute(lims::QueuedCapture{.sampleId = lims::SampleId{9999},
                                                      .baseVersion = lims::SampleVersion{1},
                                                      .capturedBy = who,
                                                      .operationKey = lims::OperationKey{"op-ghost-" + who},
                                                      .capture = reading(bench)}),
                    ReplayException);
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        model, lims::ResolveConflict{.conflictId = lims::ConflictId{424242}, .note = "ghost"}),
                    Exception);
}

// The order of every scoped lab write: a caller with no role anywhere is Forbidden before any lookup, so learns
// nothing about which ids exist. A caller with a role somewhere is refused in words for an id that names nothing, the
// same refusal as for one at a site they hold nothing in, because the scope a missing row resolves to is none. The
// replay, which judges its own rule, refuses a missing sample the same way; only a handler attached to nothing, which
// names no id, answers NotFound.
// Mutation: look the row up before asking for the role (mallory learns that sample 9999 does not exist); let the
// replay answer NotFound for a missing sample (eve is told it does not exist).
TEST_CASE(
    "declared authorization: an id that names nothing and a scope the caller holds nothing in are both Forbidden",
    "[lims][authorization][lab]") {
    DbFixture fixture;
    auto const bench = benchIn(lims::SampleState::InProgress);
    static_cast<void>(rivalWithEve());
    {
        ScopedPrincipal const mallory{"mallory"};
        checkGhostWrites<lims::Forbidden, lims::Forbidden>(bench, "mallory");
        lims::SampleModel unattached;
        CHECK_THROWS_AS(::morph::model::executeChecked(unattached, lims::ReceiveSample{}), lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(unattached, reading(bench)), lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(unattached, lims::DeleteSample{}), lims::Forbidden);
        CHECK_THROWS_WITH(::morph::model::executeChecked(unattached, lims::DeleteSample{}),
                          Catch::Matchers::ContainsSubstring("does not hold any role") &&
                              !Catch::Matchers::ContainsSubstring("at no site"));
    }
    {
        ScopedPrincipal const eve{"eve"};
        checkGhostWrites<lims::Forbidden, lims::Forbidden>(bench, "eve");
        lims::SampleModel unattached;
        CHECK_THROWS_AS(::morph::model::executeChecked(unattached, lims::ReceiveSample{}), lims::NotFound);
        CHECK_THROWS_AS(::morph::model::executeChecked(unattached, reading(bench)), lims::NotFound);
        CHECK_THROWS_AS(::morph::model::executeChecked(unattached, lims::DeleteSample{}), lims::NotFound);
    }
}

// The replay runs as the operator who captured the reading, so the check is about that operator when the queue
// reaches the server: an operator who has since lost the role is refused a new operation, and still told Skipped for
// a redelivery of one that was already applied.
// Mutation: skip the check on QueuedCapture (fiona, whose role was revoked, replays); check the role before the
// idempotency key (fiona's redelivery is refused instead of Skipped).
TEST_CASE("declared authorization: a queued capture is judged on the operator's role at replay time",
          "[lims][authorization][lab]") {
    DbFixture fixture;
    auto const bench = benchIn(lims::SampleState::InProgress);
    auto const assignment = lims::test::grant("fiona", lims::LimsRole::Analyst);
    auto const capture = [&bench](std::string const& key) {
        return lims::QueuedCapture{.sampleId = bench.sample,
                                   .baseVersion = lims::SampleVersion{3},
                                   .capturedBy = "fiona",
                                   .operationKey = lims::OperationKey{key},
                                   .capture = reading(bench)};
    };
    {
        ScopedPrincipal const fiona{"fiona"};
        lims::SampleModel model;
        CHECK(model.execute(capture("op-early")).outcome == lims::ReplayOutcome::Applied);
    }
    {
        ScopedPrincipal const alice{"alice"};
        lims::OrgModel org;
        lims::test::grant("alice", lims::LimsRole::Supervisor);
        org.execute(lims::RevokeRole{.assignmentId = assignment.id});
    }
    ScopedPrincipal const fiona{"fiona"};
    lims::SampleModel model;
    CHECK(model.execute(capture("op-early")).outcome == lims::ReplayOutcome::Skipped);
    CHECK_THROWS_WITH(model.execute(capture("op-late")), Catch::Matchers::ContainsSubstring("does not hold any role"));
}

namespace {

/// @brief Fills @p model's working copy with a header at @p site, ready to save.
/// @param model The handler.
/// @param client The client the sample belongs to.
/// @param reference The reference.
/// @param site The site the header names.
void fillHeader(lims::SampleModel& model, lims::ClientId client, std::string reference, lims::SiteId site) {
    model.execute(lims::HeaderSection{
        .clientId = client, .reference = std::move(reference), .receivedOn = 0, .remarks = {}, .siteId = site});
    model.execute(lims::SelectProject{.projectId = 2});
}

/// @brief Saves a sample at the seeded site through sample entry, as alice.
/// @param client The client.
/// @return The sample's id.
[[nodiscard]] lims::SampleId savedEntry(lims::ClientId client) {
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    model.execute(lims::NewSample{});
    fillHeader(model, client, "WW-A", lims::test::seededSite());
    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    return saved.sampleId;
}

/// @brief What a sample's row holds, to show a refused entry changed nothing.
/// @param sample The sample.
/// @return Its reference and site, or an empty reference when the row is gone.
[[nodiscard]] std::pair<std::string, std::optional<std::int64_t>> sampleRow(lims::SampleId sample) {
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<lims::db::SampleRecord>()
                          .Where(::Lightweight::FieldNameOf<&lims::db::SampleRecord::id>, "=", *sample)
                          .All();
    if (rows.empty()) {
        return {std::string{}, std::nullopt};
    }
    return {std::string{rows.front().reference.Value().ToStringView()}, rows.front().siteId.Value()};
}

}  // namespace

// Sample entry writes a sample whose site the header names, so a role is needed at that site, and, when it updates a
// stored sample, at the site it is stored at: moving a sample needs a role at both ends.
// Mutations: drop the header-site check (eve creates a sample at the seeded site, and moves a rival sample into it);
// drop the stored-site check (eve rewrites alice's sample, and a move passes with the new site's role alone); drop
// DeleteSample's check (eve deletes alice's sample).
TEST_CASE("declared authorization: sample entry is refused for another company's sites and samples",
          "[lims][authorization][lab][entry]") {
    DbFixture fixture;
    auto const rival = rivalWithEve();
    auto const bench = benchIn(lims::SampleState::Registered);
    auto const alicesSample = savedEntry(bench.client);
    auto const countBefore = sampleTables().size();
    ScopedPrincipal const eve{"eve"};

    SECTION("creating a sample at a site of another company") {
        lims::SampleModel model;
        model.execute(lims::NewSample{});
        fillHeader(model, bench.client, "WW-EVE", lims::test::seededSite());
        CHECK_THROWS_WITH(::morph::model::executeChecked(model, lims::SaveSample{}),
                          Catch::Matchers::ContainsSubstring("eve' does not hold any role where this applies"));
        CHECK(sampleTables().size() == countBefore);
    }
    SECTION("rewriting a sample of another company") {
        lims::SampleModel model;
        auto const loaded = model.execute(lims::LoadSample{.id = alicesSample});
        REQUIRE(loaded.siteId == lims::test::seededSite());
        fillHeader(model, loaded.clientId, "WW-EVE", rival.site);
        CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::Forbidden);
        fillHeader(model, loaded.clientId, "WW-EVE", lims::test::seededSite());
        CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::Forbidden);
        CHECK(sampleRow(alicesSample) == std::pair{std::string{"WW-A"}, std::optional{*lims::test::seededSite()}});
    }
    SECTION("moving her own sample into another company's site") {
        lims::SampleModel model;
        model.execute(lims::NewSample{});
        fillHeader(model, bench.client, "WW-EVE", rival.site);
        auto const own = model.execute(lims::SaveSample{});
        REQUIRE(own.error == lims::SaveError::none);
        fillHeader(model, bench.client, "WW-EVE", lims::test::seededSite());
        CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::Forbidden);
        CHECK(sampleRow(own.sampleId) == std::pair{std::string{"WW-EVE"}, std::optional{*rival.site}});
    }
    SECTION("moving a sample of another company into her own site") {
        lims::SampleModel model;
        auto const loaded = model.execute(lims::LoadSample{.id = alicesSample});
        fillHeader(model, loaded.clientId, loaded.reference, rival.site);
        CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::Forbidden);
        CHECK(sampleRow(alicesSample).second == std::optional{*lims::test::seededSite()});
    }
    SECTION("deleting a sample of another company") {
        lims::SampleModel model;
        model.execute(lims::LoadSample{.id = alicesSample});
        CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::DeleteSample{}), lims::Forbidden);
        CHECK(sampleRow(alicesSample).first == "WW-A");
    }
}

// Mutation: check the site role only (cara, an analyst at the company, is refused); check the company only (ann, an
// analyst at the site, is refused).
TEST_CASE("declared authorization: a role at the site or at its company allows sample entry",
          "[lims][authorization][lab][entry]") {
    auto const holder = GENERATE(std::pair{"ann", false}, std::pair{"cara", true});
    CAPTURE(holder.first);
    DbFixture fixture;
    auto const bench = benchIn(lims::SampleState::Registered);
    auto const stored = savedEntry(bench.client);
    lims::test::grant(
        holder.first, lims::LimsRole::Analyst,
        holder.second ? lims::Scope{lims::test::seededCompany()} : lims::Scope{lims::test::seededSite()});
    ScopedPrincipal const principal{holder.first};

    lims::SampleModel creator;
    creator.execute(lims::NewSample{});
    fillHeader(creator, bench.client, "WW-NEW", lims::test::seededSite());
    CHECK(creator.execute(lims::SaveSample{}).error == lims::SaveError::none);

    lims::SampleModel editor;
    auto const loaded = editor.execute(lims::LoadSample{.id = stored});
    fillHeader(editor, loaded.clientId, "WW-EDITED", lims::test::seededSite());
    CHECK(editor.execute(lims::SaveSample{}).error == lims::SaveError::none);
    CHECK(sampleRow(stored).first == "WW-EDITED");

    CHECK(editor.execute(lims::DeleteSample{}).sampleId == stored);
    CHECK(sampleRow(stored).first.empty());
}

// Mutation: require a role at one site only, the stored one or the header's (gina, who holds a role at both, is
// refused the move).
TEST_CASE("declared authorization: a principal holding a role at both sites may move a sample between them",
          "[lims][authorization][lab][entry]") {
    DbFixture fixture;
    auto const bench = benchIn(lims::SampleState::Registered);
    auto const stored = savedEntry(bench.client);
    lims::SiteId second;
    {
        ScopedPrincipal const alice{"alice"};
        lims::test::grant("alice", lims::LimsRole::Supervisor);
        lims::OrgModel org;
        second = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Second bench"}).id;
    }
    lims::test::grant("gina", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::test::grant("gina", lims::LimsRole::Analyst, lims::Scope{second});
    ScopedPrincipal const gina{"gina"};

    lims::SampleModel model;
    auto const loaded = model.execute(lims::LoadSample{.id = stored});
    fillHeader(model, loaded.clientId, loaded.reference, second);
    CHECK(model.execute(lims::SaveSample{}).error == lims::SaveError::none);
    CHECK(sampleRow(stored) == std::pair{std::string{"WW-A"}, std::optional{*second}});
}

// Mutation: remove `SampleModel::onGuardRefused` (no refused step appears).
TEST_CASE("declared authorization: a refused rewrite or delete of a stored sample appears in its audit trail",
          "[lims][authorization][lab][entry][audit]") {
    DbFixture fixture;
    auto const rival = rivalWithEve();
    auto const bench = benchIn(lims::SampleState::Registered);
    auto const stored = savedEntry(bench.client);
    auto log = std::make_shared<morph::journal::InMemoryActionLog>(morph::ladder::testkit::storageOwner());
    auto const refusedSteps = [&](std::string_view action) {
        ScopedPrincipal const alice{"alice"};
        lims::SampleModel reader;
        reader.attachActionLog(log, std::string{});
        reader.execute(lims::LoadSample{.id = stored});
        std::vector<lims::AuditStep> found;
        for (auto const& step : reader.execute(lims::GetAuditTrail{}).steps) {
            if (step.action == action && step.outcome == lims::AuditOutcome::Refused) {
                found.push_back(step);
            }
        }
        return found;
    };

    ScopedPrincipal const eve{"eve"};
    lims::SampleModel model;
    model.attachActionLog(log, std::string{});
    auto const loaded = model.execute(lims::LoadSample{.id = stored});
    REQUIRE(refusedSteps("DeleteSample").empty());
    CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::DeleteSample{}), lims::Forbidden);
    fillHeader(model, loaded.clientId, "WW-EVE", rival.site);
    CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::Forbidden);

    auto const deletes = refusedSteps("DeleteSample");
    REQUIRE(deletes.size() == 1);
    CHECK(deletes.front().principal == "eve");
    CHECK_FALSE(deletes.front().detail.empty());
    auto const saves = refusedSteps("SaveSample");
    REQUIRE(saves.size() == 1);
    CHECK(saves.front().principal == "eve");
}

// The nine writes on a sample the handler holds judge the scope the model resolves from its sample or working copy,
// not a scope the payload names. A role held only at another site of the same company is no role here.
// Mutation: make `DeclaredRuleGuard`'s model form judge no scope (an empty loop): every one is allowed.
TEST_CASE("declared authorization: the nine model-scoped writes are refused to a role at another site",
          "[lims][authorization][lab][entry]") {
    std::set<std::string_view> const modelScoped{"ReceiveSample",       "StartWork",     "SubmitForVerification",
                                                 "ReturnForRework",     "PublishSample", "RejectSample",
                                                 "CaptureConcentration"};
    auto writes = labWrites();
    std::erase_if(writes, [&](LabWrite const& write) { return !modelScoped.contains(write.name); });
    REQUIRE(writes.size() == 7);
    // SaveSample and DeleteSample, the two entry writes, are the other two.
    auto const index = GENERATE_COPY(Catch::Generators::range(std::size_t{0}, writes.size() + 2));
    DbFixture fixture;
    auto const bench = benchIn(index < writes.size() ? writes[index].from : lims::SampleState::Registered);
    auto const stored = savedEntry(bench.client);
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::SiteId otherSite;
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel org;
        otherSite = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"}).id;
    }
    lims::test::grant("ned", lims::LimsRole::Analyst, lims::Scope{otherSite});
    auto const before = sampleTables();

    ScopedPrincipal const ned{"ned"};
    lims::SampleModel model;
    if (index < writes.size()) {
        CAPTURE(writes[index].name);
        CHECK_THROWS_WITH(runWrite(writes[index], bench, "ned"),
                          Catch::Matchers::ContainsSubstring("does not hold any role where this applies"));
    } else {
        auto const loaded = model.execute(lims::LoadSample{.id = stored});
        if (index == writes.size()) {
            fillHeader(model, loaded.clientId, loaded.reference, otherSite);
            CHECK_THROWS_WITH(::morph::model::executeChecked(model, lims::SaveSample{}),
                              Catch::Matchers::ContainsSubstring("does not hold any role where this applies"));
        } else {
            CHECK_THROWS_WITH(::morph::model::executeChecked(model, lims::DeleteSample{}),
                              Catch::Matchers::ContainsSubstring("does not hold any role where this applies"));
        }
    }
    CHECK(sampleTables() == before);
}

// An incomplete working copy is judged by the guard before the handler reports what it lacks: a caller who holds no
// role is refused, and one who holds a role is told in the result what is missing.
// Mutation: remove `requireRoleSomewhere<Action>()` from the model form of `DeclaredRuleGuard::check` (a new working
// copy names no site, so no scope is judged and mallory is told `noClient`).
TEST_CASE("declared authorization: a role-less caller is refused before an incomplete save is described",
          "[lims][authorization][lab][entry]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Analyst);
    {
        ScopedPrincipal const mallory{"mallory"};
        lims::SampleModel model;
        model.execute(lims::NewSample{});
        CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::Forbidden);
    }
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    model.execute(lims::NewSample{});
    CHECK(::morph::model::executeChecked(model, lims::SaveSample{}).error == lims::SaveError::noClient);
}

namespace {

/// @brief The words of the `Forbidden` @p call throws.
/// @tparam Call A callable.
/// @param call The call.
/// @return The exception's text; empty when it threw nothing.
template <typename Call>
[[nodiscard]] std::string forbiddenWords(Call&& call) {
    try {
        call();
    } catch (lims::Forbidden const& refused) {
        return refused.what();
    }
    return {};
}

}  // namespace

// A rule whose scope is read from a row names no scope in its refusal, so an id that names nothing and a record in a
// scope the caller holds nothing in are refused in identical words: the caller cannot learn which ids exist, or where.
// Mutation: make judgedFactsIn name the scope for every rule (bob is told "at site N" for the real result and "at an
// unassigned site" for the missing one).
TEST_CASE("declared authorization: a payload-scoped rule refuses a missing id and another scope's record identically",
          "[lims][authorization][lab][unknown-id]") {
    DbFixture fixture;
    auto const lab = twoSites();
    lims::test::grant("bob", lims::LimsRole::Verifier, lims::Scope{lab.fieldLab});
    ScopedPrincipal const bob{"bob"};
    lims::SampleModel model;
    auto const real =
        forbiddenWords([&] { ::morph::model::executeChecked(model, lims::VerifyResult{.resultId = lab.result}); });
    auto const missing = forbiddenWords(
        [&] { ::morph::model::executeChecked(model, lims::VerifyResult{.resultId = lims::ResultId{987654}}); });
    REQUIRE_FALSE(real.empty());
    CHECK(real == missing);
    CHECK_THAT(real, Catch::Matchers::ContainsSubstring("does not hold the verifier role where this applies"));
}

// The same for a rule whose scope the model resolves: a header naming a site that does not exist and one naming a
// site of another company are refused in identical words.
// Mutation: make judgedFactsIn name the scope for every rule (eve is told "at site 1" and "at site 999").
TEST_CASE("declared authorization: a model-scoped rule refuses a missing site and another scope's site identically",
          "[lims][authorization][lab][unknown-id]") {
    DbFixture fixture;
    auto const bench = benchIn(lims::SampleState::Registered);
    static_cast<void>(rivalWithEve());
    ScopedPrincipal const eve{"eve"};
    auto const saveAt = [&](lims::SiteId site) {
        lims::SampleModel model;
        model.execute(lims::NewSample{});
        fillHeader(model, bench.client, "WW-X", site);
        return forbiddenWords([&] { ::morph::model::executeChecked(model, lims::SaveSample{}); });
    };
    auto const real = saveAt(lims::test::seededSite());
    auto const missing = saveAt(lims::SiteId{999});
    REQUIRE_FALSE(real.empty());
    CHECK(real == missing);
    CHECK_THAT(real, Catch::Matchers::ContainsSubstring("does not hold any role where this applies"));
}

// Mutation: drop the requirement from RegisterSample, ReceiveSample or ResolveConflict (eve's call is recorded in that
// mode). The edge lets eve through, as she holds a role somewhere; only the model, which sees the site, refuses her.
TEST_CASE("declared authorization: the model refuses a write at another company's site on every backend",
          "[lims][authorization][lab][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    static_cast<void>(rivalWithEve());
    auto const bench = benchIn(lims::SampleState::Registered);
    auto const before = sampleTables();

    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1,
                   std::make_shared<lims::auth::LimsAuthorizer>(std::string{kSecret}, [](std::string_view principal) {
                       return lims::OrgModel::rolesAnywhere(principal);
                   })};
    rig.bridge(0).setDefaultSession(signedAs(issuer, "eve"));

    auto registrar = rig.client<lims::SampleModel>(0);
    CHECK_THROWS_WITH(awaitQt(registrar.execute(lims::RegisterSample{
                          .siteId = lims::test::seededSite(), .clientId = bench.client, .reference = "WW-2"})),
                      Catch::Matchers::ContainsSubstring("does not hold any role at site"));

    BridgeHandler<lims::SampleModel, AllowShared> handler{rig.bridge(0), rig.executor()};
    awaitQt(handler.execute(lims::OpenSample{.sampleId = bench.sample}));
    CHECK_THROWS_WITH(awaitQt(handler.execute(lims::ReceiveSample{})),
                      Catch::Matchers::ContainsSubstring("does not hold any role where this applies"));
    CHECK(sampleTables() == before);

    // A conflict on a sample of the other company's site is refused the same way, and stays open.
    auto const conflict = flagConflict(bench);
    BridgeHandler<lims::SampleModel, AllowShared> resolver{rig.bridge(0), rig.executor()};
    awaitQt(resolver.execute(lims::OpenSample{.sampleId = bench.sample}));
    CHECK_THROWS_WITH(
        awaitQt(resolver.execute(lims::ResolveConflict{
            .conflictId = conflict, .resolution = lims::ConflictResolution::DiscardStale, .note = "not mine"})),
        Catch::Matchers::ContainsSubstring("does not hold any role where this applies"));
    CHECK(conflictStatuses() == std::vector<int>{static_cast<int>(lims::ConflictStatus::Open)});
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

// Mutation: remove `using guard` from VerifyResult (bob's verification is recorded in every mode).
TEST_CASE("Hypothesis 6: on every backend, the guard refuses what the edge let through", "[lims][authorization][h6]") {
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
    // The refusal's own words prove the guard refused: any other failure of the call would not contain them.
    CHECK_THROWS_WITH(awaitQt(verifier.execute(lims::VerifyResult{.resultId = lab.result})),
                      Catch::Matchers::ContainsSubstring("does not hold the verifier role where this applies"));
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

// ── Watches ────────────────────────────────────────────────────────────────

namespace {

/// @brief Whether a watch on @p record by @p who was admitted: the reply is `ok` either way, and names the records it
///        refused.
/// @param connection The connection the watch is sent on.
/// @param record The record's wire name.
/// @param session Who watches.
/// @return `true` when the reply refuses nothing.
[[nodiscard]] bool watchAdmitted(morph::ladder::testkit::ServerConnection& connection, std::string const& record,
                                 morph::session::Context const& session) {
    auto const reply = connection.watch({record}, session);
    if (reply.kind != "ok") {
        return false;
    }
    morph::wire::WatchReply refused;
    if (glz::read_json(refused, reply.body)) {
        return false;
    }
    return refused.refusedRecords.empty();
}

}  // namespace

// A watch runs no action and so no guard: the edge's role answer is the read gate. Mutations: make the role branch of
// `LimsAuthorizer::authorize` admit (mallory is admitted to `Ledger_Entry`, which `PostEntry` holds); make `holdsAny`
// return true for a principal with no role (mallory is admitted to both records, through `GetEntry` and `GetInvoice`,
// which declare no rule). Dropping the role branch altogether changes nothing here: a declared action then falls to
// the same "holds some role" answer.
TEST_CASE("declared authorization: a principal with no role cannot watch the ledger or the invoices",
          "[lims][authorization][watch]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    lims::test::grant("carl", lims::LimsRole::InvoiceApprover);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const connection = rig.connect();
    for (std::string const record : {"Ledger_Entry", "Billing_Invoice"}) {
        CAPTURE(record);
        CHECK_FALSE(watchAdmitted(*connection, record, signedAs(issuer, "mallory")));
        CHECK(watchAdmitted(*connection, record, signedAs(issuer, "alice")));
        CHECK(watchAdmitted(*connection, record, signedAs(issuer, "carl")));
    }
}
