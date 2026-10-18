#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <latch>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/records.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/service.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_service_support.hpp"

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

struct EntryReport {
    int count{};
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
BRIDGE_REGISTER_RECORD(EntryReport, "T_CatalogReport", morph::record::keyless, morph::record::derivedFrom<Entry>)
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

/// A server answering the UI service's kinds over the fixture bundle, every principal allowed to say hello.
struct HelloRig {
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<morph::backend::RemoteServer> server;

    HelloRig() {
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<morph::ui::UiService>(uiservice::bundle()));
        server = std::make_shared<morph::backend::RemoteServer>(pool, uiservice::authorizer(), config);
    }

    /// Sends `ui-hello` with @p body on @p cid and reads the reply.
    morph::ui::HelloReply hello(morph::backend::ConnectionId cid, std::string const& body) {
        morph::wire::Envelope env;
        env.kind = std::string{morph::ui::kHelloKind};
        env.callId = 1;
        env.body = body;
        morph::wire::Envelope const reply = morph::wire::decode(server->handleInline(morph::wire::encode(env), cid));
        REQUIRE(reply.kind == "ok");
        auto parsed = morph::ui::parseHelloReply(reply.body);
        REQUIRE(parsed);
        return *parsed;
    }

    /// Whether @p cid negotiated events, read on the server strand.
    bool negotiated(morph::backend::ConnectionId cid) {
        bool answer = false;
        std::latch done{1};
        server->strand().post([&] {
            answer = server->eventsNegotiated(cid);
            done.count_down();
        });
        done.wait();
        return answer;
    }
};

constexpr char const* kSpeaksAll = R"("vocabularies":["ui/1","expr/1","ui-theme/1"])";

}  // namespace

TEST_CASE("catalog_facts_carry_effects_and_records", "[push][catalog]") {
    auto const facts = registeredFacts();

    auto const& edit = facts.actions.at("T_CatalogEdit");
    REQUIRE(edit.effects.has_value());
    // A record derived from a changed one is stated as changed too, once.
    REQUIRE(*edit.effects == std::vector<morph::ui::ActionEffect>{{"updates", "T_CatalogEntry"},
                                                                  {"changes", "T_CatalogEntry"},
                                                                  {"changes", "T_CatalogReport"}});
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
    CHECK(text.contains(R"("effects":[{"kind":"updates","record":"T_CatalogEntry"})"));
    CHECK(text.contains(R"({"kind":"changesInstance"})"));
}

TEST_CASE("old_reply_without_fields_reads_as_reads", "[push][catalog]") {
    auto const parsed =
        morph::ui::parseCatalogReply(R"({"entries":[],"permitted":[],"actions":{"Old_Get":{"modelType":"Old"}}})");
    REQUIRE(parsed);
    auto const& info = parsed->actions->at("Old_Get");
    CHECK_FALSE(info.effects.has_value());
    CHECK(info.records.empty());
}

TEST_CASE("hello_with_events_sets_connection_fact", "[push][catalog]") {
    HelloRig rig;
    auto const cid = rig.server->openConnection();
    auto const other = rig.server->openConnection();
    REQUIRE_FALSE(rig.negotiated(cid));

    auto const reply = rig.hello(cid, std::string{"{"} + kSpeaksAll + R"(,"events":true})");
    CHECK(reply.events);
    CHECK(rig.negotiated(cid));
    CHECK_FALSE(rig.negotiated(other));

    SECTION("a later hello without the offer does not withdraw it") {
        CHECK_FALSE(rig.hello(cid, std::string{"{"} + kSpeaksAll + "}").events);
        CHECK(rig.negotiated(cid));
    }
    SECTION("a reconnect negotiates again") {
        rig.server->closeConnection(cid);
        CHECK_FALSE(rig.negotiated(cid));
    }
    SECTION("the service sets the fact it is handed") {
        morph::ui::UiService service{uiservice::bundle()};
        morph::backend::ConnectionFacts facts;
        morph::wire::Envelope env;
        env.kind = std::string{morph::ui::kHelloKind};
        env.body = std::string{"{"} + kSpeaksAll + R"(,"events":true})";
        static_cast<void>(service.handle(env, *uiservice::authorizer(), facts));
        CHECK(facts.events);
    }
}

TEST_CASE("hello_without_events_leaves_it_false", "[push][catalog]") {
    HelloRig rig;
    auto const cid = rig.server->openConnection();

    SECTION("no offer") { CHECK_FALSE(rig.hello(cid, std::string{"{"} + kSpeaksAll + "}").events); }
    SECTION("an explicit refusal of the offer") {
        CHECK_FALSE(rig.hello(cid, std::string{"{"} + kSpeaksAll + R"(,"events":false})").events);
    }
    CHECK_FALSE(rig.negotiated(cid));
}

TEST_CASE("hello_events_round_trip_on_the_wire", "[push][catalog]") {
    auto const request = morph::ui::parseHelloRequest(R"({"vocabularies":["ui/1"],"events":true})");
    REQUIRE(request);
    CHECK(request->events);
    auto const old = morph::ui::parseHelloRequest(R"({"vocabularies":["ui/1"]})");
    REQUIRE(old);
    CHECK_FALSE(old->events);

    std::string const digest = "sha256:" + std::string(64, '0');
    auto const oldReply = morph::ui::parseHelloReply(R"({"application":"a","digest":")" + digest + R"("})");
    REQUIRE(oldReply);
    CHECK_FALSE(oldReply->events);
}

TEST_CASE("watch_body_round_trips_and_is_bounded", "[push][catalog]") {
    morph::wire::WatchBody body;
    body.records = {"Entry", "Tag"};
    body.instances = {{.modelType = "Editor", .primary = "k1", .instance = 0},
                      {.modelType = "Editor", .primary = "", .instance = 7}};
    body.replace = true;
    auto const text = glz::write_json(body).value_or("");
    auto const back = morph::wire::readWatchBody(text);
    REQUIRE(back);
    CHECK(back->records == body.records);
    REQUIRE(back->instances.size() == 2);
    CHECK(back->instances[0].primary == "k1");
    CHECK(back->instances[1].instance == 7);
    CHECK(back->replace);

    SECTION("absent fields default; unknown keys are ignored") {
        auto const bare = morph::wire::readWatchBody(R"({"records":["A"],"future":1})");
        REQUIRE(bare);
        CHECK_FALSE(bare->replace);
        CHECK(bare->instances.empty());
    }
    SECTION("an over-long record list is refused") {
        morph::wire::WatchBody big;
        big.records.assign(morph::wire::kMaxWatchTopics + 1, "R");
        CHECK_FALSE(morph::wire::readWatchBody(glz::write_json(big).value_or("")));
        big.records.pop_back();
        CHECK(morph::wire::readWatchBody(glz::write_json(big).value_or("")));
    }
    SECTION("an over-long instance list or name is refused") {
        morph::wire::WatchBody big;
        big.instances.assign(morph::wire::kMaxWatchTopics + 1, {.modelType = "M", .primary = "", .instance = 0});
        CHECK_FALSE(morph::wire::readWatchBody(glz::write_json(big).value_or("")));
        morph::wire::WatchBody longName;
        longName.records = {std::string(morph::wire::kMaxTopicNameBytes + 1, 'x')};
        CHECK_FALSE(morph::wire::readWatchBody(glz::write_json(longName).value_or("")));
    }
    SECTION("garbage and oversized input are refused") {
        CHECK_FALSE(morph::wire::readWatchBody("not json"));
        CHECK_FALSE(morph::wire::readWatchBody(std::string(morph::wire::kMaxEnvelopeBytes + 1, ' ')));
    }
}

TEST_CASE("watch_reply_round_trips", "[push][catalog]") {
    morph::wire::WatchReply reply;
    reply.refusedRecords = {"Secret"};
    reply.refusedInstances = {{.modelType = "Editor", .primary = "p", .instance = 3}};
    auto const back = morph::wire::readWatchReply(glz::write_json(reply).value_or(""));
    REQUIRE(back);
    CHECK(back->refusedRecords == reply.refusedRecords);
    REQUIRE(back->refusedInstances.size() == 1);
    CHECK(back->refusedInstances[0].instance == 3);
    CHECK(morph::wire::readWatchReply("{}"));

    morph::wire::WatchReply big;
    big.refusedRecords.assign(morph::wire::kMaxWatchTopics + 1, "R");
    CHECK_FALSE(morph::wire::readWatchReply(glz::write_json(big).value_or("")));
}

TEST_CASE("changes_body_round_trips_and_is_bounded", "[push][catalog]") {
    std::string_view const text = R"({"records":[{"record":"Entry","keys":[1,"b"]},{"record":"Tag"}],)"
                                  R"("instances":[{"modelType":"Editor","primary":"p","instance":9}]})";
    auto const back = morph::wire::readChangesBody(text);
    REQUIRE(back);
    REQUIRE(back->records.size() == 2);
    REQUIRE(back->records[0].keys);
    CHECK(back->records[0].keys->size() == 2);
    CHECK_FALSE(back->records[1].keys);
    REQUIRE(back->instances.size() == 1);
    CHECK(back->instances[0].instance == 9);

    SECTION("it is the payload makeEvent carries") {
        auto const env = morph::wire::makeEvent(morph::wire::kChangesTopic, std::string{text});
        morph::wire::EventBody event;
        REQUIRE_FALSE(glz::read_json(event, env.body));
        CHECK(event.topic == "changes");
        CHECK(morph::wire::readChangesBody(event.payload.str));
    }
    SECTION("exactly the key bound is accepted") {
        std::string keys = "[";
        for (std::size_t i = 0; i < morph::effects::kMaxChangeKeys; ++i) {
            keys += (i != 0 ? ",1" : "1");
        }
        keys += "]";
        auto const at = morph::wire::readChangesBody(R"({"records":[{"record":"E","keys":)" + keys + "}]}");
        REQUIRE(at);
        CHECK(at->records[0].keys->size() == morph::effects::kMaxChangeKeys);
    }
    SECTION("too many keys in one record is refused") {
        std::string keys = "[";
        for (std::size_t i = 0; i <= morph::effects::kMaxChangeKeys; ++i) {
            keys += (i != 0 ? ",1" : "1");
        }
        keys += "]";
        CHECK_FALSE(morph::wire::readChangesBody(R"({"records":[{"record":"E","keys":)" + keys + "}]}"));
    }
    SECTION("too many records is refused") {
        std::string records = "[";
        for (std::size_t i = 0; i <= morph::wire::kMaxWatchTopics; ++i) {
            records += (i != 0 ? R"(,{"record":"R"})" : R"({"record":"R"})");
        }
        records += "]";
        CHECK_FALSE(morph::wire::readChangesBody(R"({"records":)" + records + "}"));
    }
    SECTION("the kind and topic names are the wire names") {
        CHECK(morph::wire::kWatchKind == "watch");
        CHECK(morph::wire::kUnwatchKind == "unwatch");
        CHECK(morph::wire::kResyncTopic == "resync");
    }
}
