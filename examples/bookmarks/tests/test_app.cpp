// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/app/app.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/outbox_entity.hpp"
#include "bookmarks/models/auth_model.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/session/session.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::pumpUntil;

namespace {

/// @brief A `Context` carrying only @p principal.
///
/// Built field-by-field rather than with a designated initializer on
/// purpose: `-Weverything` includes
/// `-Wmissing-designated-field-initializers`, which fires on a partial
/// designated-initializer list, and `ladder_<rung>_tests` is built with
/// `apply_warnings()` (so `-Werror` under `MORPH_ENABLE_STRICT_COMPILATION`,
/// CI's default). Same reason `makeCreate` below exists.
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief A `CreateBookmark` for @p url, optionally pre-titled. See
///        `contextFor` for why this is not a designated initializer.
[[nodiscard]] bookmarks::CreateBookmark makeCreate(std::string url, std::string title = {}) {
    bookmarks::CreateBookmark action;
    action.url = std::move(url);
    action.title = std::move(title);
    return action;
}

/// @brief Deterministic stand-in for a real fetcher: derives the "fetched"
///        title from the url, so a test can assert the exact value that came
///        back through the whole dispatch path.
class StubFetcher : public bookmarks::app::IBookmarkMetadataFetcher {
  public:
    bookmarks::app::FetchedMetadata fetch(const std::string& url) override {
        return {.title = "Fetched: " + url, .faviconPath = ""};
    }
};

/// @brief A fresh, empty action-log path per test.
///
/// `FileActionLog` appends and rebuilds its idempotency-dedup set from
/// whatever is already on disk, so a leftover file from an earlier test would
/// silently suppress a re-relayed row. Deleted before use and after, matching
/// `examples/pastebin/tests/test_paste_model.cpp`'s own App-test convention.
[[nodiscard]] std::filesystem::path freshLogPath(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("bookmarks_" + name + ".jsonl");
    std::filesystem::remove(path);
    return path;
}

constexpr std::chrono::hours kTimersOff{1};

/// @brief A fetch interval short enough that a handful of pumped event-loop
///        slices are certain to contain several ticks of it.
///
/// Only the two `stopBackgroundJobs()` cases use it; every other case keeps
/// `kTimersOff` and drives passes by hand. The pair is deliberately
/// asymmetric: the *control* case waits for a tick to arrive (bounded by
/// `pumpUntil`'s own generous, `MORPH_LADDER_DEADLINE_MS`-scaled deadline, so
/// a slow runner cannot fail it), while the case under test waits for one that
/// must never arrive — the only place a fixed budget appears, and a
/// deliberately long one.
constexpr std::chrono::milliseconds kFastFetchInterval{20};

/// @brief Records every url it was asked about, so a test can assert a pass
///        ran — or, more to the point below, that none did.
class RecordingFetcher : public bookmarks::app::IBookmarkMetadataFetcher {
  public:
    bookmarks::app::FetchedMetadata fetch(const std::string& url) override {
        calls.push_back(url);
        return {.title = "Recorded", .faviconPath = ""};
    }
    std::vector<std::string> calls;
};

}  // namespace

TEST_CASE("App::fetchMetadataOnce records a fetched title for an empty-title bookmark",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://one.example")).id;  // no title
    }

    const auto logPath = freshLogPath("fetch");
    {
        // Hour-long intervals effectively disable both timers; the pass is
        // driven directly instead, so nothing here depends on wall-clock
        // timing. `App` reaches the same database this test does because both
        // go through Lightweight's process-global default connection string,
        // which `DbFixture` (constructed above, before `App`) already set.
        bookmarks::app::App app{logPath, "test-secret", std::make_shared<StubFetcher>(), kTimersOff, kTimersOff};
        app.fetchMetadataOnce();
        REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));

        const ScopedPrincipal alice{"alice"};
        CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Fetched: https://one.example");
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("App::fetchMetadataOnce leaves an already-titled bookmark untouched", "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId titled;
    {
        const ScopedPrincipal alice{"alice"};
        titled = model.execute(makeCreate("https://one.example", "Already Set")).id;
    }

    const auto logPath = freshLogPath("fetch_titled");
    {
        bookmarks::app::App app{logPath, "test-secret", std::make_shared<StubFetcher>(), kTimersOff, kTimersOff};
        app.fetchMetadataOnce();
        REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));

        const ScopedPrincipal alice{"alice"};
        CHECK(model.execute(bookmarks::GetBookmark{.id = titled}).title == "Already Set");
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("App::fetchMetadataOnce updates a bookmark owned by someone else entirely",
          "[bookmarks][app]") {
    // The property the service principal exists for: the worker acts on
    // behalf of every owner, and is itself the owner of none of them.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId aliceId;
    bookmarks::BookmarkId bobId;
    {
        const ScopedPrincipal alice{"alice"};
        aliceId = model.execute(makeCreate("https://alice.example")).id;
    }
    {
        const ScopedPrincipal bob{"bob"};
        bobId = model.execute(makeCreate("https://bob.example")).id;
    }

    const auto logPath = freshLogPath("fetch_multi");
    {
        bookmarks::app::App app{logPath, "test-secret", std::make_shared<StubFetcher>(), kTimersOff, kTimersOff};
        app.fetchMetadataOnce();
        REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));

        {
            const ScopedPrincipal alice{"alice"};
            CHECK(model.execute(bookmarks::GetBookmark{.id = aliceId}).title == "Fetched: https://alice.example");
        }
        const ScopedPrincipal bob{"bob"};
        CHECK(model.execute(bookmarks::GetBookmark{.id = bobId}).title == "Fetched: https://bob.example");
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("App::fetchMetadataOnce with the shipped NullMetadataFetcher dispatches nothing",
          "[bookmarks][app]") {
    // A fetch that found nothing must not turn into a write: RecordMetadata
    // ignores empty fields but still stamps updated_at_ms, which every
    // client's GetChangesSince poll would then see churn on every tick.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://one.example")).id;
    }
    const ScopedPrincipal alice{"alice"};
    const auto before = model.execute(bookmarks::GetBookmark{.id = id});

    const auto logPath = freshLogPath("fetch_null");
    {
        bookmarks::app::App app{logPath, "test-secret", std::make_shared<bookmarks::app::NullMetadataFetcher>(),
                                kTimersOff, kTimersOff};
        app.fetchMetadataOnce();
        CHECK_FALSE(app.fetchInFlight());
        const auto after = model.execute(bookmarks::GetBookmark{.id = id});
        CHECK(after.title.empty());
        CHECK(after.updatedAt == before.updatedAt);
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("App::relayOutboxOnce drains a BulkEdit outbox row into the durable action log",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(makeCreate("https://one.example")).id;
    bookmarks::BulkEdit edit;
    edit.ids = {id};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    model.execute(edit);

    Lightweight::DataMapper mapper;
    REQUIRE(mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All().size() == 1);

    const auto logPath = freshLogPath("relay");
    {
        bookmarks::app::App app{logPath, "test-secret", std::make_shared<bookmarks::app::NullMetadataFetcher>(),
                                kTimersOff, kTimersOff};
        CHECK(app.relayOutboxOnce() == 1);
        CHECK(mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All().empty());

        // A second pass has nothing left to move -- the row was deleted, not
        // flagged.
        CHECK(app.relayOutboxOnce() == 0);
    }

    // The entry really reached the durable sink, not just "left the outbox".
    // Scoped so `reopened`'s file handle is closed before the remove() below
    // -- unlike POSIX, Windows refuses to delete a file a live handle still
    // has open.
    std::vector<morph::journal::LogEntry> entries;
    {
        const morph::journal::FileActionLog reopened{logPath};
        entries = reopened.entries();
    }
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].modelType == "BookmarkModel");
    CHECK(entries[0].actionType == "BulkEdit");
    CHECK(entries[0].principal == "alice");
    CHECK_FALSE(entries[0].idempotencyKey.empty());
    std::filesystem::remove(logPath);
}

TEST_CASE("AuthModel::execute(Login) mints a token that verifies against the same App's authorizer",
          "[bookmarks][app]") {
    const auto logPath = freshLogPath("login");
    {
        const bookmarks::app::App app{logPath, "login-test-secret"};
        bookmarks::AuthModel authModel;
        const auto result = authModel.execute(bookmarks::Login{.username = "alice"});
        REQUIRE(result.token.hasValue());
        CHECK(result.principal == "alice");

        // Verified against a *separately constructed* authorizer holding the
        // same secret -- exactly what the App's own RemoteServer installed.
        const bookmarks::auth::BookmarksAuthorizer authz{std::string{"login-test-secret"},
                                                           morph::session::hmacSha256};
        morph::session::Context ctx;
        ctx.token = *result.token;
        const auto principal = authz.authenticate(ctx);
        REQUIRE(principal.has_value());
        CHECK(*principal == "alice");
        CHECK(authz.authorize(ctx, "BookmarkModel", "CreateBookmark"));

        // ...and does not verify against a different secret.
        const bookmarks::auth::BookmarksAuthorizer other{std::string{"a-different-secret"},
                                                           morph::session::hmacSha256};
        CHECK_FALSE(other.authenticate(ctx).has_value());
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("AuthModel::execute(Login) refuses to mint a token in the reserved system: namespace",
          "[bookmarks][app]") {
    // Otherwise any client could log in as the metadata worker and rewrite
    // every other user's titles through RecordMetadata.
    const auto logPath = freshLogPath("login_reserved");
    {
        const bookmarks::app::App app{logPath, "login-test-secret"};
        bookmarks::AuthModel authModel;
        REQUIRE_THROWS_AS(
            authModel.execute(bookmarks::Login{.username = std::string{bookmarks::auth::kMetadataFetcherPrincipal}}),
            bookmarks::ValidationError);
        REQUIRE_THROWS_AS(authModel.execute(bookmarks::Login{.username = "system:anything"}),
                          bookmarks::ValidationError);
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("AuthModel::execute(Login) throws when no App has installed a TokenIssuer",
          "[bookmarks][app]") {
    // Every other [bookmarks][app] case constructs its App as a scoped local,
    // and ~App clears the global issuer, so this case sees a clean nullptr
    // regardless of Catch2's run order.
    REQUIRE(bookmarks::auth::tokenIssuer() == nullptr);
    bookmarks::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(bookmarks::Login{.username = "alice"}), bookmarks::ValidationError);
}

TEST_CASE("Login rejects an invalid username via the shared principal charset", "[bookmarks][app]") {
    bookmarks::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(bookmarks::Login{.username = ""}), bookmarks::ValidationError);
    REQUIRE_THROWS_AS(authModel.execute(bookmarks::Login{.username = "alice bob"}), bookmarks::ValidationError);
    CHECK_FALSE(bookmarks::Login{.username = std::string(65, 'a')}.validate());
    CHECK(bookmarks::Login{.username = "alice"}.validate());
}

TEST_CASE("App's metadata-fetch worker dispatches through the real RemoteServer, not a shortcut",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://one.example")).id;
    }

    auto fetcher = std::make_shared<RecordingFetcher>();

    const auto logPath = freshLogPath("worker_dispatch");
    {
        bookmarks::app::App app{logPath, "test-secret", fetcher, kTimersOff, kTimersOff};
        // Proves the dispatch went through the server's own registration path
        // (which requires authorizeRegister to pass -- an unauthenticated
        // internal client would fail here exactly like a real socket client
        // would): if the worker's own token/session wiring were broken, the
        // dispatched RecordMetadata would fail authorization/authentication
        // (the completion's onError path, logged but not surfaced to this
        // test directly) and fetchInFlight() would still settle to false, but
        // the title would never update -- which the assertion below catches.
        // RecordingFetcher::calls only proves fetchMetadataOnce() found the
        // untitled bookmark and called the injected fetcher in-process; it is
        // the GetBookmark title assertion afterward that can only pass if the
        // resulting RecordMetadata genuinely round-tripped through
        // RemoteServer::handle() -- BookmarkModel::execute(const
        // RecordMetadata&) is the only thing that ever writes that column.
        app.fetchMetadataOnce();
        REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));
        REQUIRE(fetcher->calls.size() == 1);
        CHECK(fetcher->calls.front() == "https://one.example");

        const ScopedPrincipal alice{"alice"};
        CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Recorded");
    }
    std::filesystem::remove(logPath);
}

// ═════════════════════════════════════════════════════════════════════════
// stopBackgroundJobs(): the shutdown precondition the server's drain needs
// ═════════════════════════════════════════════════════════════════════════
//
// `src/server/main.cpp`'s `drainMetadataFetches()` pumps `processEvents()`
// until `fetchInFlight()` settles — and pumping is exactly what delivers
// `_fetchTimer`'s ticks. With the timer still armed, the drain's own
// `processEvents()` can start a brand-new pass, re-raising `fetchInFlight()`
// after it had settled and, if that pass is still outstanding when the budget
// expires, leaving `~App` to run with a dispatch in flight — the very window
// the drain exists to close. The server therefore calls
// `App::stopBackgroundJobs()` before draining. The two cases below are a
// matched pair: the control proves the timer really does fire under a pumping
// loop (so the case under test is not vacuously green), and the case under
// test proves `stopBackgroundJobs()` genuinely disarms it.

TEST_CASE("App's fetch timer really does fire under a pumping loop (the control for stopBackgroundJobs)",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        static_cast<void>(model.execute(makeCreate("https://timer.example")).id);  // untitled: a pass has work to do
    }
    auto fetcher = std::make_shared<RecordingFetcher>();

    const auto logPath = freshLogPath("timer_control");
    {
        bookmarks::app::App app{logPath, "test-secret", fetcher, kFastFetchInterval, kTimersOff};
        // Nothing is dispatched by hand here: the *timer* is the subject.
        REQUIRE(pumpUntil([&fetcher] { return !fetcher->calls.empty(); }));
        REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("App::stopBackgroundJobs disarms the fetch timer, so a drain loop cannot provoke a new pass",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://timer.example")).id;  // untitled, exactly as above
    }
    auto fetcher = std::make_shared<RecordingFetcher>();

    const auto logPath = freshLogPath("timer_stopped");
    {
        bookmarks::app::App app{logPath, "test-secret", fetcher, kFastFetchInterval, kTimersOff};
        // No event loop has turned between the constructor's `start()` and
        // this call, so the timer has had no chance to tick yet — the state
        // `main()` is *not* in when it calls this (it calls it after `exec()`
        // returns), but the strictly harder one to keep quiet.
        app.stopBackgroundJobs();

        // The drain window, simulated: pump for far longer than the interval.
        // The predicate must never become true, so a `true` here means a tick
        // got through and `pumpUntil` returning `false` is the passing outcome
        // — the one place in this suite where a timeout is the assertion.
        CHECK_FALSE(pumpUntil([&fetcher] { return !fetcher->calls.empty(); }, std::chrono::milliseconds{500}));
        CHECK(fetcher->calls.empty());
        CHECK_FALSE(app.fetchInFlight());

        // ...and nothing was written, which is what a spurious pass would
        // have left behind (`RecordMetadata` sets the title and stamps
        // `updated_at_ms`).
        const ScopedPrincipal alice{"alice"};
        CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title.empty());
    }
    std::filesystem::remove(logPath);
}

TEST_CASE("App::stopBackgroundJobs is idempotent, and ~App still stops the timers on its own",
          "[bookmarks][app]") {
    // The refactor's two invariants: calling it twice is harmless (QTimer::stop
    // on a stopped timer is a no-op), and an owner that never calls it at all
    // — every test above, and any other consumer — still gets the destructor's
    // original stop-first behaviour, because ~App now calls it too.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        static_cast<void>(model.execute(makeCreate("https://timer.example")).id);
    }
    auto fetcher = std::make_shared<RecordingFetcher>();

    const auto logPath = freshLogPath("timer_idempotent");
    {
        bookmarks::app::App app{logPath, "test-secret", fetcher, kFastFetchInterval, kFastFetchInterval};
        app.stopBackgroundJobs();
        app.stopBackgroundJobs();
        CHECK_FALSE(pumpUntil([&fetcher] { return !fetcher->calls.empty(); }, std::chrono::milliseconds{300}));
    }
    // The App is gone; pumping now must not resurrect a tick from either timer
    // (a still-armed QTimer owned by a destroyed App would be a use-after-free,
    // not merely a stray call).
    CHECK_FALSE(pumpUntil([&fetcher] { return !fetcher->calls.empty(); }, std::chrono::milliseconds{200}));
    std::filesystem::remove(logPath);
}
