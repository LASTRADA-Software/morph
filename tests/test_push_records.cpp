#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <morph/core/bridge.hpp>
#include <morph/core/detail/result_walker.hpp>
#include <morph/core/records.hpp>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

// Glaze reflects member names through an external-linkage object, so these types cannot sit in an
// anonymous namespace.
namespace push_records_test {

struct Row {
    std::int64_t id{};
    std::string name;
};

struct Row2 {
    std::int64_t id{};
    std::string code;
};

struct SkuId {
    std::string raw;
    SkuId() = default;
    explicit SkuId(std::string value) : raw{std::move(value)} {}
    [[nodiscard]] bool hasValue() const { return !raw.empty(); }
    [[nodiscard]] std::string const& operator*() const { return raw; }
};

struct Sku {
    SkuId id;
    int qty{};
};

struct Report {
    int total{};
};

struct Secret {
    std::int64_t id{};
};

struct NoId {
    int value{};
};

struct ListResult {
    std::vector<Row> rows;
};

struct Header {
    std::vector<Row> rows;
};

struct Nested {
    std::optional<Header> header;
    std::map<std::string, Row> byName;
};

// Its codec is replaced, so the walker cannot know what its JSON holds.
struct Opaque {
    std::vector<Row> rows;
};

// A `value` mapping replaces the object codec as well.
struct Mapped {
    std::vector<Row> rows;
};

struct HoldsOpaque {
    Opaque opaque;
    Mapped mapped;
    int count{};
};

struct Node {
    std::vector<Node> children;
    std::vector<Row> rows;
};

struct Renamed {
    std::vector<Row> items;
};

struct ListRows {
    int limit{};
};

struct CountRows {
    int limit{};
};

struct WalkerModel {
    ListResult execute(ListRows const& /*action*/) { return {}; }
    Header execute(CountRows const& /*action*/) { return {}; }
};

}  // namespace push_records_test

template <>
struct glz::meta<push_records_test::Opaque> {
    static constexpr bool custom_read = true;
    static constexpr bool custom_write = true;
};

template <>
struct glz::meta<push_records_test::Mapped> {
    static constexpr auto value = &push_records_test::Mapped::rows;
};

template <>
struct glz::meta<push_records_test::Renamed> {
    static constexpr auto value = glz::object("entries", &push_records_test::Renamed::items);
};

using namespace push_records_test;

BRIDGE_REGISTER_RECORD(Row, "T_Row")
BRIDGE_REGISTER_RECORD(Row2, "T_Row2", morph::record::key<&Row2::code>)
BRIDGE_REGISTER_RECORD(Sku, "T_Sku")
BRIDGE_REGISTER_RECORD(Report, "T_Report", morph::record::keyless, morph::record::derivedFrom<Row>)
BRIDGE_REGISTER_RECORD(Secret, "T_Secret", morph::record::privateKeys)
BRIDGE_REGISTER_RECORD(NoId, "T_NoId")

BRIDGE_REGISTER_MODEL(WalkerModel, "T_WalkerModel")
BRIDGE_REGISTER_ACTION(WalkerModel, ListRows, "T_WalkerListRows")
BRIDGE_REGISTER_ACTION(WalkerModel, CountRows, "T_WalkerCountRows")

// Another model may register a different action type under an id already taken.
namespace {
[[maybe_unused]] bool const walkerConflictFirst =
    morph::model::detail::registerActionResultOnce<ListRows>("T_WalkerConflict");
[[maybe_unused]] bool const walkerConflictSecond =
    morph::model::detail::registerActionResultOnce<CountRows>("T_WalkerConflict");
[[maybe_unused]] bool const walkerSameFirst = morph::model::detail::registerActionResultOnce<ListRows>("T_WalkerSame");
[[maybe_unused]] bool const walkerSameSecond = morph::model::detail::registerActionResultOnce<ListRows>("T_WalkerSame");
}  // namespace

namespace {

morph::record::RecordInfo const* find(std::string_view name) {
    for (auto const& info : morph::record::detail::recordRegistry()) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

// Glaze cannot read this type's member names, so the walker must skip it rather than fail to compile.
struct Hidden {
    std::vector<Row> rows;
};

using morph::model::detail::ResultSlot;

bool hasSlot(std::vector<ResultSlot> const& slots, std::type_index type, std::string_view path) {
    return std::ranges::any_of(slots, [&](ResultSlot const& slot) { return slot.type == type && slot.path == path; });
}

}  // namespace

TEST_CASE("record_default_key_is_id", "[push][records]") {
    auto const* info = find("T_Row");
    REQUIRE(info != nullptr);
    REQUIRE(info->type == std::type_index{typeid(Row)});
    REQUIRE(info->keyField == "id");
    REQUIRE_FALSE(info->keyIsString);
    REQUIRE_FALSE(info->privateKeys);
    REQUIRE(info->derivedFrom.empty());
}

TEST_CASE("record_named_key_member", "[push][records]") {
    auto const* info = find("T_Row2");
    REQUIRE(info != nullptr);
    REQUIRE(info->keyField == "code");
    REQUIRE(info->keyIsString);
}

TEST_CASE("record_wrapped_key", "[push][records]") {
    auto const* info = find("T_Sku");
    REQUIRE(info != nullptr);
    REQUIRE(info->keyField == "id");
    REQUIRE(info->keyIsString);
}

TEST_CASE("record_keyless_and_derived", "[push][records]") {
    auto const* info = find("T_Report");
    REQUIRE(info != nullptr);
    REQUIRE_FALSE(info->keyField.has_value());
    REQUIRE(info->derivedFrom == std::vector<std::type_index>{std::type_index{typeid(Row)}});
}

TEST_CASE("record_private_keys", "[push][records]") {
    auto const* info = find("T_Secret");
    REQUIRE(info != nullptr);
    REQUIRE(info->privateKeys);
    REQUIRE(info->keyField == "id");
}

TEST_CASE("record_without_id_has_no_key", "[push][records]") {
    auto const* info = find("T_NoId");
    REQUIRE(info != nullptr);
    REQUIRE_FALSE(info->keyField.has_value());
}

TEST_CASE("walker_result_itself", "[push][records]") {
    auto const slots = morph::model::detail::walkResult<Row>();
    REQUIRE(hasSlot(slots, typeid(Row), ""));
    REQUIRE(slots.size() == 1);
}

TEST_CASE("walker_vector_member", "[push][records]") {
    auto const slots = morph::model::detail::walkResult<ListResult>();
    REQUIRE(hasSlot(slots, typeid(ListResult), ""));
    REQUIRE(hasSlot(slots, typeid(Row), "rows"));
    REQUIRE(slots.size() == 2);
}

TEST_CASE("walker_paths_through_optional_and_map", "[push][records]") {
    auto const slots = morph::model::detail::walkResult<Nested>();
    REQUIRE(hasSlot(slots, typeid(Nested), ""));
    REQUIRE(hasSlot(slots, typeid(Header), "header"));
    REQUIRE(hasSlot(slots, typeid(Row), "header.rows"));
    REQUIRE(hasSlot(slots, typeid(Row), "byName"));
    REQUIRE(slots.size() == 4);
}

TEST_CASE("walker_skips_custom_codec", "[push][records]") {
    auto const slots = morph::model::detail::walkResult<HoldsOpaque>();
    REQUIRE(hasSlot(slots, typeid(HoldsOpaque), ""));
    REQUIRE(slots.size() == 1);
    REQUIRE(morph::model::detail::walkResult<Opaque>().empty());
    REQUIRE(morph::model::detail::walkResult<Mapped>().empty());
}

TEST_CASE("walker_skips_anonymous_namespace_type", "[push][records]") {
    REQUIRE(morph::model::detail::walkResult<Hidden>().empty());
    REQUIRE(morph::model::detail::walkResult<std::vector<Hidden>>().empty());
}

TEST_CASE("walker_skips_scalars_strings_and_void", "[push][records]") {
    REQUIRE(morph::model::detail::walkResult<void>().empty());
    REQUIRE(morph::model::detail::walkResult<int>().empty());
    REQUIRE(morph::model::detail::walkResult<std::string>().empty());
    REQUIRE(morph::model::detail::walkResult<std::vector<std::string>>().empty());
}

TEST_CASE("walker_ends_on_recursive_type", "[push][records]") {
    auto const slots = morph::model::detail::walkResult<Node>();
    REQUIRE(hasSlot(slots, typeid(Node), ""));
    REQUIRE(hasSlot(slots, typeid(Row), "rows"));
    REQUIRE(slots.size() == 2);
}

TEST_CASE("walker_renamed_member_uses_wire_name", "[push][records]") {
    auto const slots = morph::model::detail::walkResult<Renamed>();
    REQUIRE(hasSlot(slots, typeid(Row), "entries"));
    REQUIRE_FALSE(hasSlot(slots, typeid(Row), "items"));
}

TEST_CASE("action_registration_records_result_slots", "[push][records]") {
    auto const& all = morph::model::detail::actionResultSlots();
    auto const found = all.find("T_WalkerListRows");
    REQUIRE(found != all.end());
    REQUIRE(found->second.action == std::type_index{typeid(ListRows)});
    REQUIRE(hasSlot(found->second.resultSlots, typeid(ListResult), ""));
    REQUIRE(hasSlot(found->second.resultSlots, typeid(Row), "rows"));
}

TEST_CASE("action_result_second_type_under_one_id_is_a_conflict", "[push][records]") {
    auto const& all = morph::model::detail::actionResultSlots();
    auto const& conflicts = morph::model::detail::actionResultConflicts();
    auto const conflict = all.find("T_WalkerConflict");
    REQUIRE(conflict != all.end());
    REQUIRE(conflict->second.action == std::type_index{typeid(ListRows)});
    REQUIRE(hasSlot(conflict->second.resultSlots, typeid(ListResult), ""));
    REQUIRE_FALSE(hasSlot(conflict->second.resultSlots, typeid(Header), ""));
    REQUIRE(std::ranges::count(conflicts, std::string{"T_WalkerConflict"}) == 1);

    auto const same = all.find("T_WalkerSame");
    REQUIRE(same != all.end());
    REQUIRE(std::ranges::find(conflicts, std::string{"T_WalkerSame"}) == conflicts.end());
    REQUIRE(std::ranges::find(conflicts, std::string{"T_WalkerListRows"}) == conflicts.end());
}
