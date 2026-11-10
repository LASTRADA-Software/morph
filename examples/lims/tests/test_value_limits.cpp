// SPDX-License-Identifier: Apache-2.0
//
// Value limits: up to what amount a principal may approve at a company, in its base currency.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

/// @brief @p units whole units at two decimals.
/// @param units The units.
/// @return The amount.
[[nodiscard]] lims::Amount euros(std::int64_t units) { return lims::Amount{Rational{units, DecimalPlaces{2}}}; }

}  // namespace

TEST_CASE("value limits: the business roles are named and stored after the first three", "[lims][limits]") {
    CHECK(lims::roleName(lims::LimsRole::Accountant) == "accountant");
    CHECK(lims::roleName(lims::LimsRole::BillingClerk) == "billing-clerk");
    CHECK(lims::roleName(lims::LimsRole::InvoiceApprover) == "invoice-approver");
    CHECK(lims::roleName(lims::LimsRole::Service) == "service");
    CHECK(static_cast<int>(lims::LimsRole::Supervisor) == 2);
    CHECK(static_cast<int>(lims::LimsRole::InvoiceApprover) == 5);
}

// Mutation: let valueLimit ignore the company (bob's limit at the seeded company answers for Osaka too).
TEST_CASE("value limits: a supervisor sets a limit per principal and company, and setting it again replaces it",
          "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});

    CHECK_FALSE(lims::OrgModel::valueLimit("bob", lims::test::seededCompany()).has_value());
    auto const first = org.execute(
        lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(500)});
    CHECK(first.setBy == "alice");
    org.execute(lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(800)});
    CHECK(lims::OrgModel::valueLimit("bob", lims::test::seededCompany()) == Rational{800, DecimalPlaces{2}});
    CHECK_FALSE(lims::OrgModel::valueLimit("bob", osaka.id).has_value());
    CHECK(org.execute(lims::ListValueLimits{}).limits.size() == 1U);
}

TEST_CASE("value limits: a limit must fit the company's currency and not be negative", "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{
                        .principal = "bob",
                        .company = osaka.id,
                        .limit = lims::Amount{Rational{morph::math::Numerator{25}, morph::math::Denominator{2},
                                                       DecimalPlaces{1}}}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{.principal = "bob", .company = osaka.id, .limit = euros(-1)}),
                    lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{.principal = "bob", .company = osaka.id, .limit = {}}),
                    lims::ValidationError);
}

TEST_CASE("value limits: only a supervisor of the company sets one", "[lims][limits][authorization]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::test::grant("carol", lims::LimsRole::InvoiceApprover);
    ScopedPrincipal const carol{"carol"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{
                        .principal = "carol", .company = lims::test::seededCompany(), .limit = euros(1)}),
                    lims::Forbidden);
}

TEST_CASE("value limits: findCompany answers a company's currency and fiscal-year start", "[lims][limits]") {
    DbFixture fixture;
    auto const seeded = lims::OrgModel::findCompany(lims::test::seededCompany());
    REQUIRE(seeded.has_value());
    CHECK(seeded->baseCurrency == lims::CurrencyCode::EUR);
    CHECK_FALSE(lims::OrgModel::findCompany(lims::CompanyId{999}).has_value());
    CHECK_FALSE(lims::OrgModel::findCompany(lims::CompanyId{}).has_value());
}

// Mutation: let ListValueLimits skip the visibility filter (carol, a supervisor of Osaka only, sees bob's limit at the
// seeded company).
TEST_CASE("value limits: a list shows only the limits at companies the caller can see", "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::OrgModel org;
    lims::CompanyView osaka;
    {
        ScopedPrincipal const alice{"alice"};
        org.execute(
            lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(500)});
        osaka = org.execute(lims::CreateCompany{
            .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
        org.execute(lims::AssignRole{
            .principal = "carol", .role = lims::LimsRole::Supervisor, .company = osaka.id, .site = {}});
    }
    ScopedPrincipal const carol{"carol"};
    CHECK(org.execute(lims::ListValueLimits{}).limits.empty());
    auto const yen = org.execute(lims::SetValueLimit{
        .principal = "bob", .company = osaka.id, .limit = lims::Amount{Rational{50000, DecimalPlaces{0}}}});
    auto const listed = org.execute(lims::ListValueLimits{});
    REQUIRE(listed.limits.size() == 1U);
    CHECK(listed.limits.front().id == yen.id);
    CHECK((*listed.limits.front().limit).getDecimalPlaces() == DecimalPlaces{0});
    CHECK(lims::OrgModel::valueLimit("bob", osaka.id) == Rational{50000, DecimalPlaces{0}});
    CHECK_THROWS_AS(org.execute(lims::SetValueLimit{
                        .principal = "bob", .company = lims::test::seededCompany(), .limit = euros(1)}),
                    lims::Forbidden);
}

// Mutation: drop `notOnSelf` from Authorization<SetValueLimit> (alice sets her own limit).
TEST_CASE("value limits: a supervisor does not set their own limit, another supervisor does", "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::test::grant("dave", lims::LimsRole::Supervisor);
    lims::OrgModel org;
    {
        ScopedPrincipal const alice{"alice"};
        CHECK_THROWS_AS(org.execute(lims::SetValueLimit{
                            .principal = "alice", .company = lims::test::seededCompany(), .limit = euros(1'000'000)}),
                        lims::Forbidden);
        CHECK_FALSE(lims::OrgModel::valueLimit("alice", lims::test::seededCompany()).has_value());
    }
    ScopedPrincipal const dave{"dave"};
    auto const set = org.execute(
        lims::SetValueLimit{.principal = "alice", .company = lims::test::seededCompany(), .limit = euros(2000)});
    CHECK(set.setBy == "dave");
    CHECK(lims::OrgModel::valueLimit("alice", lims::test::seededCompany()) == Rational{2000, DecimalPlaces{2}});
}

// Mutation: drop the unique index (the table then accepts a second row for the pair); make SetValueLimit always
// Create (the second set leaves two rows).
TEST_CASE("value limits: a pair holds one row, which setting twice updates", "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const first = org.execute(
        lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(500)});
    auto const second = org.execute(
        lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(900)});
    CHECK(second.id == first.id);
    auto const listed = org.execute(lims::ListValueLimits{});
    REQUIRE(listed.limits.size() == 1U);
    CHECK(*listed.limits.front().limit == Rational{900, DecimalPlaces{2}});

    // The index, not the model, is what makes a second row impossible.
    Lightweight::DataMapper mapper;
    lims::db::ValueLimitRecord duplicate;
    duplicate.principal = Lightweight::SqlAnsiString<64>{"bob"};
    duplicate.company = static_cast<std::uint64_t>(*lims::test::seededCompany());
    duplicate.setBy = Lightweight::SqlAnsiString<64>{"alice"};
    CHECK_THROWS(mapper.Create(duplicate));
}

// Mutation: let ListValueLimits answer every role holder (carol, an approver, sees bob's limit).
TEST_CASE("value limits: a limit is seen by the company's supervisors and the principal it belongs to",
          "[lims][limits]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::test::grant("bob", lims::LimsRole::InvoiceApprover);
    lims::test::grant("carol", lims::LimsRole::InvoiceApprover);
    lims::OrgModel org;
    {
        ScopedPrincipal const alice{"alice"};
        org.execute(
            lims::SetValueLimit{.principal = "bob", .company = lims::test::seededCompany(), .limit = euros(500)});
        org.execute(
            lims::SetValueLimit{.principal = "carol", .company = lims::test::seededCompany(), .limit = euros(700)});
        CHECK(org.execute(lims::ListValueLimits{}).limits.size() == 2U);
    }
    {
        ScopedPrincipal const bob{"bob"};
        auto const own = org.execute(lims::ListValueLimits{});
        REQUIRE(own.limits.size() == 1U);
        CHECK(own.limits.front().principal == "bob");
    }
    ScopedPrincipal const nobody{"nobody"};
    CHECK(org.execute(lims::ListValueLimits{}).limits.empty());
}
