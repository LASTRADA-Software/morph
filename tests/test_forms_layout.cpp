// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/layout.hpp>
#include <string>
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

// Deliberately at file scope, not inside an anonymous namespace: Task 2 below
// reuses PlainAction with morph::forms::schemaJson<PlainAction>(), which
// (like every other schemaJson<> fixture in tests/test_quantity_forms.cpp,
// e.g. QFRecordMeasurement) needs external linkage for glaze's reflection
// name-mangling (glz::detail::get_name_impl) to resolve — an anonymous-
// namespace type has no linkage and fails to compile there.
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

static_assert(!morph::forms::detail::HasFormLayout<PlainAction>);
static_assert(!morph::forms::detail::HasFieldSpans<PlainAction>);
static_assert(morph::forms::detail::HasFormLayout<LayoutOnlyAction>);
static_assert(!morph::forms::detail::HasFieldSpans<LayoutOnlyAction>);
static_assert(!morph::forms::detail::HasFormLayout<SpansOnlyAction>);
static_assert(morph::forms::detail::HasFieldSpans<SpansOnlyAction>);

// ---------------------------------------------------------------------------
// schemaJson<A>(): x-layout / x-group / x-section
// ---------------------------------------------------------------------------

// Also at file scope, for the same reason as PlainAction above — every type
// below is used with schemaJson<>().
struct LayoutGrouped {
    std::int64_t sampleId = 0;
    std::int64_t density = 0;
    std::int64_t moisture = 0;
    std::string notes;
    std::string remarks;  // deliberately not named in any group

    static constexpr std::array<std::string_view, 1> kIdent{"sampleId"};
    static constexpr std::array<std::string_view, 2> kMeas{"density", "moisture"};
    static constexpr std::array<std::string_view, 1> kNote{"notes"};

    static constexpr std::array formLayout{
        FieldGroup{.title = "Identity", .fields = kIdent},
        FieldGroup{.title = "Measurement", .fields = kMeas},
        FieldGroup{.title = "Notes", .kind = GroupKind::Accordion, .fields = kNote},
    };
};

struct LayoutWithTabs {
    std::int64_t a = 0;
    std::int64_t b = 0;

    static constexpr std::array<std::string_view, 1> kA{"a"};
    static constexpr std::array<std::string_view, 1> kB{"b"};
    static constexpr std::array formLayout{
        FieldGroup{.title = "One", .kind = GroupKind::Tab, .fields = kA},
        FieldGroup{.title = "Two", .kind = GroupKind::Tab, .fields = kB},
    };
};

struct LayoutBadGroupField {
    std::int64_t sampleId = 0;

    static constexpr std::array<std::string_view, 1> kBad{"doesNotExist"};
    static constexpr std::array formLayout{
        FieldGroup{.title = "Ghost", .fields = kBad},
    };
};

struct LayoutDuplicateMembership {
    std::int64_t sampleId = 0;

    static constexpr std::array<std::string_view, 1> kOnce{"sampleId"};
    static constexpr std::array formLayout{
        FieldGroup{.title = "First", .fields = kOnce},
        FieldGroup{.title = "Second", .fields = kOnce},
    };
};

TEST_CASE("Forms::SchemaJson::NoFormLayoutOrFieldSpansEmitsNoLayoutKeys", "[forms][layout]") {
    // Regression guard: an action declaring neither formLayout nor
    // fieldSpans must not gain any of the four new keys — schemaJson<A>()
    // stays exactly what it was before this feature existed.
    auto const schema = morph::forms::schemaJson<PlainAction>();
    CHECK_FALSE(schema.contains("x-layout"));
    CHECK_FALSE(schema.contains("x-group"));
    CHECK_FALSE(schema.contains("x-section"));
    CHECK_FALSE(schema.contains("x-colspan"));
}

TEST_CASE("Forms::SchemaJson::FormLayoutEmitsXLayoutGroupsInDeclarationOrder", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<LayoutGrouped>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));  // still valid JSON

    // The top-level wrapper, and three declared groups each carrying
    // title/kind/fields in that order.
    CHECK(schema.contains(R"("x-layout":{"groups":[)"));
    CHECK(schema.contains(R"({"title":"Identity","kind":"section","fields":["sampleId"]})"));
    CHECK(schema.contains(R"({"title":"Measurement","kind":"section","fields":["density","moisture"]})"));
    CHECK(schema.contains(R"({"title":"Notes","kind":"accordion","fields":["notes"]})"));

    // Every grouped field carries its group title and 0-based section index.
    CHECK(schema.contains(R"("x-group":"Identity")"));
    CHECK(schema.contains(R"("x-group":"Measurement")"));
    CHECK(schema.contains(R"("x-group":"Notes")"));
    CHECK(schema.contains(R"("x-section":0)"));
    CHECK(schema.contains(R"("x-section":1)"));
    CHECK(schema.contains(R"("x-section":2)"));

    // "remarks" is the 5th declared member (index 4) and belongs to no
    // group: it keeps its ordinary x-order but gets no x-group/x-section —
    // the implicit trailing default group is a renderer-side concept, not a
    // schema tag.
    CHECK(schema.contains(R"("x-order":4)"));
    CHECK_FALSE(schema.contains(R"("x-section":3)"));
}

TEST_CASE("Forms::SchemaJson::TabKindSurfacesInXLayout", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<LayoutWithTabs>();
    CHECK(schema.contains(R"({"title":"One","kind":"tab","fields":["a"]})"));
    CHECK(schema.contains(R"({"title":"Two","kind":"tab","fields":["b"]})"));
}

TEST_CASE("Forms::SchemaJson::GroupNamingNonexistentFieldIsIgnored", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<LayoutBadGroupField>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));  // never throws, stays valid JSON

    // The group survives (title/kind) but its "fields" array is empty — the
    // phantom name never became a schema property.
    CHECK(schema.contains(R"({"title":"Ghost","kind":"section","fields":[]})"));
    CHECK_FALSE(schema.contains("doesNotExist"));
}

TEST_CASE("Forms::SchemaJson::FieldClaimedByTwoGroupsKeepsTheFirst", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<LayoutDuplicateMembership>();
    // "First" claims sampleId in its "fields" array; "Second" declares the
    // same field but does not re-claim it.
    CHECK(schema.contains(R"({"title":"First","kind":"section","fields":["sampleId"]})"));
    CHECK(schema.contains(R"({"title":"Second","kind":"section","fields":[]})"));
    // The property is tagged with the first group's identity, not the second's.
    CHECK(schema.contains(R"("x-group":"First")"));
    CHECK_FALSE(schema.contains(R"("x-group":"Second")"));
}

// ---------------------------------------------------------------------------
// schemaJson<A>(): x-colspan
// ---------------------------------------------------------------------------

struct SpannedAction {
    std::int64_t sampleId = 0;
    std::string notes;
    std::string code;

    static constexpr std::array fieldSpans{
        FieldSpan{.field = "notes", .colspan = 2},
        FieldSpan{.field = "code", .colspan = 1},          // explicit default: emits nothing
        FieldSpan{.field = "doesNotExist", .colspan = 3},  // ignored, never thrown
    };
};

TEST_CASE("Forms::SchemaJson::ColspanGreaterThanOneIsEmitted", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<SpannedAction>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));  // still valid JSON; never throws

    CHECK(schema.contains(R"("x-colspan":2)"));
}

TEST_CASE("Forms::SchemaJson::ColspanOfOneEmitsNothing", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<SpannedAction>();
    // "code" declares colspan == 1 explicitly: the default width is never
    // advertised, keeping the schema minimal.
    CHECK_FALSE(schema.contains(R"("x-colspan":1)"));
}

TEST_CASE("Forms::SchemaJson::SpanNamingNonexistentFieldIsIgnored", "[forms][layout]") {
    auto const schema = morph::forms::schemaJson<SpannedAction>();
    CHECK_FALSE(schema.contains("doesNotExist"));
}

TEST_CASE("Forms::SchemaJson::NoFieldSpansEmitsNoColspan", "[forms][layout]") {
    // LayoutGrouped (Task 2) declares formLayout but no fieldSpans.
    auto const schema = morph::forms::schemaJson<LayoutGrouped>();
    CHECK_FALSE(schema.contains("x-colspan"));
}

// ---------------------------------------------------------------------------
// x-submitMode — the emitter half of explicit submit mode (morph#208)
//
// The renderer has consumed `x-submitMode` since it shipped, but nothing in
// C++ emitted it, so no *generated* schema could carry it and ten call sites
// hand-wrote `DynamicForm { controller: null }` plus an external Button
// instead.
//
// These fixtures are deliberately driven through `schemaJson<A>()` rather than
// a hand-written schema literal. `tst_DynamicFormSubmitMode.qml` already covers
// the renderer against a literal, and would keep passing with the emitter
// removed entirely — only a generated fixture can fail for that reason.
// ---------------------------------------------------------------------------

// NOLINTBEGIN(misc-use-internal-linkage) -- see below: an anonymous namespace
// is exactly what these types cannot have.
// Deliberately *not* anonymous-namespaced: glaze's reflection takes the address
// of an `extern const T`, which a type with no linkage cannot have, so an
// anonymous-namespace action fails to compile. The SM prefix keeps these unique
// across translation units instead -- the repo has a CI check for file-scope
// type-name collisions, which would otherwise be an ODR violation.
struct SMExplicitAction {
    std::string title;
    static constexpr bool explicitSubmit = true;
};

struct SMDefaultAction {
    std::string title;
};

// Declaring it false is the same statement as not declaring it at all.
struct SMOptedOutAction {
    std::string title;
    static constexpr bool explicitSubmit = false;
};
// NOLINTEND(misc-use-internal-linkage)

TEST_CASE("schemaJson emits x-submitMode:\"explicit\" for an action that declares explicitSubmit",
          "[forms][submitmode]") {
    auto const schema = morph::forms::schemaJson<SMExplicitAction>();
    // Exact key/value text: a bare "explicit" substring would also match a
    // description or a field named for it.
    CHECK(schema.contains(R"("x-submitMode":"explicit")"));
}

TEST_CASE("schemaJson omits x-submitMode for an action that declares nothing", "[forms][submitmode]") {
    // Zero behaviour change for every existing action: this is what makes the
    // emitter safe to add without touching a single shipped schema.
    auto const schema = morph::forms::schemaJson<SMDefaultAction>();
    CHECK_FALSE(schema.contains("x-submitMode"));
}

TEST_CASE("schemaJson omits x-submitMode when explicitSubmit is declared false", "[forms][submitmode]") {
    auto const schema = morph::forms::schemaJson<SMOptedOutAction>();
    CHECK_FALSE(schema.contains("x-submitMode"));
}

TEST_CASE("x-submitMode is a top-level key, like x-layout", "[forms][submitmode]") {
    // Not a property node: the renderer reads it off the schema root. Parsed
    // rather than substring-matched so a key that landed inside `properties`
    // (the mistake worth catching) fails here.
    auto const schema = morph::forms::schemaJson<SMExplicitAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    auto const& root = parsed.value();
    REQUIRE(root.contains("x-submitMode"));
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    CHECK(root["x-submitMode"].get<std::string>() == "explicit");
    REQUIRE(root.contains("properties"));
    CHECK_FALSE(root["properties"].contains("x-submitMode"));
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}
