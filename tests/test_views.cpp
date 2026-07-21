// SPDX-License-Identifier: Apache-2.0
//
// Tests for morph::views (docs/spec/forms/views.md): the view-schema layer
// that composes a query action's result rows with an optional row-opener
// action and row/collection action buttons into a list/table or
// master-detail screen descriptor.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/views.hpp>
#include <morph/util/quantity.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// A miniature registered model/action set (unique "Vt" prefix — this file is
// linked into the same morph_tests binary as every other test TU, so type and
// registration names must not collide with any other test file's).
// ---------------------------------------------------------------------------

namespace vt {

/// @brief The demo test's own tiny unit system (mass in "kg"), used only to
///        exercise ExtUnits/x-decimalPlaces derivation on a row's Quantity
///        field. No arithmetic is performed, so no unit-algebra operators
///        are needed.
enum class VtUnit : std::uint8_t { kg };

}  // namespace vt

template <>
struct morph::units::UnitTraits<vt::VtUnit> {
    static constexpr morph::units::UnitMeta meta(vt::VtUnit) noexcept {
        return {.id = "kg", .display = "kg", .defaultDecimals = 2};
    }
};

namespace vt {

using VtMass = morph::units::Quantity<VtUnit::kg>;

struct VtRow {
    std::int64_t id = 0;
    std::string label;
    VtMass weight{};
};

struct VtRowList {
    std::vector<VtRow> rows;
};

struct VtListRows {};

struct VtEditRow {
    std::int64_t id = 0;
    std::string label;
};

struct VtDeleteRow {
    std::int64_t id = 0;
};

struct VtCreateRow {};

class VtModel {
public:
    VtRowList execute(const VtListRows&) {
        return VtRowList{.rows = {{.id = 1, .label = "One"}, {.id = 2, .label = "Two"}}};
    }
    VtRow execute(const VtEditRow& action) { return VtRow{.id = action.id, .label = action.label}; }
    VtRow execute(const VtDeleteRow& action) { return VtRow{.id = action.id, .label = {}}; }
    VtRow execute(const VtCreateRow&) { return VtRow{}; }
};

}  // namespace vt

// BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION paste their type arguments into
// an identifier (`bridge_model_reg_##M`) — a namespace-qualified name like
// `vt::VtModel` cannot be pasted, so bring each type into unqualified scope
// first, exactly like the bottom of examples/forms/lab_model.hpp does.
using vt::VtCreateRow;
using vt::VtDeleteRow;
using vt::VtEditRow;
using vt::VtListRows;
using vt::VtModel;

BRIDGE_REGISTER_MODEL(VtModel, "VtModel")
BRIDGE_REGISTER_ACTION(VtModel, VtListRows, "VtListRows", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(VtModel, VtEditRow, "VtEditRow")
BRIDGE_REGISTER_ACTION(VtModel, VtDeleteRow, "VtDeleteRow")
BRIDGE_REGISTER_ACTION(VtModel, VtCreateRow, "VtCreateRow")

// ---------------------------------------------------------------------------
// Task 1: descriptor/concept unit tests.
// ---------------------------------------------------------------------------

TEST_CASE("Views::DescribeAction::DefaultsAndOverrides", "[views]") {
    using morph::views::ActionScope;
    using morph::views::describeAction;

    constexpr auto plain = describeAction<vt::VtDeleteRow>();
    STATIC_REQUIRE(plain.actionTypeId == "VtDeleteRow");
    STATIC_REQUIRE(plain.label.empty());
    STATIC_REQUIRE(plain.scope == ActionScope::Row);
    STATIC_REQUIRE(plain.bind.empty());
    STATIC_REQUIRE_FALSE(plain.confirm);

    static constexpr std::array<morph::views::BindEntry, 1> bind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"}};
    constexpr auto decorated = describeAction<vt::VtDeleteRow>("Delete", ActionScope::Row, bind, true);
    STATIC_REQUIRE(decorated.label == "Delete");
    STATIC_REQUIRE(decorated.bind.size() == 1);
    STATIC_REQUIRE(decorated.bind[0].actionField == "id");
    STATIC_REQUIRE(decorated.confirm);

    constexpr auto collectionScoped = describeAction<vt::VtCreateRow>("New", ActionScope::Collection);
    STATIC_REQUIRE(collectionScoped.actionTypeId == "VtCreateRow");
    STATIC_REQUIRE(collectionScoped.scope == ActionScope::Collection);
}

TEST_CASE("Views::ColumnOverride::Defaults", "[views]") {
    constexpr morph::views::ColumnOverride bare{.field = "label"};
    STATIC_REQUIRE(bare.field == "label");
    STATIC_REQUIRE(bare.label.empty());
    STATIC_REQUIRE_FALSE(bare.hidden);
}

TEST_CASE("Views::KindNames", "[views]") {
    using morph::views::detail::viewKindName;
    STATIC_REQUIRE(viewKindName<morph::views::CollectionView> == "collection");
    STATIC_REQUIRE(viewKindName<morph::views::MasterDetailView> == "master-detail");
}

namespace {

struct NoOverride {};

struct WithOverride {
    static constexpr std::array<morph::views::ColumnOverride, 1> columns{
        morph::views::ColumnOverride{.field = "label"}};
};

struct NoRowActionOrTitle {};

struct WithRowActionAndTitle {
    static constexpr std::string_view title = "Rows";
    static constexpr std::string_view rowKey = "rowId";
    static constexpr auto rowAction = morph::views::describeAction<vt::VtEditRow>();
    static constexpr std::array<morph::views::ActionDescriptor, 1> actions{
        morph::views::describeAction<vt::VtDeleteRow>("Delete")};
};

}  // namespace

TEST_CASE("Views::DetectionConcepts", "[views]") {
    namespace vd = morph::views::detail;
    STATIC_REQUIRE_FALSE(vd::HasColumnOverrides<NoOverride>);
    STATIC_REQUIRE(vd::HasColumnOverrides<WithOverride>);

    STATIC_REQUIRE_FALSE(vd::HasViewTitle<NoRowActionOrTitle>);
    STATIC_REQUIRE(vd::HasViewTitle<WithRowActionAndTitle>);
    STATIC_REQUIRE_FALSE(vd::HasRowKey<NoRowActionOrTitle>);
    STATIC_REQUIRE(vd::HasRowKey<WithRowActionAndTitle>);
    STATIC_REQUIRE_FALSE(vd::HasRowActionDescriptor<NoRowActionOrTitle>);
    STATIC_REQUIRE(vd::HasRowActionDescriptor<WithRowActionAndTitle>);
    STATIC_REQUIRE_FALSE(vd::HasViewActions<NoRowActionOrTitle>);
    STATIC_REQUIRE(vd::HasViewActions<WithRowActionAndTitle>);
}

// ---------------------------------------------------------------------------
// Task 2: column derivation.
// ---------------------------------------------------------------------------

namespace {

struct RowNoOverride {};

struct RowWithOverride {
    static constexpr std::array<morph::views::ColumnOverride, 2> columns{
        morph::views::ColumnOverride{.field = "label", .label = "Name"},
        morph::views::ColumnOverride{.field = "nonexistent"},
    };
};

// A local (function-scope) class cannot have a static data member, so
// (unlike the plan's original sketch) this fixture lives at namespace scope
// alongside its siblings rather than inside
// Views::DeriveColumns::HiddenColumnStillEmitted's body.
struct RowHideId {
    static constexpr std::array<morph::views::ColumnOverride, 2> columns{
        morph::views::ColumnOverride{.field = "id", .hidden = true},
        morph::views::ColumnOverride{.field = "label"},
    };
};

}  // namespace

TEST_CASE("Views::DeriveColumns::DefaultOrderAndQuantityMetadata", "[views]") {
    auto const columnsJson = morph::views::detail::deriveColumns<RowNoOverride, vt::VtRow>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, columnsJson));
    REQUIRE(dom.is_array());
    auto const& columns = dom.get_array();
    REQUIRE(columns.size() == 3);

    // Declaration order: id, label, weight (VtRow's x-order).
    CHECK(columns[0]["field"].get_string() == "id");
    CHECK(columns[0]["label"].get_string() == "id");
    CHECK_FALSE(columns[0].contains("v-hidden"));
    CHECK(columns[1]["field"].get_string() == "label");
    CHECK(columns[2]["field"].get_string() == "weight");

    // The Quantity field carries the same ExtUnits/x-decimalPlaces its own
    // form schema would (docs/spec/forms/views.md, "Column derivation").
    CHECK(columns[2]["x-decimalPlaces"].get<std::uint64_t>() == 2);
    CHECK(columns[2]["ExtUnits"]["unitAscii"].get_string() == "kg");
    CHECK(columns[2]["ExtUnits"]["unitUnicode"].get_string() == "kg");
}

TEST_CASE("Views::DeriveColumns::OverrideReordersRelabelsHidesAndSubsets", "[views]") {
    auto const columnsJson = morph::views::detail::deriveColumns<RowWithOverride, vt::VtRow>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, columnsJson));
    auto const& columns = dom.get_array();
    // Subset: only the two declared entries, "id"/"weight" suppressed.
    REQUIRE(columns.size() == 2);

    CHECK(columns[0]["field"].get_string() == "label");
    CHECK(columns[0]["label"].get_string() == "Name");  // relabeled
    CHECK_FALSE(columns[0].contains("v-hidden"));

    // A field name the row type does not have: bare column, no crash.
    CHECK(columns[1]["field"].get_string() == "nonexistent");
    CHECK(columns[1]["label"].get_string() == "nonexistent");
    CHECK_FALSE(columns[1].contains("x-decimalPlaces"));
    CHECK_FALSE(columns[1].contains("ExtUnits"));
}

TEST_CASE("Views::DeriveColumns::HiddenColumnStillEmitted", "[views]") {
    auto const columnsJson = morph::views::detail::deriveColumns<RowHideId, vt::VtRow>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, columnsJson));
    auto const& columns = dom.get_array();
    REQUIRE(columns.size() == 2);
    CHECK(columns[0]["field"].get_string() == "id");
    CHECK(columns[0]["v-hidden"].get<bool>());
}

// ---------------------------------------------------------------------------
// Task 3: full viewSchemaJson<V>() assembly.
// ---------------------------------------------------------------------------

namespace {

struct VtBasicView {
    using kind = morph::views::CollectionView;
    using query = vt::VtListRows;
};

struct VtFullView {
    using kind = morph::views::MasterDetailView;
    using query = vt::VtListRows;

    static constexpr std::string_view title = "Rows (MD)";
    static constexpr std::string_view rowKey = "id";

    static constexpr std::array<morph::views::BindEntry, 2> kEditBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
        morph::views::BindEntry{.actionField = "label", .rowField = "label"},
    };
    static constexpr auto rowAction =
        morph::views::describeAction<vt::VtEditRow>({}, morph::views::ActionScope::Row, kEditBind);

    static constexpr std::array<morph::views::BindEntry, 1> kDeleteBind{
        morph::views::BindEntry{.actionField = "id", .rowField = "id"},
    };
    static constexpr std::array<morph::views::ActionDescriptor, 2> actions{
        morph::views::describeAction<vt::VtDeleteRow>("Delete", morph::views::ActionScope::Row, kDeleteBind, true),
        morph::views::describeAction<vt::VtCreateRow>("New", morph::views::ActionScope::Collection),
    };
};

}  // namespace

TEST_CASE("Views::ViewSchemaJson::BasicCollectionDefaults", "[views]") {
    auto const schema = morph::views::viewSchemaJson<VtBasicView>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(dom["v-kind"].get_string() == "collection");
    CHECK(dom["v-query"].get_string() == "VtListRows");
    CHECK(dom["v-title"].get_string() == "VtListRows");  // default: falls back to v-query
    CHECK(dom["v-rowKey"].get_string() == "id");         // default
    REQUIRE(dom["v-columns"].is_array());
    CHECK(dom["v-columns"].get_array().size() == 3);  // id, label, weight
    CHECK_FALSE(dom.contains("v-rowAction"));
    CHECK_FALSE(dom.contains("v-actions"));
}

TEST_CASE("Views::ViewSchemaJson::MasterDetailWithRowActionAndActions", "[views]") {
    auto const schema = morph::views::viewSchemaJson<VtFullView>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(dom["v-kind"].get_string() == "master-detail");
    CHECK(dom["v-title"].get_string() == "Rows (MD)");
    CHECK(dom["v-rowKey"].get_string() == "id");

    // v-rowAction: only "action" and "bind", no label/scope/confirm.
    REQUIRE(dom.contains("v-rowAction"));
    auto const& rowAction = dom["v-rowAction"];
    CHECK(rowAction["action"].get_string() == "VtEditRow");
    CHECK(rowAction["bind"]["id"].get_string() == "id");
    CHECK(rowAction["bind"]["label"].get_string() == "label");
    CHECK_FALSE(rowAction.contains("label"));
    CHECK_FALSE(rowAction.contains("scope"));
    CHECK_FALSE(rowAction.contains("confirm"));

    // v-actions: full button fields; confirm omitted when false.
    REQUIRE(dom.contains("v-actions"));
    auto const& actions = dom["v-actions"].get_array();
    REQUIRE(actions.size() == 2);
    CHECK(actions[0]["action"].get_string() == "VtDeleteRow");
    CHECK(actions[0]["label"].get_string() == "Delete");
    CHECK(actions[0]["scope"].get_string() == "row");
    CHECK(actions[0]["bind"]["id"].get_string() == "id");
    CHECK(actions[0]["confirm"].get<bool>());
    CHECK(actions[1]["action"].get_string() == "VtCreateRow");
    CHECK(actions[1]["scope"].get_string() == "collection");
    CHECK_FALSE(actions[1].contains("confirm"));
    CHECK_FALSE(actions[1].contains("bind"));
}

TEST_CASE("Views::ViewSchemaJson::Memoized", "[views]") {
    CHECK(morph::views::viewSchemaJson<VtBasicView>() == morph::views::viewSchemaJson<VtBasicView>());
}

TEST_CASE("Views::ViewSchemaJson::SeparateDocumentFromActionSchemas", "[views]") {
    // The view document is additive and NEVER merged into any action schema
    // (docs/spec/forms/views.md, "A new, separate top-level document").
    // Regression guard: no v-* key ever appears in an ordinary action schema.
    auto const actionSchema = morph::forms::schemaJson<vt::VtEditRow>();
    CHECK(actionSchema.find("\"v-kind\"") == std::string::npos);
    CHECK(actionSchema.find("\"v-query\"") == std::string::npos);
    CHECK(actionSchema.find("\"v-columns\"") == std::string::npos);
    CHECK(actionSchema.find("\"v-rowAction\"") == std::string::npos);
    CHECK(actionSchema.find("\"v-actions\"") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Task 4: ViewTraits / BRIDGE_REGISTER_VIEW / ViewRegistry.
// ---------------------------------------------------------------------------

struct VtRegisteredView {
    using kind = morph::views::CollectionView;
    using query = vt::VtListRows;
};

// Registered immediately after the type it specialises, and BEFORE any
// TEST_CASE that references ViewTraits<VtRegisteredView> — the macro expands
// to a namespace-scope explicit specialisation, which (like any declaration)
// must be visible above the point it is used; placing it after the tests
// that use it would be a compile error, not merely bad style.
BRIDGE_REGISTER_VIEW(VtRegisteredView, "VtRegisteredView")

TEST_CASE("Views::Registry::RegisteredViewMatchesDirectCall", "[views]") {
    CHECK(morph::views::ViewTraits<VtRegisteredView>::typeId() == "VtRegisteredView");
    CHECK(morph::views::ViewRegistry::instance().schemaJson("VtRegisteredView") ==
          morph::views::viewSchemaJson<VtRegisteredView>());
}

TEST_CASE("Views::Registry::ViewIdsContainsRegisteredView", "[views]") {
    auto const ids = morph::views::ViewRegistry::instance().viewIds();
    CHECK(std::find(ids.begin(), ids.end(), "VtRegisteredView") != ids.end());
}

TEST_CASE("Views::Registry::UnknownViewThrows", "[views]") {
    CHECK_THROWS_AS(morph::views::ViewRegistry::instance().schemaJson("NoSuchView"), std::runtime_error);
}
