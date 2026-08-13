// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"
#include "testkit/db_fixture.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/outbox_entity.hpp"

#include "clock.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_busy_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/session/session.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

namespace {

/// @brief A `Context` carrying only @p principal.
///
/// Built field-by-field rather than with a designated initializer on
/// purpose: `-Weverything` includes
/// `-Wmissing-designated-field-initializers`, which fires on a partial
/// designated-initializer list, and `ladder_<rung>_tests` is built with
/// `apply_warnings()` (so `-Werror` under `MORPH_ENABLE_STRICT_COMPILATION`,
/// CI's default). Same reason `makeCreate` below exists (see
/// `test_app.cpp`'s `contextFor`/`makeCreate` for the original pattern).
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

/// @brief A `CreateBookmark` for @p url, optionally titled and/or tagged.
///        See `contextFor` for why this is not a designated initializer.
[[nodiscard]] bookmarks::CreateBookmark makeCreate(std::string url, std::string title = {},
                                                    std::vector<std::string> tags = {}) {
    bookmarks::CreateBookmark action;
    action.url = std::move(url);
    action.title = std::move(title);
    action.tags = std::move(tags);
    return action;
}

}  // namespace

TEST_CASE("CreateBookmark stores a bookmark owned by the authenticated principal",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal principal{"alice"};

    bookmarks::CreateBookmark action;
    action.url = "https://example.com";
    action.title = "Example";
    action.tags = {"work", "reading"};
    const auto id = model.execute(action).id;
    REQUIRE(id.hasValue());

    const auto view = model.execute(bookmarks::GetBookmark{.id = id});
    CHECK(view.url == "https://example.com");
    CHECK(view.title == "Example");
    CHECK(view.readState == bookmarks::ReadState::Unread);
    CHECK(view.archiveState == bookmarks::ArchiveState::Active);
    CHECK(view.tags.size() == 2);
}

TEST_CASE("CreateBookmark without a principal is Forbidden", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    // No ScopedPrincipal installed -- session::current() is nullptr.
    bookmarks::CreateBookmark action;
    action.url = "https://example.com";
    REQUIRE_THROWS_AS(model.execute(action), bookmarks::Forbidden);
}

TEST_CASE("GetBookmark refuses a different principal's bookmark with Forbidden, not NotFound",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://example.com")).id;
    }
    const ScopedPrincipal mallory{"mallory"};
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{.id = id}), bookmarks::Forbidden);
}

TEST_CASE("EditBookmark replaces the tag set: adds new tags, drops removed ones, keeps shared ones",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    auto create = makeCreate("https://example.com", {}, {"a", "b"});
    const auto id = model.execute(create).id;

    bookmarks::EditBookmark edit;
    edit.id = id;
    edit.url = "https://example.com";
    edit.tags = {"b", "c"};
    const auto edited = model.execute(edit);
    std::vector<std::string> tags = edited.tags;
    std::ranges::sort(tags);
    CHECK(tags == std::vector<std::string>{"b", "c"});  // "a" dropped, "b" kept, "c" auto-created
}

TEST_CASE("ArchiveBookmark/UnarchiveBookmark flip archiveState and nothing else",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(makeCreate("https://example.com")).id;

    model.execute(bookmarks::ArchiveBookmark{.id = id});
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).archiveState == bookmarks::ArchiveState::Archived);
    model.execute(bookmarks::UnarchiveBookmark{.id = id});
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).archiveState == bookmarks::ArchiveState::Active);
}

TEST_CASE("DeleteBookmark removes the bookmark and its tag associations", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(makeCreate("https://example.com", {}, {"a"})).id;

    model.execute(bookmarks::DeleteBookmark{.id = id});
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{.id = id}), bookmarks::NotFound);
}

TEST_CASE("GetBookmark against an unknown id throws NotFound, and an empty id is a ValidationError",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{.id = bookmarks::BookmarkId{99999}}),
                      bookmarks::NotFound);
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{}), bookmarks::ValidationError);
}

TEST_CASE("ListBookmarks filters by archive state and hides archived bookmarks by default",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto activeId = model.execute(makeCreate("https://active.example")).id;
    const auto archivedId = model.execute(makeCreate("https://archived.example")).id;
    model.execute(bookmarks::ArchiveBookmark{.id = archivedId});

    const auto defaultPage = model.execute(bookmarks::ListBookmarks{});
    REQUIRE(defaultPage.bookmarks.size() == 1);
    CHECK(*defaultPage.bookmarks.front().id == *activeId);

    bookmarks::ListBookmarks archivedOnly;
    archivedOnly.archiveFilter = bookmarks::ArchiveFilter::ArchivedOnly;
    const auto archivedPage = model.execute(archivedOnly);
    REQUIRE(archivedPage.bookmarks.size() == 1);
    CHECK(*archivedPage.bookmarks.front().id == *archivedId);
}

TEST_CASE("ListBookmarks only ever returns the calling principal's own bookmarks",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(makeCreate("https://alice.example"));
    }
    const ScopedPrincipal mallory{"mallory"};
    model.execute(makeCreate("https://mallory.example"));
    const auto page = model.execute(bookmarks::ListBookmarks{});
    REQUIRE(page.bookmarks.size() == 1);
    CHECK(page.bookmarks.front().url == "https://mallory.example");
}

TEST_CASE("ListBookmarks sets nextCursor even when a filtered page's matches are empty, "
          "so a tag/text search doesn't silently truncate",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    // Created first, so it has the lowest id and therefore sorts last in the
    // DESCENDING-by-id keyset pagination below -- i.e. it lands beyond the
    // first raw SQL page.
    const auto targetId =
        model.execute(makeCreate("https://target.example", {}, {"target"})).id;
    for (int i = 0; i < 25; ++i) {
        model.execute(makeCreate("https://filler" + std::to_string(i) + ".example"));
    }

    bookmarks::ListBookmarks filtered;
    filtered.tag = "target";
    const auto firstPage = model.execute(filtered);
    // The 20 newest raw rows are all untagged fillers, so the filtered result
    // is empty -- but a 21st raw row (eventually the tagged bookmark) still
    // exists further down the id space, so nextCursor must still be set.
    REQUIRE(firstPage.bookmarks.empty());
    REQUIRE(firstPage.nextCursor.hasValue());

    filtered.cursor = firstPage.nextCursor;
    const auto secondPage = model.execute(filtered);
    REQUIRE(secondPage.bookmarks.size() == 1);
    CHECK(*secondPage.bookmarks.front().id == *targetId);
}

TEST_CASE("GetChangesSince returns only bookmarks touched after the given instant",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    const auto before = *morph::ladder::now();
    const morph::ladder::ScopedClockOverride clock1{before + std::chrono::milliseconds{10}};
    const auto id1 = model.execute(makeCreate("https://one.example")).id;

    const auto cursor = model.execute(bookmarks::GetChangesSince{}).asOf;

    const morph::ladder::ScopedClockOverride clock2{before + std::chrono::milliseconds{20}};
    const auto id2 = model.execute(makeCreate("https://two.example")).id;

    const auto changes = model.execute(bookmarks::GetChangesSince{.since = cursor});
    REQUIRE(changes.changed.size() == 1);
    CHECK(*changes.changed.front().id == *id2);
    (void) id1;
}

TEST_CASE("GetChangesSince does not miss a write landing in the same millisecond as the cursor",
          "[bookmarks][model]") {
    // Regression test for issue #43: a cursor that compares only on
    // updated_at_ms with strict `>` can silently drop a write whose
    // timestamp equals the previous poll's asOf (same millisecond -- a
    // plausible timing window on a fast machine or a loaded CI runner, not
    // a contrived one). Frozen to a single instant, like the analogous
    // same-millisecond BulkEdit regression test above, so the race is
    // deterministic rather than relying on incidental timing.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    const auto frozenAt = *morph::ladder::now();
    const morph::ladder::ScopedClockOverride clock{frozenAt};

    // First poll: nothing exists yet. Its asOf is frozenAt with no
    // tie-break id (nothing at that instant to break a tie against).
    const auto cursor = model.execute(bookmarks::GetChangesSince{}).asOf;

    // A write lands in the *same* frozen millisecond as the cursor just
    // captured -- still under the same ScopedClockOverride, so
    // updated_at_ms for this row is bit-for-bit equal to cursor's instant.
    const auto id = model.execute(makeCreate("https://same-ms.example")).id;

    // The strict `>` bug would exclude this row: updated_at_ms == since,
    // not >. The fix must still return it via the id tie-break.
    const auto changes = model.execute(bookmarks::GetChangesSince{.since = cursor});
    REQUIRE(changes.changed.size() == 1);
    CHECK(*changes.changed.front().id == *id);
}

TEST_CASE("GetChangesSince's same-millisecond tie-break never re-delivers an already-seen write",
          "[bookmarks][model]") {
    // Companion to the test above: the id tie-break must be a strict `>`
    // on id, not `>=` -- otherwise the write that established the cursor
    // would be re-delivered forever on every subsequent poll at the same
    // frozen instant.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    const auto frozenAt = *morph::ladder::now();
    const morph::ladder::ScopedClockOverride clock{frozenAt};

    (void) model.execute(makeCreate("https://first.example"));
    const auto cursor = model.execute(bookmarks::GetChangesSince{}).asOf;

    // No further writes -- polling again with the cursor that already
    // covers the one write above must come back empty.
    const auto changes = model.execute(bookmarks::GetChangesSince{.since = cursor});
    CHECK(changes.changed.empty());
}

TEST_CASE("BulkEdit archives every listed bookmark and adds/removes tags atomically",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id1 = model.execute(makeCreate("https://one.example", {}, {"old"})).id;
    const auto id2 = model.execute(makeCreate("https://two.example")).id;

    bookmarks::BulkEdit edit;
    edit.ids = {id1, id2};
    edit.addTags = {"new"};
    edit.removeTags = {"old"};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    const auto result = model.execute(edit);
    CHECK(morph::math::floor(*result.affected) == 2);

    for (const auto id : {id1, id2}) {
        const auto view = model.execute(bookmarks::GetBookmark{.id = id});
        CHECK(view.archiveState == bookmarks::ArchiveState::Archived);
        CHECK(std::ranges::find(view.tags, "new") != view.tags.end());
        CHECK(std::ranges::find(view.tags, "old") == view.tags.end());
    }
}

TEST_CASE("BulkEdit rejects the whole batch if any id is not owned by the caller",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId aliceId;
    {
        const ScopedPrincipal alice{"alice"};
        aliceId = model.execute(makeCreate("https://alice.example")).id;
    }
    const ScopedPrincipal mallory{"mallory"};
    const auto malloryId = model.execute(makeCreate("https://mallory.example")).id;

    bookmarks::BulkEdit edit;
    edit.ids = {malloryId, aliceId};  // one owned, one not
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    REQUIRE_THROWS_AS(model.execute(edit), bookmarks::Forbidden);

    // All-or-nothing: mallory's own bookmark was NOT archived either.
    CHECK(model.execute(bookmarks::GetBookmark{.id = malloryId}).archiveState == bookmarks::ArchiveState::Active);
}

TEST_CASE("BulkEdit writes exactly one outbox row per call, consumed by an OutboxRelay",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(makeCreate("https://one.example")).id;

    bookmarks::BulkEdit edit;
    edit.ids = {id};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    model.execute(edit);

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().actionType.Value() == "BulkEdit");
    CHECK(rows.front().principal.Value() == "alice");
}

TEST_CASE("BulkEdit from the same principal in the same millisecond both succeed, "
          "each with its own outbox row",
          "[bookmarks][model]") {
    // Regression test: the outbox idempotency key used to be
    // owner + "-bulkedit-" + nowMs() alone, which collides across two
    // BulkEdit calls from the same principal landing in the same
    // millisecond (nowMs() has millisecond resolution) -- the second
    // model.execute() would throw a raw SQL constraint-violation exception
    // from idx_bookmark_outbox_idempotency instead of succeeding.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(makeCreate("https://one.example")).id;

    const auto frozenAt = *morph::ladder::now();
    const morph::ladder::ScopedClockOverride clock{frozenAt};

    bookmarks::BulkEdit edit;
    edit.ids = {id};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    REQUIRE_NOTHROW(model.execute(edit));
    // Second call, still under the same frozen instant -- must also
    // succeed, not throw on the idempotency key's unique index.
    REQUIRE_NOTHROW(model.execute(edit));

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].idempotencyKey.Value() != rows[1].idempotencyKey.Value());
}

TEST_CASE("RecordMetadata updates another principal's bookmark when the service principal dispatches it",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://one.example")).id;
    }
    // Dispatched as the service principal, not "alice" -- must not throw
    // Forbidden even though the row belongs to someone else. That asymmetry
    // is the whole point of the action.
    const ScopedPrincipal worker{std::string{bookmarks::auth::kMetadataFetcherPrincipal}};
    model.execute(bookmarks::RecordMetadata{.id = id, .title = "Fetched Title", .faviconPath = {}});

    const ScopedPrincipal alice{"alice"};
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Fetched Title");
}

TEST_CASE("RecordMetadata refuses any principal other than the metadata-fetch service principal",
          "[bookmarks][model]") {
    // The check that stands in because authorizeInstance can't express this:
    // it compares instance ownership, not row ownership, and this action
    // deliberately touches rows the calling principal (the service worker)
    // doesn't own -- an instance-level check has nothing to object to when
    // the worker dispatches through its own, legitimately-owned instance.
    // Without this model-level check, `mallory` below would silently
    // overwrite alice's title.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(makeCreate("https://one.example", "Alice's Title")).id;
    }
    {
        const ScopedPrincipal mallory{"mallory"};
        REQUIRE_THROWS_AS(model.execute(bookmarks::RecordMetadata{.id = id, .title = "Owned", .faviconPath = {}}),
                          bookmarks::Forbidden);
    }
    {
        // Not even the row's own owner may dispatch it: this action exists
        // for the internal worker, and EditBookmark is the user-facing way
        // to set a title.
        const ScopedPrincipal alice{"alice"};
        REQUIRE_THROWS_AS(model.execute(bookmarks::RecordMetadata{.id = id, .title = "By hand", .faviconPath = {}}),
                          bookmarks::Forbidden);
        CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Alice's Title");
    }
}

TEST_CASE("RecordMetadata against an already-deleted bookmark is a benign no-op",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(makeCreate("https://one.example")).id;
    model.execute(bookmarks::DeleteBookmark{.id = id});
    const ScopedPrincipal worker{std::string{bookmarks::auth::kMetadataFetcherPrincipal}};
    REQUIRE_NOTHROW(model.execute(bookmarks::RecordMetadata{.id = id, .title = "Too Late", .faviconPath = {}}));
}

TEST_CASE("ImportBookmarks stores every well-formed entry in one chunk", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    bookmarks::ImportBookmarks action;
    action.chunk = R"(<DT><A HREF="https://one.example">One</A>
<DT><A HREF="https://two.example">Two</A>
<DT><A>No href</A>)";
    action.opId = bookmarks::ImportOpId{"chunk-1"};
    const auto result = model.execute(action);
    CHECK(morph::math::floor(*result.imported) == 2);
    CHECK(morph::math::floor(*result.skipped) == 1);

    const auto page = model.execute(bookmarks::ListBookmarks{});
    CHECK(page.bookmarks.size() == 2);
}

TEST_CASE("ImportBookmarks skips an entry whose url or title exceeds this rung's field bounds",
          "[bookmarks][model]") {
    // The Netscape parser applies no field bounds of its own, so without an
    // explicit check here an import would happily write a row that
    // `EditBookmark::validate()` then refuses -- a bookmark the owner can see
    // but can never edit. Skipped-and-counted is the answer; truncation would
    // silently store a url that is not the one the user saved.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    const std::string longUrl = "https://" + std::string(bookmarks::kMaxUrlBytes, 'u') + ".example";
    const std::string longTitle(bookmarks::kMaxTitleBytes + 1, 't');
    REQUIRE(longUrl.size() > bookmarks::kMaxUrlBytes);

    bookmarks::ImportBookmarks action;
    action.chunk = R"(<DT><A HREF="https://fine.example">Fine</A>
<DT><A HREF=")" + longUrl +
                   R"(">Over-long url</A>
<DT><A HREF="https://long-title.example">)" +
                   longTitle + R"(</A>)";
    REQUIRE(action.chunk.size() <= bookmarks::kMaxImportChunkBytes);  // not the chunk bound under test
    action.opId = bookmarks::ImportOpId{"chunk-oversized-fields"};

    const auto result = model.execute(action);
    CHECK(morph::math::floor(*result.imported) == 1);
    CHECK(morph::math::floor(*result.skipped) == 2);

    // Not merely uncounted: neither oversized entry reached the store, in
    // truncated form or otherwise.
    const auto page = model.execute(bookmarks::ListBookmarks{});
    REQUIRE(page.bookmarks.size() == 1);
    CHECK(page.bookmarks.front().url == "https://fine.example");
}

TEST_CASE("An ImportBookmarks chunk over kMaxImportChunkBytes throws TooLarge, not ValidationError",
          "[bookmarks][model]") {
    // `TooLarge`'s own doc comment promises exactly this, and the distinction
    // is what lets a client tell "re-chunk your file" apart from "your
    // request was malformed". validate() deliberately does NOT bound
    // chunk size (see import_export_dto.hpp) -- an oversized-but-otherwise-
    // well-formed chunk passes validate() and reaches execute(), which is
    // what actually throws TooLarge. If validate() rejected it too, every
    // real dispatch path (Bridge::executeVia / RemoteServer both consult
    // validate() before execute() is ever reached) would fail the request
    // as ValidationError first and TooLarge would never be observable
    // outside a bare, bridge-bypassing model.execute() call like this one.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    bookmarks::ImportBookmarks action;
    action.chunk = std::string(bookmarks::kMaxImportChunkBytes + 1, 'x');
    action.opId = bookmarks::ImportOpId{"chunk-too-large"};
    REQUIRE(action.validate());

    CHECK_THROWS_AS(model.execute(action), bookmarks::TooLarge);

    // A chunk that is malformed for some *other* reason still gets the
    // untyped answer, so the check above is not vacuous.
    bookmarks::ImportBookmarks noOpId;
    noOpId.chunk = R"(<DT><A HREF="https://one.example">One</A>)";
    CHECK_THROWS_AS(model.execute(noOpId), bookmarks::ValidationError);
}

TEST_CASE("An oversized ImportBookmarks chunk reaches TooLarge through the real Bridge dispatch path, "
          "not just a bare model.execute() call",
          "[bookmarks][model]") {
    // The case above proves execute() throws the right type; it calls
    // execute() directly, bypassing ActionValidator/Bridge::executeVia
    // entirely, so it cannot by itself prove the fix above (validate() not
    // bounding chunk size) actually matters. This case drives the same
    // oversized chunk through BackendRig -- Bridge::executeVia's real
    // validate()-then-execute() sequence -- and confirms TooLarge survives
    // as a distinguishable C++ type through Completion/awaitQt's
    // exception_ptr rethrow (Local/LocalSingleThread dispatch is in-process,
    // so the exception object itself propagates; see pump.hpp's awaitQt).
    //
    // This does NOT hold over Socket/remote transport: RemoteServer encodes
    // every server-side exception as an opaque wire::makeErr(exc.what())
    // string (remote.hpp), and the client reconstructs a generic
    // std::runtime_error from it, discarding the original type. That is a
    // framework-wide property of every model's typed errors, not specific
    // to TooLarge or to this rung -- Socket-mode dispatch is deliberately
    // not exercised in this case for that reason.
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread);
    CAPTURE(mode);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    auto handler = rig.client<bookmarks::BookmarkModel>(0);

    bookmarks::ImportBookmarks action;
    action.chunk = std::string(bookmarks::kMaxImportChunkBytes + 1, 'x');
    action.opId = bookmarks::ImportOpId{"chunk-too-large-over-bridge"};
    REQUIRE(action.validate());  // must pass, or Bridge::executeVia never reaches execute() at all

    REQUIRE_THROWS_AS(awaitQt(handler.execute(action)), bookmarks::TooLarge);
}

TEST_CASE("ImportBookmarks is idempotent on a retried opId", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    bookmarks::ImportBookmarks action;
    action.chunk = R"(<DT><A HREF="https://one.example">One</A>)";
    action.opId = bookmarks::ImportOpId{"chunk-retry"};
    model.execute(action);
    model.execute(action);  // simulates a retry after a dropped connection

    const auto page = model.execute(bookmarks::ListBookmarks{});
    CHECK(page.bookmarks.size() == 1);  // not duplicated
}

TEST_CASE("ExportBookmarks emits every owned bookmark as a Netscape file, and it re-imports",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(makeCreate("https://one.example", "One"));
        model.execute(makeCreate("https://two.example", "Two"));
    }
    std::string exported;
    {
        const ScopedPrincipal alice{"alice"};
        exported = model.execute(bookmarks::ExportBookmarks{}).html;
    }
    CHECK(exported.find("https://one.example") != std::string::npos);
    CHECK(exported.find("https://two.example") != std::string::npos);

    const ScopedPrincipal bob{"bob"};
    bookmarks::ImportBookmarks reimport;
    reimport.chunk = exported;
    reimport.opId = bookmarks::ImportOpId{"reimport-1"};
    const auto result = model.execute(reimport);
    CHECK(morph::math::floor(*result.imported) == 2);
}

TEST_CASE("A URL containing '&' survives an ExportBookmarks/ImportBookmarks round trip unchanged",
          "[bookmarks][model]") {
    // Regression test: export used to escape '&' to "&amp;" in the HREF
    // attribute, but import never decoded it back out, so a reimported
    // bookmark's URL ended up with the literal "&amp;" text baked in instead
    // of the original '&'. This is the common case for URLs with query
    // strings, not an edge case.
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const std::string originalUrl = "https://example.com/search?a=1&b=2";
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(makeCreate(originalUrl, "Search"));
    }
    std::string exported;
    {
        const ScopedPrincipal alice{"alice"};
        exported = model.execute(bookmarks::ExportBookmarks{}).html;
    }
    // The exported HTML entity-escapes the '&' in the HREF attribute.
    CHECK(exported.find("https://example.com/search?a=1&amp;b=2") != std::string::npos);
    CHECK(exported.find(originalUrl) == std::string::npos);

    const ScopedPrincipal bob{"bob"};
    bookmarks::ImportBookmarks reimport;
    reimport.chunk = exported;
    reimport.opId = bookmarks::ImportOpId{"reimport-amp-1"};
    const auto result = model.execute(reimport);
    CHECK(morph::math::floor(*result.imported) == 1);

    const auto page = model.execute(bookmarks::ListBookmarks{});
    REQUIRE(page.bookmarks.size() == 1);
    CHECK(page.bookmarks[0].url == originalUrl);  // decoded back to the original, not "&amp;"
}

TEST_CASE("BookmarkModel over the full backend-mode matrix: create, list, get round-trip",
          "[bookmarks][model]") {
    // Every case above dispatches model.execute(action) directly, C++-to-C++,
    // with ScopedPrincipal standing in for a real dispatch's Context -- it
    // never exercises the dispatch machinery itself. This case drives the
    // create -> list -> get round trip through the real path instead:
    // Local/LocalSingleThread/Socket via BackendRig, authenticated with a
    // real signed token verified by a real BookmarksAuthorizer. Socket mode
    // is the one that actually matters here -- authorizeRegister is
    // unconditionally permissive by this rung's own design choice (not a
    // framework limitation -- the register envelope now carries the
    // caller's identity), so this case does not prove anything about
    // registration being gated. What it does prove is that
    // SigningAuthorizer::authorize(), which sees the token on every
    // subsequent execute(), correctly admits a validly signed token end to
    // end through the real RemoteServer/QtWebSocketServer wiring -- the
    // boundary that is genuinely enforced (see bookmarks_authorizer.hpp's
    // @file comment).
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    constexpr std::string_view kSecret = "matrix-test-secret";
    const auto authorizer =
        std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{mode, 1, authorizer};

    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    morph::session::Context ctx;
    ctx.principal = "alice";
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = "alice", .expiresAtMs = 4102444800000, .roles = {}});
    rig.bridge(0).setDefaultSession(ctx);

    auto handler = rig.client<bookmarks::BookmarkModel>(0);
    bookmarks::CreateBookmark create;
    create.url = "https://matrix.example";
    create.title = "Matrix";
    const auto createResult = awaitQt(handler.execute(create));
    REQUIRE(createResult.id.hasValue());

    const auto listResult = awaitQt(handler.execute(bookmarks::ListBookmarks{}));
    REQUIRE(listResult.bookmarks.size() == 1);

    const auto view = awaitQt(handler.execute(bookmarks::GetBookmark{.id = createResult.id}));
    CHECK(view.url == "https://matrix.example");
    CHECK(view.title == "Matrix");
}

// ═════════════════════════════════════════════════════════════════════════
// Task 15 — DoD/strain-point closers: BulkEdit atomicity under injected
// failure, cross-user Socket-mode auth, and the local-mode-no-auth strain
// point demonstrated rather than just asserted.
// ═════════════════════════════════════════════════════════════════════════

namespace {

/// @brief Installs a short SQLite `busy_timeout` on every connection opened
///        while it is alive, and restores the default afterwards.
///
/// Identical shape to `test_paste_model.cpp`'s helper of the same name
/// (rung 1) -- test-only, one file's own concern, not yet promoted. See that
/// file's doc comment for why the post-connected hook (rather than a
/// connection-string `Timeout=` override) is the seam that actually works:
/// `Lightweight::SqlConnection::PostConnect()` unconditionally issues
/// `PRAGMA busy_timeout = 60000` on every new SQLite connection, which would
/// otherwise make a contended write block for a real minute before this test
/// observed `SQLITE_BUSY`.
class ScopedShortBusyTimeout {
  public:
    explicit ScopedShortBusyTimeout(int milliseconds) {
        ::Lightweight::SqlConnection::SetPostConnectedHook([milliseconds](::Lightweight::SqlConnection& connection) {
            ::Lightweight::SqlStatement stmt{connection};
            (void) stmt.ExecuteDirect("PRAGMA busy_timeout = " + std::to_string(milliseconds));
        });
    }
    ~ScopedShortBusyTimeout() { ::Lightweight::SqlConnection::ResetPostConnectedHook(); }

    ScopedShortBusyTimeout(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout& operator=(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout(ScopedShortBusyTimeout&&) = delete;
    ScopedShortBusyTimeout& operator=(ScopedShortBusyTimeout&&) = delete;
};

}  // namespace

TEST_CASE("BulkEdit rolls back entirely when a genuine SQLITE_BUSY interrupts the batch",
          "[bookmarks][model]") {
    // DoD: "Bulk edit is atomic under injected mid-batch failure." A real
    // mid-transaction failure, not a mock -- mirrors test_paste_model.cpp's
    // proven DbBusyFixture/ScopedShortBusyTimeout recipe exactly (finding
    // 018's resolved mechanism for the SQLITE_BUSY class, rung 1).
    DbFixture fixture;
    bookmarks::BookmarkModel seedModel;
    bookmarks::BookmarkId id1;
    bookmarks::BookmarkId id2;
    {
        const ScopedPrincipal alice{"alice"};
        id1 = seedModel.execute(makeCreate("https://one.example")).id;
        id2 = seedModel.execute(makeCreate("https://two.example")).id;
    }

    // The model under test must open its connection *while* the short
    // busy-timeout hook is installed, so it must be a model that has not
    // executed anything yet (BookmarkModel's mapper connects lazily, on
    // first use) -- seedModel above already has a long-timeout connection
    // from creating id1/id2, so it is unaffected by the hook and remains
    // usable for the post-failure assertions below.
    const ScopedShortBusyTimeout shortTimeout{200};
    bookmarks::BookmarkModel contendedModel;
    const ScopedPrincipal alice{"alice"};

    const morph::ladder::testkit::DbBusyFixture busy{"bookmarks"};
    bookmarks::BulkEdit edit;
    edit.ids = {id1, id2};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    REQUIRE_THROWS(contendedModel.execute(edit));

    // Neither bookmark was archived, and no outbox row survived -- the
    // whole transaction (mutation + outbox write) rolled back together.
    CHECK(seedModel.execute(bookmarks::GetBookmark{.id = id1}).archiveState == bookmarks::ArchiveState::Active);
    CHECK(seedModel.execute(bookmarks::GetBookmark{.id = id2}).archiveState == bookmarks::ArchiveState::Active);
    Lightweight::DataMapper mapper;
    CHECK(mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All().empty());
}

TEST_CASE("BackendRig::Socket: a second principal's GetBookmark is denied by the model's own "
          "ownership re-check over a real wire transport, not by authorizeInstance",
          "[bookmarks][model][socket-only]") {
    // DoD: "authorization enforced server-side, not by the client." Two real
    // sockets, two real signed tokens, one tries to GetBookmark an id it
    // does not own.
    //
    // This is deliberately NOT titled "authorizeInstance denies ..." --
    // register envelopes now carry a session, so RemoteServer records a
    // real, non-empty owner for each of alice's and mallory's own
    // plain-registered BookmarkModel instances (confirmed empirically:
    // authorizeInstance runs with ctx.principal == ownerPrincipal == the
    // dispatching principal's own name for both). But `authorizeInstance`
    // checks instance ownership, not row ownership -- mallory dispatches
    // GetBookmark through her OWN instance, which she legitimately owns, and
    // the id she names in the action payload is alice's bookmark. An
    // instance-ownership check has no way to see that mismatch; it would
    // pass for any row id mallory happened to name, since the check never
    // looks past which instance is making the call.
    //
    // What actually denies mallory's call is
    // BookmarkModel::execute(const GetBookmark&)'s own loadOwned()/
    // requireOwner() re-check: the row's real `ownerPrincipal` DB column
    // (a column on the bookmarks table itself, keyed by the row's id, not
    // the calling instance) does not match mallory's server-verified
    // principal, so the model itself throws Forbidden -- confirmed
    // empirically (the propagated error message is "bookmark belongs to a
    // different principal", not authorizeInstance's "unauthorized"). This is
    // exactly the mechanism the README's DoD section names as what is
    // genuinely enforced today -- `SigningAuthorizer::authorize()` on every
    // action plus the models' own verified-principal, per-row scoping --
    // and it is the *only* layer that could ever catch this specific
    // mismatch, regardless of instance-ownership tracking.
    DbFixture fixture;
    constexpr std::string_view kSecret = "cross-user-secret";
    const auto authorizer =
        std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    BackendRig rig{Mode::Socket, 2, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    auto tokenFor = [&issuer](std::string principal) {
        morph::session::Context ctx;
        ctx.principal = principal;
        ctx.token = issuer.issue(morph::session::SessionToken{
            .principal = std::move(principal), .expiresAtMs = 4102444800000, .roles = {}});
        return ctx;
    };
    rig.bridge(0).setDefaultSession(tokenFor("alice"));
    rig.bridge(1).setDefaultSession(tokenFor("mallory"));

    auto aliceHandler = rig.client<bookmarks::BookmarkModel>(0);
    auto malloryHandler = rig.client<bookmarks::BookmarkModel>(1);

    const auto created = awaitQt(aliceHandler.execute(makeCreate("https://alice.example")));

    bool malloryFailed = false;
    malloryHandler.execute(bookmarks::GetBookmark{.id = created.id})
        .then([](bookmarks::BookmarkView) {})
        .onError([&malloryFailed](const std::exception_ptr&) { malloryFailed = true; });
    REQUIRE(pumpUntil([&malloryFailed] { return malloryFailed; }));
}

TEST_CASE("BackendRig::Socket: a token signed with a different secret is rejected by "
          "SigningAuthorizer::authorize(), not merely by the client",
          "[bookmarks][model][socket-only]") {
    // Closes a gap Task 14's review flagged as parked, not blocking: a
    // Socket-mode negative-auth case (wrong-secret token rejected over the
    // real wire transport) was manually fault-injection-verified during
    // Task 14's development (task-14-report.md's "Finding-027 framing
    // check") but never committed as a permanent test. Composes naturally
    // alongside this task's own cross-user case above -- same
    // BackendRig::Socket setup, one more BridgeHandler.
    //
    // Registration itself is unaffected by the wrong secret, for a different
    // reason than "no session to check": a wrong-secret token fails
    // authenticate(), so env.session.principal is cleared before
    // authorizeRegister ever runs -- but authorizeRegister is unconditionally
    // permissive here regardless of principal, by this rung's own design
    // (see its own doc comment). The rejection below can therefore only come
    // from the per-execute() check -- SigningAuthorizer::authorize()
    // verifying the token's signature against the server's real secret on
    // every action.
    DbFixture fixture;
    constexpr std::string_view kServerSecret = "socket-negauth-server-secret";
    constexpr std::string_view kWrongSecret = "socket-negauth-wrong-secret";
    const auto authorizer = std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kServerSecret},
                                                                                    morph::session::hmacSha256);
    BackendRig rig{Mode::Socket, 1, authorizer};
    const morph::session::TokenIssuer wrongIssuer{std::string{kWrongSecret}, morph::session::hmacSha256};

    morph::session::Context ctx;
    ctx.principal = "alice";
    ctx.token = wrongIssuer.issue(
        morph::session::SessionToken{.principal = "alice", .expiresAtMs = 4102444800000, .roles = {}});
    rig.bridge(0).setDefaultSession(ctx);

    auto handler = rig.client<bookmarks::BookmarkModel>(0);

    bool callFailed = false;
    handler.execute(makeCreate("https://mismatched-secret.example"))
        .then([](bookmarks::CreateBookmarkResult) {})
        .onError([&callFailed](const std::exception_ptr&) { callFailed = true; });
    REQUIRE(pumpUntil([&callFailed] { return callFailed; }));
}

TEST_CASE("Mode::Local has no authorization at all: isolation depends entirely on the model's own re-check",
          "[bookmarks][model]") {
    // Expected strain points: "Local mode has no authorization at all (the
    // local backend never authorizes): the first multi-user rung must
    // demonstrate this with a test and document the mitigation." Demonstrated
    // here, not just asserted in prose.
    DbFixture fixture;
    // No authorizer passed -- Mode::Local's LocalBackend never consults one
    // regardless (verified against backend.hpp: LocalBackend's registration
    // and dispatch paths carry no IAuthorizer reference at all -- grep for
    // it there and there is nothing to find), so this is the same as passing
    // one: the point this test makes.
    BackendRig rig{Mode::Local, 1};
    auto handler = rig.client<bookmarks::BookmarkModel>(0);

    bookmarks::BookmarkId aliceId;
    {
        const ScopedPrincipal alice{"alice"};
        // Constructed directly, not through the rig's handler -- this
        // establishes the row to attack; the attack itself goes through
        // the rig, matching a real client's only path.
        bookmarks::BookmarkModel seedModel;
        aliceId = seedModel.execute(makeCreate("https://alice.example")).id;
    }

    // No token/session set on rig.bridge(0) at all -- Local mode's own
    // Context::principal, whatever the caller sets client-side, would
    // normally be untrustworthy on a Socket transport; here there is no
    // authorizer to strip it, so it passes straight through. This test
    // simulates the honest worst case: an attacker who sets principal
    // directly, which Local mode lets through unchecked.
    morph::session::Context ctx;
    ctx.principal = "mallory";
    rig.bridge(0).setDefaultSession(ctx);

    bool malloryFailed = false;
    handler.execute(bookmarks::GetBookmark{.id = aliceId})
        .then([](bookmarks::BookmarkView) {})
        .onError([&malloryFailed](const std::exception_ptr&) { malloryFailed = true; });
    REQUIRE(pumpUntil([&malloryFailed] { return malloryFailed; }));
    // malloryFailed is true only because BookmarkModel::execute(GetBookmark)
    // itself re-checked ownership (loadOwned/requireOwner) -- Local mode
    // contributed nothing to this result. Documented, not smoothed over,
    // per the README's own "Expected strain points" framing.
}
