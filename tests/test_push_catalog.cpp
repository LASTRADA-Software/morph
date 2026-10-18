#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/bundle.hpp>
#include <optional>
#include <string>
#include <vector>

// Glaze reflects member names through an external-linkage object, so these types cannot sit in an
// anonymous namespace.
namespace push_catalog_test {

struct Entry {
    std::int64_t id{};
    std::string name;
};

struct EntryList {
    std::vector<Entry> entries;
};

struct EditEntry {
    std::int64_t id{};
};

struct ListEntries {
    int limit{};
};

struct ReopenEntry {
    std::int64_t id{};
};

struct PingEntry {
    int unused{};
};

struct CatalogModel {
    EntryList execute(EditEntry const& /*action*/) { return {}; }
    EntryList execute(ListEntries const& /*action*/) { return {}; }
    EntryList execute(ReopenEntry const& /*action*/) { return {}; }
    int execute(PingEntry const& /*action*/) { return 0; }
};

}  // namespace push_catalog_test

using namespace push_catalog_test;
namespace effects = morph::effects;

BRIDGE_REGISTER_RECORD(Entry, "T_CatalogEntry")
BRIDGE_REGISTER_MODEL(CatalogModel, "T_CatalogModel")
BRIDGE_REGISTER_ACTION(CatalogModel, EditEntry, "T_CatalogEdit")
BRIDGE_REGISTER_EFFECTS(EditEntry, effects::updates<Entry>, effects::changes<Entry>)
BRIDGE_REGISTER_ACTION(CatalogModel, ListEntries, "T_CatalogList")
BRIDGE_REGISTER_ACTION(CatalogModel, ReopenEntry, "T_CatalogReopen")
BRIDGE_REGISTER_EFFECTS(ReopenEntry, effects::changesInstance)
BRIDGE_REGISTER_ACTION(CatalogModel, PingEntry, "T_CatalogPing")
BRIDGE_REGISTER_EFFECTS(PingEntry, effects::none)

namespace {

morph::ui::author::CatalogFacts registeredFacts() {
    morph::ui::author::CatalogFacts facts;
    facts.action<CatalogModel, EditEntry>();
    facts.action<CatalogModel, ListEntries>();
    facts.action<CatalogModel, ReopenEntry>();
    facts.action<CatalogModel, PingEntry>();
    return facts;
}

}  // namespace

TEST_CASE("catalog_facts_carry_effects_and_records", "[push][catalog]") {
    auto const facts = registeredFacts();

    auto const& edit = facts.actions.at("T_CatalogEdit");
    REQUIRE(edit.effects.has_value());
    REQUIRE(*edit.effects == std::vector<morph::ui::ActionEffect>{{"updates", "T_CatalogEntry"},
                                                                  {"changes", "T_CatalogEntry"}});
    REQUIRE(edit.records.size() == 1U);
    CHECK(edit.records.front().record == "T_CatalogEntry");
    CHECK(edit.records.front().keyField == std::optional<std::string>{"id"});
    CHECK(edit.records.front().paths == std::vector<std::string>{"entries"});

    CHECK(*facts.actions.at("T_CatalogReopen").effects ==
          std::vector<morph::ui::ActionEffect>{{"changesInstance", std::nullopt}});
    CHECK(*facts.actions.at("T_CatalogPing").effects == std::vector<morph::ui::ActionEffect>{{"none", std::nullopt}});
    CHECK(facts.actions.at("T_CatalogPing").records.empty());

    // A read declares no effects but still holds the record.
    auto const& list = facts.actions.at("T_CatalogList");
    CHECK_FALSE(list.effects.has_value());
    CHECK(list.records == edit.records);

    // The catalog's lookup serves the same facts.
    auto const found = facts.catalog().action("T_CatalogEdit");
    REQUIRE(found);
    CHECK(found->effects == edit.effects);
}

TEST_CASE("ui_catalog_reply_round_trips_effects", "[push][catalog]") {
    morph::ui::CatalogReply reply;
    reply.actions = registeredFacts().actions;

    std::string text;
    REQUIRE_FALSE(glz::write_json(reply, text));
    auto const parsed = morph::ui::parseCatalogReply(text);
    REQUIRE(parsed);
    REQUIRE(parsed->actions.has_value());

    auto const& edit = parsed->actions->at("T_CatalogEdit");
    CHECK(edit.effects == reply.actions->at("T_CatalogEdit").effects);
    CHECK(edit.records == reply.actions->at("T_CatalogEdit").records);
    CHECK_FALSE(parsed->actions->at("T_CatalogList").effects.has_value());
    CHECK(text.find(R"("effects":[{"kind":"updates","record":"T_CatalogEntry"})") != std::string::npos);
    CHECK(text.find(R"({"kind":"changesInstance"})") != std::string::npos);
}

TEST_CASE("old_reply_without_fields_reads_as_reads", "[push][catalog]") {
    auto const parsed =
        morph::ui::parseCatalogReply(R"({"entries":[],"permitted":[],"actions":{"Old_Get":{"modelType":"Old"}}})");
    REQUIRE(parsed);
    auto const& info = parsed->actions->at("Old_Get");
    CHECK_FALSE(info.effects.has_value());
    CHECK(info.records.empty());
}
