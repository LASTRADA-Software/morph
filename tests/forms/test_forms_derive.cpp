// SPDX-License-Identifier: Apache-2.0

// Derived values on the server: computed members of row types recomputed on every path, derived members cleared
// before the model runs, and both written into the schema (docs/spec/table/editing.md, "Derived values", delivery
// step 2).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/derive.hpp>
#include <morph/forms/forms.hpp>
#include <string_view>
#include <vector>

struct DeriveBox {
    std::int64_t count = 0;
    std::int64_t perBox = 0;
    std::int64_t total = 0;   // computed: count * perBox
    std::int64_t limit = 0;   // derived: the model's
    static constexpr auto computedFields = morph::forms::computeList(
        morph::forms::computed<&DeriveBox::total, &DeriveBox::count, &DeriveBox::perBox>(
            [](auto const& box) { return box.count * box.perBox; }));
    static const std::array<morph::forms::FieldMeta, 1> fieldMetadata;
};
inline const std::array<morph::forms::FieldMeta, 1> DeriveBox::fieldMetadata{morph::forms::derived<&DeriveBox::limit>()};

struct RecordBoxes {
    std::vector<DeriveBox> boxes;
    std::int64_t sum = 0;     // computed: the boxes' totals
    static constexpr auto computedFields = morph::forms::computeList(morph::forms::computed<&RecordBoxes::sum, &RecordBoxes::boxes>(
        [](auto const& record) { std::int64_t s = 0; for (auto const& b : record.boxes) s += b.total; return s; }));
};
struct DeriveBoxModel {
    static inline RecordBoxes last{};
    bool execute(RecordBoxes const& record) { last = record; return true; }   // sets no derived member
};
BRIDGE_REGISTER_MODEL(DeriveBoxModel, "T_DeriveBoxes")
BRIDGE_REGISTER_ACTION(DeriveBoxModel, RecordBoxes, "T_RecordBoxes")

namespace {

constexpr std::string_view kForged = R"({"boxes":[{"count":2,"perBox":3,"total":999,"limit":555}],"sum":999})";

// Mutation: recomputeAll skipping row members (total stays 999, sum 999); a second run with clearDerived removed
// from prepareAction (limit stays 555).
TEST_CASE("derive: the server keeps recomputeAll's values and the model's", "[forms][derive]") {
    auto holder = morph::model::detail::defaultRegistry().create("T_DeriveBoxes");
    morph::model::detail::defaultDispatcher().dispatch("T_DeriveBoxes", "T_RecordBoxes", *holder, kForged);
    RecordBoxes const& kept = DeriveBoxModel::last;
    CHECK(kept.boxes.at(0).total == 6);
    CHECK(kept.sum == 6);
    CHECK(kept.boxes.at(0).limit == 0);   // the client's 555 is cleared; the model set nothing
    auto const evaluated = morph::model::detail::defaultDispatcher().evaluate("T_DeriveBoxes", "T_RecordBoxes", kForged);
    CHECK(evaluated.body == R"({"boxes":[{"count":2,"perBox":3,"total":6,"limit":0}],"sum":6})");
}

// Mutation: annotateNestedAggregate not writing x-computed for a row type.
TEST_CASE("derive: a row type's schema marks computed and derived members", "[forms][derive]") {
    auto dom = glz::read_json<glz::generic_u64>(morph::forms::schemaJson<RecordBoxes>()).value();
    auto& box = dom["properties"]["boxes"]["items"];   // DeriveBox is used once: glaze inlines it
    CHECK(glz::write_json(box["properties"]["total"]["x-readonly"]).value() == "true");
    CHECK(glz::write_json(box["properties"]["total"]["x-computed"]["inputs"]).value() == R"(["count","perBox"])");
    CHECK(glz::write_json(box["properties"]["limit"]["x-derived"]).value() == "true");
    CHECK(glz::write_json(box["required"]).value() == R"(["count","perBox"])");
}

// A derive action's input is `{"body": …}`, through the meta its deriving type inherits.
struct DeriveRecordBoxes : morph::forms::DeriveOf<RecordBoxes> {};

TEST_CASE("derive: a derive action's input and reply read and write as specified", "[forms][derive]") {
    DeriveRecordBoxes input;
    REQUIRE_FALSE(glz::read_json(input, std::string_view{R"({"body":{"boxes":[{"count":4}],"sum":0}})"}));
    CHECK(input.body.boxes.at(0).count == 4);
    morph::forms::Derived reply{.values = {{"boxes[0].limit", glz::raw_json{"7"}}}};
    CHECK(glz::write_json(reply).value() == R"({"values":[{"path":"boxes[0].limit","value":7}]})");
    RecordBoxes record{.boxes = {DeriveBox{.count = 1, .perBox = 1, .total = 1, .limit = 9}}, .sum = 1};
    morph::forms::clearDerived(record);
    CHECK(record.boxes.at(0).limit == 0);
    CHECK(record.boxes.at(0).total == 1);
}

}  // namespace
