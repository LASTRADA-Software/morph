// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <memory>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/field_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using core::tui::Modifier;
using morph::time::DateTime;
using morph::time::Timestamp;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::DateTimeInputImpl;
using morph::tui::detail::Direction;
using morph::tui::detail::FilePickerImpl;
using morph::tui::detail::formatLocal;
using morph::tui::detail::parseLocal;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextInputImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

DateTime at(int year, unsigned month, unsigned day, int hour, int minute) {
    return DateTime{std::chrono::year{year},  std::chrono::month{month},    std::chrono::day{day},
                    std::chrono::hours{hour}, std::chrono::minutes{minute}, std::chrono::seconds{0}};
}

/// A key with a character and modifiers, as a terminal reports Ctrl+U or Ctrl+Shift+A.
core::tui::KeyEvent chord(char character, Modifier modifiers) {
    return core::tui::test::charKey(character, modifiers);
}

}  // namespace

TEST_CASE("tui fields: typing reports each change and Enter submits", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> changes;
    std::vector<std::string> submits;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    input->setOnSubmit([&](std::string text) { submits.push_back(std::move(text)); });
    harness.focus(*input);
    static_cast<void>(harness.type("hi"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(changes == std::vector<std::string>{"h", "hi"});
    CHECK(submits == std::vector<std::string>{"hi"});
    CHECK(harness.draw() == Rows{"hi"});
}

TEST_CASE("tui fields: setText with the field's own text keeps the cursor and fires nothing", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> changes;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    harness.focus(*input);
    static_cast<void>(harness.type("abc"));
    static_cast<void>(harness.key(KeyCode::Left));
    input->setText("abc");  // a binding echoing what was just typed
    CHECK(changes.size() == 3);
    static_cast<void>(harness.type("X"));
    CHECK(changes.back() == "abXc");
}

TEST_CASE("tui fields: setText with new text replaces it and fires nothing", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    int changes = 0;
    input->setOnChange([&](std::string const&) { ++changes; });
    input->setText("new");
    CHECK(changes == 0);
    CHECK(harness.draw() == Rows{"new"});
}

TEST_CASE("tui fields: Tab, Shift+Tab and Esc are left to the frontend and the dialog", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    input->setText("keep");
    harness.focus(*input);
    CHECK(harness.key(KeyCode::Tab) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Tab, Modifier::Shift) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Escape) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"keep"});
}

TEST_CASE("tui fields: the placeholder shows while the field is empty", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    input->setPlaceholder("name");
    CHECK(harness.draw() == Rows{"name"});
    harness.focus(*input);
    static_cast<void>(harness.type("x"));
    CHECK(harness.draw() == Rows{"x"});
}

TEST_CASE("tui fields: a password field never shows what was typed", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::Password);
    harness.focus(*input);
    static_cast<void>(harness.type("pw"));
    auto const rows = harness.draw();
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows.front().contains("pw"));
    CHECK_FALSE(rows.front().empty());
}

TEST_CASE("tui fields: local date-time text in a display zone", "[tui][fields]") {
    auto const instant = at(2026, 10, 4, 13, 5);
    CHECK(formatLocal(instant, ui::DateMode::DateTime, 0) == "2026-10-04 13:05");
    CHECK(formatLocal(instant, ui::DateMode::DateTime, 120) == "2026-10-04 15:05");
    CHECK(formatLocal(instant, ui::DateMode::Date, 0) == "2026-10-04");
    CHECK(parseLocal("2026-10-04 15:05", ui::DateMode::DateTime, 120) == instant);
    CHECK(parseLocal("2026-10-04T13:05", ui::DateMode::DateTime, 0) == instant);
    CHECK(parseLocal("2026-10-04", ui::DateMode::Date, 0) == at(2026, 10, 4, 0, 0));
    CHECK_FALSE(parseLocal("2026-13-01", ui::DateMode::Date, 0).has_value());
    CHECK_FALSE(parseLocal("garbage", ui::DateMode::DateTime, 0).has_value());
}

TEST_CASE("tui fields: a date-time field steps with the arrow keys and commits on Enter", "[tui][fields]") {
    Harness harness{20, 1};
    auto const field = harness.make<DateTimeInputImpl>(nullptr, ui::DateMode::DateTime, 0);
    std::vector<std::optional<Timestamp>> changes;
    field->setOnChange([&](std::optional<Timestamp> value) { changes.push_back(value); });
    field->setValue(Timestamp{at(2026, 10, 4, 13, 5)});
    CHECK(harness.draw() == Rows{"2026-10-04 13:05"});
    CHECK(changes.empty());

    harness.focus(*field);
    static_cast<void>(harness.key(KeyCode::Up));
    CHECK(harness.draw() == Rows{"2026-10-04 13:06"});
    REQUIRE(changes.size() == 1);
    CHECK(changes.back()->value == at(2026, 10, 4, 13, 6));

    static_cast<void>(harness.key(KeyCode::PageDown));
    CHECK(changes.back()->value == at(2026, 10, 3, 13, 6));

    field->setValue(std::nullopt);
    static_cast<void>(harness.type("2026-01-02 03:04"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(changes.back()->value == at(2026, 1, 2, 3, 4));

    field->setValue(std::nullopt);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK_FALSE(changes.back().has_value());
}

TEST_CASE("tui fields: an unparsable date-time is not committed and is marked", "[tui][fields]") {
    Harness harness{20, 1};
    auto const field = harness.make<DateTimeInputImpl>(nullptr, ui::DateMode::Date, 0);
    int changes = 0;
    field->setOnChange([&](std::optional<Timestamp> const&) { ++changes; });
    harness.focus(*field);
    static_cast<void>(harness.type("nope"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(changes == 0);
    CHECK(harness.draw().front().back() == '!');
    field->setValue(Timestamp{at(2026, 10, 4, 0, 0)});
    CHECK(harness.draw() == Rows{"2026-10-04"});
    CHECK(changes == 0);
}

TEST_CASE("tui fields: a file picker is a path field that reports on Enter", "[tui][fields]") {
    Harness harness{30, 1};
    auto const picker = harness.make<FilePickerImpl>(nullptr, ui::FilePickerMode::Open);
    std::vector<std::string> picked;
    picker->setOnPicked([&](std::string path) { picked.push_back(std::move(path)); });
    picker->setPath("/a");
    CHECK(harness.draw() == Rows{"Open: /a"});
    CHECK(picked.empty());
    harness.focus(*picker);
    static_cast<void>(harness.type("/b"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(picked == std::vector<std::string>{"/a/b"});
}

TEST_CASE("tui fields: cursor movement and Backspace step over whole characters", "[tui][fields][unicode]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> changes;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    harness.focus(*input);

    SECTION("a combining mark stays with its base") {
        harness.sgr(
            "ae\xCC\x81"
            "b");  // a, e + COMBINING ACUTE ACCENT, b
        static_cast<void>(harness.key(KeyCode::Left));
        static_cast<void>(harness.key(KeyCode::Left));
        static_cast<void>(harness.type("X"));
        CHECK(changes.back() ==
              "aXe\xCC\x81"
              "b");
        static_cast<void>(harness.key(KeyCode::End));
        static_cast<void>(harness.key(KeyCode::Backspace));
        static_cast<void>(harness.key(KeyCode::Backspace));
        CHECK(changes.back() == "aX");
    }
    SECTION("a wide character is one step") {
        harness.sgr(
            "\xE6\xBC\xA2"
            "z");  // U+6F22, two cells wide, then z
        static_cast<void>(harness.key(KeyCode::Left));
        static_cast<void>(harness.key(KeyCode::Left));
        static_cast<void>(harness.type("X"));
        CHECK(changes.back() ==
              "X\xE6\xBC\xA2"
              "z");
        CHECK(harness.draw() == Rows{"X\xE6\xBC\xA2"
                                     "z"});
    }
    SECTION("an emoji with a skin-tone modifier is typed and deleted as one character") {
        harness.sgr("a\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");  // a, U+1F44D U+1F3FD
        CHECK(changes.back() == "a\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");
        static_cast<void>(harness.key(KeyCode::Left));
        static_cast<void>(harness.type("X"));
        CHECK(changes.back() == "aX\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");
        static_cast<void>(harness.key(KeyCode::End));
        static_cast<void>(harness.key(KeyCode::Backspace));
        CHECK(changes.back() == "aX");
    }
    SECTION("a pasted emoji is one character too") {
        static_cast<void>(harness.send(core::tui::PasteEvent{.text = "a\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"}));
        static_cast<void>(harness.key(KeyCode::Backspace));
        CHECK(changes.back() == "a");
    }
}

// Ctrl+End moves to the end of the whole text and Ctrl+U deletes everything before the cursor, wherever the cursor
// was and in every mode.
TEST_CASE("tui fields: Ctrl+End and Ctrl+U empty a field in every mode, as the conformance probe does",
          "[tui][fields]") {
    Harness harness{20, 3};
    auto const mode =
        GENERATE(ui::TextInputMode::SingleLine, ui::TextInputMode::Multiline, ui::TextInputMode::Password);
    auto const input = harness.make<TextInputImpl>(nullptr, mode);
    std::vector<std::string> changes;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    input->setText(mode == ui::TextInputMode::Multiline ? "one\ntwo" : "old");
    harness.focus(*input);
    static_cast<void>(harness.key(KeyCode::Up));
    static_cast<void>(harness.key(KeyCode::Home));
    CHECK(harness.key(KeyCode::End, Modifier::Ctrl) == EventResult::Handled);
    CHECK(harness.send(chord('u', Modifier::Ctrl)) == EventResult::Handled);
    CHECK(changes == std::vector<std::string>{""});
    static_cast<void>(harness.type("new"));
    CHECK(changes.back() == "new");
    CHECK(WidgetBase::of(*input).probeText() == "new");
}

TEST_CASE("tui fields: in a multiline field Enter starts a new line and submits nothing", "[tui][fields]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const input = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::Multiline);
    std::vector<std::string> changes;
    int submits = 0;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    input->setOnSubmit([&](std::string const&) { ++submits; });
    harness.focus(*input);
    static_cast<void>(harness.type("a"));
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    static_cast<void>(harness.type("b"));
    static_cast<void>(harness.key(KeyCode::Enter, Modifier::Alt));
    static_cast<void>(harness.type("c"));
    CHECK(submits == 0);
    CHECK(changes.back() == "a\nb\nc");
    CHECK(harness.draw() == Rows{"a", "b", "c"});
}

TEST_CASE("tui fields: a password field submits on Enter", "[tui][fields]") {
    Harness harness{20, 1};
    auto const input = harness.make<TextInputImpl>(nullptr, ui::TextInputMode::Password);
    std::vector<std::string> submits;
    input->setOnSubmit([&](std::string text) { submits.push_back(std::move(text)); });
    harness.focus(*input);
    static_cast<void>(harness.type("pw"));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(submits == std::vector<std::string>{"pw"});
}

TEST_CASE("tui fields: a date field steps by days and reports local midnight", "[tui][fields]") {
    // 23:30 UTC is already the next day two hours east.
    morph::time::ScopedNowOverride const clock{at(2026, 10, 4, 23, 30)};
    Harness harness{20, 1};
    auto const field = harness.make<DateTimeInputImpl>(nullptr, ui::DateMode::Date, 120);
    std::vector<std::optional<Timestamp>> changes;
    field->setOnChange([&](std::optional<Timestamp> value) { changes.push_back(value); });
    harness.focus(*field);
    static_cast<void>(harness.key(KeyCode::Up));
    CHECK(harness.draw() == Rows{"2026-10-06"});
    REQUIRE(changes.size() == 1);
    REQUIRE(changes.back().has_value());
    CHECK(changes.back()->value == at(2026, 10, 5, 22, 0));
    static_cast<void>(harness.key(KeyCode::Down));
    CHECK(changes.back()->value == at(2026, 10, 4, 22, 0));
}

TEST_CASE("tui fields: a date-time field reports the minute it shows", "[tui][fields]") {
    Harness harness{20, 1};
    auto const field = harness.make<DateTimeInputImpl>(nullptr, ui::DateMode::DateTime, 0);
    std::vector<std::optional<Timestamp>> changes;
    field->setOnChange([&](std::optional<Timestamp> value) { changes.push_back(value); });
    field->setValue(Timestamp{at(2026, 10, 4, 13, 5) + std::chrono::seconds{30}});
    harness.focus(*field);
    static_cast<void>(harness.key(KeyCode::Up));
    REQUIRE(changes.size() == 1);
    CHECK(changes.back()->value == at(2026, 10, 4, 13, 6));
}

TEST_CASE("tui fields: no setter calls a handler", "[tui][fields]") {
    Harness harness{30, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const input = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
    auto const field = harness.make<DateTimeInputImpl>(column.get(), ui::DateMode::DateTime, 0);
    auto const picker = harness.make<FilePickerImpl>(column.get(), ui::FilePickerMode::Save);
    int calls = 0;
    input->setOnChange([&](std::string const&) { ++calls; });
    input->setOnSubmit([&](std::string const&) { ++calls; });
    field->setOnChange([&](std::optional<Timestamp> const&) { ++calls; });
    picker->setOnPicked([&](std::string const&) { ++calls; });
    input->setText("a");
    input->setText("a");
    input->setText("");
    input->setPlaceholder("p");
    field->setValue(Timestamp{at(2026, 1, 1, 0, 0)});
    field->setValue(Timestamp{});
    field->setValue(std::nullopt);
    picker->setPath("/x");
    picker->setPath("");
    CHECK(calls == 0);
    CHECK(harness.draw() == Rows{"p", "", "Save:"});
}

TEST_CASE("tui fields: a field inside a disabled container two levels up takes no input or focus",
          "[tui][fields][gating]") {
    Harness harness{30, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const outer = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const input = harness.make<TextInputImpl>(inner.get(), ui::TextInputMode::SingleLine);
    auto const field = harness.make<DateTimeInputImpl>(inner.get(), ui::DateMode::DateTime, 0);
    auto const picker = harness.make<FilePickerImpl>(inner.get(), ui::FilePickerMode::Open);
    auto const other = harness.make<ButtonImpl>(column.get());
    other->setLabel("Other");
    int calls = 0;
    input->setOnChange([&](std::string const&) { ++calls; });
    input->setOnSubmit([&](std::string const&) { ++calls; });
    field->setOnChange([&](std::optional<Timestamp> const&) { ++calls; });
    picker->setOnPicked([&](std::string const&) { ++calls; });
    input->setText("t");
    field->setValue(Timestamp{at(2026, 10, 4, 13, 5)});
    picker->setPath("/p");
    static_cast<void>(harness.draw());

    outer->setEnabled(false);
    for (ui::Widget const* gated :
         {static_cast<ui::Widget const*>(input.get()), static_cast<ui::Widget const*>(field.get()),
          static_cast<ui::Widget const*>(picker.get())}) {
        CHECK_FALSE(WidgetBase::of(*gated).view().focusable());
        harness.focus(*gated);  // focus taken before the container was disabled
        CHECK(harness.type("x") == EventResult::Ignored);
        CHECK(harness.send(core::tui::PasteEvent{.text = "y"}) == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Up) == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    }
    CHECK(calls == 0);
    harness.screen().setFocus(nullptr);
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*other));
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Ignored);
    CHECK(harness.focused(*other));
    CHECK(harness.draw() == Rows{"t", "2026-10-04 13:05", "Open: /p", "[ Other ]"});

    outer->setEnabled(true);
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(harness.focused(*input));
    static_cast<void>(harness.type("x"));
    CHECK(calls == 1);
}

TEST_CASE("tui fields: a focused field inside a hidden container takes no typing until shown again",
          "[tui][fields][gating]") {
    Harness harness{20, 2};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const input = harness.make<TextInputImpl>(inner.get(), ui::TextInputMode::SingleLine);
    std::vector<std::string> changes;
    input->setOnChange([&](std::string text) { changes.push_back(std::move(text)); });
    harness.focus(*input);
    outer->setVisible(false);
    CHECK(harness.type("x") == EventResult::Ignored);
    CHECK(changes.empty());
    outer->setVisible(true);
    CHECK(harness.type("y") == EventResult::Handled);
    CHECK(changes == std::vector<std::string>{"y"});
}

TEST_CASE("tui fields: a field's handler may destroy its own widget", "[tui][fields][lifetime]") {
    Harness harness{30, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const survivor = harness.make<ButtonImpl>(column.get());
    survivor->setLabel("Stay");
    int calls = 0;

    SECTION("a text input's onChange, by typing") {
        auto input = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
        input->setOnChange([&input, &calls](std::string const& /*text*/) {
            input.reset();
            ++calls;
        });
        harness.focus(*input);
        CHECK(harness.type("x") == EventResult::Handled);
        CHECK(input == nullptr);
    }
    SECTION("a text input's onSubmit, by Enter") {
        auto input = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
        input->setOnSubmit([&input, &calls](std::string const& /*text*/) {
            input.reset();
            ++calls;
        });
        harness.focus(*input);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
        CHECK(input == nullptr);
    }
    SECTION("a date-time input's onChange, by a step") {
        auto field = harness.make<DateTimeInputImpl>(column.get(), ui::DateMode::DateTime, 0);
        field->setOnChange([&field, &calls](std::optional<Timestamp> const& /*value*/) {
            field.reset();
            ++calls;
        });
        harness.focus(*field);
        CHECK(harness.key(KeyCode::Up) == EventResult::Handled);
        CHECK(field == nullptr);
    }
    SECTION("a date-time input's onChange, by Enter") {
        auto field = harness.make<DateTimeInputImpl>(column.get(), ui::DateMode::Date, 0);
        field->setOnChange([&field, &calls](std::optional<Timestamp> const& /*value*/) {
            field.reset();
            ++calls;
        });
        harness.focus(*field);
        static_cast<void>(harness.type("2026-10-04"));
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
        CHECK(field == nullptr);
    }
    SECTION("a file picker's onPicked, by Enter") {
        auto picker = harness.make<FilePickerImpl>(column.get(), ui::FilePickerMode::Open);
        picker->setOnPicked([&picker, &calls](std::string const& /*path*/) {
            picker.reset();
            ++calls;
        });
        picker->setPath("/p");
        harness.focus(*picker);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
        CHECK(picker == nullptr);
    }

    CHECK(calls == 1);
    int survivorClicks = 0;
    survivor->setOnClick([&] { ++survivorClicks; });
    CHECK(harness.draw() == Rows{"[ Stay ]"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(survivorClicks == 1);
}

TEST_CASE("tui fields: no field handler runs after its widget was destroyed", "[tui][fields][lifetime]") {
    Harness harness{30, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    int calls = 0;
    SECTION("a text input") {
        auto input = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
        input->setOnChange([&](std::string const&) { ++calls; });
        input->setOnSubmit([&](std::string const&) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*input);
        input.reset();
    }
    SECTION("a date-time input") {
        auto field = harness.make<DateTimeInputImpl>(column.get(), ui::DateMode::DateTime, 0);
        field->setOnChange([&](std::optional<Timestamp> const&) { ++calls; });
        field->setValue(Timestamp{at(2026, 10, 4, 13, 5)});
        static_cast<void>(harness.draw());
        harness.focus(*field);
        field.reset();
    }
    SECTION("a file picker") {
        auto picker = harness.make<FilePickerImpl>(column.get(), ui::FilePickerMode::Open);
        picker->setOnPicked([&](std::string const&) { ++calls; });
        picker->setPath("/p");
        static_cast<void>(harness.draw());
        harness.focus(*picker);
        picker.reset();
    }
    CHECK(harness.type("x") == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Up) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Ignored);
    CHECK(calls == 0);
}

TEST_CASE("tui fields: only the focused field shows the text cursor", "[tui][fields]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
    auto const second = harness.make<TextInputImpl>(column.get(), ui::TextInputMode::SingleLine);
    first->setText("ab");
    second->setText("xyz");
    harness.focus(*first);
    CHECK(harness.draw() == Rows{"ab", "xyz"});
    auto const& frame = harness.screen().renderedBuffer();
    CHECK(frame.cursorVisible());
    CHECK(frame.cursor().x == 2);
    CHECK(frame.cursor().y == 0);
}

TEST_CASE("tui fields: a field the user cannot reach is drawn muted", "[tui][fields][gating]") {
    Harness harness{20, 1};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const input = harness.make<TextInputImpl>(inner.get(), ui::TextInputMode::SingleLine);
    input->setText("ab");
    auto const& theme = harness.screen().theme();
    REQUIRE_FALSE(theme.textMuted.fg == theme.inputNormal.fg);
    outer->setEnabled(false);
    static_cast<void>(harness.draw());
    CHECK(harness.screen().renderedBuffer().at(0, 0).style.fg == theme.textMuted.fg);
    outer->setEnabled(true);
    static_cast<void>(harness.draw());
    CHECK(harness.screen().renderedBuffer().at(0, 0).style.fg == theme.inputNormal.fg);
}

TEST_CASE("tui fields: Enter on an empty file picker picks nothing", "[tui][fields]") {
    Harness harness{30, 1};
    auto const picker = harness.make<FilePickerImpl>(nullptr, ui::FilePickerMode::Save);
    int picked = 0;
    picker->setOnPicked([&](std::string const&) { ++picked; });
    harness.focus(*picker);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(picked == 0);
}
