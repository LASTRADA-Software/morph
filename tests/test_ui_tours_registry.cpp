// SPDX-License-Identifier: Apache-2.0
//
// The bundle checks of guided tours (docs/spec/ui/tours.md, "Checks"): `UiRegistry::build` refuses a tour that names a
// screen, a target or a tour the bundle does not have, naming the tour and the step, and the tour card's id is the
// client's own, as the chat panel's is.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <morph/ui/client_ids.hpp>
#include <morph/ui/served_document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_parts.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ui = morph::ui;

namespace {

/// The id of the tour `home` starts and the valid tours declare; a test that wants it undeclared names another.
constexpr std::string_view kTourId = "getting-started";

// A screen with a literal test id in its view, one inside a component's fallback, and a head action.
constexpr std::string_view kSamples = R"({"vocab":"ui/1","id":"lims.samples","title":"Samples",
  "state":{"n":{"init":0}},
  "actions":[{"id":"new","label":"New","onClick":[{"set":"n","value":1}]}],
  "components":{"Chip":{"host":"Chip","version":1,
    "fallback":{"kind":"text","testId":"chip_fallback","text":"chip"}}},
  "view":{"kind":"column","children":[{"kind":"text","testId":"samples_table","text":"rows"}]}})";

/// A screen whose button runs `tour.start` with @p args.
std::string home(std::string_view args) {
    return R"({"vocab":"ui/1","id":"lims.home","title":"Home","state":{"which":{"init":")" + std::string{kTourId} +
           R"("}},
      "view":{"kind":"button","testId":"take_tour","label":"Take the tour",
              "onClick":[{"host":"tour.start","args":)" +
           std::string{args} + "}]}}";
}

/// The tours of the control bundle: @c kTourId, and one that belongs to a screen.
std::string validTours() {
    return R"([
  {"id":")" +
           std::string{kTourId} +
           R"(","title":"Getting started","steps":[
    {"target":"shell_tours","title":"Tours"},
    {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open Samples"},
    {"target":"samples_table","title":"The table"},
    {"target":"action_new","title":"New"},
    {"target":"chip_fallback","title":"A chip"},
    {"target":"chat_input","title":"Ask"},
    {"screen":"lims.home","target":"take_tour","title":"Again"}]},
  {"id":"register","screen":"lims.samples","title":"Register","steps":[{"target":"samples_table","title":"Here"}]}])";
}

struct Bundle {
    std::string tours = validTours();
    std::string homeArgs = R"({"obj":{"id":")" + std::string{kTourId} + R"("}})";
    /// Members added to the app document beside `tours`, each preceded by a comma.
    std::string appMembers;
    std::optional<ui::ShellParts> parts;
};

/// Registers the two screens, a host screen `explorer`, the app document with @p bundle's tours and parts, and
/// builds.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> build(Bundle const& bundle) {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "lims", .version = "1"}};
    auto const add = [&registry](std::string id, std::string text) {
        ui::CatalogEntry entry;
        entry.id = std::move(id);
        entry.title = "Screen";
        auto const added = registry.addScreen(std::move(entry), std::move(text));
        REQUIRE(added.has_value());
    };
    add("lims.samples", std::string{kSamples});
    add("lims.home", home(bundle.homeArgs));
    ui::CatalogEntry login;
    login.id = "lims.login";
    login.title = "Sign in";
    login.isPublic = true;
    auto const loginAdded = registry.addScreen(
        std::move(login), R"({"vocab":"ui/1","id":"lims.login","view":{"kind":"text","text":"x"}})");
    REQUIRE(loginAdded.has_value());
    ui::CatalogEntry explorer;
    explorer.id = "explorer";
    explorer.title = "Explorer";
    explorer.host = std::string{"explorer"};
    auto const explorerAdded = registry.addEntry(std::move(explorer));
    REQUIRE(explorerAdded.has_value());
    auto const app = registry.addFile(
        ui::BundleFile{.kind = "app",
                       .id = "app",
                       .bytes = R"({"vocab":"ui/1","id":"app","tours":)" + bundle.tours + bundle.appMembers + "}"});
    REQUIRE(app.has_value());
    if (bundle.parts) {
        REQUIRE(registry.setShellParts(*bundle.parts).has_value());
    }
    return registry.build();
}

/// The refusal of building @p bundle.
ui::Refusal refusalOf(Bundle const& bundle) {
    auto const built = build(bundle);
    REQUIRE_FALSE(built.has_value());
    return built.error();
}

/// A one-tour list whose only tour is general, with @p steps.
std::string oneTour(std::string_view steps) {
    // Named as `home`'s literal `tour.start` names its tour, so only the name a test wants refused is in doubt.
    return R"([{"id":")" + std::string{kTourId} + R"(","title":"T","steps":)" + std::string{steps} + "}]";
}

}  // namespace

// The control: every kind of accepted target, a step's screen, an advanceOn and a literal tour.start build.
TEST_CASE("tours registry: a valid tour builds", "[ui][tours][service]") {
    auto const built = build(Bundle{});
    INFO((built ? std::string{"built"} : built.error().message()));
    CHECK(built.has_value());
}

// Mutation: accept every target in `checkTour` (the misspelt id builds).
TEST_CASE("tours registry: a target nothing carries is refused, naming the tour and the step",
          "[ui][tours][service]") {
    ui::Refusal const refused =
        refusalOf(Bundle{.tours = oneTour(R"([{"screen":"lims.samples","target":"samples_tabel","title":"S"}])")});
    CHECK(refused.document == "app");
    CHECK(refused.path == "/tours/0/steps/0/target");
    CHECK(refused.reason.contains("the tour `getting-started`, step 1"));
    CHECK(refused.reason.contains("samples_tabel"));
}

// Mutation: skip the registered-screen check of the tour's `screen`, the step's `screen` or `advanceOn.screen`.
TEST_CASE("tours registry: a screen a tour names that is not registered is refused", "[ui][tours][service]") {
    CHECK(refusalOf(Bundle{.tours = R"([{"id":")" + std::string{kTourId} +
                                    R"(","screen":"lims.nope","title":"T","steps":[{"title":"S"}]}])"})
              .path == "/tours/0/screen");
    CHECK(refusalOf(Bundle{.tours = oneTour(R"([{"screen":"lims.nope","title":"S"}])")}).path ==
          "/tours/0/steps/0/screen");
    CHECK(refusalOf(Bundle{.tours = oneTour(R"([{"advanceOn":{"screen":"lims.nope"},"title":"S"}])")}).path ==
          "/tours/0/steps/0/advanceOn/screen");
}

// Mutation: read a host screen's targets as if it had a document (its step with a page target builds).
TEST_CASE("tours registry: a step on a host screen may ring only shell ids", "[ui][tours][service]") {
    ui::Refusal const refused =
        refusalOf(Bundle{.tours = oneTour(R"([{"screen":"explorer","target":"samples_table","title":"S"}])")});
    CHECK(refused.path == "/tours/0/steps/0/target");
    CHECK(refused.reason.contains("host screen"));
    CHECK(
        build(Bundle{.tours = oneTour(R"([{"screen":"explorer","target":"shell_tours","title":"S"}])")}).has_value());
}

// Mutation: accept every `shell_*` id in `shellTarget` (the undeclared help button builds).
TEST_CASE("tours registry: a shell id whose part is not declared is refused", "[ui][tours][service]") {
    std::string const tours = oneTour(R"([{"target":"shell_help","title":"S"}])");
    CHECK(refusalOf(Bundle{.tours = tours}).path == "/tours/0/steps/0/target");
    ui::ShellParts parts;
    parts.help = ui::HelpEntry{.screen = "lims.home"};
    CHECK(build(Bundle{.tours = tours, .parts = parts}).has_value());
}

// Mutation: ignore `advanceOn` when following the effective screen (the target after it is refused).
TEST_CASE("tours registry: the effective screen follows a step's screen and the advanceOn before it",
          "[ui][tours][service]") {
    CHECK(build(Bundle{.tours = oneTour(R"([
      {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open"},
      {"target":"samples_table","title":"Here"}])")})
              .has_value());
    CHECK(build(Bundle{.tours = oneTour(R"([
      {"screen":"lims.home","target":"take_tour","title":"Home"},
      {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open"},
      {"target":"samples_table","title":"Here"}])")})
              .has_value());
    CHECK(refusalOf(Bundle{.tours = oneTour(R"([
      {"target":"samples_table","title":"Too early"},
      {"target":"nav_lims.samples","advanceOn":{"screen":"lims.samples"},"title":"Open"}])")})
              .path == "/tours/0/steps/0/target");
}

// Mutation: skip `collectTourStarts` for the screens (the undeclared id builds).
TEST_CASE("tours registry: a literal tour.start naming an undeclared tour is refused, a computed one is not",
          "[ui][tours][service]") {
    ui::Refusal const refused = refusalOf(Bundle{.homeArgs = R"({"obj":{"id":"nope"}})"});
    CHECK(refused.document == "lims.home");
    CHECK(refused.path == "/view/onClick/0");
    CHECK(refused.reason.contains("`nope`"));
    CHECK(build(Bundle{.homeArgs = R"({"obj":{"id":{"ref":"which"}}})"}).has_value());
}

// Mutation: read only the `obj` form of `args` in `literalTourId` (the `lit` form builds).
TEST_CASE("tours registry: a literal tour.start written as a lit record is checked too", "[ui][tours][service]") {
    ui::Refusal const refused = refusalOf(Bundle{.homeArgs = R"({"lit":{"id":"nope"}})"});
    CHECK(refused.document == "lims.home");
    CHECK(refused.reason.contains("`nope`"));
    CHECK(build(Bundle{.homeArgs = R"({"lit":{"id":")" + std::string{kTourId} + R"("}})"}).has_value());
}

// Mutation: skip the app document's own text in `checkTours` (a start inside it builds).
TEST_CASE("tours registry: a literal tour.start inside the app document naming an undeclared tour is refused",
          "[ui][tours][service]") {
    std::string const start = R"(,"note":{"host":"tour.start","args":{"obj":{"id":"nope"}}})";
    ui::Refusal const refused = refusalOf(Bundle{.appMembers = start});
    CHECK(refused.document == "app");
    CHECK(refused.path == "/note");
    CHECK(refused.reason.contains("`nope`"));
    CHECK(build(Bundle{.appMembers = std::string{R"(,"note":{"host":"tour.start","args":{"obj":{"id":")"} +
                                     std::string{kTourId} + R"("}}})"})
              .has_value());
}

// Mutation: accept every `nav_` id in `shellTarget` (a public or a host screen's entry builds).
TEST_CASE("tours registry: a nav target of a public screen or a host screen is refused", "[ui][tours][service]") {
    for (std::string_view const screen : {"lims.login", "explorer"}) {
        INFO(screen);
        ui::Refusal const refused =
            refusalOf(Bundle{.tours = oneTour(R"([{"target":"nav_)" + std::string{screen} + R"(","title":"S"}])")});
        CHECK(refused.path == "/tours/0/steps/0/target");
        CHECK(refused.reason.contains("nav_" + std::string{screen}));
    }
    CHECK(build(Bundle{.tours = oneTour(R"([{"target":"nav_lims.samples","title":"S"}])")}).has_value());
}

// Mutation: skip the load error in `checkTourReferences` (the tours are not checked and the bundle builds).
TEST_CASE("tours registry: an app document that no longer loads refuses the build", "[ui][tours][service]") {
    auto open = std::make_shared<bool>(true);
    ui::Catalog catalog;
    catalog.model = [](std::string_view) -> std::optional<ui::ModelInfo> { return ui::ModelInfo{}; };
    catalog.action = [open](std::string_view) -> std::optional<ui::ActionInfo> {
        if (!*open) {
            return std::nullopt;
        }
        return ui::ActionInfo{.modelType = "Prefs"};
    };
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "lims", .version = "1"}, catalog};
    auto const app =
        registry.addFile(ui::BundleFile{.kind = "app",
                                        .id = "app",
                                        .bytes = R"({"vocab":"ui/1","id":"app","models":{"prefs":{"type":"Prefs"}},)"
                                                 R"("queries":{"settings":{"model":"prefs","action":"Prefs_Get"}},)"
                                                 R"("tours":[{"id":"a","title":"T","steps":[{"title":"S"}]}]})"});
    REQUIRE(app.has_value());
    CHECK(registry.build().has_value());
    *open = false;
    auto const refused = registry.build();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().document == "app");
    CHECK(refused.error().reason.contains("the app document does not load"));
}

// Mutation: drop `morph.tour` from `kReservedClientIds` (the screen registers, and no refusal names it).
TEST_CASE("tours registry: the tour card's id is the client's own", "[ui][tours][service]") {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "lims", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "morph.tour";
    entry.title = "Forged";
    auto const added = registry.addScreen(std::move(entry),
                                          R"({"vocab":"ui/1","id":"morph.tour","view":{"kind":"text","text":"x"}})");
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().reason == "the id `morph.tour` is the tour card's");
    std::optional<ui::Refusal> const served = ui::servedScreenRefusal("morph.tour");
    REQUIRE(served.has_value());
    CHECK(served->reason.contains("which is the tour card's"));
    CHECK(ui::servedScreenRefusal("morph.chat")->reason.contains("which is the chat panel's"));
    CHECK_FALSE(ui::servedScreenRefusal("lims.home").has_value());
}
