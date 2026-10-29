// SPDX-License-Identifier: Apache-2.0

// The interpreter's building blocks, one at a time: the defaults of InterpreterHost, the failure texts and error
// kinds, the row helpers `patch` and `optimistic` are built on, and BridgeDispatcher's refusal of an `evaluate` query
// it has no evaluator for.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::Value;
using ui::ValueList;
using ui::ValueRecord;

namespace {

Value record(std::initializer_list<std::pair<std::string const, Value>> fields) {
    ValueRecord made;
    for (auto const& [name, value] : fields) {
        made.insert_or_assign(name, value);
    }
    return Value{std::move(made)};
}

Value row(std::int64_t id, std::string const& name) { return record({{"id", Value{id}}, {"name", Value{name}}}); }

}  // namespace

// Mutation: make any InterpreterHost default answer `true`, or act on its arguments.
TEST_CASE("InterpreterHost: every default answers not supported and changes nothing", "[ui][interpreter][host]") {
    ui::InterpreterHost host;
    bool called = false;
    CHECK_FALSE(host.navigate(
        ui::NavigateRequest{.screen = "s", .route = {}, .params = {}, .as = {}}, [&](Value const&) { called = true; },
        [&] { called = true; }));
    CHECK_FALSE(host.closeSelf("s", Value{"r"}));
    CHECK_FALSE(host.hostCommand("beep", Value{}));
    CHECK_FALSE(host.switchBackend("staging", [&](std::expected<void, std::string> const&) { called = true; }));
    host.resetPrefs("s", "all");
    host.setLocale("de");
    CHECK_FALSE(host.notify("hi", "info"));
    CHECK_FALSE(host.signIn(Value{}));
    host.signOut();
    CHECK_FALSE(host.setAppValue("banner", Value{"hey"}));
    auto const progress = [&](Value const&) { called = true; };
    auto const done = [&](std::expected<Value, std::string> const&) { called = true; };
    CHECK_FALSE(host.upload(Value{"/tmp/a"}, progress, done));
    // uploadFor's default is upload, with the mutation dropped.
    CHECK_FALSE(
        host.uploadFor(ui::UploadRequest{.file = Value{"/tmp/a"}, .modelType = "M", .action = "A"}, progress, done));
    CHECK_FALSE(host.download(Value{"ref"}, "dialog", Value{"a.csv"}, done));
    host.preferenceChanged("s", "density", Value{"compact"});
    host.permissionsStale();
    CHECK_FALSE(called);
}

// Mutation: drop a catch arm from defaultErrorKind (that failure is classified `error`).
TEST_CASE("interpreter: the default error kinds name the bridge's own failures", "[ui][interpreter]") {
    using ui::detail::defaultErrorKind;
    CHECK(defaultErrorKind(std::make_exception_ptr(morph::backend::DisconnectedError{})) == "offline");
    CHECK(defaultErrorKind(std::make_exception_ptr(morph::backend::TimeoutError{})) == "timeout");
    CHECK(defaultErrorKind(std::make_exception_ptr(morph::backend::ClientTimeoutError{})) == "timeout");
    CHECK(defaultErrorKind(std::make_exception_ptr(morph::backend::BackendChangedError{})) == "backendChanged");
    CHECK(defaultErrorKind(std::make_exception_ptr(std::runtime_error{"x"})) == "error");
    CHECK(defaultErrorKind(std::make_exception_ptr(42)) == "error");
}

// Mutation: report a non-standard exception's text as empty instead of `unknown error`.
TEST_CASE("interpreter: a failure's text is what() or `unknown error`", "[ui][interpreter]") {
    using ui::detail::failureText;
    CHECK(failureText(std::make_exception_ptr(std::runtime_error{"it broke"})) == "it broke");
    CHECK(failureText(std::make_exception_ptr(42)) == "unknown error");
}

// Mutation: merge a non-record `with` into a record row (the row keeps its fields).
TEST_CASE("interpreter: a row merge writes record fields over a record, anything else replaces the row",
          "[ui][interpreter]") {
    using ui::detail::mergeRow;
    CHECK(mergeRow(row(1, "a"), record({{"name", Value{"b"}}})) == row(1, "b"));
    CHECK(mergeRow(row(1, "a"), Value{"gone"}) == Value{"gone"});
    CHECK(mergeRow(Value{std::int64_t{3}}, row(2, "c")) == row(2, "c"));
}

// Mutation: let patchRows treat a null key as matching rows without an id (the row without one is replaced).
TEST_CASE("interpreter: a patch finds its row by id in the list or in the record member holding it",
          "[ui][interpreter]") {
    using ui::detail::PatchMiss;
    using ui::detail::patchRows;
    Value const list{ValueList{row(1, "a"), row(2, "b"), record({{"name", Value{"no id"}}})}};
    auto const patched = patchRows(list, "", Value{std::int64_t{2}}, record({{"name", Value{"B"}}}));
    REQUIRE(patched.has_value());
    CHECK(*patched == Value{ValueList{row(1, "a"), row(2, "B"), record({{"name", Value{"no id"}}})}});

    auto const nullKey = patchRows(list, "", Value{}, record({{"name", Value{"x"}}}));
    REQUIRE_FALSE(nullKey.has_value());
    CHECK(nullKey.error() == PatchMiss::NoRow);
    auto const missing = patchRows(list, "", Value{std::int64_t{9}}, record({}));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == PatchMiss::NoRow);

    Value const holder = record({{"rows", list}, {"total", Value{std::int64_t{3}}}});
    auto const inMember = patchRows(holder, "rows", Value{std::int64_t{1}}, record({{"name", Value{"A"}}}));
    REQUIRE(inMember.has_value());
    CHECK(*inMember->field("total") == Value{std::int64_t{3}});
    CHECK(inMember->field("rows")->asList()->front() == row(1, "A"));
    CHECK(patchRows(holder, "absent", Value{std::int64_t{1}}, record({})).error() == PatchMiss::NoList);
    CHECK(patchRows(list, "rows", Value{std::int64_t{1}}, record({})).error() == PatchMiss::NoList);
    CHECK(patchRows(record({{"rows", Value{"flat"}}}), "rows", Value{std::int64_t{1}}, record({})).error() ==
          PatchMiss::NoList);
    CHECK(patchRows(Value{"flat"}, "", Value{std::int64_t{1}}, record({})).error() == PatchMiss::NoList);
}

// Mutation: strip fields from a scalar by returning null (the scalar is lost).
TEST_CASE("interpreter: withoutFields strips records and the records of a list, and keeps a scalar",
          "[ui][interpreter]") {
    using ui::detail::withoutFields;
    std::vector<std::string> const secret{"name"};
    CHECK(withoutFields(row(1, "a"), secret) == record({{"id", Value{std::int64_t{1}}}}));
    CHECK(withoutFields(Value{ValueList{row(1, "a")}}, secret) ==
          Value{ValueList{record({{"id", Value{std::int64_t{1}}}})}});
    CHECK(withoutFields(Value{"plain"}, secret) == Value{"plain"});
    CHECK(withoutFields(row(1, "a"), {}) == row(1, "a"));
}

// Mutation: execute an `evaluate` call as a plain action when no evaluator is given (it reaches the bridge).
TEST_CASE("BridgeDispatcher refuses an evaluate query it has no evaluator for", "[ui][interpreter]") {
    morph::testing::StepExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(owner), owner};
    ui::BridgeDispatcher dispatcher{bridge, &owner};
    std::shared_ptr<ui::ModelChannel> const channel =
        dispatcher.bind("Lims_SampleModel", morph::bridge::BindSharing::Private, "");
    std::optional<std::string> refused;
    auto completion = channel->execute("Lims_Attach", "{}", ui::CallOptions{.evaluate = true, .queueOffline = false});
    completion.onError([&](std::exception_ptr const& error) { refused = ui::detail::failureText(error); });
    owner.runAll();
    CHECK(refused == "evaluate needs the UI service");
}

namespace {

/// A dispatcher whose channels are null, or throw from `execute`.
class BrokenDispatcher final : public ui::Dispatcher {
public:
    explicit BrokenDispatcher(bool nullChannels) : _null{nullChannels} {}

    [[nodiscard]] std::shared_ptr<ui::ModelChannel> bind(std::string const& typeId, morph::bridge::BindSharing sharing,
                                                         std::string const& instanceKey) override {
        static_cast<void>(typeId);
        static_cast<void>(sharing);
        static_cast<void>(instanceKey);
        ++binds;
        return _null ? nullptr : std::make_shared<Throwing>();
    }

    int binds = 0;

private:
    class Throwing final : public ui::ModelChannel {
    public:
        [[nodiscard]] morph::async::Completion<std::string> execute(std::string const& actionId,
                                                                    std::string const& bodyJson,
                                                                    ui::CallOptions options) override {
            static_cast<void>(bodyJson);
            static_cast<void>(options);
            throw std::runtime_error{"wire down for " + actionId};
        }
    };

    bool _null;
};

constexpr std::string_view kBroken = R"({
  "vocab": "ui/1", "id": "broken",
  "models": { "auth": { "type": "Auth_SessionModel" }, "sample": { "type": "Lims_SampleModel" } },
  "queries": { "who": { "model": "auth", "action": "Auth_Token" } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "queryError", "text": { "fn": "coalesce", "args": [ { "error": "who" }, "-" ] } },
    { "kind": "text", "testId": "saveError", "text": { "fn": "coalesce", "args": [ { "error": "save" }, "-" ] } },
    { "kind": "button", "testId": "save", "label": "save", "onClick": [ { "run": "save" } ] } ] } })";

}  // namespace

// Mutation: send a query or a mutation through a null channel (it crashes), or let a throwing channel escape the
// interpreter (the click throws).
TEST_CASE("interpreter: a channel that is missing or throws fails the query and the mutation with its reason",
          "[ui][interpreter]") {
    bool const nullChannels = GENERATE(true, false);
    morph::ui::testing::Harness h;
    BrokenDispatcher dispatcher{nullChannels};
    ui::Interpreter interpreter{h.session, dispatcher, ui::InterpreterOptions{.host = &h.host}};
    auto screen =
        interpreter.mount(morph::ui::testing::loadOrFail(kBroken, morph::ui::testing::corpusCatalog()), {}, h.backend);
    REQUIRE(screen.has_value());
    h.settle();
    h.backend.click(h.byTestId("Button", "save"));
    h.settle();
    CHECK(dispatcher.binds > 0);
    if (nullChannels) {
        CHECK(h.text("queryError") == "the model alias has no instance");
        CHECK(h.text("saveError") == "the model alias has no instance");
    } else {
        CHECK(h.text("queryError") == "wire down for Auth_Token");
        CHECK(h.text("saveError") == "wire down for Lims_Attach");
    }
}

// GCC 15 at -O3 loses track of the variant's index once a list's make_shared is inlined, and reports the
// string branch of engaged() as reading an uninitialized length on a value that holds a list -- a branch it
// can never take.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
// Mutation: count an empty list as engaged.
TEST_CASE("interpreter: engaged is false for null, empty text and an empty list", "[ui][interpreter]") {
    using ui::detail::engaged;
    CHECK_FALSE(engaged(Value{}));
    CHECK_FALSE(engaged(Value{""}));
    CHECK_FALSE(engaged(Value{ValueList{}}));
    CHECK(engaged(Value{ValueList{Value{std::int64_t{0}}}}));
    CHECK(engaged(Value{"x"}));
    CHECK(engaged(Value{false}));
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Mutation: show a quantity without its unit.
TEST_CASE("interpreter: a quantity's text is its number and its unit", "[ui][interpreter]") {
    ui::FormatContext const format;
    Value const quantity{
        ui::Quantity{.value = morph::math::Rational{morph::math::Numerator{15}, morph::math::Denominator{10},
                                                    morph::math::DecimalPlaces{1}},
                     .unit = "mg"}};
    CHECK(ui::detail::textOf(quantity, format) == "1.5 mg");
    CHECK(ui::detail::textOf(Value{}, format).empty());
    CHECK(ui::detail::textOf(Value{true}, format) == "true");
}

// Mutation: drop a case from commandKey (that command reports as `unknown`).
TEST_CASE("interpreter: every command kind has its key, and an unknown one is `unknown`", "[ui][interpreter]") {
    using ui::CommandKind;
    using ui::detail::commandKey;
    CHECK(commandKey(CommandKind::All) == "all");
    CHECK(commandKey(CommandKind::Seq) == "seq");
    CHECK(commandKey(static_cast<CommandKind>(250)) == "unknown");
}

// Mutation: leave the user depth raised when a user event throws (later writes still count as the user's).
TEST_CASE("Session::asUser restores the user depth when the body throws", "[ui][interpreter]") {
    morph::ui::testing::Harness h;
    CHECK_THROWS_AS(h.session.asUser([] { throw std::runtime_error{"no"}; }), std::runtime_error);
    h.session.noteWrite();
    CHECK_FALSE(h.session.lastWriteByUser());
    h.session.asUser([&] { h.session.noteWrite(); });
    CHECK(h.session.lastWriteByUser());
}

namespace {

morph::math::Rational onePointFive() {
    return morph::math::Rational{morph::math::Numerator{15}, morph::math::Denominator{10},
                                 morph::math::DecimalPlaces{1}};
}

}  // namespace

// Mutation: send an exact quantity prop as formatted text (it loses its places), or drop its unit.
TEST_CASE("interpreter: a custom component's int or exact prop is plain decimal text, units included",
          "[ui][interpreter][custom]") {
    using ui::detail::componentText;
    ui::FormatContext const format;
    Value const quantity{ui::Quantity{.value = onePointFive(), .unit = "mg"}};
    CHECK(componentText(quantity, "exact", format) == "1.5 mg");
    CHECK(componentText(quantity, "int", format) == "1.5 mg");
    CHECK(componentText(Value{ui::Decimal{.value = onePointFive()}}, "exact", format) == "1.5");
    CHECK(componentText(Value{std::int64_t{7}}, "int", format) == "7");
    CHECK(componentText(Value{"x"}, "exact", format) == "x");
    CHECK(componentText(Value{"x"}, "json", format) == R"("x")");
}

// Mutation: make componentNumber read anything that is not an int as zero (a decimal or a quantity loses its value).
TEST_CASE("interpreter: a custom component's number prop is an int, decimal or quantity as a double, else zero",
          "[ui][interpreter][custom]") {
    using ui::detail::componentNumber;
    CHECK(componentNumber(Value{std::int64_t{3}}) == 3.0);
    CHECK(componentNumber(Value{ui::Decimal{.value = onePointFive()}}) == 1.5);
    CHECK(componentNumber(Value{ui::Quantity{.value = onePointFive(), .unit = "mg"}}) == 1.5);
    CHECK(componentNumber(Value{"3"}) == 0.0);
}

// Mutation: read a JSON payload without its declared type (a typed event gets text).
TEST_CASE("interpreter: a custom component's payload is an int, a string, or JSON read as its declared type",
          "[ui][interpreter][custom]") {
    using ui::detail::componentPayload;
    CHECK(componentPayload("i:42", nullptr) == Value{std::int64_t{42}});
    CHECK(componentPayload("i:4x", nullptr) == Value{"i:4x"});
    CHECK(componentPayload("s:hello", nullptr) == Value{"hello"});
    ui::Type const intType = ui::Type::of(ui::TypeKind::Int);
    CHECK(componentPayload("17", &intType) == Value{std::int64_t{17}});
    CHECK(componentPayload("17", nullptr) == Value{"17"});
    ui::Type const boolType = ui::Type::of(ui::TypeKind::Bool);
    CHECK(componentPayload("17", &boolType) == Value{"17"});
    CHECK(componentPayload("{not json", &intType) == Value{"{not json"});
}

// Mutation: read a decimal as the fallback instead of truncating it.
TEST_CASE("interpreter: intOf reads an int, truncates a decimal and falls back for anything else",
          "[ui][interpreter]") {
    using ui::detail::intOf;
    CHECK(intOf(Value{std::int64_t{9}}, -1) == 9);
    CHECK(intOf(Value{ui::Decimal{.value = onePointFive()}}, -1) == 1);
    CHECK(intOf(Value{"9"}, -1) == -1);
}

namespace {

constexpr std::string_view kRequired = R"({
  "vocab": "ui/1", "id": "required",
  "state": { "pressed": { "init": 0 } },
  "keys": { "F7": [ { "set": "pressed", "value": 1 } ] },
  "view": { "kind": "column", "children": [
    { "kind": "checkbox", "id": "agree", "testId": "agree", "label": "agree", "required": true },
    { "kind": "select", "id": "unit", "testId": "unit", "label": "Unit", "options": { "lit": [ "g", "kg" ] },
      "required": true },
    { "kind": "text", "testId": "agreeErrors",
      "text": { "fn": "format", "args": [ "{0}", { "fn": "count", "args": [ { "ref": "agree.errors" } ] } ] } },
    { "kind": "text", "testId": "unitErrors",
      "text": { "fn": "format", "args": [ "{0}", { "fn": "count", "args": [ { "ref": "unit.errors" } ] } ] } },
    { "kind": "text", "testId": "pressed", "text": { "fn": "format", "args": [ "{0}", { "ref": "pressed" } ] } } ] } })";

}  // namespace

// Mutation: read a checkbox's or select's `value` for `required` (null: the checkbox is reported unchecked as
// missing, and a chosen unit never clears its error).
TEST_CASE("interpreter: `required` reads a checkbox's checked and a select's selected", "[ui][interpreter]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(kRequired);
    // `false` is engaged (spec: only null, "" and an empty list are not), so an unchecked checkbox is not missing.
    CHECK(h.text("agreeErrors") == "0");
    CHECK(h.text("unitErrors") == "1");
    h.backend.choose(h.byTestId("Select", "unit"), ui::Key{std::string{"kg"}});
    h.settle();
    CHECK(h.text("unitErrors") == "0");
}

// Mutation: drop a node's key bindings (the chord does nothing).
TEST_CASE("interpreter: a key chord on a node runs its commands", "[ui][interpreter]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(kRequired);
    CHECK(h.text("pressed") == "0");
    CHECK(h.backend.press(h.backend.idOf(screen->root()), "F7"));
    h.settle();
    CHECK(h.text("pressed") == "1");
}

namespace {

constexpr std::string_view kNodeKeys = R"({
  "vocab": "ui/1", "id": "nodeKeys",
  "state": { "hit": { "init": 0 } },
  "view": { "kind": "column", "testId": "page", "keys": { "F8": [ { "set": "hit", "value": 1 } ] }, "children": [
    { "kind": "text", "testId": "hit", "text": { "fn": "format", "args": [ "{0}", { "ref": "hit" } ] } } ] } })";

}  // namespace

// Mutation: drop a view node's key bindings (the chord does nothing).
TEST_CASE("interpreter: a view node's chord runs its commands", "[ui][interpreter]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(kNodeKeys);
    CHECK(h.backend.press(h.byTestId("Column", "page"), "F8"));
    h.settle();
    CHECK(h.text("hit") == "1");
}

// Mutation: map a lower-case digit as an upper-case one (`chr - 'A'`), or drop `+` or `/` (each decodes wrongly).
TEST_CASE("interpreter: inline bytes are base64 text over the whole alphabet, or a list of bytes",
          "[ui][interpreter][transfer]") {
    using ui::detail::base64Decode;
    using ui::detail::inlineBytes;
    CHECK(base64Decode("TWFu") == std::string{"Man"});
    CHECK(base64Decode("bWFu") == std::string{"man"});
    CHECK(base64Decode("MTIz") == std::string{"123"});
    CHECK(base64Decode("+/8=") == std::string{"\xFB\xFF"});
    CHECK(base64Decode("TWE=") == std::string{"Ma"});
    CHECK(base64Decode("") == std::string{});
    CHECK_FALSE(base64Decode("TWFuT").has_value());  // one digit past a group carries no whole byte
    CHECK_FALSE(base64Decode("TW-u").has_value());

    CHECK(inlineBytes(Value{"YSxi"}) == std::string{"a,b"});
    CHECK(inlineBytes(Value{"not base64!"}).error() == "the text is not base64");
    CHECK(inlineBytes(Value{ValueList{Value{std::int64_t{97}}, Value{std::int64_t{0}}, Value{std::int64_t{255}}}}) ==
          std::string{"a\0\xFF", 3});
    CHECK(inlineBytes(Value{ValueList{Value{std::int64_t{256}}}}).error() == "a byte list holds integers 0 to 255");
    CHECK(inlineBytes(Value{ValueList{Value{std::int64_t{-1}}}}).error() == "a byte list holds integers 0 to 255");
    CHECK(inlineBytes(Value{ValueList{Value{"a"}}}).error() == "a byte list holds integers 0 to 255");
    CHECK(inlineBytes(Value{std::int64_t{1}}).error() == "inline bytes are base64 text or a list of bytes");
}

// Mutation: accept any JSON for a typed prop (a string payload passes as an int).
TEST_CASE("interpreter: a field component's change is its payload read as the value prop's type",
          "[ui][interpreter][custom]") {
    using ui::detail::changeWire;
    auto const text = [](std::string const& payload, std::string_view type) {
        auto const wire = changeWire(payload, type);
        REQUIRE(wire.has_value());
        REQUIRE(wire->is_string());
        return wire->get<std::string>();
    };
    CHECK(text("s:hello", "string") == "hello");
    CHECK(text("plain", "string") == "plain");
    CHECK(text("\"12.50\"", "exact") == "12.50");

    CHECK(changeWire("i:42", "int")->is_number());
    CHECK(changeWire("4.5", "number")->is_number());
    CHECK(changeWire("7", "exact")->is_number());
    CHECK(changeWire("true", "bool")->is_boolean());
    CHECK(changeWire("null", "int")->is_null());
    CHECK(changeWire("[1,2]", "json")->is_array());

    CHECK(changeWire("{not", "json").error() == "it is not JSON");
    CHECK(changeWire("\"x\"", "int").error() == "it is not an int");
    CHECK(changeWire("\"x\"", "number").error() == "it is not a number");
    CHECK(changeWire("1", "bool").error() == "it is not a bool");
    CHECK(changeWire("true", "exact").error() == "it is not an exact");
}

// Mutation: stop at a record without the key field instead of reading its members as rows (the map's rows are lost).
TEST_CASE("interpreter: held rows are found and patched in a list, under a member, and as a map's values",
          "[ui][interpreter]") {
    using ui::detail::keyedRowsOf;
    using ui::detail::patchHeldRows;
    using ui::detail::RowsByKey;
    Value const byName = record({{"first", row(1, "a")}, {"second", row(2, "b")}, {"note", Value{"not a row"}}});
    Value const held = record({{"rows", Value{ValueList{row(3, "c")}}}, {"byName", byName}});

    std::vector<Value const*> const found = keyedRowsOf(held, {"rows", "byName", "absent"}, "id");
    REQUIRE(found.size() == 3);
    CHECK(*found.at(0) == row(3, "c"));
    CHECK(keyedRowsOf(Value{"flat"}, {"rows"}, "id").empty());

    Value const newer = row(2, "B");
    Value const unrelated = row(9, "z");
    RowsByKey const rows{
        {ui::detail::canonicalJson(Value{std::int64_t{2}}), &newer},
        {ui::detail::canonicalJson(Value{std::int64_t{9}}), &unrelated},
    };
    std::optional<Value> const patched = patchHeldRows(held, {"rows", "byName", "absent"}, "id", rows);
    REQUIRE(patched.has_value());
    CHECK(*patched->field("byName")->field("second") == row(2, "B"));
    CHECK(*patched->field("byName")->field("first") == row(1, "a"));
    CHECK(*patched->field("rows") == Value{ValueList{row(3, "c")}});

    RowsByKey const none{{ui::detail::canonicalJson(Value{std::int64_t{9}}), &unrelated}};
    CHECK_FALSE(patchHeldRows(held, {"rows", "byName", "absent"}, "id", none).has_value());
    CHECK_FALSE(patchHeldRows(Value{"flat"}, {""}, "id", rows).has_value());
}

// Mutation: accept a record with a second member (`{"fixed": 3, "stretch": 1}` reads as fixed and hides the slip).
TEST_CASE("interpreter: a sizing is \"content\", or one fixed or stretch member holding an int", "[ui][interpreter]") {
    using ui::detail::isSizing;
    CHECK(isSizing(Value{"content"}));
    CHECK_FALSE(isSizing(Value{"wide"}));
    CHECK(isSizing(record({{"fixed", Value{std::int64_t{3}}}})));
    CHECK(isSizing(record({{"stretch", Value{std::int64_t{2}}}})));
    CHECK_FALSE(isSizing(record({{"fixed", Value{"3"}}})));
    CHECK_FALSE(isSizing(record({{"fill", Value{std::int64_t{1}}}})));
    CHECK_FALSE(isSizing(record({{"fixed", Value{std::int64_t{3}}}, {"stretch", Value{std::int64_t{1}}}})));
    CHECK_FALSE(isSizing(Value{std::int64_t{3}}));
}

namespace {

/// @brief The `layout=` of the widget a slot shows, the slot's direct child, from the backend's dump.
/// @param dump  The dump.
/// @param slot  The line prefix of the slot whose child to read.
/// @return The layout text; `content/content` for a child that records none, empty when the slot shows nothing.
std::string slotChildLayout(std::string const& dump, std::string const& slot) {
    std::size_t const at = dump.find(slot);
    if (at == std::string::npos) {
        return {};
    }
    std::size_t const child = dump.find('\n', at);
    std::size_t const end = dump.find('\n', child + 1);
    std::string const line = dump.substr(child + 1, end - child - 1);
    std::size_t const key = line.find("layout=");
    if (key == std::string::npos) {
        return "content/content";  // A default request is never recorded.
    }
    std::size_t const stop = line.find(' ', key);
    return line.substr(key + 7, stop == std::string::npos ? std::string::npos : stop - key - 7);
}

}  // namespace

// A slot reads its direct child's sizing alone, and the node a `switch` mounts is that child; so the holder's own
// sizing is what the checks read. Mutation: build `lazy` without the hints (the inner column still stretches, the
// slot's child does not), or build the column without `listColumn`.
TEST_CASE("interpreter: a switch case, a default and a dialog body pass their children's stretch to the slot",
          "[ui][interpreter][layout]") {
    auto const mounted = [](std::string const& cases, std::string const& fallback, ui::testing::Harness& h) {
        return h.mount(R"({ "vocab": "ui/1", "id": "t", "view": { "kind": "switch", "on": 2, "cases": [)" + cases +
                       R"(], "default": [)" + fallback + R"(] } })");
    };
    std::string const stretching =
        R"({ "kind": "column", "sizing": { "lit": { "height": { "stretch": 1 } } }, "children": [ { "kind": "spacer" } ] })";
    std::string const text = R"({ "kind": "text", "text": "a" })";
    SECTION("a case holding a column that stretches down") {
        ui::testing::Harness h;
        auto screen = mounted(R"({ "when": 2, "children": [)" + stretching + "] }", text, h);
        CHECK(slotChildLayout(h.backend.dump(), "Slot") == "content/stretch(1)");
    }
    SECTION("a case holding two texts") {
        ui::testing::Harness h;
        auto screen = mounted(R"({ "when": 2, "children": [)" + text + "," + text + "] }", text, h);
        CHECK(slotChildLayout(h.backend.dump(), "Slot") == "content/content");
    }
    SECTION("a default of two nodes, one stretching across by 2") {
        ui::testing::Harness h;
        std::string const wide = R"({ "kind": "spacer", "sizing": { "lit": { "width": { "stretch": 2 } } } })";
        auto screen = h.mount(R"({ "vocab": "ui/1", "id": "t", "view": { "kind": "switch", "on": 9, "cases": [],
            "default": [)" + wide +
                              "," + text + R"(] } })");
        CHECK(slotChildLayout(h.backend.dump(), "Slot") == "stretch(2)/content");
    }
    SECTION("a dialog body holding a stretching text input") {
        ui::testing::Harness h;
        auto screen = h.mount(R"({ "vocab": "ui/1", "id": "t", "view": { "kind": "dialog", "open": true,
            "children": [ { "kind": "textInput", "sizing": { "lit": { "stretch": 1 } } } ] } })");
        CHECK(slotChildLayout(h.backend.dump(), "Dialog") == "stretch(1)/content");
    }
    SECTION("a case whose child's sizing reads a name the case declares is held at its content size and reported") {
        ui::testing::Harness h;
        auto screen = mounted(
            R"({ "when": 2, "scope": { "state": { "wide": { "init": true } } }, "children": [
                 { "kind": "spacer", "sizing": { "if": [ { "ref": "wide" }, { "lit": { "stretch": 1 } }, "content" ] } } ] })",
            text, h);
        CHECK(slotChildLayout(h.backend.dump(), "Slot") == "content/content");
        REQUIRE_FALSE(h.diagnostics.empty());
        CHECK(h.diagnostics.front().path.contains("sizing"));
    }
}

// Mutation: skip the report (the diagnostics stay empty), or keep `wrap` on a vertical stack.
TEST_CASE("interpreter: a wrapping forEach whose computed axis is not horizontal is reported and does not wrap",
          "[ui][interpreter][layout]") {
    ui::testing::Harness h;
    auto screen = h.mount(R"({ "vocab": "ui/1", "id": "t", "state": { "on": { "init": false } },
        "view": { "kind": "forEach", "rows": { "lit": [ 1, 2 ] }, "key": { "row": "" }, "wrap": true,
                  "axis": { "if": [ { "ref": "on" }, "horizontal", "vertical" ] },
                  "children": [ { "kind": "text", "text": "x" } ] } })");
    REQUIRE_FALSE(h.diagnostics.empty());
    CHECK(h.diagnostics.front().path == "/view/wrap");
    CHECK_FALSE(h.backend.dump().contains("wrap=true"));
}

// Mutation: pass no gap to `lowerAll` or `holding` (every column records gap=0), or read the default as 0.
TEST_CASE(
    "interpreter: a panel, a collapsible, a dialog and a drawer space their children by gap, and a single child "
    "is not wrapped",
    "[ui][interpreter][layout]") {
    auto const texts = [](std::string const& extra, int count) {
        std::string made = R"("children": [)";
        for (int at = 0; at < count; ++at) {
            made += std::string{at > 0 ? "," : ""} + R"({ "kind": "text", "text": "t" })";
        }
        return extra + made + "]";
    };
    auto const dumpOf = [&](std::string const& kind, std::string const& extra, int count) {
        ui::testing::Harness h;
        auto screen = h.mount(R"({ "vocab": "ui/1", "id": "t", "view": { "kind": ")" + kind + R"(", )" +
                              texts(extra, count) + " } }");
        return h.backend.dump();
    };
    // The first column under the container that holds the children, its gap as the backend records it.
    auto const gapOf = [](std::string const& dump, std::string const& container) {
        std::size_t const at = dump.find(container);
        // An overlay's body is a one-row ForEach holding the column that carries `visible`; its gap is the column's.
        std::size_t column = dump.find("Column#", at);
        if (std::size_t const held = dump.find("visible=true", at); held != std::string::npos) {
            column = dump.rfind("Column#", held);
        }
        std::size_t const gap = dump.find("gap=", column);
        return dump.substr(gap, dump.find_first_of(" \n", gap) - gap);
    };
    CHECK(gapOf(dumpOf("panel", "", 2), "Panel#") == "gap=2");
    CHECK(gapOf(dumpOf("panel", R"("gap": 0,)", 2), "Panel#") == "gap=0");
    CHECK(gapOf(dumpOf("collapsible", "", 2), "Collapsible#") == "gap=2");
    CHECK(gapOf(dumpOf("dialog", R"("open": true,)", 2), "Dialog#") == "gap=3");
    CHECK(gapOf(dumpOf("drawer", R"("open": true, "gap": 1,)", 2), "Drawer#") == "gap=1");
    // A single child is lowered unwrapped, so a gap has nothing to space.
    CHECK_FALSE(dumpOf("panel", "", 1).contains("Column#"));
}
