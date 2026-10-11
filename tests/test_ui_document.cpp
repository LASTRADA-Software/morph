// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <expected>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace ui = morph::ui;
using Form = ui::Expr::Form;

namespace {

[[nodiscard]] ui::Json json(std::string_view text) {
    ui::Json out{};
    auto const error = glz::read_json(out, text);
    INFO(glz::format_error(error, text));
    REQUIRE_FALSE(error);
    return out;
}

[[nodiscard]] ui::Document parsed(std::string_view text) {
    std::expected<ui::Document, ui::Problem> document = ui::parseDocument(json(text));
    if (!document) {
        INFO(document.error().path << ": " << document.error().reason);
        REQUIRE(document.has_value());
    }
    return *document;
}

[[nodiscard]] ui::Problem refused(std::string_view text) {
    std::expected<ui::Document, ui::Problem> document = ui::parseDocument(json(text));
    INFO(text);
    REQUIRE_FALSE(document.has_value());
    return document.error();
}

// A screen that declares something of every kind the parser reads.
constexpr std::string_view kResults = R"({
  "vocab": "ui/1", "id": "lims.results",
  "title": {"t": "lims.results.title", "fallback": "Results"},
  "params": {"sampleId": "int", "note": "string?"},
  "identity": {"ref": "sampleId"}, "dirty": {"ref": "unsaved"}, "showErrors": "submit",
  "onMount": [{"refetch": "results"}], "onClose": [{"if": [{"ref": "unsaved"}, [{"open": "confirm"}, {"cancelClose": true}], []]}],
  "models": {"sample": {"type": "Lims_SampleModel", "instance": {"ref": "sampleId"}},
             "catalog": {"type": "Lims_CatalogModel", "instance": "shared"},
             "scratch": {"type": "Lims_ScratchModel", "instance": "private"},
             "clients": {"type": "Lims_ClientModel"}},
  "state": {"unsaved": {"init": false}, "filter": {"type": "string?", "init": null}},
  "let": {"open": {"fn": "count", "args": [{"query": "results"}]}},
  "queries": {"results": {"model": "sample", "action": "Lims_ListResults",
                          "body": {"obj": {"sampleId": {"ref": "sampleId"}}},
                          "when": true, "debounce": 300, "refreshOn": ["Lims_Capture"], "refreshEvery": 5000,
                          "staleTime": 1000},
              "calc": {"model": "sample", "evaluate": "Lims_Capture"}},
  "mutations": {"capture": {"model": "sample", "action": "Lims_Capture", "body": {"obj": {"id": 1}},
                            "concurrency": "serial", "serialKey": {"ref": "sampleId"}, "invalidates": ["results"],
                            "patch": {"query": "results", "key": {"ref": "result.id"}, "with": {"ref": "result"}},
                            "optimistic": {"query": "results", "key": {"ref": "body.id"}, "with": {"ref": "body"}},
                            "onSuccess": [{"notify": "Saved", "role": "success"}], "onError": [],
                            "offline": "queue"}},
  "watch": [{"on": {"ref": "filter"}, "source": "user", "do": [{"set": "unsaved", "value": true}]}],
  "checks": [{"id": "positive", "assert": {"fn": "gt", "args": [{"ref": "sampleId"}, 0]},
              "message": "Must be positive", "targets": ["filterInput"], "blocking": false}],
  "prefetch": ["results"],
  "components": {"Chart": {"qml": "sha256:4b1e", "props": {"series": "json"}, "events": {"pointSelected": "key"},
                           "fallback": {"kind": "text", "text": "No chart"}}},
  "customize": {"density": {"kind": "choice", "options": ["comfortable", "compact"], "default": "comfortable"},
                "limit": {"kind": "value", "type": "int?", "default": 10}},
  "view": {"kind": "column", "gap": 8, "children": [
    {"kind": "textInput", "id": "filterInput", "value": {"ref": "filter"}, "commitOn": "change",
     "onChange": [{"set": "filter", "value": {"ref": "event"}}], "a11y": {"name": "Filter"}},
    {"kind": "table", "rows": {"query": "results"}, "key": {"row": "id"},
     "scope": {"state": {"draft": {"init": {"row": "inputs"}}}},
     "columns": [{"label": "Mass", "cell": {"kind": "text", "text": {"row": "mass"}}},
                 {"label": "Edit", "cell": [{"kind": "button", "label": "Edit",
                                             "onClick": {"navigate": "lims.result", "params": {"obj": {"id": {"row": "id"}}},
                                                         "as": "dialog", "onResult": [{"refetch": "results"}]}}]}],
     "onActivate": [{"seq": [{"run": "capture", "as": "c"}, {"close": "self"}]}]},
    {"kind": "boundary", "loading": [{"kind": "busy"}], "error": [{"kind": "button", "onClick": {"retry": true}}],
     "children": [{"kind": "custom", "name": "Chart", "props": {"series": {"query": "results"}},
                   "on": {"pointSelected": [{"set": "filter", "value": {"ref": "event"}}]}}]},
    {"kind": "menu", "label": "More", "items": [{"label": "Export", "items": [{"label": "CSV", "onClick": [{"host": "export", "args": {"lit": {"format": "csv"}}}]}]}]},
    {"kind": "switch", "value": {"ref": "filter"}, "cases": [{"when": "a", "children": [{"kind": "spacer"}]}],
     "default": [{"kind": "text", "text": "other"}]},
    {"kind": "form", "model": "sample", "action": "Lims_Capture", "fields": {"mass": {"decimals": 3}}},
    {"kind": "dialog", "id": "confirm", "scope": {"state": {"choice": {"init": 0}}},
     "children": [{"kind": "button", "label": "Leave", "onClick": [{"navigate": {"route": "lims/home"}},
                                                                   {"signIn": {"from": "capture"}}, {"signOut": true},
                                                                   {"upload": {"ref": "event"}, "to": "capture", "id": "t1"}]}]}
  ]}
})";

}  // namespace

// Mutations: drop the Keyed instance (read every expression as shared); read `!visible` as unknown.
TEST_CASE("ui::parseDocument: a screen's declarations are read whole", "[ui][document]") {
    ui::Document const document = parsed(kResults);
    CHECK(document.vocab == "ui/1");
    CHECK(document.id == "lims.results");
    REQUIRE(document.params.size() == 2);
    CHECK(document.params[1].first == "note");
    CHECK(document.params[1].second.text() == "string?");
    REQUIRE(document.title.has_value());
    CHECK(document.title->form == Form::Translate);
    CHECK(document.identity->path == "/identity");
    CHECK(document.showErrors == "submit");
    REQUIRE(document.lifecycle.size() == 2);
    CHECK(document.lifecycle[1].name == "onClose");
    ui::Command const& guard = document.lifecycle[1].commands.at(0);
    CHECK(guard.verb == "if");
    CHECK(guard.list("then")->size() == 2);
    CHECK(guard.list("then")->at(1).verb == "cancelClose");
    CHECK(guard.list("else")->empty());

    ui::Scope const& scope = document.scope;
    REQUIRE(scope.models.size() == 4);
    CHECK(scope.models[0].instance == ui::ModelDecl::Instance::Keyed);
    CHECK(scope.models[0].key->path == "/models/sample/instance");
    CHECK(scope.models[1].instance == ui::ModelDecl::Instance::Shared);
    CHECK(scope.models[2].instance == ui::ModelDecl::Instance::Private);
    CHECK(scope.models[3].instance == ui::ModelDecl::Instance::Default);
    REQUIRE(scope.state.size() == 2);
    CHECK(scope.state[1].type->text() == "string?");
    CHECK(scope.lets.at(0).value.name == "count");
    ui::QueryDecl const& results = scope.queries.at(0);
    CHECK(results.action == "Lims_ListResults");
    CHECK(results.debounce == 300);
    CHECK(results.refreshEvery == 5000);
    CHECK(results.staleTime == 1000);
    CHECK(results.refreshOn == std::vector<std::string>{"Lims_Capture"});
    CHECK(scope.queries.at(1).evaluate);
    CHECK(scope.queries.at(1).body.form == Form::Object);
    ui::MutationDecl const& capture = scope.mutations.at(0);
    CHECK(capture.concurrency == ui::MutationDecl::Concurrency::Serial);
    CHECK(capture.invalidates == std::vector<std::string>{"results"});
    CHECK(capture.patch->query == "results");
    CHECK(capture.optimistic->with.name == "body");
    CHECK(capture.onSuccess.at(0).verb == "notify");
    CHECK(*capture.onSuccess.at(0).text("role") == "success");
    CHECK(capture.offlineQueue);
    CHECK(scope.watches.at(0).userOnly);
    CHECK(scope.watches.at(0).commands.at(0).target == "unsaved");
    CHECK_FALSE(scope.checks.at(0).blocking);
    CHECK(scope.checks.at(0).targets == std::vector<std::string>{"filterInput"});
    CHECK(document.prefetch == std::vector<std::string>{"results"});
    CHECK(document.components.at(0).qml == "sha256:4b1e");
    CHECK(document.components.at(0).fallback.at(0).kind == "text");
    CHECK(document.customize.at(0).options.size() == 2);
    CHECK(document.customize.at(1).defaultValue == ui::Value{std::int64_t{10}});
}

// Mutations: put a custom node's `on` events under their bare names; drop a single node's wrapping into a list.
TEST_CASE("ui::parseDocument: the view's nodes, entries, scopes and commands", "[ui][document]") {
    ui::Document const document = parsed(kResults);
    REQUIRE(document.view.has_value());
    std::vector<ui::Node> const& children = *document.view->slot("children");
    REQUIRE(children.size() == 7);
    CHECK(document.view->prop("gap") != nullptr);

    ui::Node const& input = children[0];
    CHECK(*input.text("id") == "filterInput");
    CHECK(*input.text("commitOn") == "change");
    CHECK(input.event("onChange")->at(0).expr("value")->form == Form::Event);
    CHECK(input.maps.at(0).first == "a11y");

    ui::Node const& table = children[1];
    REQUIRE(table.scope != nullptr);
    CHECK(table.scope->state.at(0).name == "draft");
    CHECK(table.scope->path == "/view/children/1/scope");
    std::vector<ui::Node> const& columns = *table.slot("columns");
    REQUIRE(columns.size() == 2);
    CHECK(columns[0].kind == "column");
    CHECK(columns[0].slot("cell")->at(0).kind == "text");
    ui::Command const& edit = columns[1].slot("cell")->at(0).event("onClick")->at(0);
    CHECK(edit.verb == "navigate");
    CHECK(edit.target == "lims.result");
    CHECK(*edit.text("as") == "dialog");
    CHECK(edit.list("onResult")->at(0).verb == "refetch");
    ui::Command const& sequence = table.event("onActivate")->at(0);
    CHECK(sequence.list("seq")->at(0).target == "capture");
    CHECK(*sequence.list("seq")->at(0).text("as") == "c");

    ui::Node const& boundary = children[2];
    CHECK(boundary.slot("error")->at(0).event("onClick")->at(0).verb == "retry");
    ui::Node const& custom = boundary.slot("children")->at(0);
    CHECK(*custom.text("name") == "Chart");
    CHECK(custom.maps.at(0).second.at(0).name == "series");
    CHECK(custom.event("on.pointSelected") != nullptr);

    ui::Node const& menu = children[3];
    CHECK(menu.slot("items")->at(0).slot("items")->at(0).event("onClick")->at(0).expr("args") != nullptr);
    CHECK(children[4].slot("cases")->at(0).prop("when") != nullptr);
    CHECK(children[5].raw.at(0).first == "fields");
    ui::Node const& dialog = children[6];
    CHECK(dialog.scope->state.at(0).name == "choice");
    std::vector<ui::Command> const& leave = *dialog.slot("children")->at(0).event("onClick");
    CHECK(leave.at(0).expr("route") != nullptr);
    CHECK(leave.at(1).target == "capture");
    CHECK(leave.at(2).verb == "signOut");
    CHECK(*leave.at(3).text("to") == "capture");
}

// Mutations: ignore `!` keys in `members`; leave a node with an unknown `!` key unrefused; accept an unknown kind.
TEST_CASE("ui::parseDocument: unknown keys are ignored, and unknown must-understand keys refuse", "[ui][document]") {
    std::string_view const known = R"({"vocab": "ui/1", "id": "s", "futureKey": 1,
        "view": {"kind": "text", "!visible": false, "futureProp": 2, "text": "a"}})";
    ui::Document const document = parsed(known);
    CHECK(document.view->prop("visible") != nullptr);
    CHECK(document.view->refused.empty());
    CHECK(document.view->prop("futureProp") == nullptr);

    ui::Document const node = parsed(R"({"vocab": "ui/1", "id": "s",
        "view": {"kind": "column", "children": [{"kind": "text", "!glow": true}]}})");
    CHECK(node.view->slot("children")->at(0).refused == "`!glow` must be understood, and `text` has no such key");
    CHECK(node.view->refused.empty());

    struct Row {
        std::string_view text;
        std::string_view path;
        std::string_view reason;
    };
    for (
        Row const& row : {
            Row{R"({"vocab": "ui/1", "id": "s", "!requires": ["x"], "view": {"kind": "spacer"}})", "/!requires",
                "`!requires` must be understood, and a screen document has no such key"},
            Row{R"({"id": "s", "queries": {"q": {"model": "m", "action": "a", "!cache": 1}}, "view": {"kind": "spacer"}})",
                "/queries/q/!cache", "`!cache` must be understood, and a query has no such key"},
            Row{R"({"id": "s", "view": {"kind": "button", "onClick": [{"run": "m", "!atomic": true}]}})",
                "/view/onClick/0/!atomic", "`!atomic` must be understood, and `run` has no such operand"},
            Row{R"({"id": "s", "view": {"kind": "carousel"}})", "/view/kind", "`carousel` is not a node kind of ui/1"},
            Row{R"({"id": "s", "view": {"kind": "column", "children": [{"kind": "menuItem"}]}})",
                "/view/children/0/kind", "`menuItem` is not a node kind of ui/1"},
        }) {
        INFO(row.text);
        ui::Problem const problem = refused(row.text);
        CHECK(problem.path == row.path);
        CHECK(problem.reason == row.reason);
    }
}

// Mutation: skip the name check in `declarations`.
TEST_CASE("ui::parseDocument: a malformed declaration, node or command is refused with its path", "[ui][document]") {
    struct Row {
        std::string_view text;
        std::string_view path;
        std::string_view reason;
    };
    for (
        Row const& row : {
            Row{R"({"id": "s"})", "", "a screen document has a `view`"},
            Row{R"({"id": "s", "state": {"2x": {}}, "view": {"kind": "spacer"}})", "/state/2x",
                "`2x` is not a name: a name is [A-Za-z_][A-Za-z0-9_]*"},
            Row{R"({"id": "s", "models": {"m": {}}, "view": {"kind": "spacer"}})", "/models/m",
                "a model names its `type`"},
            Row{R"({"id": "s", "queries": {"q": {"model": "m", "action": "a", "evaluate": "a"}}, "view": {"kind": "spacer"}})",
                "/queries/q", "a query names its `model` and one of `action` or `evaluate`"},
            Row{R"({"id": "s", "mutations": {"m": {"model": "m", "action": "a", "concurrency": "eager"}}, "view": {"kind": "spacer"}})",
                "/mutations/m/concurrency", "`concurrency` is `exclusive`, `serial`, `latest` or `parallel`"},
            Row{R"({"id": "s", "queries": {"q": {"model": "m", "action": "a", "debounce": -1}}, "view": {"kind": "spacer"}})",
                "/queries/q/debounce", "`debounce` is a non-negative int"},
            Row{R"({"id": "s", "state": {"x": {"type": "money"}}, "view": {"kind": "spacer"}})", "/state/x/type",
                "unknown type `money`"},
            Row{R"({"id": "s", "params": {"p": "list<int"}, "view": {"kind": "spacer"}})", "/params/p",
                "`list<T>` must end with `>`"},
            Row{R"({"id": "s", "watch": [{"on": 1}], "view": {"kind": "spacer"}})", "/watch/0",
                R"(a watch is {"on": e, "do": [...]})"},
            Row{R"({"id": "s", "view": {"text": "no kind"}})", "/view", "a node names its `kind`"},
            Row{R"({"id": "s", "view": {"kind": "text", "text": {"fn": "nope"}}})", "/view/text/fn",
                "`nope` is not a function of expr/1"},
            Row{R"({"id": "s", "view": {"kind": "button", "onClick": [{"launch": "x"}]}})", "/view/onClick/0",
                "`launch` is not a command of ui/1"},
            Row{R"({"id": "s", "view": {"kind": "button", "onClick": [{"set": "x"}]}})", "/view/onClick/0",
                "`set` needs `value`"},
            Row{R"({"id": "s", "view": {"kind": "button", "onClick": [{"cancelClose": 1}]}})",
                "/view/onClick/0/cancelClose", "`cancelClose` is written `true`"},
            Row{R"({"id": "s", "view": {"kind": "button", "onClick": [{"if": [true, []]}]}})", "/view/onClick/0/if",
                "`if` takes [condition, [then...], [else...]]"},
            Row{R"({"id": "s", "view": {"kind": "button", "onClick": "go"}})", "/view/onClick",
                "a command list is a list of commands"},
            Row{R"({"id": "s", "view": {"kind": "table", "columns": [{"label": "x", "cell": 3}]}})",
                "/view/columns/0/cell", "a node list is a list of nodes"},
            Row{R"({"id": "s", "components": {"C": {"props": {}}}, "view": {"kind": "spacer"}})", "/components/C",
                "a component names one of `qml` or `host`"},
            Row{R"({"id": "s", "customize": {"p": {"kind": "pinned"}}, "view": {"kind": "spacer"}})",
                "/customize/p/kind",
                "a point's `kind` is `columns`, `choice`, `value`, `layout`, `saved`, `pins` or `arrange`"},
            Row{R"({"id": "s", "customize": {"p": {"kind": "value", "type": "int", "default": "x"}}, "view": {"kind": "spacer"}})",
                "/customize/p/default", "not an int"},
        }) {
        INFO(row.text);
        ui::Problem const problem = refused(row.text);
        CHECK(problem.path == row.path);
        CHECK(problem.reason == row.reason);
    }
}
