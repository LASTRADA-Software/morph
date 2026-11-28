// SPDX-License-Identifier: Apache-2.0

// What a workspace reads of its active tab's head, one accessor at a time, over a screen with no head and over heads
// that give different optional parts, with no tab open, and with several tabs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <morph/reactive/scope.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/workspace.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::ContainerWidget;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::HeadAction;
using morph::ui::HeadBadge;
using morph::ui::HeadMenuEntry;
using morph::ui::HeadTool;
using morph::ui::HeadToolKind;
using morph::ui::LeadPlacement;
using morph::ui::MountError;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::ScreenHead;
using morph::ui::ScreenHostRegistry;
using morph::ui::TextSpan;
using morph::ui::Value;
using morph::ui::Workspace;
using morph::ui::WorkspaceOptions;
using morph::ui::WorkspaceScreen;
using morph::ui::testing::Harness;

class Host final : public DocumentScreenHost {
public:
    explicit Host(Harness& harness) : DocumentScreenHost{harness.session, harness.dispatcher, options(harness)} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        static_cast<void>(screenId);
        static_cast<void>(params);
    }

private:
    static DocumentScreenHostOptions options(Harness& harness) {
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view) -> std::shared_ptr<morph::ui::Document const> { return nullptr; };
        made.backend = &harness.backend;
        return made;
    }
};

// A native screen with no head.
class Plain : public ScreenHandle {
public:
    Plain(morph::reactive::Runtime& runtime, std::unique_ptr<morph::ui::Widget> content)
        : _content{std::move(content)},
          _scope{runtime},
          _title{_scope.computed([] { return std::string{"Native"}; })},
          _dirty{_scope.computed([] { return false; })},
          _identity{_scope.computed([] { return Value{}; })} {}

    [[nodiscard]] morph::reactive::Computed<std::string> const& title() const override { return _title; }
    [[nodiscard]] morph::reactive::Computed<bool> const& dirty() const override { return _dirty; }
    [[nodiscard]] morph::reactive::Computed<Value> const& identity() const override { return _identity; }
    void requestClose(std::function<void(bool closed)> done) override { done(true); }
    void forceClose() override {}

protected:
    morph::reactive::Scope& scope() { return _scope; }

private:
    std::unique_ptr<morph::ui::Widget> _content;
    morph::reactive::Scope _scope;
    morph::reactive::Computed<std::string>& _title;
    morph::reactive::Computed<bool>& _dirty;
    morph::reactive::Computed<Value>& _identity;
};

// Which of a head's optional parts the next `Bare` screen gives.
struct Variant {
    bool eyebrow = false;
    bool spans = false;
    bool placement = false;
    bool joined = false;
    bool edge = false;
    bool menuLabel = false;
    bool tools = false;
    bool withMenu = true;
};

Variant& variant() {
    static Variant held;
    return held;
}

// A native screen whose head gives a subtitle, a badge, an action and a menu, and the optional parts a `Variant` asks.
class Bare final : public Plain, public ScreenHead {
public:
    Bare(morph::reactive::Runtime& runtime, std::unique_ptr<morph::ui::Widget> content)
        : Plain{runtime, std::move(content)},
          _subtitle{scope().computed([] { return std::string{"sub"}; })},
          _status{scope().computed([] { return std::vector<HeadBadge>{HeadBadge{.text = "Draft"}}; })},
          _actions{scope().computed([] { return std::vector<HeadAction>{HeadAction{.id = "go", .label = "Go"}}; })},
          _menu{
              scope().computed([] { return std::vector<HeadMenuEntry>{HeadMenuEntry{.id = "m", .label = "Entry"}}; })},
          _eyebrow{scope().computed([] { return std::string{"Worksheet"}; })},
          _spans{scope().computed(
              [] { return std::vector<TextSpan>{TextSpan{.text = "Run "}, TextSpan{.text = "7", .mono = true}}; })},
          _placement{scope().computed([] { return LeadPlacement::End; })},
          _flag{scope().computed([] { return true; })},
          _label{scope().computed([] { return std::string{"Actions"}; })},
          _tools{scope().computed([] {
              return std::vector<HeadTool>{HeadTool{.id = "q", .kind = HeadToolKind::Search, .label = "Search"}};
          })} {}

    [[nodiscard]] ScreenHead* head() noexcept override { return this; }
    [[nodiscard]] morph::reactive::Computed<std::string> const& subtitle() const override { return _subtitle; }
    [[nodiscard]] morph::reactive::Computed<std::vector<HeadBadge>> const& status() const override { return _status; }
    [[nodiscard]] morph::reactive::Computed<std::vector<HeadAction>> const& actions() const override {
        return _actions;
    }
    bool run(std::string_view actionId) override {
        ran.emplace_back(actionId);
        return true;
    }
    [[nodiscard]] morph::reactive::Computed<std::vector<HeadMenuEntry>> const* menu() const override {
        return variant().withMenu ? &_menu : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<std::string> const* eyebrow() const override {
        return variant().eyebrow ? &_eyebrow : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<std::vector<TextSpan>> const* eyebrowSpans() const override {
        return variant().spans ? &_spans : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<std::vector<TextSpan>> const* titleSpans() const override {
        return variant().spans ? &_spans : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<std::vector<TextSpan>> const* subtitleSpans() const override {
        return variant().spans ? &_spans : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<LeadPlacement> const* leadPlacement() const override {
        return variant().placement ? &_placement : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<bool> const* headJoined() const override {
        return variant().joined ? &_flag : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<bool> const* pageEdge() const override {
        return variant().edge ? &_flag : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<std::string> const* menuLabel() const override {
        return variant().menuLabel ? &_label : nullptr;
    }
    [[nodiscard]] morph::reactive::Computed<std::vector<HeadTool>> const* tools() const override {
        return variant().tools ? &_tools : nullptr;
    }

    std::vector<std::string> ran;

private:
    morph::reactive::Computed<std::string>& _subtitle;
    morph::reactive::Computed<std::vector<HeadBadge>>& _status;
    morph::reactive::Computed<std::vector<HeadAction>>& _actions;
    morph::reactive::Computed<std::vector<HeadMenuEntry>>& _menu;
    morph::reactive::Computed<std::string>& _eyebrow;
    morph::reactive::Computed<std::vector<TextSpan>>& _spans;
    morph::reactive::Computed<LeadPlacement>& _placement;
    morph::reactive::Computed<bool>& _flag;
    morph::reactive::Computed<std::string>& _label;
    morph::reactive::Computed<std::vector<HeadTool>>& _tools;
};

template <typename Screen>
std::unique_ptr<Workspace> workspaceOf(Harness& harness, Host& host, ScreenHostRegistry& hosts) {
    hosts.add("Native",
              [&harness](ParamMap const&,
                         ContainerWidget& page) -> std::expected<std::unique_ptr<ScreenHandle>, MountError> {
                  auto text = harness.backend.createText(&page);
                  text->setTestId("native");
                  return std::make_unique<Screen>(harness.runtime, std::move(text));
              });
    return std::make_unique<Workspace>(
        harness.runtime, harness.backend, host,
        WorkspaceOptions{.screens = [](std::string_view) { return WorkspaceScreen{.host = "Native"}; },
                         .hosts = &hosts});
}

struct Expected {
    Variant given;
    bool plain = false;
    morph::ui::LeadPlacement placement = LeadPlacement::Below;
    bool oneLine = true;
    morph::ui::HeadTrailEnd trailEnd = morph::ui::HeadTrailEnd::None;
    bool joined = false;
    bool inset = true;
    std::size_t titleSpans = 0;
};

}  // namespace

TEST_CASE("covj workspace: the active tab's head reads as its parts say, and as nothing for a screen with none",
          "[covj][ui][workspace]") {
    using morph::ui::HeadTrailEnd;
    auto const expected = GENERATE(
        Expected{.plain = true}, Expected{.oneLine = false, .trailEnd = HeadTrailEnd::Unframed},
        Expected{.given = {.withMenu = false}, .oneLine = false, .trailEnd = HeadTrailEnd::Framed},
        Expected{.given = {.placement = true}, .placement = LeadPlacement::End, .trailEnd = HeadTrailEnd::Unframed},
        Expected{.given = {.eyebrow = true, .spans = true},
                 .oneLine = false,
                 .trailEnd = HeadTrailEnd::Unframed,
                 .titleSpans = 2},
        Expected{.given = {.joined = true, .edge = true},
                 .oneLine = false,
                 .trailEnd = HeadTrailEnd::Unframed,
                 .joined = true,
                 .inset = false},
        Expected{.given = {.joined = true}, .oneLine = false, .trailEnd = HeadTrailEnd::Unframed});
    variant() = expected.given;
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto const shell = expected.plain ? workspaceOf<Plain>(h, host, hosts) : workspaceOf<Bare>(h, host, hosts);
    REQUIRE(shell->open("native", {}).has_value());
    h.settle();
    CHECK(shell->activeLeadPlacement() == expected.placement);
    CHECK(shell->activeHeadOneLine() == expected.oneLine);
    CHECK(shell->activeHeadTrailEnd() == expected.trailEnd);
    CHECK(shell->activeHeadJoined() == expected.joined);
    CHECK(shell->activePageInset() == expected.inset);
    CHECK(shell->activeTitleSpans().size() == expected.titleSpans);
    CHECK(shell->activeTitleRole() == morph::ui::TextRole::Heading);
    CHECK(shell->activeHeadSurface().empty());
    CHECK(shell->activePageSurface().empty());
    CHECK(shell->activeHeadShown() == !expected.plain);
    variant() = Variant{};
}

TEST_CASE("covj workspace: a head that joins the page is not joined once a second tab is open",
          "[covj][ui][workspace]") {
    variant() = Variant{.joined = true, .edge = true};
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto const shell = workspaceOf<Bare>(h, host, hosts);
    REQUIRE(shell->open("native", {}).has_value());
    h.settle();
    REQUIRE(shell->activeHeadJoined());
    REQUIRE(shell->openTab("native", {}).has_value());
    h.settle();
    CHECK(shell->activePageInset());
    CHECK_FALSE(shell->activeHeadJoined());
    variant() = Variant{};
}

TEST_CASE("covj workspace: with no tab open the head reads as an empty page", "[covj][ui][workspace]") {
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto const shell = workspaceOf<Bare>(h, host, hosts);
    CHECK(shell->activeLeadPlacement() == LeadPlacement::Below);
    CHECK(shell->activeHeadOneLine());
    CHECK(shell->activeHeadTrailEnd() == morph::ui::HeadTrailEnd::None);
    CHECK_FALSE(shell->activeHeadJoined());
    CHECK(shell->activeTitleRole() == morph::ui::TextRole::Heading);
    CHECK(shell->activeHeadSurface().empty());
    CHECK(shell->activePageSurface().empty());
    CHECK(shell->activeTitleSpans().empty());
    CHECK(shell->activePageInset());
    CHECK_FALSE(shell->activeHeadShown());
}

TEST_CASE("covj workspace: the tab bar's chords move between tabs and close the active one, and need a tab to close",
          "[covj][ui][workspace]") {
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto const shell = workspaceOf<Plain>(h, host, hosts);
    REQUIRE(h.backend.all("Tabs").size() == 1);
    int const bar = h.backend.all("Tabs").front();
    // With no tab there is nothing for Ctrl+W to close.
    CHECK(h.backend.press(bar, "Ctrl+W"));
    CHECK(shell->tabs().empty());

    REQUIRE(shell->open("native", {}).has_value());
    REQUIRE(shell->openTab("native", {}).has_value());
    h.settle();
    REQUIRE(shell->tabs().size() == 2);
    REQUIRE(shell->active() == std::optional<std::size_t>{1});
    CHECK(h.backend.press(bar, "Ctrl+Shift+Tab"));
    h.settle();
    CHECK(shell->active() == std::optional<std::size_t>{0});
    CHECK(h.backend.press(bar, "Ctrl+Tab"));
    h.settle();
    CHECK(shell->active() == std::optional<std::size_t>{1});
    CHECK(h.backend.press(bar, "Ctrl+W"));
    h.settle();
    CHECK(shell->tabs().size() == 1);
}

TEST_CASE("covj workspace: activating a place with no tab changes nothing, and a closed screen takes its tab along",
          "[covj][ui][workspace]") {
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto const shell = workspaceOf<Plain>(h, host, hosts);
    REQUIRE(shell->open("native", {}).has_value());
    REQUIRE(shell->openTab("native", {}).has_value());
    h.settle();
    shell->activate(7);
    CHECK(shell->active() == std::optional<std::size_t>{1});

    Plain stray{h.runtime, h.backend.createText(nullptr)};
    shell->screenClosed(stray);
    CHECK(shell->tabs().size() == 2);

    shell->screenClosed(*shell->handleOf(0));
    h.settle();
    CHECK(shell->tabs().size() == 1);
}
