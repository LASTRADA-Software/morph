// SPDX-License-Identifier: Apache-2.0
//
// The sample list pages in the database: the page a caller gets equals the page the table engine gives over the same
// visible rows, for every sort and filter the standard corpus derives and for the date queries it cannot.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/table/plan_conformance.hpp>
#include <morph/table/query.hpp>
#include <string>
#include <utility>
#include <vector>

#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief The sites and the samples every case here starts from.
struct Samples {
    lims::SiteId fieldLab;
    lims::SiteId osakaLab;
};

/// @brief alice supervises the seeded company and a second one; bob is an analyst at the field lab only. Samples are
///        written straight into the table so every cell can differ, be empty or hold text the collator folds: states
///        in every value, duplicate versions, references with accents, case, a non-Latin-1 capital and characters a
///        statement could misread, and instants within one second, across a second and before the epoch. One sample
///        has no site and one is at a site neither principal below sees through the seeded company.
/// @return The sites.
[[nodiscard]] Samples setUp() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    auto const osakaLab = org.execute(lims::CreateSite{.companyId = osaka.id, .name = "Osaka lab"});
    org.execute(
        lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = fieldLab.id});

    lims::SampleModel bench;
    auto const first = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const second = bench.execute(lims::RegisterClient{.name = "Brewery AG"});
    Lightweight::DataMapper mapper;
    auto const clients = mapper.Query<lims::db::ClientRecord>().All();

    struct Raw {
        std::string reference;
        int state;
        int version;
        std::int64_t registeredOffsetMs;
        int siteSlot;  // 0 seeded, 1 field lab, 2 osaka lab, 3 none
        int client;
    };
    std::vector<Raw> const raws{
        {"WW-1", 0, 1, 0, 0, 0},
        {"ww-1", 1, 1, 250, 1, 1},
        {"", 2, 2, 999, 0, 0},
        {"WW-2", 3, 2, 1000, 1, 1},
        {"\xC3\x89"
         "clair",
         4, 3, 1001, 0, 0},
        {"\xC3\xA9"
         "clair",
         5, 3, 86'400'000, 1, 1},
        {"\xCE\xA9"
         "mega",
         0, 4, 86'400'000, 2, 0},
        {"\xCF\x89"
         "mega",
         1, 4, -2'000'000'000'123, 0, 1},
        {"a?b", 2, 5, 172'800'000, 1, 0},
        {"{fn now()}", 3, 5, 172'800'500, 0, 1},
        {"O'Brien", 4, 1, 172'800'999, 1, 0},
        {"a_b", 5, 1, 259'200'000, 0, 1},
        {"a%b", 0, 2, 259'200'000, 1, 0},
        {"\xC3\x9C"
         "nder",
         1, 2, 345'600'000, 3, 1},
        {"\xC3\xBC"
         "nder",
         2, 3, 345'600'001, 0, 0},
        {"\xC3\x97"
         "x",
         3, 3, 432'000'000, 1, 1},
        {"Zeta", 4, 4, 432'000'000, 0, 0},
        {"zeta", 5, 4, 518'400'000, 1, 1},
        {"k19", 0, 5, 604'800'000, 2, 0},
        {"k20", 1, 5, 691'200'000, 0, 1},
        {"k21", 2, 1, 777'600'000, 1, 0},
        {"k22", 3, 1, 864'000'000, 0, 1},
        {"k23", 4, 2, 950'400'000, 1, 0},
        {"k24", 5, 2, 1'036'800'000, 0, 1},
    };
    std::int64_t const base = lims::millisOf(morph::time::Timestamp{lims::test::at(2026, 11, 10)});
    for (auto const& raw : raws) {
        lims::db::SampleRecord row;
        row.client = clients[static_cast<std::size_t>(raw.client)];
        row.reference = raw.reference;
        row.state = raw.state;
        row.version = raw.version;
        row.registeredAt = base + raw.registeredOffsetMs;
        if (raw.siteSlot == 0) {
            row.siteId = std::optional<std::int64_t>{*lims::test::seededSite()};
        } else if (raw.siteSlot == 1) {
            row.siteId = std::optional<std::int64_t>{*fieldLab.id};
        } else if (raw.siteSlot == 2) {
            row.siteId = std::optional<std::int64_t>{*osakaLab.id};
        }
        mapper.Create(row);
    }
    static_cast<void>(first);
    static_cast<void>(second);
    return Samples{.fieldLab = fieldLab.id, .osakaLab = osakaLab.id};
}

/// @brief The sample rows a principal can see, by id, as views the engine can page: read independently of the model's
///        own list, from the table and the visible sites.
/// @param sites The visible sites.
/// @return The views.
[[nodiscard]] std::shared_ptr<std::vector<lims::SampleView> const> visibleViews(
    std::vector<lims::SiteId> const& sites) {
    Lightweight::DataMapper mapper;
    auto views = std::make_shared<std::vector<lims::SampleView>>();
    for (auto const& row : mapper.Query<lims::db::SampleRecord>().All()) {
        auto const site = lims::SiteId::fromOptional(row.siteId.Value());
        if (std::ranges::find(sites, site) == sites.end()) {
            continue;
        }
        views->push_back(lims::SampleView{
            .id = lims::SampleId{static_cast<std::int64_t>(row.id.Value())},
            .siteId = site,
            .clientId = lims::ClientId{static_cast<std::int64_t>(row.client.Value())},
            .reference = std::string{row.reference.Value().ToStringView()},
            .state = static_cast<lims::SampleState>(row.state.Value()),
            .version = lims::SampleVersion{static_cast<std::int64_t>(row.version.Value())},
            .registeredAt = lims::timestampFromMillis(row.registeredAt.Value()),
        });
    }
    std::ranges::sort(*views, {}, [](lims::SampleView const& sample) { return *sample.id; });
    return views;
}

/// @brief Date-time queries the standard corpus cannot make: its operands are the cells' own text, and a date-time
///        cell's text is not a date the parser reads. The operands are the whole seconds of the rows' own instants, so
///        every equality, bound and tie falls on a boundary, and a few instants that match no row.
/// @param samples The visible samples.
/// @return The queries.
[[nodiscard]] std::vector<morph::table::TableQuery> dateQueries(std::vector<lims::SampleView> const& samples) {
    using morph::table::ColumnFilter;
    using morph::table::FilterEntry;
    using morph::table::SortDirection;
    using morph::table::SortKey;
    using morph::table::TableQuery;
    std::vector<TableQuery> queries;
    auto const filtered = [&](FilterEntry entry, bool asExclude) {
        TableQuery query;
        ColumnFilter filter;
        (asExclude ? filter.exclude : filter.include).push_back(std::move(entry));
        query.filters.columns.emplace("registeredAt", std::move(filter));
        query.sort = {SortKey{.column = "registeredAt", .dir = SortDirection::Descending}};
        queries.push_back(std::move(query));
    };
    std::vector<std::string> instants{"2026-11-10T09:00:00.500Z", "2026-11-11", "1963-01-01T00:00:00Z"};
    for (auto const& sample : samples) {
        auto const second = lims::test::isoSecond(sample.registeredAt);
        if (std::ranges::find(instants, second) == instants.end()) {
            instants.push_back(second);
        }
    }
    for (bool const asExclude : {false, true}) {
        for (std::string const& instant : instants) {
            filtered(FilterEntry{.eq = instant}, asExclude);
            filtered(FilterEntry{.ne = instant}, asExclude);
            filtered(FilterEntry{.lt = instant}, asExclude);
            filtered(FilterEntry{.le = instant}, asExclude);
            filtered(FilterEntry{.gt = instant}, asExclude);
            filtered(FilterEntry{.ge = instant}, asExclude);
            filtered(FilterEntry{.between = std::array<std::string, 2>{instant, "2026-11-12"}}, asExclude);
        }
    }
    return queries;
}

}  // namespace

// The page the database returns is the page the table engine returns over the same visible rows, as alice sees them
// (every site of two companies, and not the sample with no site) and as bob sees them (one site), with the total cap
// lowered so the above-cap rule runs on a small table.
// Mutations: drop the plan's filters from the statement; drop the site scope (bob gets every sample); drop the Latin-1
// fold; order a state by its ordinal.
TEST_CASE(
    "lab: a page of samples from the database equals the engine's page over the same rows, for every sort and filter "
    "the list offers",
    "[lims][lab][pages]") {
    DbFixture fixture;
    static_cast<void>(setUp());
    for (std::string const principal : {"alice", "bob"}) {
        CAPTURE(principal);
        ScopedPrincipal const caller{principal};
        auto const sites = lims::OrgModel::visibleSites(principal);
        auto const rows = visibleViews(sites);
        REQUIRE_FALSE(rows->empty());
        morph::table::RowsSource<lims::SampleView> const source{
            rows, [](lims::SampleView const& sample) { return morph::table::RowId{*sample.id}; }};
        auto corpus = morph::table::standardCorpus(source);
        CHECK(corpus.size() > 300U);
        for (auto&& query : dateQueries(*rows)) {
            corpus.push_back(std::move(query));
        }
        morph::table::ConformanceOptions options;
        options.limits.totalCap = 10;
        options.translatable = lims::SampleModel::canTranslateSampleColumn;
        auto const model = [&](morph::table::QueryPlan const& plan)
            -> std::expected<morph::table::ModelPage, morph::table::TableError> {
            auto const page = lims::SampleModel::pageSamples(sites, plan);
            morph::table::ModelPage out;
            for (auto const& row : page.rows) {
                out.rows.emplace_back(*row.id);
            }
            out.total = page.total;
            out.totalKind = page.totalKind;
            out.offset = page.offset;
            return out;
        };
        auto const failures = morph::table::conformanceFailures(source, model, corpus, options);
        for (std::size_t i = 0; i < failures.size() && i < 10; ++i) {
            WARN(failures[i]);
        }
        CHECK(failures.empty());
    }
}

// Mutation: let ListSamples ignore the plan's offset.
TEST_CASE("lab: the sample list answers through the action with an exact total", "[lims][lab][pages]") {
    DbFixture fixture;
    static_cast<void>(setUp());
    ScopedPrincipal const bob{"bob"};
    lims::SampleModel model;
    lims::ListSamples request;
    request.query.page = morph::table::PageRequest{.offset = 2, .limit = 3};
    auto const page = model.execute(request);
    auto const expected = visibleViews(lims::OrgModel::visibleSites("bob"));
    CHECK(page.total == static_cast<std::int64_t>(expected->size()));
    CHECK(page.totalKind == morph::table::TotalKind::Exact);
    REQUIRE(page.rows.size() == 3U);
    CHECK(*page.rows.front().id == *(*expected)[2].id);
    CHECK(page.offset == 2);
    request.query.sort = {morph::table::SortKey{.column = "nope", .dir = morph::table::SortDirection::Ascending}};
    CHECK_THROWS_AS(model.execute(request), lims::ValidationError);
}
