// SPDX-License-Identifier: Apache-2.0
//
// Tests for morph::views (docs/spec/forms/views.md): the view-schema layer
// that composes a query action's result rows with an optional row-opener
// action and row/collection action buttons into a list/table or
// master-detail screen descriptor.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/views.hpp>
#include <morph/util/quantity.hpp>
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
