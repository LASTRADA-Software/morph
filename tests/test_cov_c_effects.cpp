// SPDX-License-Identifier: Apache-2.0
//
// Equality of the declared-effects value types, and which differences make two record registrations unequal.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace eff = morph::effects;

namespace {
struct RecA {};
struct RecB {};
}  // namespace

TEST_CASE("an effect, an action record and a change time source differ in any one member", "[covc][core][effects]") {
    eff::Effect const base{.kind = eff::Kind::Updates, .record = "R"};
    CHECK(base == eff::Effect{.kind = eff::Kind::Updates, .record = "R"});
    CHECK_FALSE(base == eff::Effect{.kind = eff::Kind::Changes, .record = "R"});
    CHECK_FALSE(base == eff::Effect{.kind = eff::Kind::Updates, .record = "S"});

    eff::ActionRecord const record{
        .record = "R", .keyField = "id", .paths = {"a"}, .opaque = false, .open = {"o"}, .partial = {"p"}};
    auto const changed = [&](auto edit) {
        auto copy = record;
        edit(copy);
        return !(copy == record);
    };
    CHECK(record == eff::ActionRecord{record});
    CHECK(changed([](eff::ActionRecord& r) { r.record = "S"; }));
    CHECK(changed([](eff::ActionRecord& r) { r.keyField = std::nullopt; }));
    CHECK(changed([](eff::ActionRecord& r) { r.paths = {"b"}; }));
    CHECK(changed([](eff::ActionRecord& r) { r.opaque = true; }));
    CHECK(changed([](eff::ActionRecord& r) { r.open = {}; }));
    CHECK(changed([](eff::ActionRecord& r) { r.partial = {}; }));

    eff::ChangeTimeSource const source{.action = "A", .model = "M", .records = {"R"}};
    CHECK(source == eff::ChangeTimeSource{source});
    CHECK_FALSE(source == eff::ChangeTimeSource{.action = "B", .model = "M", .records = {"R"}});
    CHECK_FALSE(source == eff::ChangeTimeSource{.action = "A", .model = "N", .records = {"R"}});
    CHECK_FALSE(source == eff::ChangeTimeSource{.action = "A", .model = "M", .records = {}});
}

TEST_CASE("a declared effect and a declared change time differ in any one member", "[covc][core][effects]") {
    using eff::detail::DeclaredChangeTime;
    using eff::detail::DeclaredEffect;
    DeclaredEffect const effect{.kind = eff::Kind::Updates, .record = std::type_index{typeid(RecA)}};
    CHECK(effect == DeclaredEffect{effect});
    CHECK_FALSE(effect == DeclaredEffect{.kind = eff::Kind::Changes, .record = std::type_index{typeid(RecA)}});
    CHECK_FALSE(effect == DeclaredEffect{.kind = eff::Kind::Updates, .record = std::type_index{typeid(RecB)}});
    CHECK_FALSE(effect == DeclaredEffect{.kind = eff::Kind::Updates, .record = std::nullopt});

    DeclaredChangeTime const time{.action = "A", .model = "M", .records = {std::type_index{typeid(RecA)}}};
    CHECK(time == DeclaredChangeTime{time});
    CHECK_FALSE(time == DeclaredChangeTime{.action = "B", .model = "M", .records = time.records});
    CHECK_FALSE(time == DeclaredChangeTime{.action = "A", .model = "N", .records = time.records});
    CHECK_FALSE(time == DeclaredChangeTime{.action = "A", .model = "M", .records = {}});
}

TEST_CASE("two registrations of one record are the same only when every member agrees", "[covc][core][effects]") {
    using morph::record::RecordInfo;
    RecordInfo const base{.name = "R",
                          .type = std::type_index{typeid(RecA)},
                          .keyField = "id",
                          .previousKeyField = std::nullopt,
                          .derivedFrom = {std::type_index{typeid(RecB)}},
                          .privateKeys = false,
                          .keyIsString = false,
                          .table = false};
    CHECK(eff::detail::sameRegistration(base, base));
    auto const differs = [&](auto edit) {
        auto copy = base;
        edit(copy);
        return !eff::detail::sameRegistration(base, copy);
    };
    CHECK(differs([](RecordInfo& r) { r.name = "S"; }));
    CHECK(differs([](RecordInfo& r) { r.type = std::type_index{typeid(RecB)}; }));
    CHECK(differs([](RecordInfo& r) { r.keyField = "other"; }));
    CHECK(differs([](RecordInfo& r) { r.previousKeyField = "old"; }));
    CHECK(differs([](RecordInfo& r) { r.derivedFrom = {}; }));
    CHECK(differs([](RecordInfo& r) { r.privateKeys = true; }));
    CHECK(differs([](RecordInfo& r) { r.keyIsString = true; }));
    CHECK(differs([](RecordInfo& r) { r.table = true; }));
}

TEST_CASE("an unregistered pair has no schema and no required fields", "[covc][core][registry]") {
    auto& dispatcher = morph::model::detail::ActionDispatcher::instance();
    CHECK(dispatcher.actionSchema("CovC_NoSuchModel", "CovC_NoSuchAction").empty());
    CHECK(dispatcher.requiredFieldsFor("CovC_NoSuchModel", "CovC_NoSuchAction") == nullptr);
}
