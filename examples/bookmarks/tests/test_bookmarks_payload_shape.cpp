// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's payload-shape fingerprints
// (`morph::model::payloadShapeString` / `payloadFingerprint`,
// `morph/core/payload_schema.hpp`).
//
// `BookmarkId`, `TagId`, `Cursor`, `ImportOpId` and `AuthToken` all carry
// their own `glz::meta` -- on the wire each *is* its nullable underlying
// scalar, which is what makes `BRIDGE_REGISTER_ACTION` on a DTO carrying one
// compile at all (`bookmarks/core/types.hpp`'s own `glz::meta` section). The
// cost, stated in `morph/core/payload_shape_tag.hpp` and in
// `docs/spec/journal/journal.md`'s "Custom-codec types name themselves", is
// that a custom-codec type has no reflected members for `payloadShape` to
// decompose: absent a declared `morph::model::PayloadShapeTag` it renders as
// the bare opaque `x`, indistinguishable from every other such type.
//
// Three of the five -- `BookmarkId`, `TagId`, `Cursor` -- are
// `std::optional<std::int64_t>` on the wire, so a retype between any two of
// them produces byte-identical JSON: no decode on any path can catch it, and
// the shape tag is the only place it is visible at all. The two that are most
// easily confused are the ones this rung deliberately keeps apart:
// `ListBookmarks::cursor` is a keyset cursor *over* bookmark row ids
// (`core/types.hpp`), so "a cursor is just a bookmark id" is a plausible one-
// line edit, and `RenameTag::id` names a tag in a rung where nearly every
// other action's `id` names a bookmark.
//
// Without declared tags, a build that made either edit stamps its journal
// entries with a fingerprint bit-identical to this build's, so
// `journal::replay()`'s mismatch gate has nothing to fire on and the recorded
// integer decodes into the wrong slot -- a tag renamed by bookmark id.
//
// These cases pin the tags that close it, and the refusal that follows.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "bookmarks/core/types.hpp"
#include "bookmarks/dto/auth_dto.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/tag_model.hpp"
#include "testkit/db_fixture.hpp"

using morph::ladder::testkit::DbFixture;
using morph::model::payloadFingerprint;
using morph::model::payloadShapeString;

// A named namespace, not an anonymous one: glaze's traditional reflection
// derives member names from a pointer-to-member mangling that requires the
// reflected type to have linkage, so a payload struct declared in an anonymous
// namespace does not compile -- see ledger's identical fixture namespace
// (`examples/ledger/tests/test_ledger_payload_shape.cpp`) for the same
// rationale, spelled out in full there.
namespace bookmarks_payload_shape_fixtures {

/// @brief `RenameTag` with its `id` field *retyped* from `TagId` to
///        `BookmarkId`, the member name left alone -- the shape a build that
///        made that one edit would stamp its entries with.
///
///        Never registered: it exists only to produce a fingerprint, which is
///        the whole of what a retained journal hands a later reader.
struct RenameTagIdRetyped {
    bookmarks::BookmarkId id;
    std::string name;
};

/// @brief `ListBookmarks` with `cursor` retyped from `Cursor` to
///        `BookmarkId`, same idea. The enum and string members are carried
///        verbatim so the two renderings differ in exactly one place.
struct ListBookmarksCursorRetyped {
    bookmarks::BookmarkId cursor;
    bookmarks::ReadFilter readFilter = bookmarks::ReadFilter::Any;
    bookmarks::ArchiveFilter archiveFilter = bookmarks::ArchiveFilter::ActiveOnly;
    std::string tag;
    std::string searchText;
};

}  // namespace bookmarks_payload_shape_fixtures

using bookmarks_payload_shape_fixtures::ListBookmarksCursorRetyped;
using bookmarks_payload_shape_fixtures::RenameTagIdRetyped;

namespace {

/// @brief A `Context` carrying only @p principal -- not a designated
///        initializer, for the reason `test_tag_model.cpp`'s own `contextFor`
///        gives (`-Wmissing-designated-field-initializers` under
///        `-Weverything`).
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

/// @brief A `CreateBookmark` for @p url tagged @p tag. See `contextFor` for
///        why this is not a designated initializer.
[[nodiscard]] bookmarks::CreateBookmark makeCreate(std::string url, std::string tag) {
    bookmarks::CreateBookmark action;
    action.url = std::move(url);
    action.tags = {std::move(tag)};
    return action;
}

}  // namespace

// ── The tags themselves ──────────────────────────────────────────────────────

TEST_CASE("Every bookmarks strong id and the auth token render a distinct payload shape",
          "[bookmarks][journal][payload_shape]") {
    INFO("BookmarkId  -> " << payloadShapeString<bookmarks::BookmarkId>());
    INFO("TagId       -> " << payloadShapeString<bookmarks::TagId>());
    INFO("Cursor      -> " << payloadShapeString<bookmarks::Cursor>());
    INFO("ImportOpId  -> " << payloadShapeString<bookmarks::ImportOpId>());
    INFO("AuthToken   -> " << payloadShapeString<bookmarks::AuthToken>());

    const std::vector<std::string> shapes{
        payloadShapeString<bookmarks::BookmarkId>(), payloadShapeString<bookmarks::TagId>(),
        payloadShapeString<bookmarks::Cursor>(),     payloadShapeString<bookmarks::ImportOpId>(),
        payloadShapeString<bookmarks::AuthToken>(),
    };

    // None of them may still be the bare opaque tag, and no two may collide.
    for (const auto& shape : shapes) {
        CHECK(shape != "x");
    }
    auto sorted = shapes;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
}

// ── What the tags buy: the retypes the fingerprint can now see ───────────────

TEST_CASE("RenameTag's id is not interchangeable with a bookmark id", "[bookmarks][journal][payload_shape]") {
    INFO("RenameTag   -> " << payloadShapeString<bookmarks::RenameTag>());
    INFO("id retyped  -> " << payloadShapeString<RenameTagIdRetyped>());

    CHECK(payloadShapeString<bookmarks::RenameTag>() != payloadShapeString<RenameTagIdRetyped>());
    CHECK(payloadFingerprint<bookmarks::RenameTag>() != payloadFingerprint<RenameTagIdRetyped>());
}

TEST_CASE("ListBookmarks' cursor is not interchangeable with a bookmark id", "[bookmarks][journal][payload_shape]") {
    INFO("ListBookmarks   -> " << payloadShapeString<bookmarks::ListBookmarks>());
    INFO("cursor retyped  -> " << payloadShapeString<ListBookmarksCursorRetyped>());

    CHECK(payloadShapeString<bookmarks::ListBookmarks>() != payloadShapeString<ListBookmarksCursorRetyped>());
    CHECK(payloadFingerprint<bookmarks::ListBookmarks>() != payloadFingerprint<ListBookmarksCursorRetyped>());
}

// ── The refusal ──────────────────────────────────────────────────────────────

TEST_CASE("replay() refuses a RenameTag entry stamped by a build whose id was a bookmark id",
          "[bookmarks][journal][payload_shape]") {
    // A real recorded entry, not a hand-built one. This rung journals through
    // morph's own registry execution site (`bookmarks::App` installs a
    // process-wide log via `journal::setActionLog`), so the entry is produced
    // by dispatching through that site rather than by calling `execute()`
    // directly -- which also pins that the stamp on it is the fingerprint this
    // build computes.
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};

    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    bookmarkModel.execute(makeCreate("https://one.example", "old"));
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    REQUIRE(tags.size() == 1);

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    auto holder = morph::model::detail::defaultRegistry().create("TagModel");
    REQUIRE(holder != nullptr);
    holder->attachActionLog(log, std::string{});

    bookmarks::RenameTag rename;
    rename.id = tags.front().id;
    rename.name = "new";
    morph::model::detail::defaultDispatcher().dispatch(
        "TagModel", "RenameTag", *holder, morph::model::ActionTraits<bookmarks::RenameTag>::toJson(rename));

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries.front().schema == payloadFingerprint<bookmarks::RenameTag>());

    // Re-stamp it as the retyped build would have. The payload bytes are
    // byte-identical either way -- one JSON integer under one field name -- so
    // the fingerprint is the only evidence that the recorded `id` is not this
    // build's `TagId`, and replay() must refuse rather than rename whatever tag
    // happens to share that row id.
    entries.front().schema = payloadFingerprint<RenameTagIdRetyped>();
    REQUIRE_THROWS_AS(morph::journal::replay("TagModel", entries), morph::journal::SchemaMismatchError);
}
