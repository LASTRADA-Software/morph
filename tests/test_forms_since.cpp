// SPDX-License-Identifier: Apache-2.0
//
// `FieldMeta::since` and `FieldVersion`: the version a field first ships in, emitted as `x-since`, the action's
// `schemaVersion`, and the field-inventory check an application runs against the snapshot of its last release.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/version.hpp>
#include <optional>
#include <string>
#include <vector>

// File-scope (not anonymous-namespaced): glaze's reflection needs a type with
// linkage. Same suppression, for the same reason, as tests/test_forms_blank_as.cpp.
// NOLINTBEGIN(misc-use-internal-linkage)

struct FSRecordAction {
    std::string sample;
    double density = 0.0;
    std::optional<std::string> remark;
    std::string untouched;

    static constexpr std::array<morph::forms::FieldMeta, 3> fieldMetadata{
        morph::forms::FieldMeta{.field = "sample", .label = "Sample"},
        morph::forms::FieldMeta{.field = "density", .since = "6.11.0"},
        morph::forms::FieldMeta{.field = "remark"}.withSince("6.9"),
    };
};

struct FSPlainAction {
    std::string name;
};

struct FSBadSinceAction {
    std::string name;
    std::string code;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "code", .since = "6.x"},
    };
};

// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::forms::FieldInventoryEntry;
using morph::forms::FieldInventoryProblemKind;
using morph::forms::FieldVersion;

FieldVersion version(std::string_view text) {
    auto parsed = FieldVersion::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::string refusal(std::string_view text) {
    auto parsed = FieldVersion::parse(text);
    REQUIRE_FALSE(parsed.has_value());
    return parsed.error();
}

glz::generic_u64 schemaDom(std::string const& schema) {
    glz::generic_u64 dom{};
    REQUIRE_FALSE(schema.empty());
    REQUIRE_FALSE(glz::read_json(dom, schema));
    return dom;
}

std::vector<FieldInventoryProblemKind> kinds(std::vector<morph::forms::FieldInventoryProblem> const& problems) {
    std::vector<FieldInventoryProblemKind> result;
    result.reserve(problems.size());
    for (auto const& problem : problems) {
        result.push_back(problem.kind);
    }
    return result;
}

}  // namespace

TEST_CASE("Forms::FieldVersion::OrdersComponentByComponent", "[forms][since]") {
    CHECK(version("6.11.0") > version("6.9.5"));
    CHECK(version("6.9.5") < version("6.11"));
    CHECK(version("7") > version("6.99.99"));
    CHECK(version("6.11.1") > version("6.11"));
    CHECK(version("6.11") < version("6.11.1"));
    CHECK(version("1.2.3") == version("1.2.3"));
}

TEST_CASE("Forms::FieldVersion::MissingTrailingComponentsAreZero", "[forms][since]") {
    CHECK(version("6.11") == version("6.11.0"));
    CHECK(version("6.11.0.0") == version("6.11"));
    CHECK((version("6.11") <=> version("6.11.0")) == std::strong_ordering::equal);
    CHECK(version("0") == FieldVersion{});
    CHECK(version("0.0") == FieldVersion{});
    CHECK(version("0.0.1") > FieldVersion{});
}

TEST_CASE("Forms::FieldVersion::KeepsTheTextAsWritten", "[forms][since]") {
    CHECK(version("6.11.0").text() == "6.11.0");
    CHECK(FieldVersion{}.text() == "0");
    CHECK(version("6.11.0").components().size() == 3);
    CHECK(FieldVersion{}.components().empty());
    CHECK(version("4294967295").components().front() == 4294967295U);
}

TEST_CASE("Forms::FieldVersion::RefusesWhatIsNotADottedNumber", "[forms][since]") {
    CHECK(refusal("").contains("must not be empty"));
    CHECK(refusal("6..1").contains("empty component"));
    CHECK(refusal(".6").contains("empty component"));
    CHECK(refusal("6.").contains("empty component"));
    CHECK(refusal("6.01").contains("leading zero"));
    CHECK(refusal("6.x").contains("not decimal digits"));
    CHECK(refusal("6.1b").contains("not decimal digits"));
    CHECK(refusal("-1").contains("not decimal digits"));
    CHECK(refusal("v6").contains("'v6' is not a version"));
    CHECK(refusal("4294967296").contains("above 4294967295"));
    CHECK(refusal(std::string(65, '1')).contains("at most 64 bytes"));
    std::string longest;
    for (int index = 0; index < 31; ++index) {
        longest += "0.";
    }
    longest += "10";
    REQUIRE(longest.size() == FieldVersion::kMaxTextBytes);
    CHECK(FieldVersion::parse(longest).has_value());
}

TEST_CASE("Forms::FieldVersion::ParseSinceReadsEmptyAsTheFirstVersion", "[forms][since]") {
    auto const empty = morph::forms::parseSince("");
    REQUIRE(empty.has_value());
    CHECK(*empty == FieldVersion{});
    CHECK(morph::forms::parseSince("6.1").value() == version("6.1"));
    CHECK_FALSE(morph::forms::parseSince("x").has_value());
}

TEST_CASE("Forms::FieldMeta::SinceIsEmittedAsXSince", "[forms][since]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FSRecordAction>());
    REQUIRE(dom["properties"]["density"].contains("x-since"));
    CHECK(dom["properties"]["density"]["x-since"].get<std::string>() == "6.11.0");
    CHECK(dom["properties"]["remark"]["x-since"].get<std::string>() == "6.9");
    CHECK_FALSE(dom["properties"]["sample"].contains("x-since"));
    CHECK_FALSE(dom["properties"]["untouched"].contains("x-since"));
}

TEST_CASE("Forms::FieldMeta::SchemaVersionIsTheHighestSince", "[forms][since]") {
    CHECK(morph::forms::schemaVersion<FSRecordAction>() == version("6.11"));
    CHECK(morph::forms::schemaVersion<FSRecordAction>().text() == "6.11.0");
    CHECK(morph::forms::schemaVersion<FSPlainAction>() == FieldVersion{});
}

TEST_CASE("Forms::FieldMeta::FieldInventoryListsEveryFieldInOrder", "[forms][since]") {
    auto const inventory = morph::forms::fieldInventory<FSRecordAction>();
    REQUIRE(inventory.size() == 4);
    CHECK(inventory.at(0).field == "sample");
    CHECK(inventory.at(0).since.empty());
    CHECK(inventory.at(1).field == "density");
    CHECK(inventory.at(1).since == "6.11.0");
    CHECK(inventory.at(2).since == "6.9");
    CHECK(inventory.at(3).field == "untouched");
    CHECK(inventory.at(3).since.empty());
    for (auto const& entry : inventory) {
        CHECK_FALSE(entry.removed);
    }
}

TEST_CASE("Forms::FieldMeta::ASinceThatIsNotAVersionIsRefused", "[forms][since]") {
    CHECK_THROWS_AS(morph::forms::schemaVersion<FSBadSinceAction>(), morph::forms::InvalidSinceError);
    CHECK_THROWS_AS(morph::forms::fieldInventory<FSBadSinceAction>(), morph::forms::InvalidSinceError);
    try {
        static_cast<void>(morph::forms::detail::mergeSchemaExtras<FSBadSinceAction>(
            glz::write_json_schema<FSBadSinceAction>().value_or(std::string{})));
        FAIL("a since that is not a version must be refused");
    } catch (morph::forms::InvalidSinceError const& error) {
        CHECK(std::string{error.what()}.contains("FSBadSinceAction.code"));
        CHECK(std::string{error.what()}.contains("'6.x' is not a version"));
    }
}

TEST_CASE("Forms::FieldInventory::AnUnchangedInventoryHasNoProblem", "[forms][since]") {
    auto const inventory = morph::forms::fieldInventory<FSRecordAction>();
    CHECK(morph::forms::checkFieldInventory(inventory, inventory).empty());
    CHECK(morph::forms::checkFieldInventory({}, inventory).empty());
}

TEST_CASE("Forms::FieldInventory::AFieldAddedNeedsASinceAboveTheLastRelease", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{{.field = "sample", .since = ""},
                                                    {.field = "density", .since = "6.11"}};
    std::vector<FieldInventoryEntry> const withoutSince{previous.at(0), previous.at(1), {.field = "pressure"}};
    auto const problems = morph::forms::checkFieldInventory(previous, withoutSince);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().kind == FieldInventoryProblemKind::AddedWithoutNewerSince);
    CHECK(problems.front().field == "pressure");
    CHECK(problems.front().message.contains("above 6.11"));

    // Equal to the highest is not above it.
    std::vector<FieldInventoryEntry> const equal{
        previous.at(0), previous.at(1), {.field = "pressure", .since = "6.11.0"}};
    CHECK(kinds(morph::forms::checkFieldInventory(previous, equal)) ==
          std::vector{FieldInventoryProblemKind::AddedWithoutNewerSince});

    std::vector<FieldInventoryEntry> const above{
        previous.at(0), previous.at(1), {.field = "pressure", .since = "6.12"}};
    CHECK(morph::forms::checkFieldInventory(previous, above).empty());
}

TEST_CASE("Forms::FieldInventory::ARemovedFieldStillRaisesTheHighestSince", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{{.field = "sample"},
                                                    {.field = "legacy", .since = "7.0", .removed = true}};
    std::vector<FieldInventoryEntry> const current{{.field = "sample"}, {.field = "pressure", .since = "6.12"}};
    CHECK(kinds(morph::forms::checkFieldInventory(previous, current)) ==
          std::vector{FieldInventoryProblemKind::AddedWithoutNewerSince});
}

TEST_CASE("Forms::FieldInventory::ASinceThatChangedIsAProblem", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{{.field = "density", .since = "6.11"}};
    std::vector<FieldInventoryEntry> const changed{{.field = "density", .since = "6.12"}};
    auto const problems = morph::forms::checkFieldInventory(previous, changed);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().kind == FieldInventoryProblemKind::SinceChanged);
    CHECK(problems.front().message.contains("shipped with since 6.11 and now declares 6.12"));

    std::vector<FieldInventoryEntry> const dropped{{.field = "density", .since = ""}};
    CHECK(kinds(morph::forms::checkFieldInventory(previous, dropped)) ==
          std::vector{FieldInventoryProblemKind::SinceChanged});

    // The same version, written differently, has not changed.
    std::vector<FieldInventoryEntry> const respelled{{.field = "density", .since = "6.11.0"}};
    CHECK(morph::forms::checkFieldInventory(previous, respelled).empty());
}

TEST_CASE("Forms::FieldInventory::AFieldThatReappearsAfterRemovalIsAProblem", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{{.field = "sample"},
                                                    {.field = "legacy", .since = "6.1", .removed = true}};
    std::vector<FieldInventoryEntry> const current{{.field = "sample"}, {.field = "legacy", .since = "6.1"}};
    auto const problems = morph::forms::checkFieldInventory(previous, current);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().kind == FieldInventoryProblemKind::Reappeared);
    CHECK(problems.front().field == "legacy");
}

TEST_CASE("Forms::FieldInventory::ASinceThatIsNotAVersionIsAProblem", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{{.field = "sample", .since = "six"}};
    std::vector<FieldInventoryEntry> const current{{.field = "sample", .since = "6..0"}};
    auto const problems = morph::forms::checkFieldInventory(previous, current);
    CHECK(kinds(problems) ==
          std::vector{FieldInventoryProblemKind::InvalidSince, FieldInventoryProblemKind::InvalidSince});
    REQUIRE(problems.size() == 2);
    CHECK(problems.at(0).message.contains("the snapshot's 'sample'"));
    CHECK(problems.at(1).message.contains("'6..0' is not a version"));

    // A snapshot entry that is not a version cannot be compared, so a valid current since is not "changed".
    std::vector<FieldInventoryEntry> const valid{{.field = "sample", .since = "6.0"}};
    CHECK(kinds(morph::forms::checkFieldInventory(previous, valid)) ==
          std::vector{FieldInventoryProblemKind::InvalidSince});
}

TEST_CASE("Forms::FieldInventory::RemovingAFieldIsAllowedAndCurrentRemovedEntriesAreSkipped", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{{.field = "sample"}, {.field = "legacy"}};
    std::vector<FieldInventoryEntry> const current{{.field = "sample"},
                                                   {.field = "ghost", .since = "", .removed = true}};
    CHECK(morph::forms::checkFieldInventory(previous, current).empty());
}

TEST_CASE("Forms::FieldInventory::TheNextSnapshotKeepsRemovedFields", "[forms][since]") {
    std::vector<FieldInventoryEntry> const previous{
        {.field = "sample"}, {.field = "legacy", .since = "6.1"}, {.field = "older", .since = "", .removed = true}};
    std::vector<FieldInventoryEntry> const current{{.field = "sample"}, {.field = "pressure", .since = "6.12"}};
    auto const next = morph::forms::nextFieldInventory(previous, current);
    REQUIRE(next.size() == 4);
    CHECK(next.at(0).field == "sample");
    CHECK_FALSE(next.at(0).removed);
    CHECK(next.at(1).field == "pressure");
    CHECK_FALSE(next.at(1).removed);
    CHECK(next.at(2).field == "legacy");
    CHECK(next.at(2).since == "6.1");
    CHECK(next.at(2).removed);
    CHECK(next.at(3).field == "older");
    CHECK(next.at(3).removed);

    // The kept snapshot round-trips as JSON and catches the removed name coming back.
    std::string json;
    REQUIRE_FALSE(glz::write_json(next, json));
    std::vector<FieldInventoryEntry> restored;
    REQUIRE_FALSE(glz::read_json(restored, json));
    std::vector<FieldInventoryEntry> const back{
        {.field = "sample"}, {.field = "pressure", .since = "6.12"}, {.field = "legacy", .since = "6.1"}};
    CHECK(kinds(morph::forms::checkFieldInventory(restored, back)) ==
          std::vector{FieldInventoryProblemKind::Reappeared});
}
