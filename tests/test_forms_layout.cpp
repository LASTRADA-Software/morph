// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/forms/layout.hpp>
#include <string_view>

using morph::forms::FieldGroup;
using morph::forms::FieldSpan;
using morph::forms::GroupKind;
using morph::forms::groupKindName;

// ---------------------------------------------------------------------------
// GroupKind / groupKindName
// ---------------------------------------------------------------------------

static_assert(groupKindName(GroupKind::Section) == "section");
static_assert(groupKindName(GroupKind::Tab) == "tab");
static_assert(groupKindName(GroupKind::Accordion) == "accordion");

TEST_CASE("groupKindName maps every GroupKind to its wire string", "[forms][layout]") {
    CHECK(groupKindName(GroupKind::Section) == "section");
    CHECK(groupKindName(GroupKind::Tab) == "tab");
    CHECK(groupKindName(GroupKind::Accordion) == "accordion");
}

// ---------------------------------------------------------------------------
// FieldGroup / FieldSpan aggregates
// ---------------------------------------------------------------------------

namespace {
constexpr std::array<std::string_view, 2> kSampleFields{"sampleId", "density"};
}  // namespace

static_assert(FieldGroup{}.kind == GroupKind::Section);  // Section is the default kind
static_assert(FieldSpan{}.colspan == 1);                 // 1 is the default span

TEST_CASE("FieldGroup carries a title, kind, and member field list", "[forms][layout]") {
    FieldGroup const group{.title = "Identity", .fields = kSampleFields};
    CHECK(group.title == "Identity");
    CHECK(group.kind == GroupKind::Section);  // not overridden
    REQUIRE(group.fields.size() == 2);
    CHECK(group.fields[0] == "sampleId");
    CHECK(group.fields[1] == "density");

    FieldGroup const tab{.title = "Notes", .kind = GroupKind::Tab, .fields = kSampleFields};
    CHECK(tab.kind == GroupKind::Tab);
}

TEST_CASE("FieldSpan defaults colspan to 1", "[forms][layout]") {
    FieldSpan const span{.field = "notes"};
    CHECK(span.field == "notes");
    CHECK(span.colspan == 1);

    FieldSpan const wide{.field = "notes", .colspan = 2};
    CHECK(wide.colspan == 2);
}

// ---------------------------------------------------------------------------
// HasFormLayout / HasFieldSpans concepts
// ---------------------------------------------------------------------------

namespace {

struct PlainAction {
    std::int64_t sampleId = 0;
};

struct LayoutOnlyAction {
    std::int64_t sampleId = 0;

    static constexpr std::array<std::string_view, 1> kIdent{"sampleId"};
    static constexpr std::array formLayout{FieldGroup{.title = "Identity", .fields = kIdent}};
};

struct SpansOnlyAction {
    std::int64_t sampleId = 0;

    static constexpr std::array fieldSpans{FieldSpan{.field = "sampleId", .colspan = 2}};
};

}  // namespace

static_assert(!morph::forms::detail::HasFormLayout<PlainAction>);
static_assert(!morph::forms::detail::HasFieldSpans<PlainAction>);
static_assert(morph::forms::detail::HasFormLayout<LayoutOnlyAction>);
static_assert(!morph::forms::detail::HasFieldSpans<LayoutOnlyAction>);
static_assert(!morph::forms::detail::HasFormLayout<SpansOnlyAction>);
static_assert(morph::forms::detail::HasFieldSpans<SpansOnlyAction>);
