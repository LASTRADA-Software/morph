// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
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
#include <thread>
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

// A section whose model call blocks until the test releases it, so a dispatch
// can be held genuinely in flight across the SectionSet's destruction. Both
// flags are function-local statics for the same reason as the recorder: the
// model's execute() and the test body both need them, and SecModel is
// stateless. Mirrors test_flows_apps.cpp's FlowStepSlow.
namespace {
std::atomic<bool>& secSlowStarted() {
    static std::atomic<bool> flag{false};
    return flag;
}
std::atomic<bool>& secSlowRelease() {
    static std::atomic<bool> flag{false};
    return flag;
}
}  // namespace

struct SecSlow {
    std::string label;
    [[nodiscard]] bool validate() const { return !label.empty(); }
};
struct SecSlowResult {
    std::int64_t id = 0;
};

struct SecModel {
    SecProfileResult execute(const SecProfile& action) {
        recorder().record("SectionsTest_Profile");
        return {.id = static_cast<std::int64_t>(action.name.size())};
    }
    SecPrefsResult execute(const SecPrefs& action) {
        recorder().record("SectionsTest_Prefs");
        return {.summary = action.theme};
    }
    SecExplodesResult execute(const SecExplodes&) {
        recorder().record("SectionsTest_Explodes");
        throw std::runtime_error{"section boom"};
    }
    static SecSlowResult execute(const SecSlow& /*action*/) {
        secSlowStarted().store(true, std::memory_order_relaxed);
        while (!secSlowRelease().load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        throw std::runtime_error{"late section boom"};
    }
};

BRIDGE_REGISTER_MODEL(SecModel, "SectionsTest_Model")
BRIDGE_REGISTER_ACTION(SecModel, SecProfile, "SectionsTest_Profile")
BRIDGE_REGISTER_ACTION(SecModel, SecPrefs, "SectionsTest_Prefs")
BRIDGE_REGISTER_ACTION(SecModel, SecExplodes, "SectionsTest_Explodes")
BRIDGE_REGISTER_ACTION(SecModel, SecSlow, "SectionsTest_Slow")

using ProfileSection = morph::forms::Section<SecProfile, "Profile">;
using PrefsSection =
    morph::forms::Section<SecPrefs, "Preferences", morph::forms::Bind<"profileId", "SectionsTest_Profile.id">>;
using ExplodesSection = morph::forms::Section<SecExplodes, "Explodes">;
using SlowSection = morph::forms::Section<SecSlow, "Slow">;

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

TEST_CASE("SectionSet: a not-ready draft is not sent at all", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};

    // The gate is observed through onError, not through the recorder. The
    // bridge enforces ActionValidator on its own dispatch path (bridge.hpp),
    // so an ungated draft would never reach SecModel::execute either -- it
    // would come back as a validation failure. A round trip that can only fail
    // is the cost this gate exists to avoid, and the error callback is where
    // that cost is visible.
    std::atomic<int> errors{0};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{
        handler, [&errors](const std::exception_ptr&) { errors.fetch_add(1); }};

    recorder().clear();

    // SecPrefs::validate() requires a non-empty theme; profileId alone is not ready.
    sections.set<&SecPrefs::profileId>(7);
    CHECK(morph::testing::waitUntil([&errors] { return errors.load() != 0; }, std::chrono::milliseconds{300}) ==
          false);

    // Completing it dispatches, carrying the field set earlier, and succeeds.
    // Waiting on the capture rather than on the recorder: the recorder is
    // written inside execute(), which runs before the completion that captures.
    sections.set<&SecPrefs::theme>("light");
    REQUIRE(morph::testing::waitUntil([&] { return sections.resolved("SectionsTest_Prefs.summary").has_value(); }));
    CHECK(recorder().snapshot().size() == 1);
    CHECK(errors.load() == 0);
    CHECK(sections.resolved("SectionsTest_Prefs.profileId") == std::string{"7"});
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

TEST_CASE("SectionSet: reset clears one section and leaves the others intact", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().clear();
    sections.set<&SecPrefs::profileId>(42);  // not ready: no theme yet
    sections.set<&SecProfile::name>("ada");  // ready: fires

    CHECK(sections.draft<SecPrefs>().profileId == 42);
    CHECK(sections.draft<SecProfile>().name == "ada");

    sections.reset<SecPrefs>();

    CHECK(sections.draft<SecPrefs>().profileId == 0);
    // Per-section isolation is the point: resetting one editor must not wipe
    // what the user typed into another.
    CHECK(sections.draft<SecProfile>().name == "ada");
}

TEST_CASE("SectionSet: a fired section's fields are resolvable; an unfired one is not", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};
    morph::forms::SectionSet<SecModel, ProfileSection, PrefsSection> sections{handler};

    recorder().clear();
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return sections.resolved("SectionsTest_Profile.id").has_value(); }));

    // The result's field: SecProfileResult::id == name.size() == 3.
    CHECK(sections.resolved("SectionsTest_Profile.id") == std::string{"3"});
    // The submitted draft's field is captured too, JSON-encoded.
    CHECK(sections.resolved("SectionsTest_Profile.name") == std::string{R"("ada")"});

    // Both halves matter. Without these an implementation that returned a value
    // for every path would still pass the checks above.
    CHECK_FALSE(sections.resolved("SectionsTest_Prefs.summary").has_value());
    CHECK_FALSE(sections.resolved("SectionsTest_Profile.nosuchfield").has_value());
}

TEST_CASE("SectionSet: a failing dispatch reaches the onError callback", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};

    std::atomic<int> errors{0};
    morph::forms::SectionSet<SecModel, ProfileSection, ExplodesSection> sections{
        handler, [&errors](const std::exception_ptr&) { errors.fetch_add(1); }};

    recorder().clear();
    sections.set<&SecExplodes::label>("boom");
    REQUIRE(morph::testing::waitUntil([&] { return errors.load() == 1; }));

    // A section that succeeds does not route to onError, so the count above is
    // the failure and not merely "some callback ran".
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return sections.resolved("SectionsTest_Profile.id").has_value(); }));
    CHECK(errors.load() == 1);
}

TEST_CASE("SectionSet: an unhandled failure logs instead of escaping", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};

    // No onError: the failure has nowhere to go but the log. It must not
    // propagate out of the completion and take the executor thread down.
    morph::forms::SectionSet<SecModel, ProfileSection, ExplodesSection> sections{handler};

    recorder().clear();
    REQUIRE_NOTHROW(sections.set<&SecExplodes::label>("boom"));
    REQUIRE(morph::testing::waitUntil([&] { return recorder().snapshot().size() == 1; }));

    // The set survives its own failed section: an unrelated one still works.
    sections.set<&SecProfile::name>("ada");
    REQUIRE(morph::testing::waitUntil([&] { return sections.resolved("SectionsTest_Profile.id").has_value(); }));
}

TEST_CASE("SectionSet: destroying it with a dispatch in flight delivers nothing", "[sections]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<SecModel> handler{bridge, &cbExec};

    secSlowStarted().store(false);
    secSlowRelease().store(false);

    std::atomic<int> errors{0};
    {
        morph::forms::SectionSet<SecModel, ProfileSection, SlowSection> sections{
            handler, [&errors](const std::exception_ptr&) { errors.fetch_add(1); }};
        sections.set<&SecSlow::label>("held");
        // Wait until the model call is genuinely inside execute() before
        // leaving the scope. Without this the dispatch would usually finish
        // first and the destructor would race nothing at all.
        REQUIRE(morph::testing::waitUntil([] { return secSlowStarted().load(); }));
    }

    // The set is gone; only now does the model return (by throwing). Its error
    // continuation resolves against an object that no longer exists, and must
    // find the callback scope stopped and do nothing.
    secSlowRelease().store(true);
    CHECK(morph::testing::waitUntil([&errors] { return errors.load() != 0; }, std::chrono::milliseconds{500}) ==
          false);
    CHECK(errors.load() == 0);
}
