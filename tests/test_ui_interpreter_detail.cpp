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
