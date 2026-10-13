// SPDX-License-Identifier: Apache-2.0
//
// collectionView on RecordingBackend: what src/qt/forms/tests/tst_collectionview.qml, tst_ViewChrome.qml and the
// collection half of tst_LargeIdPrecision.qml assert about a drawn collection (cells and buttons by test id, the
// confirm and editor dialogs' visibility, the chrome slots, the master-detail layout), re-expressed on mounted
// widgets. The session's own claims are test_forms_engine_collection.cpp's.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <morph/forms/engine/collection.hpp>
#include <morph/forms/engine/collection_view.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/overrides.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::CollectionModel;
using morph::forms::CollectionRow;
using morph::forms::CollectionSession;
using morph::forms::CollectionViewOptions;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;
using morph::ui::testing::RecordingBackend;
namespace ui = morph::ui;

// The view and schemas of tst_collectionview.qml and tst_ViewChrome.qml.
constexpr std::string_view kView = R"({
  "v-kind": "collection", "v-title": "Rows", "v-query": "ListRows", "v-rowKey": "id",
  "v-columns": [
    {"field": "id", "label": "ID", "kind": "integer", "v-hidden": true},
    {"field": "name", "label": "Name", "kind": "text"},
    {"field": "amount", "label": "Amount", "kind": "quantity", "x-decimalPlaces": 1,
     "ExtUnits": {"unitAscii": "kg", "unitUnicode": "kg"}}],
  "v-rowAction": {"action": "EditRow", "bind": {"id": "id"}},
  "v-actions": [
    {"action": "DeleteRow", "label": "Delete", "scope": "row", "bind": {"id": "id"}, "confirm": true},
    {"action": "CreateRow", "label": "New", "scope": "collection"}]})";

constexpr std::string_view kEditRow =
    R"({"properties":{"id":{"type":"integer","x-order":0},"name":{"type":"string","x-order":1}},"required":["id","name"]})";

constexpr std::string_view kRows = R"({"rows":[{"id":1,"name":"First","amount":{"num":15,"den":10,"dp":1}},)"
                                   R"({"id":2,"name":"Second","amount":{"num":30,"den":10,"dp":1}}]})";

[[nodiscard]] std::optional<std::string> rowsServer(Call const& call) {
    if (call.action == "ListRows") {
        return std::string{kRows};
    }
    return std::string{R"({"ok":true})"};
}

[[nodiscard]] std::string masterDetail() {
    std::string view{kView};
    view.replace(view.find(R"("collection")"), std::string_view{R"("collection")"}.size(), R"("master-detail")");
    return view;
}

/// A mounted collection: the session first, the mount after it, so the mount goes first.
struct Mounted {
    Harness harness;
    RecordingBackend backend;
    std::unique_ptr<CollectionSession> session;
    std::unique_ptr<ui::Mounted> view;

    explicit Mounted(CollectionViewOptions options = {}, std::string_view json = kView,
                     morph::test::formsengine::FakeServer::Responder responder = rowsServer) {
        harness.server.replyWith(std::move(responder));
        auto model = CollectionModel::fromSchema(json);
        REQUIRE(model.has_value());
        session = std::make_unique<CollectionSession>(
            harness.rt, *std::move(model),
            [](std::string_view action) -> std::optional<std::string> {
                return action == "EditRow" ? std::optional<std::string>{std::string{kEditRow}} : std::nullopt;
            },
            harness.server.submitter(), harness.server.fetcher());
        view = std::make_unique<ui::Mounted>(harness.rt, backend,
                                             morph::forms::collectionView(*session, std::move(options)));
        harness.settle();
    }

    ~Mounted() { view.reset(); }
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    [[nodiscard]] int widget(std::string_view kind, std::string_view testId) const {
        auto const found = backend.find(kind, "testId", testId);
        INFO("no " << kind << " with testId " << testId << "\n" << backend.dump());
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] bool has(std::string_view kind, std::string_view testId) const {
        return backend.find(kind, "testId", testId).has_value();
    }

    [[nodiscard]] std::string text(std::string_view kind, std::string_view testId) const {
        return backend.prop(widget(kind, testId), kind == "TextInput" || kind == "Text" ? "text" : "label");
    }

    void click(std::string_view testId) {
        backend.click(widget("Button", testId));
        harness.settle();
    }

    void edit(std::string_view field, std::string text) {
        backend.edit(widget("TextInput", "field_" + std::string{field}), std::move(text));
        harness.settle();
    }

    [[nodiscard]] std::size_t count(std::string_view action) const {
        std::size_t total = 0;
        for (Call const& call : harness.server.submits()) {
            total += call.action == action ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] std::optional<std::string> lastBody(std::string_view action) const {
        std::optional<std::string> body;
        for (Call const& call : harness.server.submits()) {
            if (call.action == action) {
                body = call.body;
            }
        }
        return body;
    }

    /// Whether @p widgetId lies inside @p rootId.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the names say which is which.
    [[nodiscard]] bool within(int widgetId, int rootId) const {
        std::vector<int> pending{rootId};
        while (!pending.empty()) {
            int const current = pending.back();
            pending.pop_back();
            if (current == widgetId) {
                return true;
            }
            std::ranges::copy(backend.children(current), std::back_inserter(pending));
        }
        return false;
    }
};

}  // namespace

TEST_CASE("collectionView: the table shows the visible columns' exact cells and each row's buttons",
          "[forms-engine][collection-view]") {
    Mounted const mounted;
    CHECK(mounted.text("Text", "collection_title") == "Rows");
    int const table = mounted.widget("Table", "collection_rows");
    // The hidden id is no column.
    CHECK(mounted.backend.prop(table, "columns") == "[Name:content,Amount:content,:content]");
    CHECK(mounted.text("Text", "cell_amount_1") == "1.5 kg");
    CHECK(mounted.text("Text", "cell_name_2") == "Second");
    CHECK_FALSE(mounted.has("Text", "cell_id_1"));
    CHECK(mounted.text("Button", "rowOpen_1") == "Open");
    CHECK(mounted.text("Button", "rowAction_DeleteRow_2") == "Delete");
    CHECK(mounted.text("Button", "collectionAction_CreateRow") == "New");
    CHECK_FALSE(mounted.has("Button", "collectionAction_DeleteRow"));  // a row action is not in the header
    CHECK(mounted.backend.prop(mounted.widget("Text", "collection_error"), "visible") == "false");
}

TEST_CASE("collectionView: Open shows the editor dialog prefilled from bind; an edit submits, closes and reloads",
          "[forms-engine][collection-view]") {
    Mounted mounted;
    int const dialog = mounted.widget("Dialog", "editorDialog");
    CHECK(mounted.backend.prop(dialog, "open") == "false");
    CHECK(mounted.backend.prop(dialog, "title") == "EditRow");
    CHECK_FALSE(mounted.has("TextInput", "field_id"));  // a closed dialog holds no content
    mounted.click("rowOpen_1");
    CHECK(mounted.backend.prop(dialog, "open") == "true");
    CHECK(mounted.text("TextInput", "field_id") == "1");
    CHECK(mounted.text("TextInput", "field_name").empty());  // name is not bound
    CHECK(mounted.count("EditRow") == 0);                    // opening alone submits nothing
    mounted.edit("name", "Renamed");
    CHECK(mounted.lastBody("EditRow") == R"({"id":1,"name":"Renamed"})");
    CHECK(mounted.backend.prop(dialog, "open") == "false");
    CHECK(mounted.count("ListRows") == 2);
}

TEST_CASE("collectionView: dismissing the editor closes it; another row opens without the typed values",
          "[forms-engine][collection-view]") {
    Mounted mounted;
    int const dialog = mounted.widget("Dialog", "editorDialog");
    mounted.click("rowOpen_1");
    mounted.backend.dismiss(dialog);
    mounted.harness.settle();
    CHECK(mounted.backend.prop(dialog, "open") == "false");
    CHECK_FALSE(mounted.session->openRow().has_value());
    mounted.click("rowOpen_2");
    CHECK(mounted.text("TextInput", "field_id") == "2");
    CHECK(mounted.text("TextInput", "field_name").empty());
}

TEST_CASE("collectionView: a confirm action shows the dialog, and only accepting it sends",
          "[forms-engine][collection-view]") {
    Mounted mounted;
    int const dialog = mounted.widget("Dialog", "confirmDialog");
    CHECK(mounted.backend.prop(dialog, "open") == "false");
    mounted.click("rowAction_DeleteRow_1");
    CHECK(mounted.backend.prop(dialog, "open") == "true");
    CHECK(mounted.text("Text", "confirm_message") == "Are you sure?");
    CHECK(mounted.count("DeleteRow") == 0);
    mounted.click("confirm_cancel");
    CHECK(mounted.backend.prop(dialog, "open") == "false");
    CHECK(mounted.count("DeleteRow") == 0);

    mounted.click("rowAction_DeleteRow_2");
    mounted.click("confirm_accept");
    CHECK(mounted.lastBody("DeleteRow") == R"({"id":2})");
    CHECK(mounted.backend.prop(dialog, "open") == "false");
    CHECK(mounted.count("ListRows") == 2);  // reloaded after the delete resolved
}

TEST_CASE("collectionView: a collection action sends an empty body", "[forms-engine][collection-view]") {
    Mounted mounted;
    mounted.click("collectionAction_CreateRow");
    CHECK(mounted.lastBody("CreateRow") == "{}");
}

TEST_CASE("collectionView: a failed list shows its error", "[forms-engine][collection-view]") {
    Mounted mounted{{}, kView, [](Call const& call) -> std::optional<std::string> {
                        if (call.action == "ListRows") {
                            return std::nullopt;
                        }
                        return std::string{"{}"};
                    }};
    mounted.harness.server.rejectSubmit(0, "list refused");
    mounted.harness.settle();
    int const error = mounted.widget("Text", "collection_error");
    CHECK(mounted.backend.prop(error, "visible") == "true");
    CHECK(mounted.backend.prop(error, "text") == "error: list refused");
}

TEST_CASE("collectionView: a row key past 2^53 addresses its cells and buttons exactly",
          "[forms-engine][collection-view]") {
    Mounted mounted{{}, kView, [](Call const& call) -> std::optional<std::string> {
                        if (call.action == "ListRows") {
                            return std::string{R"({"rows":[{"id":9007199254740993,"name":"Big"}]})"};
                        }
                        return std::string{R"({"ok":true})"};
                    }};
    CHECK(mounted.text("Text", "cell_name_9007199254740993") == "Big");
    mounted.click("rowOpen_9007199254740993");
    CHECK(mounted.text("TextInput", "field_id") == "9007199254740993");
    mounted.click("rowAction_DeleteRow_9007199254740993");
    mounted.click("confirm_accept");
    CHECK(mounted.lastBody("DeleteRow") == R"({"id":9007199254740993})");
}

TEST_CASE("collectionView: master-detail shows the editor beside the table, opened by selecting a row",
          "[forms-engine][collection-view]") {
    Mounted mounted{{}, masterDetail()};
    CHECK_FALSE(mounted.has("Dialog", "editorDialog"));
    int const detail = mounted.widget("Column", "detailForm");
    int const table = mounted.widget("Table", "collection_rows");
    CHECK(mounted.backend.prop(detail, "visible") == "false");
    CHECK(mounted.backend.prop(table, "selectionMode") == "Single");
    mounted.backend.selectRows(table, {ui::Key{std::string{"2"}}});
    mounted.harness.settle();
    CHECK(mounted.backend.prop(detail, "visible") == "true");
    CHECK(mounted.text("TextInput", "field_id") == "2");
    CHECK(mounted.backend.prop(table, "selection") == R"(["2"])");
    mounted.click("rowOpen_1");  // Open selects the row it opens
    CHECK(mounted.backend.prop(table, "selection") == R"(["1"])");
    mounted.edit("name", "Renamed");
    CHECK(mounted.lastBody("EditRow") == R"({"id":1,"name":"Renamed"})");
    CHECK(mounted.backend.prop(detail, "visible") == "false");  // a success closes the detail
    CHECK(mounted.backend.prop(table, "selection") == "[]");
}

TEST_CASE("collectionView: the header and row chrome replace the built-ins and drive the session",
          "[forms-engine][collection-view][chrome]") {
    CollectionViewOptions options{};
    options.chrome.header = [](CollectionSession& session) {
        std::string summary{session.model().title()};
        for (auto const& action : session.model().actions()) {
            if (action.scope == morph::forms::CollectionActionScope::Collection) {
                summary += "|" + action.action;
            }
        }
        ui::Text text{.text = summary};
        text.common.testId = "headerChrome";
        CollectionSession* const view = &session;
        ui::Button create{.label = "create", .onClick = [view] { view->fire(1); }};
        create.common.testId = "headerChrome_fire";
        return ui::row(ui::Row{.children = {ui::text(std::move(text)), ui::button(std::move(create))}});
    };
    options.chrome.row = [](CollectionSession& session, morph::reactive::Signal<CollectionRow> const& slot) {
        morph::reactive::Signal<CollectionRow> const* const row = &slot;
        std::string const key = slot.peek().key;
        ui::Text cells{.text = [row] {
            std::string joined;
            for (std::string const& cell : row->get().cells) {
                joined += (joined.empty() ? "" : ",") + cell;
            }
            return joined;
        }};
        cells.common.testId = "rowChrome_" + key;
        CollectionSession* const view = &session;
        ui::Button open{.label = "open", .onClick = [view, key] { view->open(key); }};
        open.common.testId = "rowChrome_open_" + key;
        ui::Button remove{.label = "remove", .onClick = [view, key] { view->fire(0, key); }};
        remove.common.testId = "rowChrome_fire_" + key;
        return ui::row(ui::Row{
            .children = {ui::text(std::move(cells)), ui::button(std::move(open)), ui::button(std::move(remove))}});
    };
    Mounted mounted{std::move(options)};
    CHECK(mounted.text("Text", "headerChrome") == "Rows|CreateRow");
    CHECK(mounted.text("Text", "rowChrome_1") == "First,1.5 kg");
    // The built-ins are gone: no title, no header buttons, no table, no cells, no Open buttons.
    CHECK_FALSE(mounted.has("Text", "collection_title"));
    CHECK_FALSE(mounted.has("Button", "collectionAction_CreateRow"));
    CHECK_FALSE(mounted.has("Table", "collection_rows"));
    CHECK_FALSE(mounted.has("Text", "cell_amount_1"));
    CHECK_FALSE(mounted.has("Button", "rowOpen_1"));
    mounted.click("headerChrome_fire");
    CHECK(mounted.lastBody("CreateRow") == "{}");
    mounted.click("rowChrome_open_2");
    CHECK(mounted.backend.prop(mounted.widget("Dialog", "editorDialog"), "open") == "true");
    CHECK(mounted.text("TextInput", "field_id") == "2");
}

TEST_CASE("collectionView: the confirm chrome is mounted only while an action waits, and holds it until accepted",
          "[forms-engine][collection-view][chrome]") {
    CollectionViewOptions options{};
    options.chrome.confirm = [](CollectionSession& session) {
        CollectionSession* const view = &session;
        ui::Text what{.text = [view] {
            auto const& pending = view->pendingConfirm();
            return pending.has_value() ? view->model().actions()[pending->action].action + " " + pending->rowKey
                                       : std::string{};
        }};
        what.common.testId = "confirmChrome";
        ui::Button accept{.label = "yes", .onClick = [view] { view->confirm(); }};
        accept.common.testId = "confirmChrome_accept";
        ui::Button reject{.label = "no", .onClick = [view] { view->cancelConfirm(); }};
        reject.common.testId = "confirmChrome_reject";
        return ui::column(ui::Column{
            .children = {ui::text(std::move(what)), ui::button(std::move(accept)), ui::button(std::move(reject))}});
    };
    Mounted mounted{std::move(options)};
    CHECK_FALSE(mounted.has("Dialog", "confirmDialog"));
    CHECK_FALSE(mounted.has("Text", "confirmChrome"));
    mounted.click("rowAction_DeleteRow_1");
    CHECK(mounted.text("Text", "confirmChrome") == "DeleteRow 1");
    mounted.click("confirmChrome_reject");
    CHECK_FALSE(mounted.has("Text", "confirmChrome"));
    CHECK(mounted.count("DeleteRow") == 0);

    mounted.click("rowAction_DeleteRow_2");
    CHECK(mounted.count("DeleteRow") == 0);
    mounted.click("confirmChrome_accept");
    CHECK(mounted.lastBody("DeleteRow") == R"({"id":2})");
    CHECK_FALSE(mounted.has("Text", "confirmChrome"));
}

TEST_CASE("collectionView: the editor chrome hosts the prefilled editor; overrides reach its fields",
          "[forms-engine][collection-view][chrome]") {
    CollectionViewOptions options{};
    options.form.overrides.byField("name", [](morph::forms::FieldView const& field) {
        ui::Text text{.text = "custom " + field.state().path()};
        text.common.testId = "override_name";
        return ui::text(std::move(text));
    });
    options.chrome.editor = [](CollectionSession& session, ui::Node editor) {
        CollectionSession* const view = &session;
        ui::Text state{.text = [view] {
            return std::string{view->model().rowAction()->action} +
                   (view->openRow().has_value() ? " open" : " closed");
        }};
        state.common.testId = "editorChrome";
        ui::Button close{.label = "close", .onClick = [view] { view->close(); }};
        close.common.testId = "editorChrome_close";
        ui::Column body{.children = {std::move(editor)}};
        body.common.visible = [view] { return view->openRow().has_value(); };
        body.common.testId = "editorBody";
        return ui::column(ui::Column{
            .children = {ui::text(std::move(state)), ui::column(std::move(body)), ui::button(std::move(close))}});
    };
    Mounted mounted{std::move(options)};
    CHECK_FALSE(mounted.has("Dialog", "editorDialog"));
    CHECK(mounted.text("Text", "editorChrome") == "EditRow closed");
    mounted.click("rowOpen_2");
    CHECK(mounted.text("Text", "editorChrome") == "EditRow open");
    int const body = mounted.widget("Column", "editorBody");
    CHECK(mounted.within(mounted.widget("TextInput", "field_id"), body));
    CHECK(mounted.text("TextInput", "field_id") == "2");
    CHECK(mounted.text("Text", "override_name") == "custom name");
    mounted.click("editorChrome_close");
    CHECK(mounted.text("Text", "editorChrome") == "EditRow closed");
}

TEST_CASE("collectionView: a fully chromed collection draws no button or text of its own",
          "[forms-engine][collection-view][chrome]") {
    CollectionViewOptions options{};
    options.chrome.header = [](CollectionSession&) { return ui::spacer(); };
    options.chrome.row = [](CollectionSession&, morph::reactive::Signal<CollectionRow> const&) {
        return ui::spacer();
    };
    options.chrome.confirm = [](CollectionSession&) { return ui::spacer(); };
    options.chrome.editor = [](CollectionSession&, ui::Node editor) {
        ui::Column body{.children = {std::move(editor)}};
        body.common.testId = "editorBody";
        return ui::column(std::move(body));
    };
    Mounted mounted{std::move(options)};
    mounted.session->open("1");
    mounted.session->fire(0, "1");
    mounted.harness.settle();
    int const body = mounted.widget("Column", "editorBody");
    for (std::string_view const kind : {"Button", "Text", "Dialog", "Table"}) {
        for (int const id : mounted.backend.all(kind)) {
            bool const ownText = kind == "Text" && mounted.backend.prop(id, "testId") == "collection_error" &&
                                 mounted.backend.prop(id, "visible") == "false";
            INFO(kind << " #" << id << " outside the chrome\n" << mounted.backend.dump());
            CHECK((mounted.within(id, body) || ownText));
        }
    }
}
