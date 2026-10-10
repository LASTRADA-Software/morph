// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ui = morph::ui;

static_assert(std::is_same_v<ui::Node, std::shared_ptr<ui::NodeData const>>);
static_assert(std::is_same_v<ui::Key, std::variant<std::int64_t, std::string>>);

TEST_CASE("ui::Prop: a constant is not bound, a callable is", "[ui]") {
    ui::Prop<std::string> const fixed = "hello";
    CHECK_FALSE(fixed.isBound());
    CHECK(fixed.constant() == "hello");
    CHECK(fixed.evaluate() == "hello");

    int calls = 0;
    ui::Prop<std::string> const bound = [&calls] {
        ++calls;
        return std::string{"bound"};
    };
    CHECK(bound.isBound());
    CHECK(bound.evaluate() == "bound");
    CHECK(bound.binding()() == "bound");
    CHECK(calls == 2);
}

TEST_CASE("ui::Prop<bool>: a captureless lambda is a binding, not a truthy constant", "[ui]") {
    ui::Prop<bool> const bound = [] { return false; };
    CHECK(bound.isBound());
    CHECK_FALSE(bound.evaluate());
}

TEST_CASE("ui::Prop: a default-constructed prop is the value-initialised constant", "[ui]") {
    ui::Prop<std::string> const text;
    ui::Prop<std::optional<ui::Key>> const key;
    CHECK_FALSE(text.isBound());
    CHECK(text.constant().empty());
    CHECK_FALSE(key.constant().has_value());
}

TEST_CASE("ui::Sizing and ui::LayoutHints: the three rules and their equality", "[ui]") {
    CHECK(ui::Sizing::content() == ui::Sizing{});
    CHECK(ui::Sizing::fixed(3) == ui::Sizing{.kind = ui::Sizing::Kind::Fixed, .amount = 3});
    CHECK(ui::Sizing::stretch() == ui::Sizing{.kind = ui::Sizing::Kind::Stretch, .amount = 1});
    CHECK(ui::LayoutHints{} == ui::LayoutHints{.width = ui::Sizing::content(), .height = ui::Sizing::content()});
    CHECK_FALSE(ui::LayoutHints{.width = ui::Sizing::fixed(2)} == ui::LayoutHints{});
}

TEST_CASE("ui::Common: shown, enabled, content-sized, not draggable, not a drop target", "[ui]") {
    ui::Common const common;
    CHECK(common.visible.constant());
    CHECK(common.enabled.constant());
    CHECK(common.layout == ui::LayoutHints{});
    CHECK_FALSE(common.dragKey.constant().has_value());
    CHECK_FALSE(common.accepts);
    CHECK_FALSE(common.onDrop);
}

TEST_CASE("ui builders: nodes are shared, immutable data", "[ui]") {
    ui::Node const tree = ui::column({
        .children =
            {
                ui::text({.text = "Title", .role = ui::TextRole::Heading}),
                ui::button({.label = "Go", .onClick = [] {}}),
                ui::spacer(),
            },
        .gap = 1,
    });
    REQUIRE(tree != nullptr);
    auto const* column = std::get_if<ui::Column>(&tree->kind);
    REQUIRE(column != nullptr);
    REQUIRE(column->children.size() == 3);
    CHECK(column->gap == 1);
    auto const* title = std::get_if<ui::Text>(&column->children.at(0)->kind);
    REQUIRE(title != nullptr);
    CHECK(title->text.constant() == "Title");
    CHECK(title->role.constant() == ui::TextRole::Heading);
    CHECK(std::holds_alternative<ui::Spacer>(column->children.at(2)->kind));
}

TEST_CASE("ui builders: the input kinds carry their modes and ranges", "[ui]") {
    using morph::time::DateTime;
    using morph::time::Timestamp;
    Timestamp const when{DateTime{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{4},
                                  std::chrono::hours{9}, std::chrono::minutes{30}, std::chrono::seconds{0}}};
    ui::Node const date =
        ui::dateTimeInput({.value = std::optional<Timestamp>{when}, .mode = ui::DateMode::Date, .offsetMinutes = 120});
    ui::Node const slider = ui::slider({.value = 5, .minimum = 0, .maximum = 10, .step = 5});
    ui::Node const picker = ui::filePicker({.path = "/tmp/out.csv", .mode = ui::FilePickerMode::Save});
    ui::Node const panel = ui::panel({.title = "Advanced", .collapsible = true, .collapsed = true});

    auto const& dateSpec = std::get<ui::DateTimeInput>(date->kind);
    CHECK(dateSpec.value.constant() == std::optional<Timestamp>{when});
    CHECK(dateSpec.offsetMinutes == 120);
    CHECK(std::get<ui::Slider>(slider->kind).step == 5);
    CHECK(std::get<ui::FilePicker>(picker->kind).mode == ui::FilePickerMode::Save);
    CHECK(std::get<ui::Panel>(panel->kind).collapsed.constant());
}

namespace {

std::string boundLabel() { return "from a function"; }

}  // namespace

TEST_CASE("ui::Prop: a pointer is not a bool constant, a floating-point value is not an integral one", "[ui]") {
    // Runtime checks rather than static_asserts, so that loosening the constraint fails a test instead of the build.
    CHECK_FALSE(std::is_constructible_v<ui::Prop<bool>, char const(&)[5]>);
    CHECK_FALSE(std::is_constructible_v<ui::Prop<bool>, int*>);
    CHECK_FALSE(std::is_constructible_v<ui::Prop<std::int64_t>, double>);
    CHECK_FALSE(std::is_constructible_v<ui::Prop<int>, float>);
    CHECK_FALSE(std::is_constructible_v<ui::Prop<bool>, double>);

    CHECK(std::is_constructible_v<ui::Prop<bool>, bool>);
    CHECK(std::is_constructible_v<ui::Prop<double>, int>);
    ui::Prop<std::size_t> const count = 0;
    CHECK(count.constant() == 0U);
    ui::Prop<std::string> const literal = "text";
    CHECK(literal.constant() == "text");
}

TEST_CASE("ui::Prop: an empty binding is refused at construction", "[ui]") {
    std::function<std::string()> const empty;
    std::string (*const nullBinding)() = nullptr;
    CHECK_THROWS_AS(ui::Prop<std::string>{empty}, std::invalid_argument);
    CHECK_THROWS_AS(ui::Prop<std::string>{nullBinding}, std::invalid_argument);
}

TEST_CASE("ui::Prop: every callable form is a binding", "[ui]") {
    ui::Prop<std::string> const fromPointer = &boundLabel;
    CHECK(fromPointer.isBound());
    CHECK(fromPointer.evaluate() == "from a function");

    std::function<std::string()> const function = [] { return std::string{"from a std::function"}; };
    ui::Prop<std::string> const fromFunction = function;
    CHECK(fromFunction.isBound());
    CHECK(fromFunction.evaluate() == "from a std::function");

    ui::Prop<std::int64_t> const counter = [next = std::int64_t{0}]() mutable { return ++next; };
    CHECK(counter.isBound());
    CHECK(counter.evaluate() == 1);
    CHECK(counter.evaluate() == 2);

    ui::Prop<std::optional<ui::Key>> const key = [] { return std::optional<ui::Key>{std::int64_t{7}}; };
    ui::Prop<std::optional<ui::Key>> const keyFromKey = [] { return ui::Key{std::string{"seven"}}; };
    CHECK(key.isBound());
    CHECK(key.evaluate() == std::optional<ui::Key>{std::int64_t{7}});
    CHECK(keyFromKey.isBound());
    CHECK(keyFromKey.evaluate() == std::optional<ui::Key>{std::string{"seven"}});
}

TEST_CASE("ui::Prop: copying a prop copies it, binding or constant", "[ui]") {
    // The copies below are the subject of the test, so the copy-initialisation check is suppressed on them.
    int calls = 0;
    ui::Prop<std::string> const bound = [&calls] {
        ++calls;
        return std::string{"bound"};
    };
    ui::Prop<std::string> const boundCopy = bound;  // NOLINT(performance-unnecessary-copy-initialization)
    CHECK(boundCopy.isBound());
    CHECK(boundCopy.evaluate() == "bound");
    CHECK(calls == 1);

    ui::Prop<bool> const fixed = false;
    ui::Prop<bool> const fixedCopy = fixed;  // NOLINT(performance-unnecessary-copy-initialization)
    CHECK_FALSE(fixedCopy.isBound());
    CHECK_FALSE(fixedCopy.constant());
}
