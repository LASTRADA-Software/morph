// SPDX-License-Identifier: Apache-2.0
//
// The custom screens' memory store, and the model through a RemoteServer.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/async/Task.hpp>
#include <future>
#include <initializer_list>
#include <morph/core/completion.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/custom_screens.hpp>
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
    auto const storedHers = settled(store.put({.person = "alice", .app = "lims", .id = "custom:2"}, std::move(hers)));
    REQUIRE(storedHers);
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
        REQUIRE(settled(
            store->put({.person = "alice", .app = "lims", .id = id}, morph::ui::CustomScreen{.id = id,
                                                                                             .title = control,
                                                                                             .basedOn = control,
                                                                                             .document = "{}",
                                                                                             .savedAt = {},
                                                                                             .savedBy = control})));
    }
    Server s{store, options};
    Envelope const reply = s.call(morph::ui::kCustomScreensList, R"({"app":"lims"})", "alice");
    REQUIRE(reply.kind == "ok");
    CHECK(morph::wire::encode(morph::wire::makeOk(0, reply.body)).size() <= morph::wire::kMaxEnvelopeBytes);
    morph::ui::CustomScreensListed out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    CHECK(out.screens.size() == options.maxListed);
    CHECK(out.truncated);
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
    morph::exec::ThreadPoolExecutor pool{1};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
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
