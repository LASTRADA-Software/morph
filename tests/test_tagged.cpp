// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/util/tagged.hpp>
#include <string>
#include <type_traits>

using morph::util::Tagged;

namespace {

using UserId = Tagged<std::string, "UserId">;
using AccountId = Tagged<std::string, "AccountId">;
using EventSeq = Tagged<std::int64_t, "EventSeq">;

}  // namespace

// Global scope on purpose: glaze reflection needs a type with linkage.
struct TaggedProbe {
    UserId user;
    EventSeq seq;
};

TEST_CASE("Tagged::ConstructionAndAccess", "[tagged]") {
    UserId const id{"u-42"};
    CHECK(*id == "u-42");
    CHECK(id.get() == "u-42");

    EventSeq const seq{7};
    CHECK(*seq == 7);
}

TEST_CASE("Tagged::TypeSafeIdentity", "[tagged]") {
    // Two Tagged<std::string, ...> with different tags are distinct types:
    // neither constructs from nor is constructible from the other.
    static_assert(!std::is_convertible_v<UserId, AccountId>);
    static_assert(!std::is_constructible_v<AccountId, UserId>);
    static_assert(!std::is_same_v<UserId, AccountId>);
}

TEST_CASE("Tagged::IsTaggedTrait", "[tagged]") {
    static_assert(morph::util::isTagged<UserId>);
    static_assert(morph::util::isTagged<EventSeq>);
    static_assert(morph::util::isTagged<const UserId&>);
    static_assert(!morph::util::isTagged<std::string>);
    static_assert(!morph::util::isTagged<std::int64_t>);
}

TEST_CASE("Tagged::Equality", "[tagged]") {
    CHECK(UserId{"a"} == UserId{"a"});
    CHECK_FALSE(UserId{"a"} == UserId{"b"});
}

TEST_CASE("Tagged::HasValueAlwaysEngaged", "[tagged]") {
    // Tagged is a required opaque scalar, not an empty-capable field: it always
    // reports engaged so it composes with the forms palette without being
    // mistaken for an optional field.
    UserId const id{"u-1"};
    CHECK(id.hasValue());
    static_assert(noexcept(id.hasValue()));
}

TEST_CASE("Tagged::WireIsTransparentScalar", "[tagged][wire]") {
    UserId const id{"u-99"};
    auto const written = glz::write_json(id);
    REQUIRE(written.has_value());
    CHECK(*written == R"("u-99")");

    UserId restored{};
    REQUIRE_FALSE(glz::read_json(restored, R"("u-99")"));
    CHECK(restored == id);

    EventSeq const seq{123};
    auto const writtenSeq = glz::write_json(seq);
    REQUIRE(writtenSeq.has_value());
    CHECK(*writtenSeq == "123");
}

TEST_CASE("Tagged::WireAsStructMember", "[tagged][wire]") {
    TaggedProbe const probe{.user = UserId{"u-1"}, .seq = EventSeq{5}};
    auto const written = glz::write_json(probe);
    REQUIRE(written.has_value());
    CHECK(*written == R"({"user":"u-1","seq":5})");

    TaggedProbe restored{};
    REQUIRE_FALSE(glz::read_json(restored, *written));
    CHECK(restored.user == probe.user);
    CHECK(restored.seq == probe.seq);
}

TEST_CASE("Tagged::JsonSchemaMatchesUnderlyingType", "[tagged][schema]") {
    // The generated schema for the tagged scalar has the same shape (type,
    // and any bounds) as its underlying type's schema -- transparent wire
    // serialization means the wire-facing part of the schema is identical.
    // Only the `title` differs: it carries the compile-time tag, so distinct
    // tags produce distinctly labelled schema entries even when T is shared.
    auto const taggedSchema = glz::write_json_schema<UserId>();
    auto const plainSchema = glz::write_json_schema<std::string>();
    REQUIRE(taggedSchema.has_value());
    REQUIRE(plainSchema.has_value());
    CHECK(taggedSchema->find(R"("type":"string")") != std::string::npos);
    CHECK(taggedSchema->find(R"("title":"UserId")") != std::string::npos);

    auto const taggedIntSchema = glz::write_json_schema<EventSeq>();
    auto const plainIntSchema = glz::write_json_schema<std::int64_t>();
    REQUIRE(taggedIntSchema.has_value());
    REQUIRE(plainIntSchema.has_value());
    CHECK(taggedIntSchema->find(R"("type":"integer")") != std::string::npos);
    CHECK(taggedIntSchema->find(R"("title":"EventSeq")") != std::string::npos);
    // Same numeric bounds as the underlying int64_t (schema is otherwise
    // untouched by the tag).
    CHECK(taggedIntSchema->find(R"("minimum":-9223372036854775808)") != std::string::npos);
    CHECK(taggedIntSchema->find(R"("maximum":9223372036854775807)") != std::string::npos);
}
