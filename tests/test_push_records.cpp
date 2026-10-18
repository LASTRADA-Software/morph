#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/records.hpp>
#include <string>
#include <typeindex>

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

}  // namespace push_records_test

using namespace push_records_test;

BRIDGE_REGISTER_RECORD(Row, "T_Row")
BRIDGE_REGISTER_RECORD(Row2, "T_Row2", morph::record::key<&Row2::code>)
BRIDGE_REGISTER_RECORD(Sku, "T_Sku")
BRIDGE_REGISTER_RECORD(Report, "T_Report", morph::record::keyless, morph::record::derivedFrom<Row>)
BRIDGE_REGISTER_RECORD(Secret, "T_Secret", morph::record::privateKeys)
BRIDGE_REGISTER_RECORD(NoId, "T_NoId")

namespace {

morph::record::RecordInfo const* find(std::string_view name) {
    for (auto const& info : morph::record::detail::recordRegistry()) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
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
