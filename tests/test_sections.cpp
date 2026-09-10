// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/sections.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support.hpp"

// ---------------------------------------------------------------------------
// Fixture: two independent sections of one screen -- a profile block and a
// preferences block. Neither is a step of the other. Editing them in either
// order is the whole point of SectionSet, and is what FlowSession refuses.
// ---------------------------------------------------------------------------

namespace {
/// Records which actions the model actually executed, in order.
struct SecRecorder {
    std::mutex mtx;
    std::vector<std::string> fired;

    void record(std::string what) {
        std::scoped_lock const lock{mtx};
        fired.push_back(std::move(what));
    }
    void clear() {
        std::scoped_lock const lock{mtx};
        fired.clear();
    }
    [[nodiscard]] std::vector<std::string> snapshot() {
        std::scoped_lock const lock{mtx};
        return fired;
    }
};
SecRecorder& recorder() {
    static SecRecorder inst;
    return inst;
}
}  // namespace

struct SecProfile {
    std::string name;
    [[nodiscard]] bool validate() const { return !name.empty(); }
};
struct SecProfileResult {
    std::int64_t id = 0;
};

struct SecPrefs {
    std::int64_t profileId = 0;
    std::string theme;
    [[nodiscard]] bool validate() const { return !theme.empty(); }
};
struct SecPrefsResult {
    std::string summary;
};

struct SecExplodes {
    std::string label;
    [[nodiscard]] bool validate() const { return !label.empty(); }
};
struct SecExplodesResult {
    std::int64_t id = 0;
};

struct SecModel {
    SecProfileResult execute(SecProfile action) {
        recorder().record("SectionsTest_Profile");
        return {.id = static_cast<std::int64_t>(action.name.size())};
    }
    SecPrefsResult execute(SecPrefs action) {
        recorder().record("SectionsTest_Prefs");
        return {.summary = action.theme};
    }
    SecExplodesResult execute(SecExplodes) {
        recorder().record("SectionsTest_Explodes");
        throw std::runtime_error{"section boom"};
    }
};

BRIDGE_REGISTER_MODEL(SecModel, "SectionsTest_Model")
BRIDGE_REGISTER_ACTION(SecModel, SecProfile, "SectionsTest_Profile")
BRIDGE_REGISTER_ACTION(SecModel, SecPrefs, "SectionsTest_Prefs")
BRIDGE_REGISTER_ACTION(SecModel, SecExplodes, "SectionsTest_Explodes")

using ProfileSection = morph::forms::Section<SecProfile, "Profile">;
using PrefsSection =
    morph::forms::Section<SecPrefs, "Preferences", morph::forms::Bind<"profileId", "SectionsTest_Profile.id">>;
using ExplodesSection = morph::forms::Section<SecExplodes, "Explodes">;

using DemoGroup = morph::forms::SectionGroup<"Account settings", ProfileSection, PrefsSection>;
BRIDGE_REGISTER_SECTION_GROUP(DemoGroup, "SectionsTest_DemoGroup")

TEST_CASE("sectionGroupSchemaJson carries each section's title, action and binds", "[sections]") {
    auto const json = morph::forms::sectionGroupSchemaJson<DemoGroup>();
    REQUIRE_FALSE(json.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, json));

    CHECK(json.contains(R"("s-id":"SectionsTest_DemoGroup")"));
    CHECK(json.contains(R"("s-title":"Account settings")"));
    CHECK(json.contains(R"("action":"SectionsTest_Profile")"));
    CHECK(json.contains(R"("title":"Profile")"));
    CHECK(json.contains(R"("action":"SectionsTest_Prefs")"));
    CHECK(json.contains(R"("prefill":{"profileId":"SectionsTest_Profile.id"})"));

    // A section with no Bind carries no prefill key at all, rather than an
    // empty object a renderer would have to special-case.
    auto const& sections = dom["s-sections"].get_array();
    REQUIRE(sections.size() == 2);
    CHECK_FALSE(sections[0].contains("prefill"));

    // No order is implied: a section carries no index field, because a renderer
    // chooses its own arrangement and an emitted position would suggest a
    // sequence a SectionSet does not have.
    CHECK_FALSE(sections[0].contains("index"));
    CHECK_FALSE(sections[1].contains("index"));
}

// ---------------------------------------------------------------------------
// Each case builds its own bridge/handler/section set: a SectionSet holds no
// global state, and a shared one would let a late dispatch from a previous case
// land in this one's recorder.
// ---------------------------------------------------------------------------

TEST_CASE("SectionSet: sections fire independently, in any order", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().clear();

    // Edit the SECOND section first. Under FlowSession this throws
    // std::logic_error -- "field belongs to an action that is not the current
    // step" -- which is exactly the gap morph#513 reports.
    sections.set<&SecPrefs::theme>("dark");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));

    // Then the first. Both fire; neither was ever "current".
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 2; }));

    auto const fired = recorder().snapshot();
    CHECK(fired[0] == "SectionsTest_Prefs");
    CHECK(fired[1] == "SectionsTest_Profile");
}

TEST_CASE("SectionSet: a not-ready draft does not dispatch", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().clear();

    // SecPrefs::validate() requires a non-empty theme; profileId alone is not
    // ready. Without the readiness gate this would dispatch a half-filled action.
    sections.set<&SecPrefs::profileId>(7);
    CHECK(recorder().snapshot().empty());

    // Completing it dispatches, carrying the field set earlier.
    sections.set<&SecPrefs::theme>("light");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));
}

TEST_CASE("SectionSet: an already-fired section fires again on the next edit", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().clear();
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));

    // No latch: the draft is still ready, so it dispatches again. Matching
    // FlowSession, which also re-fires a ready step on every set<>.
    sections.set<&SecProfile::name>("grace");
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 2; }));
}
