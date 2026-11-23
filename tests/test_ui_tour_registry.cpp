// SPDX-License-Identifier: Apache-2.0
//
// Runtime tours on the server (docs/spec/ui/tours.md, "Registering tours at runtime"): `TourRegistry` runs the
// bundle's checks on each tour it is given, `list` answers the tours a person may open in registration order, every
// change is told and fans out as a change of the record `morph.tours`, and a bundle change drops a tour that no longer
// passes.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/tour_registry.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"
#include "ui_service_support.hpp"

namespace ui = morph::ui;

namespace {

/// The service fixture's bundle, with a message catalogue that has the key `tours.new.title`.
std::shared_ptr<ui::ServedBundle const> served() {
    ui::UiRegistry registry = uiservice::registry();
    auto const added = registry.addFile(uiservice::file("messages", "i18n/en.json", R"({"tours.new.title":"New"})"));
    REQUIRE(added.has_value());
    auto built = registry.build();
    REQUIRE(built.has_value());
    return *built;
}

/// A tour on `lims.results` (every principal may open it) whose one step rings nothing.
std::string resultsTour(std::string const& id, std::string const& title = R"("Results")") {
    return R"({"id":")" + id + R"(","screen":"lims.results","title":)" + title + R"(,"steps":[{"title":"Here"}]})";
}

/// A general tour whose one step opens `lims.verify`, which only a verifier opens.
std::string verifyTour(std::string const& id) {
    return R"({"id":")" + id + R"(","title":"Verify","steps":[{"screen":"lims.verify","title":"Check"}]})";
}

struct Rig {
    std::shared_ptr<ui::ServedBundle const> bundle = served();
    std::shared_ptr<ui::TourRegistry> tours =
        std::make_shared<ui::TourRegistry>([this] { return std::shared_ptr<ui::ServedBundle const>{bundle}; });
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    int changes = 0;
    std::vector<ui::TourRefusal> dropped;
    Rig() {
        tours->setOnChanged([this] { ++changes; });
        tours->setOnDropped([this](ui::TourRefusal const& refusal) { dropped.push_back(refusal); });
    }
    [[nodiscard]] std::vector<std::string> listedIds(std::string const& person) {
        morph::session::Context session;
        session.principal = person;
        std::vector<std::string> ids;
        for (ui::ListedTour const& listed : tours->listFor(session, *authorizer).tours) {
            ids.push_back(listed.id);
        }
        return ids;
    }
};

}  // namespace

// Mutation: skip `detail::checkTour` in `TourRegistry::checked` (the unknown target is added).
TEST_CASE("tour registry: add runs the bundle's checks and refuses with the tour and the step",
          "[ui][tours][runtime]") {
    Rig r;
    CHECK(r.tours->add(resultsTour("whats-new")).has_value());
    CHECK(r.changes == 1);
    auto const unknownScreen = r.tours->add(R"({"id":"a","screen":"lims.nope","title":"A","steps":[{"title":"S"}]})");
    REQUIRE_FALSE(unknownScreen.has_value());
    CHECK(unknownScreen.error().tourId == "a");
    CHECK(unknownScreen.error().path == "/screen");
    auto const unknownTarget = r.tours->add(
        R"({"id":"b","screen":"lims.results","title":"B","steps":[{"title":"S"},{"target":"nothing_here","title":"T"}]})");
    REQUIRE_FALSE(unknownTarget.has_value());
    CHECK(unknownTarget.error().path == "/steps/1/target");
    CHECK(unknownTarget.error().reason.contains("step 2"));
    auto const malformed = r.tours->add(R"({"id":"Bad Id","title":"C","steps":[{"title":"S"}]})");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().path == "/id");
    CHECK(r.changes == 1);
}

// Mutation: build the refusal with `Refusal::document` / the app document's pointers kept (path
// `/tours/0/steps/1/target`).
TEST_CASE("tour registry: a refusal names the runtime tour and points into the submitted tour, not the app document",
          "[ui][tours][runtime]") {
    Rig r;
    auto const target = r.tours->add(
        R"({"id":"b","screen":"lims.results","title":"B","steps":[{"title":"S"},{"target":"nothing_here","title":"T"}]})");
    REQUIRE_FALSE(target.has_value());
    CHECK(target.error().tourId == "b");
    CHECK(target.error().path == "/steps/1/target");
    CHECK(target.error().reason.contains("the tour `b`, step 2"));
    std::string const line = target.error().message();
    CHECK(line.contains("the tour `b`"));
    CHECK_FALSE(line.contains("/tours/"));
    CHECK_FALSE(line.contains("app"));
    // A shape the loader refuses, in a step.
    auto const shape = r.tours->add(R"({"id":"c","title":"C","steps":[{"title":"S"},{"title":"T","advanceOn":7}]})");
    REQUIRE_FALSE(shape.has_value());
    CHECK(shape.error().tourId == "c");
    CHECK(shape.error().path.starts_with("/steps/1"));
    CHECK_FALSE(shape.error().message().contains("/tours/"));
}

// Mutation: drop the id check against the registry's own tours in `add` (the second add succeeds).
TEST_CASE("tour registry: an id the app document or another runtime tour uses is refused by add",
          "[ui][tours][runtime]") {
    Rig r;
    REQUIRE(r.tours->add(resultsTour("whats-new")).has_value());
    auto const again = r.tours->add(resultsTour("whats-new"));
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().path == "/id");
    // A bundle whose app document declares the tour `basics`.
    auto const appBundle = [] {
        ui::UiRegistry made{ui::ApplicationInfo{.id = "lims", .version = "1"}, uiservice::facts()};
        REQUIRE(made.addScreen(uiservice::entry("lims.results"), std::string{uiservice::kResults}).has_value());
        REQUIRE(made.addFile(uiservice::file("app", "app",
                                             R"({"vocab":"ui/1","id":"app","tours":[{"id":"basics","title":"B",)"
                                             R"("steps":[{"title":"S"}]}]})"))
                    .has_value());
        auto built = made.build();
        REQUIRE(built.has_value());
        return *built;
    }();
    ui::TourRegistry withAppTour{[appBundle] { return appBundle; }};
    auto const clash = withAppTour.add(resultsTour("basics"));
    REQUIRE_FALSE(clash.has_value());
    CHECK(clash.error().path == "/id");
    CHECK(clash.error().reason.contains("the app document already declares a tour with the id `basics`"));
}

// Mutation: accept a `t` key in no catalogue in `missingTextKey` (the tour is added).
TEST_CASE("tour registry: a text key in neither the catalogues nor the server's texts is refused",
          "[ui][tours][runtime]") {
    Rig r;
    CHECK(r.tours->add(resultsTour("known", R"({"t":"tours.new.title","fallback":"New"})")).has_value());
    auto const unknown = r.tours->add(resultsTour("unknown", R"({"t":"tours.nowhere","fallback":"Nowhere"})"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().path == "/title");
    CHECK(unknown.error().reason.contains("tours.nowhere"));
}

// Mutation: let `replace` add an id it does not hold (the unknown replace succeeds).
TEST_CASE("tour registry: replace and remove refuse an id the registry does not hold, and each change is told",
          "[ui][tours][runtime]") {
    Rig r;
    CHECK_FALSE(r.tours->replace(resultsTour("missing")).has_value());
    CHECK_FALSE(r.tours->remove("missing").has_value());
    REQUIRE(r.tours->add(resultsTour("whats-new")).has_value());
    morph::session::Context anyone;
    std::uint64_t const first = r.tours->listFor(anyone, *r.authorizer).tours.front().revision;
    REQUIRE(r.tours->replace(resultsTour("whats-new", R"("Results, again")")).has_value());
    ui::ToursListed const listed = r.tours->listFor(anyone, *r.authorizer);
    REQUIRE(listed.tours.size() == 1);
    CHECK(listed.tours.front().revision > first);
    CHECK(listed.tours.front().tour.contains("Results, again"));
    REQUIRE(r.tours->remove("whats-new").has_value());
    CHECK(r.tours->listFor(anyone, *r.authorizer).tours.empty());
    CHECK(r.changes == 3);
}

// Mutation: list every tour whatever `screenAdmitted` says (bob sees the verifier's tour).
TEST_CASE("tour registry: list answers registration order, leaving out a tour whose screens the person may not open",
          "[ui][tours][runtime]") {
    Rig r;
    REQUIRE(r.tours->add(resultsTour("first")).has_value());
    REQUIRE(r.tours->add(verifyTour("second")).has_value());
    REQUIRE(r.tours->add(resultsTour("third")).has_value());
    r.authorizer->denied["bob"].insert({"Lims_Sample", "Lims_Pending"});
    CHECK(r.listedIds("ann") == std::vector<std::string>{"first", "second", "third"});
    CHECK(r.listedIds("bob") == std::vector<std::string>{"first", "third"});
}

// Mutation: skip the digest comparison in `TourRegistry` (the tour on the vanished screen stays listed).
TEST_CASE("tour registry: a bundle change drops a tour that no longer passes, logs it and tells onDropped",
          "[ui][tours][runtime]") {
    Rig r;
    REQUIRE(r.tours->add(verifyTour("verify")).has_value());
    REQUIRE(r.tours->add(resultsTour("results")).has_value());
    // A bundle without `lims.verify`.
    ui::UiRegistry smaller{ui::ApplicationInfo{.id = "lims", .version = "2"}, uiservice::facts()};
    REQUIRE(smaller.addScreen(uiservice::entry("lims.results"), std::string{uiservice::kResults}).has_value());
    REQUIRE(smaller.addFile(uiservice::file("app", "app", std::string{uiservice::kApp})).has_value());
    REQUIRE(smaller.addFile(uiservice::file("messages", "i18n/en.json", R"({"tours.new.title":"New"})")).has_value());
    auto built = smaller.build();
    REQUIRE(built.has_value());
    r.bundle = *built;
    CHECK(r.listedIds("ann") == std::vector<std::string>{"results"});
    REQUIRE(r.dropped.size() == 1);
    CHECK(r.dropped.front().tourId == "verify");
    CHECK(r.dropped.front().reason.contains("lims.verify"));
}

// Mutation: leave out the warning in `TourRegistry::tell` (nothing is logged), or log it on every change.
TEST_CASE("tour registry: a change made while nothing publishes it logs one warning", "[ui][tours][runtime]") {
    auto const bundle = served();
    std::vector<std::string> warnings;
    morph::log::ScopedLoggerOverride const guard{[&warnings](morph::log::LogLevel level, std::string_view line) {
                                                     if (level == morph::log::LogLevel::warn) {
                                                         warnings.emplace_back(line);
                                                     }
                                                 },
                                                 morph::log::LogLevel::debug};
    ui::TourRegistry silent{[bundle] { return bundle; }};
    CHECK_FALSE(silent.add(R"({"id":"a","screen":"lims.nope","title":"A","steps":[{"title":"S"}]})").has_value());
    CHECK(warnings.empty());
    REQUIRE(silent.add(resultsTour("one")).has_value());
    REQUIRE(silent.add(resultsTour("two")).has_value());
    REQUIRE(silent.remove("two").has_value());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().contains("publishTourChanges"));
    // A registry with a handler says nothing.
    warnings.clear();
    int told = 0;
    ui::TourRegistry heard{[bundle] { return bundle; }};
    heard.setOnChanged([&told] { ++told; });
    REQUIRE(heard.add(resultsTour("one")).has_value());
    CHECK(told == 1);
    CHECK(warnings.empty());
}

// Mutation: make `recheck` do nothing (the tour on the vanished screen stays, and nothing is told).
TEST_CASE("tour registry: recheck drops a tour the swapped bundle refuses, without a call that reads the bundle",
          "[ui][tours][runtime]") {
    Rig r;
    REQUIRE(r.tours->add(verifyTour("verify")).has_value());
    REQUIRE(r.tours->add(resultsTour("results")).has_value());
    CHECK(r.dropped.empty());
    ui::UiRegistry smaller{ui::ApplicationInfo{.id = "lims", .version = "2"}, uiservice::facts()};
    REQUIRE(smaller.addScreen(uiservice::entry("lims.results"), std::string{uiservice::kResults}).has_value());
    REQUIRE(smaller.addFile(uiservice::file("app", "app", std::string{uiservice::kApp})).has_value());
    auto built = smaller.build();
    REQUIRE(built.has_value());
    r.bundle = *built;
    int const before = r.changes;
    r.tours->recheck();
    REQUIRE(r.dropped.size() == 1);
    CHECK(r.dropped.front().tourId == "verify");
    CHECK(r.changes == before + 1);
    CHECK(r.listedIds("ann") == std::vector<std::string>{"results"});
    CHECK(r.dropped.size() == 1);
    r.bundle = nullptr;
    CHECK_THROWS_AS(r.tours->recheck(), std::logic_error);
}

namespace {

/// Negotiates events for the connection it arrives on, as the UI service's `ui-hello` does.
class EventsHello final : public morph::backend::IEnvelopeService {
public:
    using IEnvelopeService::handle;
    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == "t-tours-hello"; }
    [[nodiscard]] std::string handle(morph::wire::Envelope const& request,
                                     morph::session::IAuthorizer const& /*authorizer*/) override {
        return morph::wire::encode(morph::wire::makeOk(request.callId));
    }
    [[nodiscard]] std::string handle(morph::wire::Envelope const& request,
                                     morph::session::IAuthorizer const& authorizer,
                                     morph::backend::ConnectionFacts& facts) override {
        facts.events = true;
        return handle(request, authorizer);
    }
};

}  // namespace

// Mutation: leave `publishTourChanges` with no handler (the connection receives no `changes`).
TEST_CASE("tour registry: published changes reach a connection watching morph.tours", "[ui][tours][runtime]") {
    Rig r;
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    ui::registerTours(r.tours, r.authorizer, dispatcher, registry);
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.coalesceWindow = std::chrono::milliseconds{0};
    config.services.push_back(std::make_shared<EventsHello>());
    auto const server =
        std::make_shared<morph::backend::RemoteServer>(pool, r.authorizer, config, dispatcher, registry);
    ui::publishTourChanges(*r.tours, server);
    std::mutex mutex;
    std::vector<std::string> frames;
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, {.write =
                                          [&](std::string frame) {
                                              std::scoped_lock const lock{mutex};
                                              frames.push_back(std::move(frame));
                                          },
                                      .backlog = [] { return std::size_t{0}; }});
    morph::wire::Envelope hello;
    hello.kind = "t-tours-hello";
    hello.callId = 1;
    REQUIRE(morph::wire::decode(server->handleInline(morph::wire::encode(hello), cid)).kind == "ok");
    morph::wire::Envelope watch;
    watch.kind = std::string{morph::wire::kWatchKind};
    watch.callId = 2;
    watch.session.token = "ann";
    watch.body = glz::write_json(morph::wire::WatchBody{
                                     .records = {std::string{ui::kTourRecord}}, .instances = {}, .replace = false})
                     .value_or(std::string{});
    auto const watched =
        morph::wire::readWatchReply(morph::wire::decode(server->handleInline(morph::wire::encode(watch), cid)).body);
    REQUIRE(watched.has_value());
    REQUIRE(watched->refusedRecords.empty());
    REQUIRE(r.tours->add(resultsTour("whats-new")).has_value());
    REQUIRE(morph::testing::waitUntil([&] {
        std::scoped_lock const lock{mutex};
        return std::ranges::any_of(frames, [](std::string const& frame) {
            return frame.contains(morph::wire::kChangesTopic) && frame.contains("morph.tours");
        });
    }));
}
