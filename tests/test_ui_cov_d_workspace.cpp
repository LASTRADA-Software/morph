// SPDX-License-Identifier: Apache-2.0

// The workspace's head over screens that give it less than a document screen does: a screen with no head, and one
// whose head gives only the parts it must.

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
    bool run(std::string_view /*actionId*/) override { return true; }
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

}  // namespace

TEST_CASE("covd workspace: a native screen with no head shows no head row", "[ui][workspace][covd]") {
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto shell = workspaceOf<Plain>(h, host, hosts);
    REQUIRE(shell->open("native", {}).has_value());
    h.settle();
    CHECK(h.backend.find("Text", "testId", "native").has_value());
    CHECK_FALSE(h.backend.find("Text", "testId", "shell_head_subtitle").has_value());
    CHECK(shell->tabs().at(0).caption == "Native");
}

TEST_CASE("covd workspace: a head with only a subtitle, a badge, an action and a menu draws them",
          "[ui][workspace][covd]") {
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto shell = workspaceOf<Bare>(h, host, hosts);
    REQUIRE(shell->open("native", {}).has_value());
    h.settle();
    CHECK(h.backend.find("Badge", "text", "Draft").has_value());
    CHECK(h.backend.find("Button", "label", "Go").has_value());
}

TEST_CASE("covd workspace: a head's optional parts each change what the head row draws, and none is needed",
          "[ui][workspace][covd]") {
    auto const given =
        GENERATE(Variant{}, Variant{.menuLabel = true}, Variant{.eyebrow = true, .spans = true, .placement = true},
                 Variant{.joined = true, .edge = true, .tools = true}, Variant{.tools = true, .withMenu = false},
                 Variant{.withMenu = false});
    variant() = given;
    Harness h;
    Host host{h};
    ScreenHostRegistry hosts;
    auto shell = workspaceOf<Bare>(h, host, hosts);
    REQUIRE(shell->open("native", {}).has_value());
    h.settle();
    CHECK(h.backend.find("Badge", "text", "Draft").has_value());
    CHECK(h.backend.find("Button", "label", "Go").has_value());
    variant() = Variant{};
}
