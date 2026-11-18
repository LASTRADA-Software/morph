// SPDX-License-Identifier: Apache-2.0
//
// The custom screens' memory store, and the model through a RemoteServer.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui_custom_screens_support.hpp"

namespace {

/// The value of a completion the inline executor has already settled.
template <typename T>
T settled(morph::async::Completion<T> completion) {
    std::optional<T> out;
    completion.then([&out](T const& value) { out = value; });
    REQUIRE(out.has_value());
    return std::move(*out);
}

morph::ui::CustomScreen screen(std::string id, std::string person) {
    return morph::ui::CustomScreen{.id = std::move(id),
                                   .title = "Mine",
                                   .basedOn = "lims.base",
                                   .document = "{}",
                                   .savedAt = {},
                                   .savedBy = std::move(person)};
}

}  // namespace

TEST_CASE("MemoryCustomScreenStore keeps each person's screens apart", "[ui][custom-screens][store]") {
    morph::ui::MemoryCustomScreenStore store{morph::exec::detail::inlineExecutor()};
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:1"}, screen("custom:1", "alice"))));
    REQUIRE(settled(store.put({.person = "bob", .app = "lims", .id = "custom:2"}, screen("custom:2", "bob"))));

    std::vector<morph::ui::CustomScreen> const alices = settled(store.list("alice", "lims"));
    REQUIRE(alices.size() == 1);
    CHECK(alices.front().id == "custom:1");
    CHECK(settled(store.list("alice", "other")).empty());

    CHECK(settled(store.get({.person = "alice", .app = "lims", .id = "custom:1"})).has_value());
    auto const notHers = settled(store.get({.person = "alice", .app = "lims", .id = "custom:2"}));
    REQUIRE_FALSE(notHers);
    CHECK(notHers.error().reason == "no such custom screen");

    CHECK_FALSE(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:1"})));
    CHECK(settled(store.list("alice", "lims")).empty());

    // Alice's writes under an id Bob also uses leave Bob's screen alone.
    REQUIRE(settled(store.put({.person = "bob", .app = "lims", .id = "custom:2"}, screen("custom:2", "bob"))));
    morph::ui::CustomScreen hers = screen("custom:2", "alice");
    hers.title = "Hers";
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:2"}, std::move(hers))));
    auto const bobs = settled(store.get({.person = "bob", .app = "lims", .id = "custom:2"}));
    REQUIRE(bobs);
    CHECK(bobs->title == "Mine");
    CHECK(bobs->savedBy == "bob");
    CHECK(settled(store.list("bob", "lims")).size() == 1);

    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.get({.person = "bob", .app = "lims", .id = "custom:2"})).has_value());
    CHECK(settled(store.list("bob", "lims")).size() == 1);
}

TEST_CASE("MemoryCustomScreenStore lists one person's screens ordered by id", "[ui][custom-screens][store]") {
    morph::ui::MemoryCustomScreenStore store{morph::exec::detail::inlineExecutor()};
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:b"}, screen("custom:b", "alice"))));
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:a"}, screen("custom:a", "alice"))));
    std::vector<morph::ui::CustomScreen> const listed = settled(store.list("alice", "lims"));
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == "custom:a");
    CHECK(listed[1].id == "custom:b");
}

using screens_test::baseScreen;
using screens_test::Envelope;
using screens_test::quoted;
using screens_test::Server;

namespace {

/// The `list` reply's screens.
morph::ui::CustomScreensListed listed(Server& s, std::string token) {
    Envelope const reply = s.call(morph::ui::kCustomScreensList, R"({"app":"lims"})", std::move(token));
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreensListed out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out;
}

/// The id a save answered.
std::string savedId(Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreenSaved out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out.id;
}

}  // namespace

TEST_CASE("custom screens: a save is listed and read back under its own id", "[ui][custom-screens][remote]") {
    Server s;
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    CHECK(id.starts_with("custom:"));

    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK(mine.screens.front().id == id);
    CHECK(mine.screens.front().savedBy == "alice");
    CHECK(mine.screens.front().mine);
    CHECK(mine.screens.front().opensWithoutParams);
    CHECK_FALSE(mine.screens.front().broken.has_value());

    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    CHECK(screens_test::load(found.document).id == id);
}

TEST_CASE("custom screens: one person never reaches another's", "[ui][custom-screens][remote]") {
    Server s;
    std::string const bobs = savedId(s.save(baseScreen("lims.base"), "bob"));
    CHECK(listed(s, "alice").screens.empty());
    Envelope const got =
        s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(bobs) + R"(,"person":"bob"})", "alice");
    REQUIRE(got.kind == "err");
    CHECK(got.message.contains("no such custom screen"));
    Envelope const overwrite = s.call(morph::ui::kCustomScreensSave,
                                      R"({"app":"lims","id":)" + quoted(bobs) +
                                          R"(,"title":"Taken","basedOn":"lims.base","document":)" +
                                          quoted(baseScreen("lims.base")) + "}",
                                      "alice");
    REQUIRE(overwrite.kind == "err");
    CHECK(overwrite.message.contains("no such custom screen"));
    CHECK(listed(s, "bob").screens.front().title == "Mine");
}

TEST_CASE("custom screens: without a verified person nothing is read or written", "[ui][custom-screens][remote]") {
    Server s;
    CHECK(listed(s, "").screens.empty());
    Envelope const write = s.save(baseScreen("lims.base"), "");
    REQUIRE(write.kind == "err");
    CHECK(write.message.contains("signed-in person"));
    CHECK(s.call(morph::ui::kCustomScreensCheck,
                 R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(baseScreen("lims.base")) + "}", "")
              .kind == "err");
    CHECK(s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":"custom:x"})", "").kind == "err");
}

TEST_CASE("custom screens: check answers the refusal and its pointer; save refuses the same",
          "[ui][custom-screens][remote]") {
    Server s;
    std::string const out =
        screens_test::replaced(baseScreen("lims.base"), R"({"host":"print"})", R"({"signOut":true})");
    Envelope const checked = s.call(morph::ui::kCustomScreensCheck,
                                    R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(out) + "}", "alice");
    REQUIRE(checked.kind == "ok");
    morph::ui::CustomScreenChecked verdict;
    REQUIRE_FALSE(glz::read_json(verdict, checked.body));
    REQUIRE(verdict.refusal.has_value());
    CHECK(verdict.refusal->reason.contains("signOut"));
    CHECK_FALSE(verdict.refusal->pointer.empty());

    Envelope const saved = s.save(out, "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains("signOut"));
    CHECK(listed(s, "alice").screens.empty());
}

TEST_CASE("custom screens: a store's refusal reaches the person", "[ui][custom-screens][remote]") {
    struct Refusing final : morph::ui::ICustomScreenStore {
        explicit Refusing(morph::exec::IExecutor& base) : inner{base}, strand{base} {}
        morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(std::string p, std::string a) override {
            return inner.list(std::move(p), std::move(a));
        }
        morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
            morph::ui::CustomScreenKey k) override {
            return inner.get(std::move(k));
        }
        morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
            morph::ui::CustomScreenKey /*k*/, morph::ui::CustomScreen /*s*/) override {
            auto [completion, promise] =
                morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>>::makeSettleable(&strand);
            promise.resolve(
                std::unexpected(morph::ui::CustomScreenRefusal{.reason = "over your limit of custom screens"}));
            return std::move(completion);
        }
        morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
            morph::ui::CustomScreenKey k) override {
            return inner.remove(std::move(k));
        }
        morph::ui::MemoryCustomScreenStore inner;
        /// The serial owner the refusal is delivered on, declared last so it is destroyed first.
        morph::exec::OwnerStrand strand;
    };
    morph::exec::ThreadPoolExecutor storePool{1};
    Server s{std::make_shared<Refusing>(storePool)};
    Envelope const saved = s.save(baseScreen("lims.base"), "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains("over your limit of custom screens"));
}

TEST_CASE("custom screens: a screen the person has since lost permission for is listed broken",
          "[ui][custom-screens][remote]") {
    Server s;
    REQUIRE(s.save(baseScreen("lims.base"), "alice").kind == "ok");
    s.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_Verify"}};
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    REQUIRE(mine.screens.front().broken.has_value());
    CHECK(mine.screens.front().broken->pointer == "/mutations/verify");
}

TEST_CASE("custom screens: save with an id overwrites, remove removes", "[ui][custom-screens][remote]") {
    Server s;
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    Envelope const again = s.call(morph::ui::kCustomScreensSave,
                                  R"({"app":"lims","id":)" + quoted(id) +
                                      R"(,"title":"Renamed","basedOn":"lims.base","document":)" +
                                      quoted(baseScreen("lims.base")) + "}",
                                  "alice");
    CHECK(savedId(again) == id);
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK(mine.screens.front().title == "Renamed");

    Envelope const removed =
        s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(removed.kind == "ok");
    CHECK(listed(s, "alice").screens.empty());
    CHECK(s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":)" + quoted(id) + "}", "alice").kind ==
          "err");
}

TEST_CASE("custom screens: lists hold the record that saves and removes change", "[ui][custom-screens][effects]") {
    std::vector<std::string> const holding = morph::effects::actionsHolding(morph::ui::kCustomScreenRecord);
    CHECK(std::ranges::find(holding, std::string{morph::ui::kCustomScreensList}) != holding.end());
    CHECK(std::ranges::find(holding, std::string{morph::ui::kCustomScreensGet}) != holding.end());
    for (std::string_view const action : {morph::ui::kCustomScreensSave, morph::ui::kCustomScreensRemove}) {
        auto const effects = morph::effects::effectsOf(action);
        REQUIRE(effects.has_value());
        REQUIRE(effects->size() == 1);
        CHECK(effects->front().kind == morph::effects::Kind::Changes);
        CHECK(effects->front().record == morph::ui::kCustomScreenRecord);
    }
    CHECK(morph::model::ActionTraits<morph::ui::CustomScreensList>::typeId() == morph::ui::kCustomScreensList);
}

TEST_CASE("custom screens: a save whose stored text would exceed the bound is refused", "[ui][custom-screens][remote]") {
    std::string const document = baseScreen("lims.base");
    Server s{nullptr, morph::ui::CustomScreensOptions{.maxDocumentBytes = document.size()}};
    // The document itself is within the bound; the id the model writes into it is longer than the one it had.
    CHECK(s.call(morph::ui::kCustomScreensCheck,
                 R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(document) + "}", "alice")
              .kind == "ok");
    Envelope const saved = s.save(document, "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains("longer than"));
    CHECK(listed(s, "alice").screens.empty());
}

TEST_CASE("custom screens: a title outside its bound is refused", "[ui][custom-screens][remote]") {
    Server s;
    CHECK(s.save(baseScreen("lims.base"), "alice", "").kind == "err");
    CHECK(s.save(baseScreen("lims.base"), "alice", std::string(morph::ui::kMaxNameBytes + 1, 'x')).kind == "err");
    CHECK(s.save(baseScreen("lims.base"), "alice", std::string(morph::ui::kMaxNameBytes, 'x')).kind == "ok");
}
