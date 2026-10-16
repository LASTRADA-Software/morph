// SPDX-License-Identifier: Apache-2.0

// `ui-evaluate` (spec 5 §7): an action body evaluated on the server, decoded, reconciled and recomputed, never
// executed, and answered with its field errors; the dispatcher's evaluator, the service's gate, and the client's
// evaluator, end to end through a RemoteServer.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/field_errors.hpp>
#include <morph/forms/forms.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ui_service_support.hpp"

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::wire::Envelope;

// File scope, not an anonymous namespace: UnitTraits and the model traits are specialised for these types.
// NOLINTBEGIN(misc-use-internal-linkage)
enum class UEUnit : std::uint8_t { qty, price, total };

template <>
struct morph::units::UnitTraits<UEUnit> {
    static constexpr morph::units::UnitMeta meta(UEUnit unit) noexcept {
        switch (unit) {
            case UEUnit::qty:
                return {.id = "qty", .display = "qty", .defaultDecimals = 2};
            case UEUnit::price:
                return {.id = "price", .display = "$/u", .defaultDecimals = 2};
            case UEUnit::total:
            default:
                return {.id = "total", .display = "$", .defaultDecimals = 2};
        }
    }
    static constexpr std::array<morph::units::UnitRelation<UEUnit>, 0> relations{};
    // A quantity of 0 to 1000 items; the money units are unbounded in practice.
    static constexpr morph::units::QuantityBounds bounds(UEUnit unit) noexcept {
        if (unit == UEUnit::qty) {
            return {.min = Rational{Numerator{0}, Denominator{1}, DecimalPlaces{2}},
                    .max = Rational{Numerator{1000}, Denominator{1}, DecimalPlaces{2}}};
        }
        return {.min = Rational{Numerator{-1'000'000'000}, Denominator{1}, DecimalPlaces{2}},
                .max = Rational{Numerator{1'000'000'000}, Denominator{1}, DecimalPlaces{2}}};
    }
};

consteval UEUnit operator*(UEUnit lhs, UEUnit rhs) {
    if ((lhs == UEUnit::qty && rhs == UEUnit::price) || (lhs == UEUnit::price && rhs == UEUnit::qty)) {
        return UEUnit::total;
    }
    throw "unsupported unit product";
}

template <UEUnit U>
using UEQ = morph::units::Quantity<U, morph::units::UnitTraits<UEUnit>::meta(U).defaultDecimals>;

/// A line: total = qty * price, a price of at least zero, at most ten items, a discount of at most one, and a note
/// whose metadata declares no bound.
struct UELine {
    UEQ<UEUnit::qty> qty;
    UEQ<UEUnit::price> price;
    UEQ<UEUnit::total> total;
    std::int64_t items = 0;
    Rational discount;
    std::string note;

    static constexpr auto computedFields =
        morph::forms::computeList(morph::forms::computed<&UELine::total, &UELine::qty, &UELine::price>(
            [](auto const& s) { return s.qty * s.price; }));
    static constexpr std::array<morph::forms::FieldMeta, 4> fieldMetadata{
        morph::forms::FieldMeta{.field = "price", .minimum = Rational{0, DecimalPlaces{0}}},
        morph::forms::FieldMeta{.field = "items", .maximum = Rational{10, DecimalPlaces{0}}},
        morph::forms::FieldMeta{.field = "discount", .maximum = Rational{1, DecimalPlaces{0}}},
        morph::forms::FieldMeta{.field = "note", .label = "Note"}};

    [[nodiscard]] bool validate() const {
        return morph::forms::allRequiredEngaged(*this) && morph::forms::allFieldBoundsSatisfied(*this);
    }
};

/// Counts its executions: an evaluation must not add one.
struct UELineModel {
    static inline std::atomic<int> executions{0};
    UELine execute(UELine const& line) {
        executions.fetch_add(1);
        return line;
    }
};
// NOLINTEND(misc-use-internal-linkage)

template <>
struct morph::model::ModelTraits<UELineModel> {
    static constexpr std::string_view typeId() { return "UE_Line"; }
};
template <>
struct morph::model::ActionTraits<UELine> {
    using Result = UELine;
    static constexpr std::string_view typeId() { return "UE_Price"; }
    static std::string toJson(UELine const& line) { return glz::write_json(line).value_or("{}"); }
    static UELine fromJson(std::string_view json) {
        UELine line{};
        if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(line, json)) {
            throw std::runtime_error{"UE_Price: " + glz::format_error(error, json)};
        }
        return line;
    }
    static std::string resultToJson(UELine const& line) { return toJson(line); }
    static UELine resultFromJson(std::string_view json) { return fromJson(json); }
};

namespace {

constexpr std::string_view kComplete = R"({"qty":{"num":3,"den":1,"dp":2},"price":{"num":5,"den":2,"dp":2},)"
                                       R"("total":{"num":99900,"den":100,"dp":2},"items":4})";
constexpr std::string_view kNoPrice = R"({"qty":{"num":3,"den":1,"dp":2},"items":4})";
constexpr std::string_view kTooMany = R"({"qty":{"num":3,"den":1,"dp":2},"price":{"num":5,"den":2,"dp":2},)"
                                      R"("items":11})";
constexpr std::string_view kOutOfRange = R"({"qty":{"num":2000,"den":1,"dp":2},"price":{"num":5,"den":2,"dp":2}})";
constexpr std::string_view kNegativePrice = R"({"qty":{"num":3,"den":1,"dp":2},"price":{"num":-1,"den":1,"dp":2}})";
constexpr std::string_view kBigDiscount = R"({"qty":{"num":3,"den":1,"dp":2},"price":{"num":5,"den":2,"dp":2},)"
                                          R"("discount":{"num":2,"den":1,"dp":0}})";

UELine lineOf(std::string_view json) { return morph::model::ActionTraits<UELine>::fromJson(json); }

morph::session::Context as(std::string principal) {
    morph::session::Context session;
    session.token = std::move(principal);
    return session;
}

/// A server whose dispatcher knows the line, with the UI service, and a synchronous transport to it.
struct Harness {
    Harness() {
        dispatcher.registerAction<UELineModel, UELine>("UE_Line", "UE_Price");
        auto authz = uiservice::authorizer();
        authz->denied["clerk"] = {{"UE_Line", "UE_Price"}};
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<morph::ui::UiService>(uiservice::bundle(), dispatcher));
        server = std::make_shared<morph::backend::RemoteServer>(pool, authz, config, dispatcher, registry);
    }

    [[nodiscard]] morph::ui::Exchange exchange() {
        return [this](Envelope const& request, std::function<void(morph::ui::ExchangeReply)> const& done) {
            done(morph::wire::decode(server->handleInline(morph::wire::encode(request))));
        };
    }

    [[nodiscard]] Envelope evaluate(std::string_view body, std::string principal = "tech") {
        Envelope request;
        request.kind = std::string{morph::ui::kEvaluateKind};
        request.callId = 7;
        request.session = as(std::move(principal));
        request.modelType = "UE_Line";
        request.actionType = "UE_Price";
        request.body = std::string{body};
        return morph::wire::decode(server->handleInline(morph::wire::encode(request)));
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    std::shared_ptr<morph::backend::RemoteServer> server;
};

}  // namespace

// Mutation: report a field only when every check fails (the first two maps come back empty).
TEST_CASE("fieldErrors reports each failing field by name, and nothing for a valid action", "[forms][field-errors]") {
    UELine missing = lineOf(kNoPrice);
    morph::forms::recomputeAll(missing);
    CHECK(morph::forms::fieldErrors(missing) ==
          std::map<std::string, std::string, std::less<>>{{"price", "required"}});

    UELine tooMany = lineOf(kTooMany);
    morph::forms::recomputeAll(tooMany);
    CHECK(morph::forms::fieldErrors(tooMany) ==
          std::map<std::string, std::string, std::less<>>{{"items", "out of bounds"}});

    UELine complete = lineOf(kComplete);
    morph::forms::recomputeAll(complete);
    CHECK(morph::forms::fieldErrors(complete).empty());

    using Errors = std::map<std::string, std::string, std::less<>>;
    CHECK(morph::forms::fieldErrors(lineOf(kOutOfRange)) == Errors{{"qty", "out of range"}});
    CHECK(morph::forms::fieldErrors(lineOf(kNegativePrice)) == Errors{{"price", "out of bounds"}});
    CHECK(morph::forms::fieldErrors(lineOf(kBigDiscount)) == Errors{{"discount", "out of bounds"}});
}

// Mutation: skip recomputeAll in the evaluator (the wire's 999.00 total comes back).
TEST_CASE("ActionDispatcher::evaluate recomputes, reports and never executes", "[registry][ui-evaluate]") {
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<UELineModel, UELine>("UE_Line", "UE_Price");
    int const before = UELineModel::executions.load();

    auto const complete = dispatcher.evaluate("UE_Line", "UE_Price", kComplete);
    CHECK(complete.ready);
    CHECK(complete.errors.empty());
    UELine const recomputed = lineOf(complete.body);
    REQUIRE(recomputed.total.hasValue());
    CHECK(*recomputed.total == Rational{Numerator{15}, Denominator{2}, DecimalPlaces{2}});  // 3 * 2.50

    auto const missing = dispatcher.evaluate("UE_Line", "UE_Price", kNoPrice);
    CHECK_FALSE(missing.ready);
    CHECK(missing.errors.size() == 1);
    CHECK(missing.errors.contains("price"));

    auto const tooMany = dispatcher.evaluate("UE_Line", "UE_Price", kTooMany);
    CHECK_FALSE(tooMany.ready);
    CHECK(tooMany.errors.contains("items"));

    // Outside the unit's bounds: `dispatch` refuses it before the validator runs, so it is not ready either.
    auto const outOfRange = dispatcher.evaluate("UE_Line", "UE_Price", kOutOfRange);
    CHECK_FALSE(outOfRange.ready);
    CHECK(outOfRange.errors.contains("qty"));

    CHECK(UELineModel::executions.load() == before);
    CHECK_THROWS_AS(dispatcher.evaluate("UE_Line", "UE_Other", kComplete), std::runtime_error);
    CHECK_THROWS_AS(dispatcher.evaluate("UE_Line", "UE_Price", "not json"), std::runtime_error);
}

// Mutation: drop the action gate from UiService::evaluate (the clerk gets an ok).
TEST_CASE("ui-evaluate is answered through a RemoteServer, gated as the action it evaluates",
          "[ui-service][ui-evaluate]") {
    Harness harness;
    int const before = UELineModel::executions.load();

    Envelope const answered = harness.evaluate(kNoPrice);
    REQUIRE(answered.kind == "ok");
    CHECK(answered.callId == 7);
    auto const reply = morph::ui::parseEvaluateReply(answered.body);
    REQUIRE(reply.has_value());
    CHECK_FALSE(reply->ready);
    CHECK(reply->errors.at("price") == "required");
    CHECK(reply->body.is_object());

    Envelope const refused = harness.evaluate(kComplete, "clerk");
    CHECK(refused.kind == "err");
    CHECK(refused.message == "unauthorized");

    Envelope const malformed = harness.evaluate("not json");
    CHECK(malformed.kind == "err");

    CHECK(UELineModel::executions.load() == before);
}

// Mutation: leave `errors` out of evaluationValue (the query value has no `errors.price`).
TEST_CASE("evaluatorOver resolves with the body's members, its field errors and its verdict",
          "[ui-service][ui-evaluate]") {
    Harness harness;
    morph::exec::MainThreadExecutor owner;
    auto const evaluator = morph::ui::evaluatorOver(harness.exchange(), as("tech"), &owner);

    std::optional<std::string> value;
    evaluator("UE_Line", "UE_Price", std::string{kNoPrice}).then([&](std::string const& json) { value = json; });
    owner.drain();
    REQUIRE(value.has_value());
    glz::generic_u64 tree{};
    REQUIRE_FALSE(glz::read_json(tree, *value));
    CHECK(tree["errors"]["price"].get<std::string>() == "required");
    CHECK_FALSE(tree["ready"].get<bool>());
    CHECK(tree["qty"].is_object());

    std::optional<std::string> refusal;
    morph::ui::evaluatorOver(harness.exchange(), as("clerk"), &owner)("UE_Line", "UE_Price", std::string{kComplete})
        .onError([&](std::exception_ptr const& error) {
            try {
                std::rethrow_exception(error);
            } catch (std::exception const& thrown) {
                refusal = thrown.what();
            }
        });
    owner.drain();
    CHECK(refusal == "unauthorized");
}

// Mutation: let evaluationValue read a body from a refused reply (the transport's or server's reason is lost).
TEST_CASE("evaluate: a reply that did not arrive, was refused or is not an evaluation is why there is no value",
          "[ui][evaluate]") {
    using morph::ui::evaluationValue;
    CHECK(evaluationValue(std::unexpected(std::string{"no route"})).error() == "no route");
    morph::wire::Envelope refused;
    refused.kind = "err";
    refused.message = "denied";
    CHECK(evaluationValue(refused).error() == "denied");
    morph::wire::Envelope garbled;
    garbled.kind = "ok";
    garbled.body = "{not json";
    auto const notEvaluation = evaluationValue(garbled);
    REQUIRE_FALSE(notEvaluation.has_value());
    CHECK_FALSE(notEvaluation.error().empty());
}
