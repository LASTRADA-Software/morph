// SPDX-License-Identifier: Apache-2.0
//
// The custom screens' memory store, and the model through a RemoteServer.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <core/async/Task.hpp>
#include <future>
#include <initializer_list>
#include <map>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/forms.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/custom_screens.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_custom_screens_support.hpp"

namespace {

/// The concatenation of @p parts.
std::string joined(std::initializer_list<std::string_view> parts) {
    std::string text;
    for (std::string_view const part : parts) {
        text += part;
    }
    return text;
}

/// The value of a completion the inline executor has already settled.
template <typename T>
T settled(morph::async::Completion<T> completion) {
    std::optional<T> out;
    completion.then([&out](T const& value) { out = value; });
    REQUIRE(out.has_value());
    return std::move(*out);
}

morph::ui::CustomScreenScope scope(std::string person, std::string app) {
    return morph::ui::CustomScreenScope{.person = std::move(person), .app = std::move(app), .session = {}};
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

    std::vector<morph::ui::CustomScreen> const alices = settled(store.list(scope("alice", "lims")));
    REQUIRE(alices.size() == 1);
    CHECK(alices.front().id == "custom:1");
    CHECK(settled(store.list(scope("alice", "other"))).empty());

    CHECK(settled(store.get({.person = "alice", .app = "lims", .id = "custom:1"})).has_value());
    auto const notHers = settled(store.get({.person = "alice", .app = "lims", .id = "custom:2"}));
    REQUIRE_FALSE(notHers);
    CHECK(notHers.error().reason == "no such custom screen");

    CHECK_FALSE(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:1"})));
    CHECK(settled(store.list(scope("alice", "lims"))).empty());

    // Alice's writes under an id Bob also uses leave Bob's screen alone.
    REQUIRE(settled(store.put({.person = "bob", .app = "lims", .id = "custom:2"}, screen("custom:2", "bob"))));
    morph::ui::CustomScreen hers = screen("custom:2", "alice");
    hers.title = "Hers";
    auto const storedHers = settled(store.put({.person = "alice", .app = "lims", .id = "custom:2"}, std::move(hers)));
    REQUIRE(storedHers);
    auto const bobs = settled(store.get({.person = "bob", .app = "lims", .id = "custom:2"}));
    REQUIRE(bobs);
    CHECK(bobs->title == "Mine");
    CHECK(bobs->savedBy == "bob");
    CHECK(settled(store.list(scope("bob", "lims"))).size() == 1);

    CHECK(settled(store.remove({.person = "alice", .app = "lims", .id = "custom:2"})));
    CHECK(settled(store.get({.person = "bob", .app = "lims", .id = "custom:2"})).has_value());
    CHECK(settled(store.list(scope("bob", "lims"))).size() == 1);
}

TEST_CASE("MemoryCustomScreenStore lists one person's screens ordered by id", "[ui][custom-screens][store]") {
    morph::ui::MemoryCustomScreenStore store{morph::exec::detail::inlineExecutor()};
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:b"}, screen("custom:b", "alice"))));
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = "custom:a"}, screen("custom:a", "alice"))));
    std::vector<morph::ui::CustomScreen> const listed = settled(store.list(scope("alice", "lims")));
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == "custom:a");
    CHECK(listed[1].id == "custom:b");
}

using screens_test::baseScreen;
using screens_test::Envelope;
using screens_test::quoted;
using screens_test::replaced;
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
    Envelope const overwrite =
        s.call(morph::ui::kCustomScreensSave,
               R"({"app":"lims","id":)" + quoted(bobs) + R"(,"title":"Taken","basedOn":"lims.base","document":)" +
                   quoted(baseScreen("lims.base")) + "}",
               "alice");
    REQUIRE(overwrite.kind == "err");
    CHECK(overwrite.message.contains("no such custom screen"));
    auto const bobsList = listed(s, "bob");
    REQUIRE(bobsList.screens.size() == 1);
    CHECK(bobsList.screens.front().title == "Mine");
}

TEST_CASE("custom screens: without a verified person nothing is read or written", "[ui][custom-screens][remote]") {
    Server s;
    CHECK(listed(s, "").screens.empty());
    Envelope const write = s.save(baseScreen("lims.base"), "");
    REQUIRE(write.kind == "err");
    CHECK(write.message.contains("signed-in person"));
    Envelope const check =
        s.call(morph::ui::kCustomScreensCheck,
               R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(baseScreen("lims.base")) + "}", "");
    REQUIRE(check.kind == "err");
    CHECK(check.message.contains("signed-in person"));
    Envelope const remove = s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":"custom:x"})", "");
    REQUIRE(remove.kind == "err");
    CHECK(remove.message.contains("signed-in person"));
    Envelope const get = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":"custom:x"})", "");
    REQUIRE(get.kind == "err");
    CHECK(get.message.contains("signed-in person"));
}

TEST_CASE("custom screens: every action refuses an application the bundle does not serve",
          "[ui][custom-screens][remote]") {
    Server s;
    std::string const doc = quoted(baseScreen("lims.base"));
    std::string const other = R"("app":"other")";
    for (auto const& [action, body] : std::vector<std::pair<std::string_view, std::string>>{
             {morph::ui::kCustomScreensList, "{" + other + "}"},
             {morph::ui::kCustomScreensGet, "{" + other + R"(,"id":"custom:x"})"},
             {morph::ui::kCustomScreensCheck, joined({"{", other, R"(,"basedOn":"lims.base","document":)", doc, "}"})},
             {morph::ui::kCustomScreensSave,
              joined({"{", other, R"(,"title":"T","basedOn":"lims.base","document":)", doc, "}"})},
             {morph::ui::kCustomScreensRemove, "{" + other + R"(,"id":"custom:x"})"}}) {
        Envelope const reply = s.call(action, body, "alice");
        INFO(action);
        REQUIRE(reply.kind == "err");
        CHECK(reply.message.contains("custom screens belong to `lims`, not `other`"));
    }
}

TEST_CASE("registerCustomScreens refuses what it cannot serve", "[ui][custom-screens][remote]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(pool);
    auto const served = screens_test::bundle();
    // NOLINTNEXTLINE(performance-no-automatic-move): the closure returns a copy of its capture on every call.
    auto const getBundle = [served] { return served; };
    auto const authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    CHECK_THROWS_AS(morph::ui::registerCustomScreens(
                        store, getBundle, authorizer, dispatcher, registry,
                        morph::ui::CustomScreensOptions{.maxDocumentBytes = morph::ui::kMaxFileBytes + 1}),
                    std::invalid_argument);
    CHECK_THROWS_AS(morph::ui::registerCustomScreens(nullptr, getBundle, authorizer, dispatcher, registry),
                    std::invalid_argument);
    CHECK_THROWS_AS(morph::ui::registerCustomScreens(store, nullptr, authorizer, dispatcher, registry),
                    std::invalid_argument);
    CHECK_THROWS_AS(morph::ui::registerCustomScreens(store, getBundle, nullptr, dispatcher, registry),
                    std::invalid_argument);
    // The largest document whose escaped `get` reply fits one envelope is accepted; one byte more is not.
    std::size_t const largest = (morph::wire::kMaxEnvelopeBytes - morph::ui::kCustomScreenReplyOverhead) / 4;
    CHECK_THROWS_AS(morph::ui::registerCustomScreens(store, getBundle, authorizer, dispatcher, registry,
                                                     morph::ui::CustomScreensOptions{.maxDocumentBytes = largest + 1}),
                    std::invalid_argument);
    CHECK_NOTHROW(morph::ui::registerCustomScreens(store, getBundle, authorizer, dispatcher, registry,
                                                   morph::ui::CustomScreensOptions{.maxDocumentBytes = largest}));
    // The same for a list: the most summaries whose escaped reply fits one envelope, and one more.
    std::size_t const mostListed = (morph::wire::kMaxEnvelopeBytes - morph::ui::kCustomScreenReplyOverhead) /
                                   (morph::ui::kMaxEscapedGrowth * morph::ui::kMaxSummaryBytes);
    CHECK_THROWS_AS(morph::ui::registerCustomScreens(store, getBundle, authorizer, dispatcher, registry,
                                                     morph::ui::CustomScreensOptions{.maxListed = mostListed + 1}),
                    std::invalid_argument);
    CHECK_NOTHROW(morph::ui::registerCustomScreens(store, getBundle, authorizer, dispatcher, registry,
                                                   morph::ui::CustomScreensOptions{.maxListed = mostListed}));
}

TEST_CASE("custom screens: a document of escaping characters at the default bound is served within the envelope",
          "[ui][custom-screens][remote]") {
    Server s;
    morph::ui::CustomScreensOptions const bounds;
    // Backslashes are the worst case: each is one byte in the document, two in the reply's JSON and four in the
    // envelope's body. The padding leaves room for the longer id the model writes in.
    std::string const filled = replaced(baseScreen("lims.base"), "Results", "@@");
    std::size_t const room = bounds.maxDocumentBytes - filled.size() - 64;
    std::string const document = replaced(filled, "@@", [&] {
        std::string pad;
        for (std::size_t i = 0; i < room / 4; ++i) {
            pad += R"(\\\\)";
        }
        return pad;
    }());
    REQUIRE(document.size() <= bounds.maxDocumentBytes);
    REQUIRE(document.size() > bounds.maxDocumentBytes - 256);

    std::string const id = savedId(s.save(document, "alice"));
    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    std::size_t const wireSize = morph::wire::encode(morph::wire::makeOk(0, got.body)).size();
    CHECK(wireSize <= morph::wire::kMaxEnvelopeBytes);
    CHECK(wireSize > 3 * document.size());
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    CHECK(screens_test::load(found.document).id == id);
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
        morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(
            morph::ui::CustomScreenScope scope) override {
            return inner.list(std::move(scope));
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
    Envelope const again =
        s.call(morph::ui::kCustomScreensSave,
               R"({"app":"lims","id":)" + quoted(id) + R"(,"title":"Renamed","basedOn":"lims.base","document":)" +
                   quoted(screens_test::replaced(baseScreen("lims.base"), "Results", "Changed")) + "}",
               "alice");
    CHECK(savedId(again) == id);
    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    CHECK(found.document.contains("Changed"));
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK(mine.screens.front().title == "Renamed");

    Envelope const removed =
        s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(removed.kind == "ok");
    CHECK(listed(s, "alice").screens.empty());
    CHECK(s.call(morph::ui::kCustomScreensRemove, R"({"app":"lims","id":)" + quoted(id) + "}", "alice").kind == "err");
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

TEST_CASE("custom screens: check and save bound the stored text, which carries the longer id",
          "[ui][custom-screens][remote]") {
    std::string const document = baseScreen("lims.base");
    auto const checkBody = R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(document) + "}";
    auto const verdictOf = [&](Server& s) {
        Envelope const reply = s.call(morph::ui::kCustomScreensCheck, checkBody, "alice");
        REQUIRE(reply.kind == "ok");
        morph::ui::CustomScreenChecked verdict;
        REQUIRE_FALSE(glz::read_json(verdict, reply.body));
        return verdict;
    };
    // The document itself is within the bound; the id the model writes into it is longer than the one it had.
    Server tight{nullptr, morph::ui::CustomScreensOptions{.maxDocumentBytes = document.size()}};
    auto const refused = verdictOf(tight);
    REQUIRE(refused.refusal.has_value());
    CHECK(refused.refusal->reason.contains("longer than"));
    Envelope const saved = tight.save(document, "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains(refused.refusal->reason));
    CHECK(listed(tight, "alice").screens.empty());

    // Once the bound holds the rewritten text, both accept.
    std::size_t const stored =
        morph::ui::withScreenId(document, std::string{morph::ui::kCustomScreenIdPrefix} + std::string(32, '0'))
            ->size();
    Server roomy{nullptr, morph::ui::CustomScreensOptions{.maxDocumentBytes = stored}};
    CHECK_FALSE(verdictOf(roomy).refusal.has_value());
    CHECK(roomy.save(document, "alice").kind == "ok");
}

TEST_CASE("custom screens: a document far above the bound is refused without being parsed",
          "[ui][custom-screens][remote]") {
    Server s;
    morph::ui::CustomScreensOptions const bounds;
    // Not JSON: a parse would refuse it for that, so the size refusal shows the length was checked first.
    std::string const huge(bounds.maxDocumentBytes + 1024, '{');
    auto const body = R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(huge) + "}";
    Envelope const checked = s.call(morph::ui::kCustomScreensCheck, body, "alice");
    REQUIRE(checked.kind == "ok");
    morph::ui::CustomScreenChecked verdict;
    REQUIRE_FALSE(glz::read_json(verdict, checked.body));
    REQUIRE(verdict.refusal.has_value());
    CHECK(verdict.refusal->reason ==
          "a custom screen is longer than " + std::to_string(bounds.maxDocumentBytes) + " bytes");
    Envelope const saved = s.save(huge, "alice");
    REQUIRE(saved.kind == "err");
    CHECK(saved.message.contains("is longer than"));
}

TEST_CASE("custom screens: a title outside its bound is refused", "[ui][custom-screens][remote]") {
    Server s;
    CHECK(s.save(baseScreen("lims.base"), "alice", "").kind == "err");
    CHECK(s.save(baseScreen("lims.base"), "alice", std::string(morph::ui::kMaxNameBytes + 1, 'x')).kind == "err");
    CHECK(s.save(baseScreen("lims.base"), "alice", std::string(morph::ui::kMaxNameBytes, 'x')).kind == "ok");
}

namespace {

/// Stores a screen that the base no longer matches: it carries a host command with a huge literal argument, so the
/// refusal that makes it broken quotes the whole command.
void storeBrokenScreen(morph::ui::MemoryCustomScreenStore& store, std::string const& id, std::size_t argBytes) {
    std::string const document =
        replaced(baseScreen(id), R"({"host":"print"})",
                 R"({"host":"print","args":{"lit":")" + std::string(argBytes, 'x') + R"("}})");
    REQUIRE(settled(store.put({.person = "alice", .app = "lims", .id = id},
                              morph::ui::CustomScreen{.id = id,
                                                      .title = "Big",
                                                      .basedOn = "lims.base",
                                                      .document = document,
                                                      .savedAt = {},
                                                      .savedBy = std::string(1000, 'p')})));
}

}  // namespace

TEST_CASE("custom screens: a reply's fields other than the document are capped", "[ui][custom-screens][remote]") {
    morph::ui::CustomScreensOptions options;
    options.maxDocumentBytes = (morph::wire::kMaxEnvelopeBytes - morph::ui::kCustomScreenReplyOverhead) / 4;
    // The store runs inline, so the test thread can fill it directly.
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(morph::exec::detail::inlineExecutor());
    Server s{store, options};
    std::string const id = "custom:0123456789abcdef0123456789abcdef";
    storeBrokenScreen(*store, id, std::size_t{200} * 1024);

    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    CHECK(morph::wire::encode(morph::wire::makeOk(0, got.body)).size() <= morph::wire::kMaxEnvelopeBytes);
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    REQUIRE(found.summary.broken.has_value());
    std::string const& reason = found.summary.broken->reason;
    CHECK(reason.size() <= morph::ui::kMaxBrokenReasonBytes);
    CHECK(reason.ends_with("\xE2\x80\xA6"));
    CHECK(found.summary.savedBy.size() <= morph::ui::kMaxNameBytes);
    CHECK(found.summary.savedBy.ends_with("\xE2\x80\xA6"));
    CHECK(found.summary.broken->pointer.size() <= morph::ui::kMaxNameBytes);
}

TEST_CASE("custom screens: list answers at most maxListed and says it truncated", "[ui][custom-screens][remote]") {
    Server s{nullptr, morph::ui::CustomScreensOptions{.maxListed = 2}};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(s.save(baseScreen("lims.base"), "alice").kind == "ok");
    }
    auto const all = listed(s, "alice");
    CHECK(all.screens.size() == 2);
    CHECK(all.truncated);

    Server roomy{nullptr, morph::ui::CustomScreensOptions{.maxListed = 3}};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(roomy.save(baseScreen("lims.base"), "alice").kind == "ok");
    }
    auto const exact = listed(roomy, "alice");
    CHECK(exact.screens.size() == 3);
    CHECK_FALSE(exact.truncated);
}

TEST_CASE("custom screens: a full list of adversarial summaries fits one envelope", "[ui][custom-screens][remote]") {
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(morph::exec::detail::inlineExecutor());
    morph::ui::CustomScreensOptions const options;
    // Every field is full of control bytes, which are the most an escaped byte grows, and the id and basedOn are
    // longer than a name may be.
    std::string const control(5000, '\x0b');
    for (std::size_t i = 0; i < options.maxListed + 5; ++i) {
        std::string const id = "custom:" + std::to_string(1000 + i) + control;
        // A record at its bound, of control bytes too: each is written as a six-byte escape, which the bound counts.
        morph::ui::Json::object_t record;
        record.emplace("pad", std::string((morph::ui::kMaxAttributesBytes - 10) / 6, '\x0b'));
        REQUIRE(settled(store->put({.person = "alice", .app = "lims", .id = id, .session = {}},
                                   morph::ui::CustomScreen{.id = id,
                                                           .title = control,
                                                           .basedOn = control,
                                                           .document = "{}",
                                                           .savedAt = {},
                                                           .savedBy = control,
                                                           .attributes = record})));
    }
    Server s{store, options};
    Envelope const reply = s.call(morph::ui::kCustomScreensList, R"({"app":"lims"})", "alice");
    REQUIRE(reply.kind == "ok");
    CHECK(morph::wire::encode(morph::wire::makeOk(0, reply.body)).size() <= morph::wire::kMaxEnvelopeBytes);
    morph::ui::CustomScreensListed out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    CHECK(out.screens.size() == options.maxListed);
    CHECK(out.truncated);
    // The records were sent, not dropped as too long.
    CHECK(std::ranges::all_of(
        out.screens, [](morph::ui::CustomScreenSummary const& screen) { return screen.attributes.has_value(); }));
}

TEST_CASE("custom screens: only an id that starts with `custom:` is read, written or removed",
          "[ui][custom-screens][remote]") {
    Server s;
    std::string const doc = quoted(baseScreen("lims.base"));
    for (auto const& [action, body] : std::vector<std::pair<std::string_view, std::string>>{
             {morph::ui::kCustomScreensGet, R"({"app":"lims","id":"lims.base"})"},
             {morph::ui::kCustomScreensRemove, R"({"app":"lims","id":"lims.base"})"},
             {morph::ui::kCustomScreensSave,
              joined(
                  {R"({"app":"lims","id":"lims.base","title":"T","basedOn":"lims.base","document":)", doc, "}"})}}) {
        Envelope const reply = s.call(action, body, "alice");
        INFO(action);
        REQUIRE(reply.kind == "err");
        CHECK(reply.message.contains("starts with `custom:`"));
    }
}

TEST_CASE("custom screens: a stored entry whose id is not its document's is listed broken",
          "[ui][custom-screens][remote]") {
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(morph::exec::detail::inlineExecutor());
    auto const plant = [&](std::string const& id, std::string const& document) {
        REQUIRE(settled(
            store->put({.person = "alice", .app = "lims", .id = id}, morph::ui::CustomScreen{.id = id,
                                                                                             .title = id,
                                                                                             .basedOn = "lims.base",
                                                                                             .document = document,
                                                                                             .savedAt = {},
                                                                                             .savedBy = "alice"})));
    };
    plant("custom:a", baseScreen("custom:a"));
    plant("custom:b", baseScreen("custom:other"));
    plant("lims.base", baseScreen("lims.base"));
    Server s{store};
    auto const out = listed(s, "alice");
    REQUIRE(out.screens.size() == 3);
    for (auto const& screen : out.screens) {
        INFO(screen.id);
        if (screen.id == "custom:a") {
            CHECK_FALSE(screen.broken.has_value());
        } else {
            REQUIRE(screen.broken.has_value());
            CHECK(screen.broken->pointer == "/id");
        }
    }
}

TEST_CASE("custom screens: a summary's hash is the stored document's and changes with it",
          "[ui][custom-screens][remote]") {
    Server s;
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    auto const hashOf = [&] {
        auto const out = listed(s, "alice");
        REQUIRE(out.screens.size() == 1);
        return out.screens.front().hash;
    };
    std::string const first = hashOf();
    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    CHECK(first == morph::ui::contentHash(found.document));
    CHECK(found.summary.hash == first);

    Envelope const edited =
        s.call(morph::ui::kCustomScreensSave,
               R"({"app":"lims","id":)" + quoted(id) + R"(,"title":"Mine","basedOn":"lims.base","document":)" +
                   quoted(replaced(baseScreen("lims.base"), R"("text":"Results")", R"("text":"Edited")")) + "}",
               "alice");
    REQUIRE(edited.kind == "ok");
    CHECK(hashOf() != first);
}

TEST_CASE("custom screens: opensWithoutParams follows the base's params", "[ui][custom-screens][remote]") {
    std::string const withParams =
        replaced(baseScreen("lims.params"), R"("models":)", R"("params":{"sampleId":"int"},"models":)");
    morph::ui::UiRegistry made = uiservice::registry();
    REQUIRE(made.addScreen(uiservice::entry("lims.base"), baseScreen("lims.base")));
    REQUIRE(made.addScreen(uiservice::entry("lims.params"), withParams));
    auto const built = made.build();
    REQUIRE(built);
    Server s{nullptr, {}, *built};

    REQUIRE(s.save(baseScreen("lims.base"), "alice").kind == "ok");
    std::string const custom = replaced(withParams, R"("id":"lims.params")", R"("id":"custom:p")");
    Envelope const saved =
        s.call(morph::ui::kCustomScreensSave,
               R"({"app":"lims","title":"P","basedOn":"lims.params","document":)" + quoted(custom) + "}", "alice");
    REQUIRE(saved.kind == "ok");

    auto const out = listed(s, "alice");
    REQUIRE(out.screens.size() == 2);
    for (auto const& screen : out.screens) {
        INFO(screen.basedOn);
        CHECK_FALSE(screen.broken.has_value());
        CHECK(screen.opensWithoutParams == (screen.basedOn == "lims.base"));
    }
}

TEST_CASE("custom screens: a client that finds no custom screens model is told it is unknown",
          "[ui][custom-screens][remote]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    morph::exec::ThreadPoolExecutor pool{1};
    auto const authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
    std::promise<std::string> replied;
    auto reply = replied.get_future();
    server->handle(morph::wire::encode(morph::wire::makeRegister(std::string{morph::ui::kCustomScreensModel})),
                   [&replied](std::string text) { replied.set_value(std::move(text)); });
    REQUIRE(reply.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    Envelope const answer = morph::wire::decode(reply.get());
    CHECK(answer.kind == "err");
    CHECK(answer.message == "unknown model type: morph.customScreens");
}

TEST_CASE("custom screens: get refuses a stored document longer than the bound", "[ui][custom-screens][remote]") {
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(morph::exec::detail::inlineExecutor());
    std::string const id = "custom:0123456789abcdef0123456789abcdef";
    std::string const document = baseScreen(id);
    REQUIRE(settled(
        store->put({.person = "alice", .app = "lims", .id = id}, morph::ui::CustomScreen{.id = id,
                                                                                         .title = "Big",
                                                                                         .basedOn = "lims.base",
                                                                                         .document = document,
                                                                                         .savedAt = {},
                                                                                         .savedBy = "alice"})));
    Server s{store, morph::ui::CustomScreensOptions{.maxDocumentBytes = document.size() - 1}};
    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "err");
    CHECK(got.message.contains("longer than"));
    Server roomy{store, morph::ui::CustomScreensOptions{.maxDocumentBytes = document.size()}};
    CHECK(roomy.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice").kind == "ok");
}

TEST_CASE("custom screens: with no bundle, custom screens are not served", "[ui][custom-screens][remote]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    auto const authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    auto const screens = morph::ui::registerCustomScreens(
        std::make_shared<morph::ui::MemoryCustomScreenStore>(pool),
        []() -> std::shared_ptr<morph::ui::ServedBundle const> { return nullptr; }, authorizer, dispatcher, registry);
    morph::session::Context alice;
    alice.principal = "alice";
    auto const allowance = screens->allowance(alice, "lims", "lims.base");
    REQUIRE_FALSE(allowance);
    CHECK(allowance.error().reason == "custom screens are not served: no bundle");
}

namespace {

/// Runs `CustomScreens::check` for @p session on @p executor and waits for it.
::core::async::Task<void> askCheck(morph::ui::CustomScreens* screens, morph::session::Context session,
                                   std::string document, std::promise<std::optional<morph::ui::Refusal>>* out) {
    out->set_value(co_await screens->check(session, "lims", "lims.base", std::move(document)));
}

/// Runs `CustomScreens::get` for @p session.
::core::async::Task<void> askGet(
    morph::ui::CustomScreens* screens, morph::session::Context session, std::string id,
    std::promise<std::expected<morph::ui::CustomScreenFound, morph::ui::CustomScreenRefusal>>* out) {
    out->set_value(co_await screens->get(session, "lims", std::move(id)));
}

morph::session::Context asPerson(std::string principal) {
    morph::session::Context session;
    session.principal = std::move(principal);
    return session;
}

}  // namespace

TEST_CASE("custom screens: the handle answers what the actions answer", "[ui][custom-screens][handle]") {
    Server s;
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    // The fixture authorizer admits `alice` by the token the envelope carries; the handle takes the session as is.
    morph::session::Context const alice = asPerson("alice");

    SECTION("check") {
        std::promise<std::optional<morph::ui::Refusal>> ok;
        morph::async::spawn(s.pool, askCheck(s.screens.get(), alice, baseScreen("lims.base"), &ok));
        auto accepted = ok.get_future();
        REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        CHECK_FALSE(accepted.get().has_value());

        std::promise<std::optional<morph::ui::Refusal>> no;
        morph::async::spawn(
            s.pool, askCheck(s.screens.get(), alice,
                             replaced(baseScreen("lims.base"), R"({"host":"print"})", R"({"signOut":true})"), &no));
        auto refused = no.get_future();
        REQUIRE(refused.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        auto const refusal = refused.get();
        REQUIRE(refusal.has_value());
        CHECK(refusal->reason.contains("signOut"));
    }
    SECTION("check without a verified person") {
        std::promise<std::optional<morph::ui::Refusal>> none;
        morph::async::spawn(s.pool, askCheck(s.screens.get(), asPerson(""), baseScreen("lims.base"), &none));
        auto answer = none.get_future();
        REQUIRE(answer.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        auto const refusal = answer.get();
        REQUIRE(refusal.has_value());
        CHECK(refusal->reason.contains("signed-in person"));
    }
    SECTION("get") {
        std::promise<std::expected<morph::ui::CustomScreenFound, morph::ui::CustomScreenRefusal>> found;
        morph::async::spawn(s.pool, askGet(s.screens.get(), alice, id, &found));
        auto answer = found.get_future();
        REQUIRE(answer.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        auto const screen = answer.get();
        REQUIRE(screen);
        CHECK(screen->summary.id == id);
        CHECK(screens_test::load(screen->document).id == id);

        std::promise<std::expected<morph::ui::CustomScreenFound, morph::ui::CustomScreenRefusal>> bobs;
        morph::async::spawn(s.pool, askGet(s.screens.get(), asPerson("bob"), id, &bobs));
        auto refused = bobs.get_future();
        REQUIRE(refused.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        auto const refusal = refused.get();
        REQUIRE_FALSE(refusal);
        CHECK(refusal.error().reason.contains("no such custom screen"));
    }
    SECTION("allowance") {
        auto const text = s.screens->allowance(alice, "lims", "lims.base");
        REQUIRE(text);
        // One of each thing the base rule compares, as the base uses it.
        CHECK(text->contains("Lims_Sample/Lims_List (query)"));
        CHECK(text->contains("Lims_Sample/Lims_Save (mutation)"));
        CHECK(text->contains("onMount: run Lims_Sample/Lims_Save"));
        CHECK(text->contains("upload"));
        CHECK(text->contains("host `print`"));
        CHECK(text->contains("host `ResultGrid`"));
        CHECK(text->contains(R"({"host":"print"})"));
        CHECK(text->contains("component `Chart`"));
        CHECK_FALSE(text->contains("signOut"));
        CHECK_FALSE(s.screens->allowance(alice, "lims", "lims.nothing"));
    }
    SECTION("allowance is refused where the checks refuse") {
        auto const otherApp = s.screens->allowance(alice, "other", "lims.base");
        REQUIRE_FALSE(otherApp);
        CHECK(otherApp.error().reason.contains("custom screens belong to `lims`, not `other`"));
        auto const noPerson = s.screens->allowance(asPerson(""), "lims", "lims.base");
        REQUIRE_FALSE(noPerson);
        CHECK(noPerson.error().reason.contains("signed-in person"));
        // A person who may not run the base's queries is shown nothing of it, as `check` refuses them.
        s.authorizer->denied["alice"] = {{"Lims_Sample", "Lims_List"}};
        auto const barred = s.screens->allowance(alice, "lims", "lims.base");
        REQUIRE_FALSE(barred);
        CHECK(barred.error().reason == "this person may not open the base screen `lims.base`");
        CHECK(s.screens->allowance(asPerson("bob"), "lims", "lims.base").has_value());
    }
}

namespace {

/// A store where every screen of an application is shared: anyone lists and opens it, its author overwrites it, and
/// only its author or a session in the `admin` role removes it. The role is read from the session the model passes,
/// here its metadata; a deployment reads it from the claims of the verified token the same session carries.
class SharedStore final : public morph::ui::ICustomScreenStore {
public:
    explicit SharedStore(morph::exec::IExecutor& base) : _strand{base} {}

    morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(morph::ui::CustomScreenScope scope) override {
        std::vector<morph::ui::CustomScreen> out;
        {
            std::scoped_lock const lock{_mutex};
            _seen.push_back(scope.session);
            for (auto const& [key, screen] : _entries) {
                if (key.first == scope.app) {
                    out.push_back(screen);
                }
            }
        }
        return settledWith(std::move(out));
    }

    morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
        morph::ui::CustomScreenKey key) override {
        std::scoped_lock const lock{_mutex};
        _seen.push_back(key.session);
        auto const found = _entries.find({key.app, key.id});
        if (found == _entries.end()) {
            return settledWith(std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>{
                std::unexpected(morph::ui::CustomScreenRefusal{.reason = "no such custom screen"})});
        }
        return settledWith(std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>{found->second});
    }

    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
        morph::ui::CustomScreenKey key, morph::ui::CustomScreen screen) override {
        std::scoped_lock const lock{_mutex};
        _seen.push_back(key.session);
        _entries.insert_or_assign(std::pair{key.app, key.id}, std::move(screen));
        return settledWith(std::expected<void, morph::ui::CustomScreenRefusal>{});
    }

    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
        morph::ui::CustomScreenKey key) override {
        std::scoped_lock const lock{_mutex};
        _seen.push_back(key.session);
        auto const found = _entries.find({key.app, key.id});
        if (found == _entries.end()) {
            return settledWith(std::expected<void, morph::ui::CustomScreenRefusal>{
                std::unexpected(morph::ui::CustomScreenRefusal{.reason = "no such custom screen"})});
        }
        auto const role = key.session.metadata.find("role");
        bool const admin = role != key.session.metadata.end() && role->second == "admin";
        if (found->second.savedBy != key.person && !admin) {
            return settledWith(std::expected<void, morph::ui::CustomScreenRefusal>{std::unexpected(
                morph::ui::CustomScreenRefusal{.reason = "only its author or an administrator may remove it"})});
        }
        _entries.erase(found);
        return settledWith(std::expected<void, morph::ui::CustomScreenRefusal>{});
    }

    /// The sessions every call carried, in order; read once the calls have answered.
    [[nodiscard]] std::vector<morph::session::Context> const& seen() const { return _seen; }

private:
    template <typename T>
    morph::async::Completion<T> settledWith(T value) {
        auto [completion, promise] = morph::async::Completion<T>::makeSettleable(&_strand);
        promise.resolve(std::move(value));
        return std::move(completion);
    }

    std::mutex _mutex;
    std::vector<morph::session::Context> _seen;
    std::map<std::pair<std::string, std::string>, morph::ui::CustomScreen> _entries;
    /// The serial owner the answers are delivered on, declared last so it is destroyed first.
    morph::exec::OwnerStrand _strand;
};

/// Runs `remove` of @p screenId as @p token, with @p role in the session's metadata when it is given.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen, the caller's token, then the role.
Envelope removeAs(Server& s, std::string const& screenId, std::string token, std::string const& role = {}) {
    Envelope request;
    request.kind = "execute";
    request.callId = ++s.lastCallId;
    request.modelId = s.instance;
    request.modelType = std::string{morph::ui::kCustomScreensModel};
    request.actionType = std::string{morph::ui::kCustomScreensRemove};
    request.body = R"({"app":"lims","id":)" + quoted(screenId) + "}";
    request.session.token = std::move(token);
    if (!role.empty()) {
        request.session.metadata.emplace("role", role);
    }
    return s.send(request);
}

}  // namespace

// Mutation: build the store's key without the session (the store reads no role, and the administrator is refused).
TEST_CASE("custom screens: the store is given the verified session, so its policy can rest on a role",
          "[ui][custom-screens][remote]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<SharedStore>(storePool);
    Server s{store};
    std::string const id = savedId(s.save(baseScreen("lims.base"), "alice"));
    REQUIRE(listed(s, "bob").screens.size() == 1);

    Envelope const refused = removeAs(s, id, "bob");
    REQUIRE(refused.kind == "err");
    CHECK(refused.message.contains("only its author or an administrator may remove it"));
    Envelope const removed = removeAs(s, id, "carol", "admin");
    INFO(removed.message);
    CHECK(removed.kind == "ok");
    CHECK(listed(s, "alice").screens.empty());

    // Every call carried the session of the verified person, and the removal the session it was asked under.
    REQUIRE_FALSE(store->seen().empty());
    CHECK(std::ranges::none_of(store->seen(),
                               [](morph::session::Context const& session) { return session.principal.empty(); }));
    CHECK(std::ranges::any_of(store->seen(), [](morph::session::Context const& session) {
        return session.principal == "carol" && session.metadata.contains("role");
    }));
}

namespace {

/// @p text read as an attributes record; fails the test when it is not one.
morph::ui::Json::object_t attributesOf(std::string const& text) {
    morph::ui::Json::object_t out;
    REQUIRE_FALSE(glz::read_json(out, text));
    return out;
}

/// @p attributes written as JSON, for comparing records.
std::string written(std::optional<morph::ui::Json::object_t> const& attributes) {
    return attributes.has_value() ? glz::write_json(*attributes).value_or("?") : std::string{"absent"};
}

}  // namespace

// Mutation: drop the attributes from the stored screen (the list and get answer none).
TEST_CASE("custom screens: a store's attributes round-trip through save, list and get",
          "[ui][custom-screens][remote]") {
    Server s;
    std::string const record = R"({"default":false,"note":"for the morning shift","shared":["team-a"]})";
    Envelope const saved = s.call(morph::ui::kCustomScreensSave,
                                  R"({"app":"lims","title":"Mine","basedOn":"lims.base","document":)" +
                                      quoted(baseScreen("lims.base")) + R"(,"attributes":)" + record + "}",
                                  "alice");
    std::string const id = savedId(saved);
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK(written(mine.screens.front().attributes) == written(attributesOf(record)));

    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    CHECK(written(found.summary.attributes) == written(attributesOf(record)));

    // A save that names none stores none: the record is the store's, and the model adds nothing of its own.
    REQUIRE(s.save(baseScreen("lims.base"), "bob").kind == "ok");
    auto const bobs = listed(s, "bob");
    REQUIRE(bobs.screens.size() == 1);
    CHECK_FALSE(bobs.screens.front().attributes.has_value());
}

// Mutation: skip the attributes bound on save (the oversized record is stored and listed).
TEST_CASE("custom screens: a save whose attributes are over the bound is refused", "[ui][custom-screens][remote]") {
    Server s;
    std::string const pad(morph::ui::kMaxAttributesBytes, 'x');
    Envelope const refused = s.call(morph::ui::kCustomScreensSave,
                                    R"({"app":"lims","title":"Mine","basedOn":"lims.base","document":)" +
                                        quoted(baseScreen("lims.base")) + R"(,"attributes":{"pad":")" + pad + "\"}}",
                                    "alice");
    REQUIRE(refused.kind == "err");
    CHECK(refused.message.contains("`attributes` are at most 1024 bytes"));
    CHECK(listed(s, "alice").screens.empty());

    // At the bound exactly, it is stored: `{"pad":"` and `"}` take 10 bytes of it.
    std::string const fits(morph::ui::kMaxAttributesBytes - 10, 'x');
    CHECK(s.call(morph::ui::kCustomScreensSave,
                 R"({"app":"lims","title":"Mine","basedOn":"lims.base","document":)" +
                     quoted(baseScreen("lims.base")) + R"(,"attributes":{"pad":")" + fits + "\"}}",
                 "alice")
              .kind == "ok");
}

// Mutation: send a stored record whatever its length (the list carries it, and the screen is not broken).
TEST_CASE("custom screens: a stored record too long to send is listed broken, without it",
          "[ui][custom-screens][remote]") {
    auto const store = std::make_shared<morph::ui::MemoryCustomScreenStore>(morph::exec::detail::inlineExecutor());
    std::string const id = "custom:0123456789abcdef0123456789abcdef";
    REQUIRE(settled(store->put(
        {.person = "alice", .app = "lims", .id = id, .session = {}},
        morph::ui::CustomScreen{
            .id = id,
            .title = "Mine",
            .basedOn = "lims.base",
            .document = baseScreen(id),
            .savedAt = {},
            .savedBy = "alice",
            .attributes = attributesOf(R"({"pad":")" + std::string(morph::ui::kMaxAttributesBytes, 'x') + "\"}")})));
    Server s{store};
    auto const mine = listed(s, "alice");
    REQUIRE(mine.screens.size() == 1);
    CHECK_FALSE(mine.screens.front().attributes.has_value());
    REQUIRE(mine.screens.front().broken.has_value());
    CHECK(mine.screens.front().broken->reason.contains("attributes"));
    CHECK_FALSE(mine.screens.front().opensWithoutParams);
}

// ---- Versions ----

// Registered globally with ids of their own, and in a fixture's dispatcher under the base's form action, so the
// model's schema lookup finds a field with a version.
// NOLINTBEGIN(misc-use-internal-linkage): glaze's reflection needs a type with linkage.
struct CSVVerify {
    std::string sample;
    double pressure = 0.0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "pressure", .since = "3.0"},
    };
};

struct CSVModel {
    static int execute(CSVVerify const& /*action*/) { return 0; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(CSVModel, "Test_CSVModel")
BRIDGE_REGISTER_ACTION(CSVModel, CSVVerify, "Test_CSVVerify")

namespace {

/// The base screen with `results` given a version: what 2.0 added.
std::string versionedBase(std::string_view id) {
    return replaced(baseScreen(id), R"({"kind":"text","text":"Results"})",
                    R"({"kind":"text","id":"results","since":"2.0","text":"Results"})");
}

/// A server whose `lims.base` is @p base.
Server serverWith(std::string const& base, morph::ui::CustomScreensOptions options = {}) {
    morph::ui::UiRegistry made = uiservice::registry();
    REQUIRE(made.addScreen(uiservice::entry("lims.base"), base));
    auto const built = made.build();
    REQUIRE(built);
    return Server{nullptr, options, *built};
}

/// The document `get` answers for @p id.
std::string storedDocument(Server& s, std::string const& id) {
    Envelope const got = s.call(morph::ui::kCustomScreensGet, R"({"app":"lims","id":)" + quoted(id) + "}", "alice");
    REQUIRE(got.kind == "ok");
    morph::ui::CustomScreenFound found;
    REQUIRE_FALSE(glz::read_json(found, got.body));
    return found.document;
}

/// The summary `list` answers for @p id.
morph::ui::CustomScreenSummary summaryOf(Server& s, std::string const& id) {
    for (auto const& screen : listed(s, "alice").screens) {
        if (screen.id == id) {
            return screen;
        }
    }
    FAIL("not listed: " << id);
    return {};
}

std::optional<std::string> knownVersionIn(std::string const& document) {
    auto const json = morph::ui::parseJson(document).value();
    auto const& object = json.get_object();
    auto const* const found = object.find("knownVersion");
    if (found == object.end()) {
        return std::nullopt;
    }
    return found->second.get_string();
}

}  // namespace

// Mutation: skip withBaseVersion in the save of a new screen (the stored document records no version).
TEST_CASE("custom screens: a new screen records its base's version and lists as current",
          "[ui][custom-screens][versions]") {
    Server s = serverWith(versionedBase("lims.base"));
    std::string const id = savedId(s.save(versionedBase("lims.base"), "alice"));
    CHECK(knownVersionIn(storedDocument(s, id)) == std::optional<std::string>{"2.0"});
    auto const summary = summaryOf(s, id);
    CHECK_FALSE(summary.broken.has_value());
    CHECK_FALSE(summary.needsUpdate);
    CHECK_FALSE(summary.needsAttention);

    // An overwrite keeps the version the document carries, and so does a new screen that records one.
    std::string const older =
        replaced(versionedBase("lims.base"), R"("vocab":"ui/1",)", R"("vocab":"ui/1","knownVersion":"1.0",)");
    Envelope const overwritten =
        s.call(morph::ui::kCustomScreensSave,
               R"({"app":"lims","id":)" + quoted(id) + R"(,"title":"Mine","basedOn":"lims.base","document":)" +
                   quoted(older) + "}",
               "alice");
    REQUIRE(overwritten.kind == "ok");
    CHECK(knownVersionIn(storedDocument(s, id)) == std::optional<std::string>{"1.0"});
    CHECK(summaryOf(s, id).needsUpdate);
    std::string const copied = savedId(s.save(older, "alice"));
    CHECK(knownVersionIn(storedDocument(s, copied)) == std::optional<std::string>{"1.0"});

    // A base with no version gives a new screen none to record.
    Server plain;
    std::string const unversioned = savedId(plain.save(baseScreen("lims.base"), "alice"));
    CHECK_FALSE(knownVersionIn(storedDocument(plain, unversioned)).has_value());
}

// Mutation: leave needsAttention false in summarize (the screen with a removed element is not marked).
TEST_CASE("custom screens: list marks a screen that refers to what its base removed",
          "[ui][custom-screens][versions]") {
    Server s;
    std::string const legacy =
        replaced(baseScreen("lims.base"), R"({"kind":"text","text":"Results"})",
                 R"({"kind":"text","text":"Results"},{"kind":"text","id":"legacy","since":"1.0","text":"Old"})");
    std::string const id = savedId(s.save(legacy, "alice"));
    auto const summary = summaryOf(s, id);
    CHECK_FALSE(summary.broken.has_value());
    CHECK(summary.needsAttention);
    CHECK_FALSE(summary.needsUpdate);
}

// Mutation: drop kKnownVersionSlack from the size pre-check (a document within the bound once stamped is refused
// without being parsed).
TEST_CASE("custom screens: check and save bound the stored text with the base's version in it",
          "[ui][custom-screens][versions]") {
    std::string const document = versionedBase("lims.base");
    auto const checkBody = R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(document) + "}";
    std::string const rewritten =
        morph::ui::withScreenId(document, std::string{morph::ui::kCustomScreenIdPrefix} + std::string(32, '0'))
            .value();
    std::size_t const stored =
        morph::ui::withKnownVersion(rewritten, morph::forms::FieldVersion::parse("2.0").value()).value().size();
    REQUIRE(stored > rewritten.size());
    auto const verdictOf = [&](Server& s) {
        Envelope const reply = s.call(morph::ui::kCustomScreensCheck, checkBody, "alice");
        REQUIRE(reply.kind == "ok");
        morph::ui::CustomScreenChecked verdict;
        REQUIRE_FALSE(glz::read_json(verdict, reply.body));
        return verdict;
    };
    Server tight = serverWith(document, morph::ui::CustomScreensOptions{.maxDocumentBytes = rewritten.size()});
    auto const refused = verdictOf(tight);
    REQUIRE(refused.refusal.has_value());
    CHECK(refused.refusal->reason.contains("longer than"));
    CHECK(tight.save(document, "alice").kind == "err");

    Server roomy = serverWith(document, morph::ui::CustomScreensOptions{.maxDocumentBytes = stored});
    CHECK_FALSE(verdictOf(roomy).refusal.has_value());
    CHECK(roomy.save(document, "alice").kind == "ok");

    // White space the rewrite drops: the sent text is longer than the bound plus the id's room, and what is stored
    // fits, so it is parsed rather than refused for its length.
    std::string const padded = "{" + std::string(stored - document.size() + 100, ' ') + document.substr(1);
    REQUIRE(padded.size() == stored + 100);
    auto const paddedBody = R"({"app":"lims","basedOn":"lims.base","document":)" + quoted(padded) + "}";
    Envelope const paddedReply = roomy.call(morph::ui::kCustomScreensCheck, paddedBody, "alice");
    REQUIRE(paddedReply.kind == "ok");
    morph::ui::CustomScreenChecked paddedVerdict;
    REQUIRE_FALSE(glz::read_json(paddedVerdict, paddedReply.body));
    CHECK_FALSE(paddedVerdict.refusal.has_value());
}

// Mutation: answer nothing from the dispatcher's schema lookup (the field's version is not the base's).
TEST_CASE("custom screens: the base's version counts the fields of the actions its forms bind",
          "[ui][custom-screens][versions]") {
    std::string const withForm =
        replaced(baseScreen("lims.base"), R"({"kind":"text","text":"Results"})",
                 R"({"kind":"text","text":"Results"},{"kind":"form","id":"check","model":"sample",)"
                 R"("action":"Lims_Verify","submit":"explicit"})");
    Server s = serverWith(withForm);
    s.dispatcher.registerAction<CSVModel, CSVVerify>("Lims_Sample", "Lims_Verify");
    std::string const id = savedId(s.save(withForm, "alice"));
    CHECK(knownVersionIn(storedDocument(s, id)) == std::optional<std::string>{"3.0"});

    // A copy that knows an older version is told that `pressure` came since.
    std::string const older = replaced(withForm, R"("vocab":"ui/1",)", R"("vocab":"ui/1","knownVersion":"2.0",)");
    std::string const old = savedId(s.save(older, "alice"));
    CHECK(summaryOf(s, old).needsUpdate);
}

// The bound `list` and `get` are sized by: every capped field at its cap, every flag set.
TEST_CASE("custom screens: a summary at every cap with every flag set is within kMaxSummaryBytes",
          "[ui][custom-screens][versions]") {
    std::string const name(morph::ui::kMaxNameBytes, 'n');
    morph::ui::Json::object_t record;
    // Written as JSON the record is exactly at its bound: the key, its quotes, the colon and the braces take 10 bytes.
    record.emplace("pad", std::string(morph::ui::kMaxAttributesBytes - 10, 'a'));
    REQUIRE(glz::write_json(record).value().size() == morph::ui::kMaxAttributesBytes);
    morph::ui::CustomScreenSummary const summary{
        .id = name,
        .title = name,
        .basedOn = name,
        .savedAt = morph::time::DateTime::now(),
        .savedBy = name,
        .mine = true,
        .hash = "sha256:" + std::string(64, 'f'),
        .opensWithoutParams = true,
        .broken =
            morph::ui::CheckRefusal{.pointer = name, .reason = std::string(morph::ui::kMaxBrokenReasonBytes, 'r')},
        .attributes = record,
        .needsUpdate = true,
        .needsAttention = true,
    };
    std::string const written = glz::write_json(summary).value();
    UNSCOPED_INFO("a full summary is " << written.size() << " bytes");
    CHECK(written.size() <= morph::ui::kMaxSummaryBytes);
}
