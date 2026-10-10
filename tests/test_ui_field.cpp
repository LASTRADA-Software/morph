// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <vector>

#include "test_support.hpp"
#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::testing::intKey;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Lines = std::vector<std::string>;

// A text and one of each input, all given @p field, in a column: Text#2 to FilePicker#8.
[[nodiscard]] ui::Node everyField(ui::FieldState const& field) {
    return ui::column(
        {.children = {ui::text({.text = "Total", .field = field}), ui::textInput({.field = field}),
                      ui::checkbox({.label = "On", .field = field}),
                      ui::select({.options = std::vector<ui::SelectOption>{{.key = intKey(1), .label = "One"}},
                                  .field = field}),
                      ui::dateTimeInput({.field = field}), ui::slider({.field = field}),
                      ui::filePicker({.field = field})}});
}

}  // namespace

// Mutations: leave out applyField from one kind's mount (the text's, the select's); leave out any one of its four
// setters.
TEST_CASE("ui::FieldState: read-only, required, errors and stale reach the text and every input", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        everyField({.readonly = true, .required = true, .errors = Lines{"Too long", "Not a number"}, .stale = true})};
    for (int widgetId = 2; widgetId <= 8; ++widgetId) {
        INFO(backend.kindOf(widgetId));
        CHECK(backend.prop(widgetId, "readonly") == "true");
        CHECK(backend.prop(widgetId, "required") == "true");
        CHECK(backend.prop(widgetId, "errors") == "[Too long,Not a number]");
        CHECK(backend.prop(widgetId, "stale") == "true");
    }
}

// Mutation: drop any one of the four guards in applyField (every field then gets that setter call).
TEST_CASE("ui::FieldState: the defaults call no setter", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::text({.text = "plain"})};
    CHECK(backend.log() == Lines{"create Text#1 in root", "set Text#1 text=plain", "set Text#1 role=Normal"});
}

// Mutation: bind errors once (set the constant it first reads) instead of through bind.
TEST_CASE("ui::FieldState: bound errors and staleness follow their bindings", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> value{runtime, "12"};
    Signal<bool> computing{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::textInput({.value = [&] { return value.get(); },
                                          .field = {.errors =
                                                        [&] {
                                                            Lines errors;
                                                            if (value.get().size() > 3) {
                                                                errors.emplace_back("Too long");
                                                            }
                                                            return errors;
                                                        },
                                                    .stale = [&] { return computing.get(); }}})};
    CHECK(backend.prop(1, "errors") == "[]");
    CHECK(backend.prop(1, "stale") == "false");
    backend.clearLog();
    value.set("12345");
    computing.set(true);
    owner.runAll();
    CHECK(backend.log() ==
          Lines{"set TextInput#1 errors=[Too long]", "set TextInput#1 stale=true", "set TextInput#1 text=12345"});
}

// Mutation: drop the `isBound()` half of the required guard (a binding that starts false then calls no setter).
TEST_CASE("ui::FieldState: a bound required mark follows its binding", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> required{runtime, false};
    ui::Mounted const view{runtime, backend, ui::textInput({.field = {.required = [&] { return required.get(); }}})};
    CHECK(backend.prop(1, "required") == "false");
    required.set(true);
    owner.runAll();
    CHECK(backend.prop(1, "required") == "true");
}

// A read-only field shows its value and takes no user change; made editable again, it takes one. Mutations: let
// any one of edit, submit, commit, toggle, setDateTime, slide or pick use `actionable` instead of `editable`; drop
// the read-only check from choose.
TEST_CASE("RecordingBackend: a read-only field takes no edit, and takes one once editable again", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> readonly{runtime, true};
    Lines ran;
    auto const note = [&ran](std::string what) { ran.push_back(std::move(what)); };
    ui::FieldState const field{.readonly = [&] { return readonly.get(); }};
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {
                 ui::textInput({.value = "kept",
                                .onChange = [&](std::string const&) { note("change"); },
                                .onSubmit = [&](std::string const&) { note("submit"); },
                                .onCommit = [&](std::string const&) { note("commit"); },
                                .field = field}),
                 ui::checkbox({.onToggle = [&](bool) { note("toggle"); }, .field = field}),
                 ui::select({.options = std::vector<ui::SelectOption>{{.key = intKey(1), .label = "One"}},
                             .onSelect = [&](ui::Key const&) { note("select"); },
                             .field = field}),
                 ui::dateTimeInput({.onChange = [&](std::optional<morph::time::Timestamp> const&) { note("date"); },
                                    .field = field}),
                 ui::slider({.onChange = [&](std::int64_t) { note("slide"); }, .field = field}),
                 ui::filePicker({.onPicked = [&](std::string const&) { note("pick"); }, .field = field})}})};
    auto const tryEverything = [&] {
        backend.edit(2, "typed");
        backend.submit(2, "entered");
        backend.commit(2);
        backend.toggle(3);
        backend.choose(4, intKey(1));
        backend.setDateTime(5, std::nullopt);
        backend.slide(6, 40);
        backend.pick(7, "/tmp/in.csv");
    };
    tryEverything();
    CHECK(ran.empty());
    CHECK(backend.prop(2, "text") == "kept");
    CHECK(backend.prop(3, "checked") == "false");
    readonly.set(false);
    owner.runAll();
    tryEverything();
    CHECK(ran == Lines{"change", "commit", "submit", "commit", "toggle", "select", "date", "slide", "pick"});
}

// Enter commits before it submits; focus leaving commits the text the field shows, the user's own included.
// Mutations: leave out setOnCommit in the text input's mount; invoke submit before commit in
// RecordingBackend::submit; commit the formatted `text` property (it escapes the newline) instead of the text.
TEST_CASE("ui::Mounted: a text input commits on Enter, before the submit, and when focus leaves it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Lines ran;
    ui::Mounted const view{
        runtime, backend,
        ui::textInput({.value = "first",
                       .onSubmit = [&](std::string const& text) { ran.push_back("submit " + text); },
                       .onCommit = [&](std::string const& text) { ran.push_back("commit " + text); },
                       .mode = ui::TextInputMode::Multiline})};
    backend.commit(1);
    backend.edit(1, "two\nlines");
    backend.commit(1);
    backend.submit(1, "done");
    CHECK(ran == Lines{"commit first", "commit two\nlines", "commit done", "submit done"});
}
