// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/detail/reflected_member.hpp>
#include <morph/forms/forms.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Every member holds a distinct value (each string member holds its own name),
// so a walk that pairs a key with a different member's value shows up as a
// mismatch between the key and the rendered value.

namespace fmm {

struct PartialMeta {
    std::int64_t id = 1;
    std::string internalNote = "internalNote";
    std::string analysis = "analysis";
    double ph = 7.5;
};

struct ReorderedMeta {
    std::string a = "a";
    std::string b = "b";
};

struct Plain {
    std::string first = "first";
    std::int64_t second = 2;
};

/// One visited key and the text of the value the walk handed it.
struct Visit {
    std::string name;
    std::string value;

    bool operator==(Visit const&) const = default;
};

template <typename Member>
std::string render(Member const& member) {
    if constexpr (std::is_same_v<Member, std::string>) {
        return member;
    } else {
        return glz::write_json(member).value_or("?");
    }
}

template <typename A>
std::vector<Visit> walk(A& action) {
    std::vector<Visit> visits;
    morph::forms::detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, auto const& member) {
        visits.push_back(Visit{.name = std::string{name}, .value = render(member)});
    });
    return visits;
}

}  // namespace fmm

template <>
struct Catch::StringMaker<fmm::Visit> {
    static std::string convert(fmm::Visit const& visit) { return visit.name + "=" + visit.value; }
};

template <>
struct glz::meta<fmm::PartialMeta> {
    using T = fmm::PartialMeta;
    static constexpr auto value = glz::object("id", &T::id, "analysis", &T::analysis, "ph", &T::ph);
};

template <>
struct glz::meta<fmm::ReorderedMeta> {
    using T = fmm::ReorderedMeta;
    static constexpr auto value = glz::object("b", &T::b, "a", &T::a);
};

TEST_CASE("Forms::ForEachNamedMember::PartialMetaPairsEachKeyWithItsOwnMember", "[forms][reflection]") {
    fmm::PartialMeta action{};
    std::vector<fmm::Visit> const expected{
        {.name = "id", .value = "1"},
        {.name = "analysis", .value = "analysis"},
        {.name = "ph", .value = "7.5"},
    };
    CHECK(fmm::walk(action) == expected);
}

TEST_CASE("Forms::ForEachNamedMember::PartialMetaNeverOffersAHiddenMember", "[forms][reflection]") {
    fmm::PartialMeta action{};
    action.internalNote = "SECRET";
    for (auto const& visit : fmm::walk(action)) {
        CHECK(visit.name != "internalNote");
        CHECK(visit.value != "SECRET");
    }
}

TEST_CASE("Forms::ForEachNamedMember::ReorderedMetaFollowsTheMetaOrder", "[forms][reflection]") {
    fmm::ReorderedMeta action{};
    std::vector<fmm::Visit> const expected{
        {.name = "b", .value = "b"},
        {.name = "a", .value = "a"},
    };
    CHECK(fmm::walk(action) == expected);
}

TEST_CASE("Forms::ForEachNamedMember::PlainAggregateWalksDeclarationOrder", "[forms][reflection]") {
    fmm::Plain action{};
    std::vector<fmm::Visit> const expected{
        {.name = "first", .value = "first"},
        {.name = "second", .value = "2"},
    };
    CHECK(fmm::walk(action) == expected);
}

TEST_CASE("Forms::ForEachNamedMember::VisitorWritesThroughToTheNamedMember", "[forms][reflection]") {
    fmm::ReorderedMeta action{};
    morph::forms::detail::forEachNamedMember(
        action, [&]<std::size_t I>(std::string_view name, auto& member) { member = std::string{name} + "-written"; });
    CHECK(action.a == "a-written");
    CHECK(action.b == "b-written");
}

TEST_CASE("Forms::MemberWireName::ResolvesUnderAReorderedMeta", "[forms][reflection]") {
    CHECK(morph::forms::detail::memberWireName<&fmm::ReorderedMeta::a>() == "a");
    CHECK(morph::forms::detail::memberWireName<&fmm::ReorderedMeta::b>() == "b");
    CHECK(morph::forms::detail::memberWireName<&fmm::PartialMeta::ph>() == "ph");
    CHECK(morph::forms::detail::memberWireName<&fmm::PartialMeta::internalNote>().empty());
}

TEST_CASE("Detail::ReflectedMember::RefersIntoTheObjectWithItsConstness", "[forms][reflection]") {
    fmm::PartialMeta action{};
    fmm::PartialMeta const& view = action;

    static_assert(std::is_same_v<decltype(morph::detail::reflectedMember<1>(action)), std::string&>);
    static_assert(std::is_same_v<decltype(morph::detail::reflectedMember<1>(view)), std::string const&>);
    static_assert(std::is_same_v<decltype(morph::detail::reflectedMember<2>(view)), double const&>);
    CHECK(&morph::detail::reflectedMember<1>(view) == &action.analysis);
    CHECK(&morph::detail::reflectedMember<0>(action) == &action.id);

    fmm::Plain plain{};
    static_assert(std::is_same_v<decltype(morph::detail::reflectedMember<1>(plain)), std::int64_t&>);
    CHECK(&morph::detail::reflectedMember<1>(plain) == &plain.second);
}
