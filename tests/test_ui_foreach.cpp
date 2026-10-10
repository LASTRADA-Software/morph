// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"
#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::testing::EchoingBackend;
using morph::testing::failWhenSpent;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

struct Lap {
    std::int64_t id = 0;
    std::string label;
    bool operator==(Lap const&) const = default;
};

struct Tag {
    std::string name;
    int uses = 0;
    bool operator==(Tag const&) const = default;
};

// A row type that cannot be compared: every assignment of its row signal notifies.
struct Plain {
    std::int64_t id = 0;
    std::string label;
};

ui::Key keyOf(Lap const& lap) { return ui::Key{lap.id}; }

ui::Node lapView(Signal<Lap> const& lap) {
    return ui::text({.text = [&lap] { return lap.get().label; }});
}

ui::Node lapList(Signal<std::vector<Lap>> const& rows) { return ui::forEach<Lap>(rows, keyOf, lapView); }

std::vector<Lap> abc() { return {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}, {.id = 3, .label = "c"}}; }

void removeLap(Signal<std::vector<Lap>>& rows, std::int64_t lapId) {
    rows.mutate(
        [lapId](std::vector<Lap>& laps) { std::erase_if(laps, [lapId](Lap const& lap) { return lap.id == lapId; }); });
}

// A widget that does nothing; a FakeList's children.
class FakeItem final : public ui::Widget {
public:
    void setVisible(bool /*visible*/) override {}
    void setEnabled(bool /*enabled*/) override {}
    void setLayout(ui::LayoutHints const& /*hints*/) override {}
    void setDragKey(std::optional<ui::Key> const& /*key*/) override {}
    void setDropHandler(std::function<bool(ui::Key const&)> /*accepts*/,
                        std::function<void(ui::Key)> /*onDrop*/) override {}
};

// A container that applies `moveChild` as the contract defines it and counts the moves; the move numbered
// `failingMove` (from 0), if any, throws instead.
class FakeList final : public ui::ContainerWidget {
public:
    explicit FakeList(std::vector<ui::Widget*> children, std::optional<std::size_t> failingMove = std::nullopt)
        : _children{std::move(children)}, _failingMove{failingMove} {}

    void setVisible(bool /*visible*/) override {}
    void setEnabled(bool /*enabled*/) override {}
    void setLayout(ui::LayoutHints const& /*hints*/) override {}
    void setDragKey(std::optional<ui::Key> const& /*key*/) override {}
    void setDropHandler(std::function<bool(ui::Key const&)> /*accepts*/,
                        std::function<void(ui::Key)> /*onDrop*/) override {}

    void moveChild(ui::Widget& child, std::size_t index) override {
        if (_failingMove == _moves) {
            throw std::runtime_error{"the backend could not move the widget"};
        }
        ++_moves;
        auto const found = std::ranges::find(_children, &child);
        REQUIRE(found != _children.end());
        _children.erase(found);
        _children.insert(_children.begin() + static_cast<std::ptrdiff_t>(std::min(index, _children.size())), &child);
    }

    [[nodiscard]] std::vector<ui::Widget*> const& children() const noexcept { return _children; }
    [[nodiscard]] std::size_t moves() const noexcept { return _moves; }

private:
    std::vector<ui::Widget*> _children;
    std::optional<std::size_t> _failingMove;
    std::size_t _moves = 0;
};

// The length of a longest increasing run in @p sequence, by the quadratic definition.
std::size_t longestIncreasing(std::vector<std::size_t> const& sequence) {
    std::vector<std::size_t> ending(sequence.size(), 1);
    std::size_t longest = 0;
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (sequence.at(j) < sequence.at(i)) {
                ending.at(i) = std::max(ending.at(i), ending.at(j) + 1);
            }
        }
        longest = std::max(longest, ending.at(i));
    }
    return longest;
}

}  // namespace

TEST_CASE("ui::forEach: a session snapshots the rows and makes a slot per row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    ui::Node const node = ui::forEach<Lap>(rows, keyOf, lapView, ui::Axis::Horizontal, 2);
    auto const& each = std::get<ui::ForEach>(node->kind);
    CHECK(each.axis == ui::Axis::Horizontal);
    CHECK(each.gap == 2);
    auto session = each.model->open(runtime);
    CHECK(session->pull() == std::vector<ui::Key>{ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{2}}});
    auto slot = session->makeRow(1);
    REQUIRE(slot->view() != nullptr);
    CHECK(std::get<ui::Text>(slot->view()->kind).text.evaluate() == "b");
    rows.set({{.id = 2, .label = "b2"}});
    CHECK(session->pull() == std::vector<ui::Key>{ui::Key{std::int64_t{2}}});
    slot->assign(0);
    CHECK(std::get<ui::Text>(slot->view()->kind).text.evaluate() == "b2");
}

TEST_CASE("ui::forEach: an empty function is refused where it is written", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    Signal<std::vector<Lap>> const rows{runtime};
    CHECK_THROWS_AS(ui::forEach<Lap>(std::function<std::vector<Lap>()>{}, keyOf, lapView), std::invalid_argument);
    CHECK_THROWS_AS(ui::forEach<Lap>(rows, std::function<ui::Key(Lap const&)>{}, lapView), std::invalid_argument);
    CHECK_THROWS_AS(ui::forEach<Lap>(rows, keyOf, std::function<ui::Node(Signal<Lap> const&)>{}),
                    std::invalid_argument);
}

TEST_CASE("ui::forEach: mounts one row per key, in order", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> const rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  Text#2 role=Normal text=a\n"
          "  Text#3 role=Normal text=b\n"
          "  Text#4 role=Normal text=c\n");
}

TEST_CASE("ui::forEach: an update of a kept key keeps its widget", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b2"}, {.id = 3, .label = "c"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"set Text#3 text=b2"});
}

TEST_CASE("ui::forEach: an equal kept row notifies nobody; a row type without == notifies every kept row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;

    SECTION("a row type with ==") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        int evaluations = 0;
        ui::Mounted const view{runtime, backend, ui::forEach<Lap>(rows, keyOf, [&evaluations](Signal<Lap> const& lap) {
                                   return ui::text({.text = [&evaluations, &lap] {
                                       ++evaluations;
                                       return lap.get().label;
                                   }});
                               })};
        CHECK(evaluations == 3);
        rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b2"}, {.id = 3, .label = "c"}});
        owner.runAll();
        CHECK(evaluations == 4);
    }
    SECTION("a row type without ==") {
        Signal<std::vector<Plain>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
        int evaluations = 0;
        ui::Mounted const view{runtime, backend,
                               ui::forEach<Plain>(
                                   rows, [](Plain const& plain) { return ui::Key{plain.id}; },
                                   [&evaluations](Signal<Plain> const& plain) {
                                       return ui::text({.text = [&evaluations, &plain] {
                                           ++evaluations;
                                           return plain.get().label;
                                       }});
                                   })};
        CHECK(evaluations == 2);
        backend.clearLog();
        rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
        owner.runAll();
        CHECK(evaluations == 4);
        CHECK(backend.log().empty());  // the text's own Computed is equality-gated
        rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b2"}});
        owner.runAll();
        CHECK(evaluations == 6);
        CHECK(backend.log() == Lines{"set Text#3 text=b2"});
    }
}

TEST_CASE("ui::forEach: an append only creates and a removal only destroys", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};

    backend.clearLog();
    rows.mutate([](std::vector<Lap>& laps) { laps.push_back({.id = 4, .label = "d"}); });
    owner.runAll();
    CHECK(backend.log() == Lines{"create Text#5 in Column#1", "set Text#5 text=d", "set Text#5 role=Normal"});

    backend.clearLog();
    rows.set({{.id = 1, .label = "a"}, {.id = 3, .label = "c"}, {.id = 4, .label = "d"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3"});
}

TEST_CASE("ui::forEach: rows that go in one snapshot are unmounted last first", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 2, .label = "b"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#4", "destroy Text#2"});
}

TEST_CASE("ui::forEach: an insert at the front moves only the new row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
    owner.runAll();
    CHECK(backend.log() ==
          Lines{"create Text#4 in Column#1", "set Text#4 text=c", "set Text#4 role=Normal", "move Text#4 to 0"});
}

TEST_CASE("ui::forEach: a key that changes while its index stays replaces that row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 1, .label = "a"}, {.id = 5, .label = "b"}, {.id = 3, .label = "c"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3", "create Text#5 in Column#1", "set Text#5 text=b",
                                 "set Text#5 role=Normal", "move Text#4 to 2"});
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  Text#2 role=Normal text=a\n"
          "  Text#5 role=Normal text=b\n"
          "  Text#4 role=Normal text=c\n");
}

TEST_CASE("ui::forEach: a reorder moves only the rows outside the longest run in order", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;

    SECTION("the last row to the front is one move") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        ui::Mounted const view{runtime, backend, lapList(rows)};
        backend.clearLog();
        rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
        owner.runAll();
        CHECK(backend.log() == Lines{"move Text#4 to 0"});
    }
    SECTION("the first row to the end is one move") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        ui::Mounted const view{runtime, backend, lapList(rows)};
        backend.clearLog();
        rows.set({{.id = 2, .label = "b"}, {.id = 3, .label = "c"}, {.id = 1, .label = "a"}});
        owner.runAll();
        CHECK(backend.log() == Lines{"move Text#2 to 2"});
    }
    SECTION("a reversal of three is two moves") {
        Signal<std::vector<Lap>> rows{runtime, abc()};
        ui::Mounted const view{runtime, backend, lapList(rows)};
        backend.clearLog();
        rows.set({{.id = 3, .label = "c"}, {.id = 2, .label = "b"}, {.id = 1, .label = "a"}});
        owner.runAll();
        CHECK(backend.log() == Lines{"move Text#2 to 2", "move Text#3 to 1"});
        CHECK(backend.dump() ==
              "Column#1 gap=0\n"
              "  Text#4 role=Normal text=c\n"
              "  Text#3 role=Normal text=b\n"
              "  Text#2 role=Normal text=a\n");
    }
}

TEST_CASE("ui::detail::reorderChildren: every permutation of five ends in order with the fewest moves", "[ui]") {
    std::array<FakeItem, 5> items;
    std::vector<ui::Widget*> start;
    start.reserve(items.size());
    for (FakeItem& item : items) {
        start.push_back(&item);
    }
    std::vector<std::size_t> permutation{0, 1, 2, 3, 4};
    int permutations = 0;
    for (bool more = true; more; more = std::ranges::next_permutation(permutation).found) {
        std::vector<ui::Widget*> target;
        target.reserve(permutation.size());
        for (std::size_t const index : permutation) {
            target.push_back(start.at(index));
        }
        // Each child's target position, in current order.
        std::vector<std::size_t> sequence(start.size());
        for (std::size_t position = 0; position < permutation.size(); ++position) {
            sequence.at(permutation.at(position)) = position;
        }
        FakeList list{start};
        std::vector<ui::Widget*> current = start;
        ui::detail::reorderChildren(list, current, target);
        CHECK(list.children() == target);
        CHECK(current == target);
        CHECK(list.moves() == start.size() - longestIncreasing(sequence));
        ++permutations;
    }
    CHECK(permutations == 120);
}

TEST_CASE("ui::detail::reorderChildren: a move that throws leaves the order as far as it got", "[ui]") {
    std::array<FakeItem, 4> items;
    std::vector<ui::Widget*> const start{&items.at(0), &items.at(1), &items.at(2), &items.at(3)};
    std::vector<ui::Widget*> const reversed{&items.at(3), &items.at(2), &items.at(1), &items.at(0)};
    FakeList list{start, 1};
    std::vector<ui::Widget*> current = start;
    CHECK_THROWS_AS(ui::detail::reorderChildren(list, current, reversed), std::runtime_error);
    CHECK(list.moves() == 1);
    CHECK(current == list.children());
    CHECK(current != start);
}

TEST_CASE("ui::forEach: string keys are keys too", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Tag>> tags{runtime, {{.name = "red"}, {.name = "blue"}}};
    ui::Mounted const view{
        runtime, backend,
        ui::forEach<Tag>(
            tags, [](Tag const& tag) { return ui::Key{tag.name}; },
            [](Signal<Tag> const& tag) {
                return ui::text({.text = [&tag] { return tag.get().name + " " + std::to_string(tag.get().uses); }});
            })};
    backend.clearLog();
    tags.set({{.name = "blue", .uses = 1}, {.name = "red"}});
    owner.runAll();
    // The move happens in the ForEach's own run; the row's binding runs after it in the same flush.
    CHECK(backend.log() == Lines{"move Text#2 to 1", "set Text#3 text=blue 1"});
}

TEST_CASE("ui::forEach: a duplicate key is reported and refused; the first one is kept", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> const rows{runtime,
                                        {{.id = 1, .label = "a"}, {.id = 1, .label = "dup"}, {.id = 2, .label = "b"}}};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    CHECK(probe.count(ui::detail::site::kDuplicateKey) == 1);
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#2 role=Normal text=a\n  Text#3 role=Normal text=b\n");
}

TEST_CASE("ui::forEach: a duplicate in a later snapshot is refused; the first one decides value and place", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    backend.clearLog();
    rows.set({{.id = 2, .label = "x"}, {.id = 1, .label = "a"}, {.id = 2, .label = "z"}});
    owner.runAll();
    CHECK(probe.count(ui::detail::site::kDuplicateKey) == 1);
    CHECK(backend.log() == Lines{"move Text#2 to 1", "set Text#3 text=x"});
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#3 role=Normal text=x\n  Text#2 role=Normal text=a\n");
}

TEST_CASE("ui::forEach: emptied then refilled", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, lapList(rows)};
    rows.set({});
    owner.runAll();
    CHECK(backend.dump() == "Column#1 gap=0\n");
    CHECK(backend.all("Text").empty());
    rows.set({{.id = 9, .label = "z"}});
    owner.runAll();
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#5 role=Normal text=z\n");
}

TEST_CASE("ui::forEach: a null row view mounts no widget, and the other rows keep their order", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    ui::Mounted const view{runtime, backend, ui::forEach<Lap>(rows, keyOf, [](Signal<Lap> const& lap) -> ui::Node {
                               return lap.peek().id == 2 ? nullptr : lapView(lap);
                           })};
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#2 role=Normal text=a\n  Text#3 role=Normal text=c\n");
    backend.clearLog();
    rows.set({{.id = 3, .label = "c"}, {.id = 2, .label = "b"}, {.id = 1, .label = "a"}});
    owner.runAll();
    CHECK(backend.log() == Lines{"move Text#2 to 1"});
    backend.clearLog();
    rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}});
    owner.runAll();
    CHECK(backend.log().empty());
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#3 role=Normal text=c\n  Text#2 role=Normal text=a\n");
}

TEST_CASE("ui::forEach: building a row reads nothing on the ForEach's behalf", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}}};
    Signal<std::string> theme{runtime, "light"};
    int pulls = 0;
    ui::Mounted const view{
        runtime, backend,
        ui::forEach<Lap>(
            [&] {
                ++pulls;
                return rows.get();
            },
            keyOf,
            [&theme](Signal<Lap> const& lap) { return ui::text({.text = theme.get() + ":" + lap.peek().label}); })};
    CHECK(backend.prop(2, "text") == "light:a");
    theme.set("dark");
    CHECK(owner.pending() == 0);
    rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
    owner.runAll();
    CHECK(pulls == 2);
    CHECK(backend.prop(3, "text") == "dark:b");
}

TEST_CASE(
    "ui::forEach: a row whose mount throws leaves nothing behind, the others mount, and it mounts again on "
    "the next change",
    "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    RecordingBackend& recording = backend.recording();
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    auto const pairView = [](Signal<Lap> const& lap) {
        return ui::column({.children = {lapView(lap), ui::text({.text = "-"})}});
    };

    SECTION("in a later snapshot") {
        Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
        ui::Mounted const view{runtime, backend, ui::forEach<Lap>(rows, keyOf, pairView)};
        std::size_t const mountedNodes = runtime.core()->liveNodes();

        recording.clearLog();
        spare = 1;  // the new row's Column; its first Text fails
        rows.set({{.id = 1, .label = "a"}, {.id = 3, .label = "x"}, {.id = 2, .label = "b"}, {.id = 4, .label = "d"}});
        owner.runAll();
        CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
        CHECK(recording.log() == Lines{"create Column#8 in Column#1", "set Column#8 gap=0", "destroy Column#8",
                                       "create Column#9 in Column#1", "set Column#9 gap=0",
                                       "create Text#10 in Column#9", "set Text#10 text=d", "set Text#10 role=Normal",
                                       "create Text#11 in Column#9", "set Text#11 text=-", "set Text#11 role=Normal"});
        // One row more: its signal, and its label's Computed and Effect.
        CHECK(runtime.core()->liveNodes() == mountedNodes + 3);

        recording.clearLog();
        rows.mutate([](std::vector<Lap>& /*laps*/) {});
        owner.runAll();
        CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
        CHECK(recording.log().back() == "move Column#12 to 1");
        CHECK(recording.dump() ==
              "Column#1 gap=0\n"
              "  Column#2 gap=0\n"
              "    Text#3 role=Normal text=a\n"
              "    Text#4 role=Normal text=-\n"
              "  Column#12 gap=0\n"
              "    Text#13 role=Normal text=x\n"
              "    Text#14 role=Normal text=-\n"
              "  Column#5 gap=0\n"
              "    Text#6 role=Normal text=b\n"
              "    Text#7 role=Normal text=-\n"
              "  Column#9 gap=0\n"
              "    Text#10 role=Normal text=d\n"
              "    Text#11 role=Normal text=-\n");
    }
    SECTION("in the first mount, which is reported rather than thrown") {
        Signal<std::vector<Lap>> const rows{runtime, {{.id = 1, .label = "a"}, {.id = 2, .label = "b"}}};
        spare = 1;  // the ForEach's own Column; the first row's Column fails
        ui::Mounted const view{runtime, backend, ui::forEach<Lap>(rows, keyOf, pairView)};
        CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
        CHECK(recording.dump() ==
              "Column#1 gap=0\n"
              "  Column#2 gap=0\n"
              "    Text#3 role=Normal text=b\n"
              "    Text#4 role=Normal text=-\n");
    }
    SECTION("from the row view itself") {
        Signal<std::vector<Lap>> rows{runtime, {{.id = 1, .label = "a"}}};
        bool refuse = true;
        ui::Mounted const view{runtime, backend, ui::forEach<Lap>(rows, keyOf, [&refuse](Signal<Lap> const& lap) {
                                   if (std::exchange(refuse, false)) {
                                       throw std::runtime_error{"the row view could not be built"};
                                   }
                                   return lapView(lap);
                               })};
        CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
        CHECK(recording.dump() == "Column#1 gap=0\n");
        rows.set({{.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
        owner.runAll();
        CHECK(recording.dump() == "Column#1 gap=0\n  Text#2 role=Normal text=a\n  Text#3 role=Normal text=b\n");
    }
}

TEST_CASE("ui::forEach: a row's own button may remove its row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    // Larger than std::function's inline buffer, so the handler's closure lives on the heap, where ASan watches it.
    std::string const note(64, 'n');
    bool aliveAfterWrite = false;
    std::string seenAfter;
    ui::Mounted const view{
        runtime, backend,
        ui::forEach<Lap>(rows, keyOf, [&rows, &backend, &aliveAfterWrite, &seenAfter, &note](Signal<Lap> const& lap) {
            return ui::button({.label = [&lap] { return lap.get().label; },
                               .onClick =
                                   [&rows, &backend, &aliveAfterWrite, &seenAfter, &lap, note = note] {
                                       removeLap(rows, lap.peek().id);
                                       aliveAfterWrite = backend.exists(3);
                                       seenAfter = note + lap.peek().label;
                                   }});
        })};
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    backend.clearLog();
    backend.click(3);
    CHECK(aliveAfterWrite);
    CHECK(seenAfter == note + "b");
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Button#3"});
    CHECK(backend.dump() == "Column#1 gap=0\n  Button#2 label=a\n  Button#4 label=c\n");
    // The row's signal, and its label's Computed and Effect, are gone with it.
    CHECK(runtime.core()->liveNodes() == mountedNodes - 3);
}

TEST_CASE("ui::forEach: a row's button survives its removal coming due while its handler runs", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    std::string const note(64, 'n');
    bool aliveInHandler = false;
    std::string seenInHandler;
    ui::Mounted const view{
        runtime, backend,
        ui::forEach<Lap>(
            rows, keyOf, [&owner, &backend, &aliveInHandler, &seenInHandler, &note](Signal<Lap> const& lap) {
                return ui::button({.label = [&lap] { return lap.get().label; },
                                   .onClick =
                                       [&owner, &backend, &aliveInHandler, &seenInHandler, &lap, note = note] {
                                           CHECK(owner.runOne());  // a modal loop
                                           aliveInHandler = backend.exists(3);
                                           seenInHandler = note + lap.peek().label;
                                       }});
            })};
    removeLap(rows, 2);  // a write from elsewhere: its posted flush would unmount the row
    backend.click(3);
    CHECK(aliveInHandler);
    CHECK(seenInHandler == note + "b");
    owner.runAll();
    CHECK(backend.dump() == "Column#1 gap=0\n  Button#2 label=a\n  Button#4 label=c\n");
}

TEST_CASE("ui::forEach: unmounting destroys the rows last first, and every node they made", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Lap>> rows{runtime, abc()};
    std::size_t const before = runtime.core()->liveNodes();
    auto view = std::make_unique<ui::Mounted>(runtime, backend, lapList(rows));
    // Shown in an order that is not the order the rows were made in.
    rows.set({{.id = 3, .label = "c"}, {.id = 1, .label = "a"}, {.id = 2, .label = "b"}});
    owner.runAll();
    backend.clearLog();
    view.reset();
    CHECK(backend.log() == Lines{"destroy Text#3", "destroy Text#2", "destroy Text#4", "destroy Column#1"});
    CHECK(runtime.core()->liveNodes() == before);
}
