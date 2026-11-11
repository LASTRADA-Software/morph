// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 10 on the ledger: a list or a report over rows already in the database should not load them all. Two
// Stress cases measure what the journal's list and the trial balance cost today at 10^6 entries and 10^7 lines. The
// rows are written through Lightweight's DataMapper in batches, one transaction per 10,000; the posting path's cost
// is not what is measured. Each case prints its measurements to stderr as it goes, so a run that does not finish
// still says how far it got.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <morph/table/query.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/ledger/models/ledger_report_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::int64_t kEntriesForPage = 1'000'000;
constexpr std::int64_t kEntriesForTrial = 5'000'000;
constexpr std::int64_t kBatch = 10'000;
constexpr std::int64_t kMaxMinorUnits = 1'000'000'000;
constexpr std::int64_t kPageSize = 50;

/// @brief A field of /proc/self/status in megabytes.
/// @param key The field, such as `VmHWM:`.
/// @return Its value in MB; 0 if it cannot be read.
[[nodiscard]] double statusMegabytes(std::string_view key) {
    std::ifstream status{"/proc/self/status"};
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with(key)) {
            return std::stod(line.substr(key.size())) / 1024.0;
        }
    }
    return 0.0;
}

/// @brief Milliseconds since @p start.
/// @param start The start.
/// @return The elapsed milliseconds.
[[nodiscard]] double millisSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// @brief Prints a measurement line to stderr, unbuffered.
/// @param what What was measured.
/// @param millis How long it took.
void report(std::string const& what, double millis) {
    std::fprintf(stderr, "[scale] %s: %.0f ms, VmRSS %.0f MB, VmHWM %.0f MB\n", what.c_str(), millis,
                 statusMegabytes("VmRSS:"), statusMegabytes("VmHWM:"));
    std::fflush(stderr);
}

/// @brief What the entries need to exist: two accounts and November 2026's period.
struct Books {
    std::int64_t asset = 0;
    std::int64_t revenue = 0;
    std::int64_t period = 0;
};

/// @brief What the independent accumulation of a written table holds.
struct Written {
    std::vector<std::int64_t> entryIds;  ///< Every entry's id, in write order.
    std::int64_t debit = 0;              ///< The sum of the debit lines' minor units.
    std::int64_t credit = 0;             ///< The sum of the credit lines' minor units.
    std::int64_t lines = 0;              ///< The lines written.
};

/// @brief Writes @p entries posted two-line entries for the seeded company, debiting the asset account and crediting
///        the revenue account by the same amount of up to 10^9 minor units.
/// @param books The accounts and the period.
/// @param entries How many entries.
/// @param keepIds Whether to record each entry's id.
/// @return What was written, summed independently of the ledger's own code.
[[nodiscard]] Written writeEntries(Books const& books, std::int64_t entries, bool keepIds) {
    Written written;
    if (keepIds) {
        written.entryIds.reserve(static_cast<std::size_t>(entries));
    }
    std::mt19937_64 random{42};
    std::uniform_int_distribution<std::int64_t> amounts{1, kMaxMinorUnits};
    std::int64_t const dateMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::sys_days{std::chrono::year{2026} / 11 / 10} + std::chrono::hours{9}).time_since_epoch())
            .count();
    Lightweight::DataMapper mapper;
    auto const start = Clock::now();
    for (std::int64_t first = 0; first < entries; first += kBatch) {
        Lightweight::SqlTransaction transaction{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        for (std::int64_t index = first; index < std::min(first + kBatch, entries); ++index) {
            lims::db::EntryRecord entry;
            entry.companyId = lims::db::kSeededCompanyId;
            entry.period = books.period;
            entry.dateMs = dateMs;
            entry.state = static_cast<int>(lims::EntryState::Posted);
            entry.sourceKey = "scale:" + std::to_string(index);
            entry.sourceReference = "scale " + std::to_string(index);
            entry.memo = "a posted two-line entry";
            entry.currency = static_cast<int>(lims::CurrencyCode::EUR);
            entry.createdBy = "scale";
            entry.postedBy = "scale";
            mapper.Create(entry);
            auto const entryId = static_cast<std::int64_t>(entry.id.Value());
            std::int64_t const amount = amounts(random);
            for (int position = 0; position < 2; ++position) {
                lims::db::EntryLineRecord line;
                line.entry = entryId;
                line.position = position;
                line.account = position == 0 ? books.asset : books.revenue;
                line.side = static_cast<int>(position == 0 ? lims::PostingSide::Debit : lims::PostingSide::Credit);
                line.amountNum = amount;
                line.amountDen = 1;
                line.companyNum = amount;
                line.companyDen = 1;
                mapper.Create(line);
            }
            written.debit += amount;
            written.credit += amount;
            written.lines += 2;
            if (keepIds) {
                written.entryIds.push_back(entryId);
            }
        }
        transaction.Commit();
    }
    report("wrote " + std::to_string(written.lines) + " lines", millisSince(start));
    return written;
}

/// @brief Creates the two accounts the entries use and opens November 2026, through the ledger's own actions.
/// @return Their ids.
[[nodiscard]] Books createBooks() {
    lims::LedgerModel ledger;
    for (auto const& [code, type] :
         {std::pair{"1200", lims::AccountType::Asset}, std::pair{"4000", lims::AccountType::Revenue}}) {
        ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                           .code = code,
                                           .name = std::string{"Account "} + code,
                                           .type = type});
    }
    auto const period = ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                        .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
    Lightweight::DataMapper mapper;
    Books books{.period = *period.id};
    for (auto const& account : mapper.Query<lims::db::AccountRecord>().All()) {
        (account.code.Value().ToStringView() == "1200" ? books.asset : books.revenue) =
            static_cast<std::int64_t>(account.id.Value());
    }
    return books;
}

}  // namespace

// Named, not anonymous: glaze reflects the row's member names through an extern object, which an anonymous type
// cannot have.
namespace lims::scale_test {

/// @brief The columns of the journal's list the page is drawn from.
struct JournalRow {
    std::int64_t id = 0;
    std::string sourceKey;
    std::string sourceReference;
    std::string memo;
};

}  // namespace lims::scale_test

using lims::scale_test::JournalRow;

TEST_CASE("Stress: Hypothesis 10: a page of the journal over 10^6 entries", "[lims][ledger][scale][stress]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    auto const books = createBooks();
    auto const written = writeEntries(books, kEntriesForPage, true);
    REQUIRE(written.entryIds.size() == static_cast<std::size_t>(kEntriesForPage));

    double const rssBefore = statusMegabytes("VmRSS:");
    double const peakBefore = statusMegabytes("VmHWM:");
    auto start = Clock::now();
    lims::LedgerModel ledger;
    auto listed = ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()});
    double const listMillis = millisSince(start);
    report("ListEntries over " + std::to_string(kEntriesForPage) + " entries", listMillis);
    std::fprintf(stderr, "[scale] ListEntries: RSS %.0f MB -> %.0f MB, peak %.0f MB -> %.0f MB\n", rssBefore,
                 statusMegabytes("VmRSS:"), peakBefore, statusMegabytes("VmHWM:"));
    REQUIRE(listed.entries.size() == static_cast<std::size_t>(kEntriesForPage));

    // The page: the rows are projected from what ListEntries returned, then paged by the engine.
    start = Clock::now();
    std::vector<JournalRow> rows;
    rows.reserve(listed.entries.size());
    for (auto const& entry : listed.entries) {
        rows.push_back(JournalRow{.id = *entry.id,
                                  .sourceKey = entry.sourceKey,
                                  .sourceReference = entry.sourceReference,
                                  .memo = entry.memo});
    }
    morph::table::TableQuery query;
    query.page.offset = 500'000;
    query.page.limit = kPageSize;
    auto page = morph::table::apply<JournalRow>(
        rows, [](JournalRow const& row) { return morph::table::RowId{row.id}; }, query);
    double const pageMillis = millisSince(start);
    report("projection plus a page of " + std::to_string(kPageSize) + " through table::apply", pageMillis);
    REQUIRE(page.has_value());
    REQUIRE(page->rows.size() == static_cast<std::size_t>(kPageSize));

    // The control: the page's first and last rows against an independently sorted id list.
    auto ids = written.entryIds;
    std::ranges::sort(ids);
    CHECK(page->rows.front().id == ids[500'000]);
    CHECK(page->rows.back().id == ids[500'000 + kPageSize - 1]);
    CHECK(page->offset == 500'000);
}

TEST_CASE("Stress: Hypothesis 10: a trial balance over 10^7 posted lines", "[lims][ledger][scale][stress]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    auto const books = createBooks();
    auto const written = writeEntries(books, kEntriesForTrial, false);
    REQUIRE(written.lines == 2 * kEntriesForTrial);

    double const rssBefore = statusMegabytes("VmRSS:");
    double const peakBefore = statusMegabytes("VmHWM:");
    auto const start = Clock::now();
    auto const view = lims::LedgerReportModel{}.execute(lims::TrialBalance{
        .company = lims::test::seededCompany(), .asOf = morph::time::Timestamp{lims::test::at(2026, 11, 30)}});
    double const millis = millisSince(start);
    report("TrialBalance over " + std::to_string(written.lines) + " lines", millis);
    std::fprintf(stderr, "[scale] TrialBalance: RSS %.0f MB -> %.0f MB, peak %.0f MB -> %.0f MB\n", rssBefore,
                 statusMegabytes("VmRSS:"), peakBefore, statusMegabytes("VmHWM:"));

    // The control: the totals equal the independently accumulated sums exactly, so a dropped or doubled line fails.
    REQUIRE(view.rows.size() == 2);
    CHECK((*view.totalDebit).numerator == written.debit);
    CHECK((*view.totalDebit).denominator == 1);
    CHECK((*view.totalCredit).numerator == written.credit);
    CHECK((*view.totalCredit).denominator == 1);
}
