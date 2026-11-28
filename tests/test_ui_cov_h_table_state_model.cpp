// SPDX-License-Identifier: Apache-2.0

// The table-state model over a store whose entries a test writes and erases: calls the model refuses, entries it
// finds damaged, and the leftovers of a stopped save that the next call clears. Each case reads the reply of a raw
// call and the entries left in the store.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/table/state_store.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/preferences.hpp>
#include <morph/ui/table_state_model.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_preferences_support.hpp"
#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
namespace ui = morph::ui;
using morph::ui::testing::ProbePreferencesStore;

constexpr char const* kPerson = "alice";

table::TableStateKey keyOfPart(std::string part) {
    return table::TableStateKey{.application = "lims",
                                .model = "Lims_SampleModel",
                                .table = "samples",
                                .qualifier = "samples",
                                .part = std::move(part)};
}

// A server whose every task runs on a step executor.
struct Harness {
    explicit Harness(ui::TableStateOptions options = {}) {
        ui::registerTableState(prefs, dispatcher, registry, std::move(options));
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<prefs_test::TokenAuthorizer>(),
                                                                dispatcher, registry);
        auto const reply = send(morph::wire::makeRegister(std::string{ui::kTableStateModel}), "");
        REQUIRE(reply.kind == "ok");
        instance = reply.modelId;
    }

    morph::wire::Envelope send(morph::wire::Envelope request, std::string const& token) {
        request.session.token = token;
        std::optional<morph::wire::Envelope> reply;
        server->handle(morph::wire::encode(request),
                       [&reply](std::string const& text) { reply = morph::wire::decode(text); });
        pool.runAll();
        REQUIRE(reply.has_value());
        return *reply;
    }

    morph::wire::Envelope call(std::string_view action, std::string body, std::string const& token = kPerson) {
        morph::wire::Envelope request;
        request.kind = "execute";
        request.callId = ++lastCallId;
        request.modelId = instance;
        request.modelType = std::string{ui::kTableStateModel};
        request.actionType = std::string{action};
        request.body = std::move(body);
        return send(std::move(request), token);
    }

    // Writes an entry as the store would hold it.
    void seed(table::TableStateKey const& key, std::string const& point, std::string text,
              std::string const& person = kPerson) {
        auto const done = prefs->set(
            ui::PrefKey{
                .person = person, .app = key.application, .scope = ui::detail::tableStateScope(key), .point = point},
            std::move(text));
        static_cast<void>(done);
        pool.runAll();
    }

    [[nodiscard]] ui::PrefKey pointOf(table::TableStateKey const& key, std::string const& point,
                                      std::string const& person = kPerson) const {
        return ui::PrefKey{
            .person = person, .app = key.application, .scope = ui::detail::tableStateScope(key), .point = point};
    }

    [[nodiscard]] std::set<std::string> points(table::TableStateKey const& key) const {
        return prefs->pointsUnder(ui::detail::tableStateScope(key));
    }

    morph::testing::StepExecutor pool;
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<ProbePreferencesStore> prefs = std::make_shared<ProbePreferencesStore>(pool);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::uint64_t instance = 0;
    std::uint64_t lastCallId = 0;
};

std::string saveBody(table::TableStateKey const& key, std::string text, std::string upload = {},
                     std::uint64_t offset = 0, bool last = true, std::string expected = {}) {
    return morph::model::ActionTraits<ui::TableStateSave>::toJson(ui::TableStateSave{.key = key,
                                                                                     .expected = std::move(expected),
                                                                                     .upload = std::move(upload),
                                                                                     .offset = offset,
                                                                                     .text = std::move(text),
                                                                                     .last = last});
}

std::string loadBody(std::vector<table::TableStateKey> keys) {
    return morph::model::ActionTraits<ui::TableStateLoad>::toJson(ui::TableStateLoad{.keys = std::move(keys)});
}

std::string readBody(table::TableStateKey const& key, std::string revision, std::uint64_t offset) {
    return morph::model::ActionTraits<ui::TableStateRead>::toJson(
        ui::TableStateRead{.key = key, .revision = std::move(revision), .offset = offset});
}

std::string removeBody(table::TableStateKey const& key) {
    return morph::model::ActionTraits<ui::TableStateRemove>::toJson(ui::TableStateRemove{.key = key});
}

std::string publishBodyOf(table::TableStateKey const& key, std::string operation) {
    return morph::model::ActionTraits<ui::TableStatePublish>::toJson(
        ui::TableStatePublish{.key = key, .operation = std::move(operation), .expected = {}});
}

std::string headJson(table::TableStateKey const& key, std::string const& revision, std::string const& rest) {
    return R"({"key":)" + glz::write_json(key).value_or("") + R"(,"revision":")" + revision + "\"" + rest + "}";
}

// A state whose filter ticks many values, so that it is stored in segments.
std::string bigState(int values) {
    std::string out = R"({"version":1,"filters":{"columns":{"name":{"include":[{"oneOf":[)";
    for (int at = 0; at < values; ++at) {
        out += at == 0 ? "\"" : ",\"";
        out += "sample number " + std::to_string(at) + " of a long-named batch\"";
    }
    out += "]}]}}}}";
    return out;
}

std::string savedRevision(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    auto const result = morph::model::ActionTraits<ui::TableStateSave>::resultFromJson(reply.body).result;
    REQUIRE(result.outcome == table::SaveOutcome::Saved);
    return result.revision;
}

}  // namespace

TEST_CASE("cov h state model: a call out of bounds is refused with its reason", "[covh][ui][state]") {
    struct Case {
        char const* what;
        std::string action;
        std::string body;
        std::string message;
    };
    auto const key = keyOfPart("state");
    std::vector<table::TableStateKey> many(257, key);
    ui::TableStateOptions small;
    small.partBytes = 16;
    std::vector<Case> const cases{
        {"load names more keys than a call takes", "load", loadBody(many), "more than 256 keys"},
        {"save part longer than a part", "save", saveBody(key, std::string(17, ' ')), "longer than 16 bytes"},
        {"save of a middle part names no upload", "save", saveBody(key, "{}", {}, 5, true), "names its upload"},
        {"save of a first part names no upload", "save", saveBody(key, "{}", {}, 0, false), "names its upload"},
        {"save with an upload id past the bound", "save", saveBody(key, "{}", std::string(257, 'u'), 0, false),
         "upload id is at most 256 bytes"},
        {"save of text that is no JSON", "save", saveBody(key, "{not json"), "state is JSON text"},
        {"remove of the shared part", "remove", removeBody(keyOfPart("shared")), "written by publish"},
        {"publish naming another part", "publish", publishBodyOf(key, "{}"), "names the part `shared`"},
        {"publish of an operation that does not read", "publish", publishBodyOf(keyOfPart("shared"), "[1]"), ""},
    };
    for (auto const& item : cases) {
        INFO(item.what);
        Harness h{item.what == std::string{"save part longer than a part"} ? small : ui::TableStateOptions{}};
        auto const reply = h.call(item.action, item.body);
        CHECK(reply.kind != "ok");
        CHECK(reply.message.contains(item.message));
        CHECK(h.points(key).empty());
    }
}

TEST_CASE("cov h state model: a read answers the stored revision, and refuses an offset past the end",
          "[covh][ui][state]") {
    Harness h;
    auto const key = keyOfPart("state");
    std::string const doc = R"({"version":1,"view":"abc"})";
    auto const revision = savedRevision(h.call("save", saveBody(key, doc)));
    using Chunk = ui::TableStateChunk;
    auto const chunk = [&](morph::wire::Envelope const& reply) {
        REQUIRE(reply.kind == "ok");
        return morph::model::ActionTraits<ui::TableStateRead>::resultFromJson(reply.body);
    };
    Chunk const stale = chunk(h.call("read", readBody(key, "not-the-revision", 0)));
    CHECK(stale.revision == revision);
    CHECK(stale.text.empty());
    Chunk const whole = chunk(h.call("read", readBody(key, revision, 0)));
    CHECK(whole.text == doc);
    Chunk const tail = chunk(h.call("read", readBody(key, revision, 10)));
    CHECK(tail.text == doc.substr(10));
    Chunk const atEnd = chunk(h.call("read", readBody(key, revision, doc.size())));
    CHECK(atEnd.revision == revision);
    CHECK(atEnd.text.empty());
    auto const past = h.call("read", readBody(key, revision, doc.size() + 1));
    CHECK(past.kind != "ok");
    CHECK(past.message.contains("past the document's end"));
    Chunk const missing = chunk(h.call("read", readBody(keyOfPart("views"), "x", 0)));
    CHECK(missing.revision.empty());
    CHECK(missing.text.empty());
}

TEST_CASE("cov h state model: a segment missing under an unchanged head fails the read and the load",
          "[covh][ui][state]") {
    Harness h;
    auto const key = keyOfPart("state");
    std::string const doc = bigState(1'500);
    REQUIRE(doc.size() > 30'000);
    auto const revision = savedRevision(h.call("save", saveBody(key, doc)));
    h.prefs->erase(h.pointOf(key, "state." + revision + ".1"));
    auto const read = h.call("read", readBody(key, revision, 0));
    CHECK(read.kind != "ok");
    CHECK(read.message.contains("missing segment 1"));
    auto const load = h.call("load", loadBody({key}));
    REQUIRE(load.kind == "ok");
    auto const loaded = morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(load.body);
    REQUIRE(loaded.entries.size() == 1);
    CHECK_FALSE(loaded.entries.front().found);
    CHECK(loaded.entries.front().error.contains("missing segment 1"));

    // A segment that is no JSON string reads as missing as well.
    h.seed(key, "state." + revision + ".1", "[1]");
    auto const damaged = h.call("read", readBody(key, revision, 0));
    CHECK(damaged.kind != "ok");
    CHECK(damaged.message.contains("missing segment 1"));
}

TEST_CASE("cov h state model: a head that does not describe a document is no document", "[covh][ui][state]") {
    auto const key = keyOfPart("state");
    auto const other = keyOfPart("views");
    struct Case {
        char const* what;
        std::string head;
    };
    std::vector<Case> const cases{
        {"not JSON", "head"},
        {"another key", headJson(other, "r1", R"(,"document":{"version":1})")},
        {"no revision", headJson(key, "", R"(,"document":{"version":1})")},
        {"neither document nor segments", headJson(key, "r1", "")},
        {"segments of none", headJson(key, "r1", R"(,"segments":0,"bytes":10)")},
        {"segments without bytes", headJson(key, "r1", R"(,"segments":2)")},
    };
    for (auto const& item : cases) {
        INFO(item.what);
        Harness h;
        h.seed(key, "state", item.head);
        auto const load = h.call("load", loadBody({key}));
        REQUIRE(load.kind == "ok");
        auto const loaded = morph::model::ActionTraits<ui::TableStateLoad>::resultFromJson(load.body);
        REQUIRE(loaded.entries.size() == 1);
        CHECK_FALSE(loaded.entries.front().found);
        CHECK(loaded.entries.front().error.empty());
        auto const removed = h.call("remove", removeBody(key));
        REQUIRE(removed.kind == "ok");
        CHECK_FALSE(morph::model::ActionTraits<ui::TableStateRemove>::resultFromJson(removed.body).removed);
    }
}

TEST_CASE("cov h state model: removing a segmented document removes its segments and its intent",
          "[covh][ui][state]") {
    Harness h{[] {
        ui::TableStateOptions options;
        options.maxBytesPerPerson = 1'000'000;
        return options;
    }()};
    auto const key = keyOfPart("state");
    std::string const doc = bigState(1'500);
    auto const revision = savedRevision(h.call("save", saveBody(key, doc)));
    REQUIRE(h.points(key).size() > 2);
    auto const removed = h.call("remove", removeBody(key));
    REQUIRE(removed.kind == "ok");
    CHECK(morph::model::ActionTraits<ui::TableStateRemove>::resultFromJson(removed.body).removed);
    CHECK(h.points(key).empty());
    CHECK(h.prefs->value(
              ui::PrefKey{.person = kPerson, .app = "morph.tableState", .scope = "morph.table", .point = "usage"}) ==
          R"({"bytes":0})");
    auto const again = h.call("remove", removeBody(key));
    REQUIRE(again.kind == "ok");
    CHECK_FALSE(morph::model::ActionTraits<ui::TableStateRemove>::resultFromJson(again.body).removed);
    CHECK_FALSE(revision.empty());
}

TEST_CASE("cov h state model: the next save clears what a stopped save's intent names", "[covh][ui][state]") {
    struct Case {
        char const* what;
        bool damaged;       // the intent does not read
        bool keptRevision;  // the intent names the revision the head keeps
    };
    for (Case const item : {Case{"names strays", false, false}, Case{"names the kept revision", false, true},
                            Case{"does not read", true, false}}) {
        INFO(item.what);
        Harness h;
        auto const key = keyOfPart("state");
        std::string const doc = bigState(1'500);
        auto const revision = savedRevision(h.call("save", saveBody(key, doc)));
        h.seed(key, "state.stray.0", R"("x")");
        h.seed(key, "state.old.0", R"("y")");
        std::string const intent = item.damaged ? "not json"
                                                : R"({"revision":")" + (item.keptRevision ? revision : "stray") +
                                                      R"(","segments":1,"previous":"old","previousSegments":1})";
        h.seed(key, "state.pending", intent);
        auto const next = h.call("remove", removeBody(key));
        REQUIRE(next.kind == "ok");
        CHECK(morph::model::ActionTraits<ui::TableStateRemove>::resultFromJson(next.body).removed);
        CHECK_FALSE(h.prefs->has(h.pointOf(key, "state.pending")));
        CHECK(h.prefs->has(h.pointOf(key, "state.stray.0")) == (item.damaged || item.keptRevision));
        CHECK(h.prefs->has(h.pointOf(key, "state.old.0")) == item.damaged);
    }
}

TEST_CASE("cov h state model: a usage entry that does not read counts as nothing", "[covh][ui][state]") {
    ui::TableStateOptions options;
    options.maxBytesPerPerson = 100;
    Harness h{options};
    auto const key = keyOfPart("state");
    ui::PrefKey const usage{.person = kPerson, .app = "morph.tableState", .scope = "morph.table", .point = "usage"};
    auto const done = h.prefs->set(usage, "garbage");
    static_cast<void>(done);
    h.pool.runAll();
    std::string const doc = R"({"version":1,"view":"abc"})";
    static_cast<void>(savedRevision(h.call("save", saveBody(key, doc))));
    CHECK(h.prefs->value(usage) == R"({"bytes":)" + std::to_string(doc.size()) + "}");
    auto const over = h.call("save", saveBody(key, std::string(120, ' ') + doc));
    REQUIRE(over.kind == "ok");
    CHECK(morph::model::ActionTraits<ui::TableStateSave>::resultFromJson(over.body).result.outcome ==
          table::SaveOutcome::Refused);
}

TEST_CASE(
    "cov h state model: a publish over a shared document that does not read is refused, one that ends early fails",
    "[covh][ui][state]") {
    auto const shared = keyOfPart("shared");
    std::string const operation = table::writeViewOperation(
        table::AddView{table::SavedView{.name = "A", .layout = {}, .sort = {}, .filters = {}}});
    {
        Harness h;
        h.seed(shared, "shared", headJson(shared, "r1", R"(,"document":{"version":9})"), "");
        auto const reply = h.call("publish", publishBodyOf(shared, operation));
        REQUIRE(reply.kind == "ok");
        auto const result = morph::model::ActionTraits<ui::TableStatePublish>::resultFromJson(reply.body).result;
        CHECK(result.outcome == table::SaveOutcome::Refused);
        CHECK(result.revision == "r1");
        CHECK_FALSE(result.reason.empty());
    }
    {
        Harness h;
        h.seed(shared, "shared", headJson(shared, "r1", R"(,"segments":1,"bytes":100)"), "");
        h.seed(shared, "shared.r1.0", R"("{}")", "");
        auto const reply = h.call("publish", publishBodyOf(shared, operation));
        CHECK(reply.kind != "ok");
        CHECK(reply.message.contains("ended before its size"));
    }
}

TEST_CASE("cov h state model: with no verified principal a read answers nothing and a save is refused",
          "[covh][ui][state]") {
    Harness h;
    auto const key = keyOfPart("state");
    std::string const doc = R"({"version":1,"view":"abc"})";
    auto const revision = savedRevision(h.call("save", saveBody(key, doc)));
    auto const read = h.call("read", readBody(key, revision, 0), "");
    REQUIRE(read.kind == "ok");
    ui::TableStateChunk const chunk = morph::model::ActionTraits<ui::TableStateRead>::resultFromJson(read.body);
    CHECK(chunk.revision.empty());
    CHECK(chunk.text.empty());
    CHECK(h.call("save", saveBody(key, doc), "").kind != "ok");
    CHECK(h.call("remove", removeBody(key), "").kind != "ok");
}
