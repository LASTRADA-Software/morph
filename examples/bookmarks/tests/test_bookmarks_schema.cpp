// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/imported_op_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"
#include "testkit/db_fixture.hpp"

using morph::ladder::testkit::DbFixture;

TEST_CASE("The bookmarks schema creates all four tables and a bookmark round-trips", "[bookmarks][schema]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;

    bookmarks::db::BookmarkRecord rec;
    rec.ownerPrincipal = "alice";
    rec.url = "https://example.com";
    rec.title = "Example";
    rec.createdAtMs = 1000;
    rec.updatedAtMs = 1000;
    mapper.Create(rec);
    REQUIRE(rec.id.Value() > 0);

    bookmarks::db::TagRecord tag;
    tag.ownerPrincipal = "alice";
    tag.name = "example";
    mapper.Create(tag);
    REQUIRE(tag.id.Value() > 0);

    bookmarks::db::BookmarkTagRecord junction;
    junction.bookmark = rec.id.Value();
    junction.tag = tag.id.Value();
    mapper.Create(junction);
    REQUIRE(junction.id.Value() > 0);

    bookmarks::db::ImportedOpRecord op;
    op.ownerPrincipal = "alice";
    op.opId = "chunk-1";
    op.appliedAtMs = 1000;
    mapper.Create(op);
    REQUIRE(op.id.Value() > 0);

    // Tag reads go through a plain query, never an embedded relation field
    // (Global Constraints) -- proving that path works end-to-end here.
    auto rows = mapper.Query<bookmarks::db::BookmarkTagRecord>()
                    .Where(Lightweight::FieldNameOf<&bookmarks::db::BookmarkTagRecord::bookmark>, "=", rec.id.Value())
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().tag.Value() == tag.id.Value());
}

TEST_CASE("Duplicate (ownerPrincipal, name) tags are rejected by the unique index", "[bookmarks][schema]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    bookmarks::db::TagRecord first;
    first.ownerPrincipal = "alice";
    first.name = "dup";
    mapper.Create(first);

    bookmarks::db::TagRecord second;
    second.ownerPrincipal = "alice";
    second.name = "dup";
    CHECK_THROWS_AS(mapper.Create(second), Lightweight::SqlException);

    // A different owner may reuse the same name -- the index is scoped per owner.
    bookmarks::db::TagRecord thirdOwner;
    thirdOwner.ownerPrincipal = "bob";
    thirdOwner.name = "dup";
    CHECK_NOTHROW(mapper.Create(thirdOwner));
}

TEST_CASE("BookmarkRecord has no relation-typed member -- Update() must compile", "[bookmarks][schema]") {
    // A compile-time proof, not a runtime assertion: if BookmarkRecord ever
    // grows an embedded HasMany/HasManyThrough field, this line stops
    // compiling with the exact "no member IsModified" error the Global
    // Constraints section documents -- catching the regression at build
    // time, in the one file whose entire job is proving this works.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    bookmarks::db::BookmarkRecord rec;
    rec.ownerPrincipal = "alice";
    rec.url = "https://example.com";
    rec.createdAtMs = 1;
    rec.updatedAtMs = 1;
    mapper.Create(rec);
    rec.title = "Changed";
    CHECK_NOTHROW(mapper.Update(rec));
}
