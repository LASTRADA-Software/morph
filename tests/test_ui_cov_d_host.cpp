// SPDX-License-Identifier: Apache-2.0

// What the document host says of a screen handle that is not one of its own: it holds no draft, names no screen,
// and a mount over it is refused rather than guessed at.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/ui/host.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::ScreenHandle;
using morph::ui::ScreenHostRegistry;
using morph::ui::Value;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

constexpr std::string_view kNote = R"({"vocab":"ui/1","id":"note","title":"Note",
  "view":{"kind":"text","testId":"body","text":"x"}})";

// A screen some other host made: it has a title and nothing of a document's.
class Foreign final : public ScreenHandle {
public:
    explicit Foreign(morph::reactive::Runtime& runtime)
        : _scope{runtime},
          _title{_scope.computed([] { return std::string{"Foreign"}; })},
          _dirty{_scope.computed([] { return false; })},
          _identity{_scope.computed([] { return Value{}; })} {}

    [[nodiscard]] morph::reactive::Computed<std::string> const& title() const override { return _title; }
    [[nodiscard]] morph::reactive::Computed<bool> const& dirty() const override { return _dirty; }
    [[nodiscard]] morph::reactive::Computed<Value> const& identity() const override { return _identity; }
    void requestClose(std::function<void(bool closed)> done) override { done(true); }
    void forceClose() override {}

private:
    morph::reactive::Scope _scope;
    morph::reactive::Computed<std::string>& _title;
    morph::reactive::Computed<bool>& _dirty;
    morph::reactive::Computed<Value>& _identity;
};

class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, DocumentScreenHostOptions options)
        : DocumentScreenHost{harness.session, harness.dispatcher, std::move(options)} {}

    // A host over one document of its own.
    Host(Harness& harness, std::shared_ptr<Document const> document, DocumentScreenHostOptions options = {})
        : DocumentScreenHost{harness.session, harness.dispatcher,
                             forDocument(harness, std::move(document), std::move(options))} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, morph::ui::ParamMap const& params) override {
        static_cast<void>(screenId);
        static_cast<void>(params);
    }

private:
    static DocumentScreenHostOptions forDocument(Harness& harness, std::shared_ptr<Document const> document,
                                                 DocumentScreenHostOptions options) {
        options.documents = [held = std::move(document)](std::string_view id) -> std::shared_ptr<Document const> {
            return id == held->id ? held : nullptr;
        };
        options.backend = &harness.backend;
        return options;
    }
};

}  // namespace

TEST_CASE("covd host: a handle some other host made holds no draft and names no screen of this one",
          "[ui][host][covd]") {
    Harness h;
    DocumentScreenHostOptions options;
    options.documents = [](std::string_view) -> std::shared_ptr<Document const> { return nullptr; };
    options.backend = &h.backend;
    Host host{h, std::move(options)};
    Foreign foreign{h.runtime};
    auto const note = loadOrFail(kNote);

    CHECK_FALSE(DocumentScreenHost::drafted(foreign));
    CHECK_FALSE(DocumentScreenHost::draftedNow(foreign));
    CHECK_FALSE(DocumentScreenHost::screenIdOf(foreign).has_value());
    CHECK_FALSE(DocumentScreenHost::paramsOf(foreign).has_value());
    CHECK_FALSE(DocumentScreenHost::chatContext(foreign).has_value());
    DocumentScreenHost::endDraft(foreign);
    DocumentScreenHost::resumeDraft(foreign);

    auto const remounted = host.remount(foreign, note);
    REQUIRE_FALSE(remounted.has_value());
    CHECK(remounted.error().reason == "not a document screen of this host");
    CHECK(remounted.error().screenId == "note");
    auto const blank = host.remount(foreign, nullptr);
    REQUIRE_FALSE(blank.has_value());
    CHECK(blank.error().screenId.empty());

    auto const drafted = host.mountDraft(foreign, note);
    REQUIRE_FALSE(drafted.has_value());
    CHECK(drafted.error().reason == "not an open document screen of this host");
    CHECK(drafted.error().screenId == "note");
    auto const noDraft = host.mountDraft(foreign, nullptr);
    REQUIRE_FALSE(noDraft.has_value());
    CHECK(noDraft.error().screenId.empty());

    auto const discarded = host.discardDraft(foreign);
    REQUIRE_FALSE(discarded.has_value());
    CHECK(discarded.error().reason == "no draft is mounted");
    CHECK(discarded.error().screenId.empty());
}

TEST_CASE("covd host: a registry names the screens it can make and no others", "[ui][host][covd]") {
    ScreenHostRegistry registry;
    CHECK(registry.find("scanner") == nullptr);
    registry.add(
        "scanner",
        [](morph::ui::ParamMap const&,
           morph::ui::ContainerWidget&) -> std::expected<std::unique_ptr<ScreenHandle>, morph::ui::MountError> {
            return std::unexpected(morph::ui::MountError{.screenId = "scanner", .reason = "no device"});
        });
    CHECK(registry.find("scanner") != nullptr);
    CHECK(registry.find("printer") == nullptr);
}

namespace {

// A sample whose keyed model and query read an optional param.
constexpr std::string_view kKeyed = R"({
  "vocab": "ui/1", "id": "keyed", "params": { "sampleId": "int?" },
  "models": { "sample": { "type": "Lims_SampleModel", "instance": { "ref": "sampleId" } },
              "clientModel": { "type": "Lims_ClientModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults",
                            "body": { "obj": { "sampleId": { "ref": "sampleId" } } } },
               "clients": { "model": "clientModel", "action": "Lims_ClientList",
                            "body": { "obj": { "sampleId": { "ref": "sampleId" } } } } },
  "prefetch": [ "results", "clients" ],
  "view": { "kind": "text", "testId": "keyed", "text": "keyed" } })";

}  // namespace

TEST_CASE("covd host: a prefetch whose key or body reads a param the route lacks is not sent", "[ui][host][covd]") {
    Harness h;
    Host host{h, loadOrFail(kKeyed, morph::ui::testing::corpusCatalog())};
    host.prefetch("keyed", {});
    h.settle();
    CHECK(h.dispatcher.count("Lims_ListResults") == 0U);  // no key to bind the instance by
    CHECK(h.dispatcher.count("Lims_ClientList") == 0U);   // a body that reads the missing param
    host.prefetch("keyed", morph::ui::ParamMap{{"sampleId", Value{std::int64_t{3}}}});
    h.settle();
    CHECK(h.dispatcher.count("Lims_ListResults") == 1U);
    CHECK(h.dispatcher.count("Lims_ClientList") == 1U);
}

TEST_CASE("covd host: a screen the host closed cannot be mounted over again or drafted", "[ui][host][covd]") {
    Harness h;
    Host host{h, loadOrFail(kNote)};
    auto mounted = host.mount("note", {});
    REQUIRE(mounted.has_value());
    ScreenHandle& handle = **mounted;
    auto const note = loadOrFail(kNote);
    CHECK(DocumentScreenHost::screenIdOf(handle) == std::optional<std::string>{"note"});
    CHECK(DocumentScreenHost::paramsOf(handle).has_value());
    CHECK_FALSE(DocumentScreenHost::drafted(handle));
    CHECK_FALSE(DocumentScreenHost::draftedNow(handle));
    // A screen that has no draft has none to discard, whatever its screen is.
    auto const none = host.discardDraft(handle);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().reason == "no draft is mounted");
    CHECK(none.error().screenId == "note");

    handle.forceClose();
    h.settle();
    auto const remounted = host.remount(handle, note);
    REQUIRE_FALSE(remounted.has_value());
    CHECK(remounted.error().reason == "the screen is closed");
    auto const drafted = host.mountDraft(handle, note);
    REQUIRE_FALSE(drafted.has_value());
    CHECK(drafted.error().reason == "not an open document screen of this host");
    CHECK_FALSE(DocumentScreenHost::screenIdOf(handle).has_value());
    DocumentScreenHost::resumeDraft(handle);
    DocumentScreenHost::endDraft(handle);
}

TEST_CASE("covd host: a draft keeps its screen's id, and a draft of a screen can be dropped", "[ui][host][covd]") {
    Harness h;
    Host host{h, loadOrFail(kNote)};
    auto mounted = host.mount("note", {});
    REQUIRE(mounted.has_value());
    ScreenHandle& handle = **mounted;
    auto const other = loadOrFail(R"({"vocab":"ui/1","id":"other","view":{"kind":"text","text":"x"}})");
    auto const refused = host.mountDraft(handle, other);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().pointer == "/id");
    CHECK(refused.error().reason.contains("a draft keeps the screen's id, `note`"));
    auto const drafted = host.mountDraft(handle, loadOrFail(kNote));
    REQUIRE(drafted.has_value());
    CHECK(DocumentScreenHost::drafted(handle));
    CHECK(DocumentScreenHost::draftedNow(handle));
    auto const discarded = host.discardDraft(handle);
    REQUIRE(discarded.has_value());
    CHECK_FALSE(DocumentScreenHost::draftedNow(handle));
}
