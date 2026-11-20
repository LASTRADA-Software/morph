// SPDX-License-Identifier: Apache-2.0
//
// The versions of a custom screen: an element's `since`, a copy's `knownVersion`, what a base gained or lost since the
// version a copy knows, and the update that takes the person's choices over.

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <morph/ui/custom_screen_versions.hpp>
#include <morph/ui/document.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using morph::ui::AddedChoice;
using morph::ui::LayoutItemKind;
using morph::ui::RemovedChoice;

/// The base screen as it shipped in 1.0: an intro and a form.
constexpr std::string_view kBaseOne = R"({"vocab":"ui/1","id":"lims.edit","models":{"sample":{"type":"S"}},)"
                                      R"("view":{"kind":"column","id":"main","children":[)"
                                      R"({"kind":"text","id":"intro","text":"Hello"},)"
                                      R"({"kind":"form","id":"entry","model":"sample","action":"Save"}]}})";

/// The base in 2.0: a note after the intro, and a panel at the end with a text of its own. `legacy`, which a copy
/// made from an earlier base carries, is gone.
constexpr std::string_view kBaseTwo = R"({"vocab":"ui/1","id":"lims.edit","models":{"sample":{"type":"S"}},)"
                                      R"("view":{"kind":"column","id":"main","children":[)"
                                      R"({"kind":"text","id":"intro","text":"Hello"},)"
                                      R"({"kind":"text","id":"note","since":"2.0","text":"New"},)"
                                      R"({"kind":"form","id":"entry","model":"sample","action":"Save"},)"
                                      R"({"kind":"column","id":"extra","since":"2.0","children":[)"
                                      R"({"kind":"text","id":"inner","since":"2.0","text":"Inside"}]}]}})";

/// A person's copy of 1.0: their own text, an element of an earlier base, and an overlay for a field the action has
/// since lost.
constexpr std::string_view kLayout = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                                     R"("models":{"sample":{"type":"S"}},)"
                                     R"("view":{"kind":"column","id":"main","children":[)"
                                     R"({"kind":"text","id":"intro","text":"Hello"},)"
                                     R"({"kind":"text","id":"mine","text":"My own"},)"
                                     R"({"kind":"text","id":"legacy","since":"1.0","text":"Old"},)"
                                     R"({"kind":"form","id":"entry","model":"sample","action":"Save",)"
                                     R"("fields":{"oldField":{"hidden":true}}}]}})";

/// The schema of `Save`: `pressure` came in 2.0.
constexpr std::string_view kSaveSchema = R"({"type":"object","properties":{"density":{"type":"number"},)"
                                         R"("pressure":{"type":"number","x-since":"2.0"}}})";

morph::ui::ActionSchemaLookup schemasOf(std::map<std::string, std::string, std::less<>> known) {
    return [known = std::move(known)](std::string_view action) -> std::optional<std::string> {
        auto const found = known.find(action);
        if (found == known.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

morph::ui::ActionSchemaLookup saveSchemas() { return schemasOf({{"Save", std::string{kSaveSchema}}}); }

morph::forms::FieldVersion version(std::string_view text) { return morph::forms::FieldVersion::parse(text).value(); }

morph::ui::LayoutChanges changesOf(std::string_view base, std::string_view layout,
                                   morph::ui::ActionSchemaLookup const& schemas = saveSchemas()) {
    auto changes = morph::ui::layoutChanges(base, schemas, layout);
    if (!changes) {
        FAIL("layoutChanges refused: " << changes.error().message());
    }
    return std::move(*changes);
}

std::string applied(std::string_view base, std::string_view layout, morph::ui::LayoutChoices const& choices,
                    morph::ui::ActionSchemaLookup const& schemas = saveSchemas()) {
    auto updated = morph::ui::applyLayoutChoices(base, schemas, layout, choices);
    if (!updated) {
        FAIL("applyLayoutChoices refused: " << updated.error().message());
    }
    return std::move(*updated);
}

morph::ui::Refusal refusalOf(std::string_view base, std::string_view layout, morph::ui::LayoutChoices const& choices,
                             morph::ui::ActionSchemaLookup const& schemas = saveSchemas()) {
    auto updated = morph::ui::applyLayoutChoices(base, schemas, layout, choices);
    REQUIRE_FALSE(updated.has_value());
    return updated.error();
}

morph::ui::Json parsed(std::string const& text) { return morph::ui::parseJson(text).value(); }

/// The ids of the children of the column at @p pointer, in order.
std::vector<std::string> childIds(std::string const& text, std::string_view pointer = "/view") {
    morph::ui::Json const json = parsed(text);
    morph::ui::Json const* node = &json;
    std::string_view rest = pointer;
    while (!rest.empty()) {
        rest.remove_prefix(1);
        auto const slash = rest.find('/');
        std::string const token{rest.substr(0, slash)};
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
        node = node->is_array() ? &node->get_array().at(std::stoul(token)) : &node->get_object().at(token);
    }
    std::vector<std::string> ids;
    for (auto const& child : node->get_object().at("children").get_array()) {
        ids.push_back(child.get_object().at("id").get_string());
    }
    return ids;
}

morph::ui::Refusal loadRefusal(std::string const& text) {
    auto loaded = morph::ui::loadDocumentText(text, morph::ui::LoadOptions{});
    REQUIRE_FALSE(loaded.has_value());
    return loaded.error();
}

}  // namespace

TEST_CASE("custom screen versions: the loader reads an element's since and a document's knownVersion",
          "[ui][custom-screens][versions]") {
    auto const loaded = morph::ui::loadDocumentText(kBaseTwo, morph::ui::LoadOptions{});
    REQUIRE(loaded.has_value());
    auto const children = loaded->view->nodes("children");
    REQUIRE(children.size() == 4);
    CHECK(children[0]->since.empty());
    CHECK(children[1]->since == "2.0");
    CHECK(morph::ui::loadDocumentText(kLayout, morph::ui::LoadOptions{}).has_value());
    // A form's `fields` record maps field names, so a field named `since` is a field there.
    CHECK(morph::ui::loadDocumentText(R"({"vocab":"ui/1","id":"s","models":{"m":{"type":"S"}},)"
                                      R"("view":{"kind":"form","model":"m","action":"A",)"
                                      R"("fields":{"since":{"hidden":true}}}})",
                                      morph::ui::LoadOptions{})
              .has_value());
}

TEST_CASE("custom screen versions: the loader refuses a since or a knownVersion that is not a version",
          "[ui][custom-screens][versions]") {
    auto const withNote = [](std::string_view note) {
        return R"({"vocab":"ui/1","id":"s","view":{"kind":"column","children":[)" + std::string{note} + "]}}";
    };
    auto const notText = loadRefusal(withNote(R"({"kind":"text","id":"a","since":2,"text":"x"})"));
    CHECK(notText.path == "/view/children/0/since");
    CHECK(notText.reason.contains("is a version"));
    auto const notVersion = loadRefusal(withNote(R"({"kind":"text","id":"a","since":"6.x","text":"x"})"));
    CHECK(notVersion.path == "/view/children/0/since");
    CHECK(notVersion.reason.contains("'6.x' is not a version"));
    auto const noId = loadRefusal(withNote(R"({"kind":"text","since":"6.1","text":"x"})"));
    CHECK(noId.path == "/view/children/0/since");
    CHECK(noId.reason.contains("has an `id`"));

    auto const knownNotText =
        loadRefusal(R"({"vocab":"ui/1","id":"s","knownVersion":1,"view":{"kind":"text","text":"x"}})");
    CHECK(knownNotText.path == "/knownVersion");
    auto const knownBad =
        loadRefusal(R"({"vocab":"ui/1","id":"s","knownVersion":"1..2","view":{"kind":"text","text":"x"}})");
    CHECK(knownBad.path == "/knownVersion");
    CHECK(knownBad.reason.contains("empty component"));
}

TEST_CASE("custom screen versions: a base's version is the highest since of its elements and its forms' fields",
          "[ui][custom-screens][versions]") {
    CHECK(morph::ui::baseVersion(kBaseOne, saveSchemas()).value() == version("2.0"));
    CHECK(morph::ui::baseVersion(kBaseOne, {}).value() == morph::forms::FieldVersion{});
    CHECK(morph::ui::baseVersion(kBaseTwo, {}).value() == version("2.0"));
    auto const later = schemasOf({{"Save", R"({"properties":{"x":{"x-since":"3.1"}}})"}});
    CHECK(morph::ui::baseVersion(kBaseTwo, later).value().text() == "3.1");
    auto const refused = morph::ui::baseVersion("not json", {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().path.empty());
}

TEST_CASE("custom screen versions: layoutChanges lists what the base gained and what the copy refers to that it lost",
          "[ui][custom-screens][versions]") {
    auto const changes = changesOf(kBaseTwo, kLayout);
    CHECK(changes.baseVersion == version("2"));
    CHECK(changes.knownVersion == version("1"));
    CHECK(changes.needsUpdate());
    CHECK(changes.needsAttention());

    REQUIRE(changes.added.size() == 3);
    auto const& note = changes.added.at(0);
    CHECK(note.kind == LayoutItemKind::Element);
    CHECK(note.key == "note");
    CHECK(note.since == "2.0");
    CHECK(note.place.parent == "main");
    CHECK(note.place.path == "/children");
    CHECK(note.place.index == std::optional<std::size_t>{1});
    // `inner` comes with `extra`, so it is not listed on its own.
    auto const& extra = changes.added.at(1);
    CHECK(extra.key == "extra");
    CHECK(extra.place.index == std::optional<std::size_t>{3});
    auto const& pressure = changes.added.at(2);
    CHECK(pressure.kind == LayoutItemKind::Field);
    CHECK(pressure.key == "entry/pressure");
    CHECK(pressure.id == "pressure");
    CHECK(pressure.form == "entry");
    CHECK(pressure.action == "Save");

    REQUIRE(changes.removed.size() == 2);
    CHECK(changes.removed.at(0).kind == LayoutItemKind::Element);
    CHECK(changes.removed.at(0).key == "legacy");
    CHECK(changes.removed.at(1).kind == LayoutItemKind::Field);
    CHECK(changes.removed.at(1).key == "entry/oldField");
    CHECK(changes.removed.at(1).action == "Save");
}

TEST_CASE("custom screen versions: what the copy already knows or has is not listed",
          "[ui][custom-screens][versions]") {
    // A copy that knows 2.0 is current; its own elements and the base's are not changes.
    std::string const current = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"2.0",)"
                                R"("models":{"sample":{"type":"S"}},)"
                                R"("view":{"kind":"column","id":"main","children":[)"
                                R"({"kind":"text","id":"intro","text":"Hello"},)"
                                R"({"kind":"text","id":"mine","text":"My own"},)"
                                R"({"kind":"form","id":"entry","model":"sample","action":"Save"}]}})";
    auto const changes = changesOf(kBaseTwo, current);
    CHECK(changes.added.empty());
    CHECK(changes.removed.empty());
    CHECK_FALSE(changes.needsUpdate());
    CHECK_FALSE(changes.needsAttention());

    // A copy with no knownVersion knows the first version; an element it already has and a field it decided are not
    // added.
    std::string const decided = R"({"vocab":"ui/1","id":"custom:x","models":{"sample":{"type":"S"}},)"
                                R"("view":{"kind":"column","id":"main","children":[)"
                                R"({"kind":"text","id":"note","since":"2.0","text":"Mine now"},)"
                                R"({"kind":"form","id":"entry","model":"sample","action":"Save",)"
                                R"("fields":{"pressure":{"omit":true}}}]}})";
    auto const undecided = changesOf(kBaseTwo, decided);
    CHECK(undecided.knownVersion == morph::forms::FieldVersion{});
    REQUIRE(undecided.added.size() == 1);
    CHECK(undecided.added.front().key == "extra");

    // A copy that dropped the form shows none of its fields; a form whose action the lookup does not know is not
    // compared.
    std::string const noForm = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                               R"("view":{"kind":"column","id":"main","children":[)"
                               R"({"kind":"text","id":"intro","text":"Hello"}]}})";
    CHECK(changesOf(kBaseTwo, noForm).added.size() == 2);
    auto const unknown = changesOf(kBaseTwo, kLayout, {});
    CHECK(unknown.added.size() == 2);
    REQUIRE(unknown.removed.size() == 1);
    CHECK(unknown.removed.front().key == "legacy");
}

TEST_CASE("custom screen versions: a form without an id is matched by its action and keyed by it",
          "[ui][custom-screens][versions]") {
    std::string const base = R"({"vocab":"ui/1","id":"b","models":{"sample":{"type":"S"}},)"
                             R"("view":{"kind":"form","model":"sample","action":"Save"}})";
    std::string const layout = R"({"vocab":"ui/1","id":"custom:x","models":{"sample":{"type":"S"}},)"
                               R"("view":{"kind":"form","model":"sample","action":"Save",)"
                               R"("fields":{"oldField":{"hidden":true}}}})";
    auto const changes = changesOf(base, layout);
    REQUIRE(changes.added.size() == 1);
    CHECK(changes.added.front().key == "Save/pressure");
    CHECK(changes.added.front().form.empty());
    REQUIRE(changes.removed.size() == 1);
    CHECK(changes.removed.front().key == "Save/oldField");

    std::string const updated = applied(
        base, layout,
        {.added = {{"Save/pressure", AddedChoice::LeaveOut}}, .removed = {{"Save/oldField", RemovedChoice::Drop}}});
    auto const fields = parsed(updated).get_object().at("view").get_object().at("fields").get_object();
    CHECK(fields.contains("pressure"));
    CHECK_FALSE(fields.contains("oldField"));
}

TEST_CASE("custom screen versions: a schema that is not an object or has an x-since that is not a version is refused",
          "[ui][custom-screens][versions]") {
    auto const refusedBy = [](std::string schema) {
        auto const changes = morph::ui::layoutChanges(kBaseTwo, schemasOf({{"Save", std::move(schema)}}), kLayout);
        REQUIRE_FALSE(changes.has_value());
        return changes.error();
    };
    auto const notObject = refusedBy("[1]");
    CHECK(notObject.path == "/view/children/2");
    CHECK(notObject.reason.contains("is not a JSON object"));
    CHECK(refusedBy("not json").reason.contains("is not a JSON object"));
    auto const badSince = refusedBy(R"({"properties":{"p":{"x-since":"two"}}})");
    CHECK(badSince.reason.contains("gives `p` an `x-since` that is not a version"));
    CHECK(refusedBy(R"({"properties":{"p":{"x-since":2}}})").reason.contains("`x-since` that is not a version"));

    // A schema without properties, or a property that is not an object, has no versioned fields.
    auto const plain = changesOf(kBaseTwo, kLayout, schemasOf({{"Save", R"({"type":"object"})"}}));
    CHECK(plain.added.size() == 2);
    auto const odd = changesOf(kBaseTwo, kLayout, schemasOf({{"Save", R"({"properties":{"p":true}})"}}));
    CHECK(odd.added.size() == 2);
    // The copy's overlay names a field the schema lacks, so it is removed; the base's form gives the same refusal.
    REQUIRE(odd.removed.size() == 2);

    // Only the copy's form reads a refused schema: the base has none.
    std::string const bare = R"({"vocab":"ui/1","id":"b","view":{"kind":"text","id":"t","text":"x"}})";
    auto const copyOnly =
        morph::ui::layoutChanges(bare, schemasOf({{"Save", R"({"properties":{"p":{"x-since":"x"}}})"}}), kLayout);
    REQUIRE_FALSE(copyOnly.has_value());
    CHECK(copyOnly.error().path == "/view/children/3");
}

TEST_CASE("custom screen versions: documents that do not load are refused", "[ui][custom-screens][versions]") {
    CHECK_FALSE(morph::ui::layoutChanges("{", saveSchemas(), kLayout).has_value());
    auto const layout = morph::ui::layoutChanges(kBaseTwo, saveSchemas(), R"({"vocab":"ui/1"})");
    REQUIRE_FALSE(layout.has_value());
    CHECK(layout.error().reason.contains("`id`"));
    CHECK_FALSE(morph::ui::applyLayoutChoices(kBaseTwo, saveSchemas(), "[]", {}).has_value());
}

TEST_CASE("custom screen versions: round trip: the copy takes one change over and leaves one out and is current",
          "[ui][custom-screens][versions]") {
    std::string const updated =
        applied(kBaseTwo, kLayout,
                {.added = {{"note", AddedChoice::TakeOver}, {"entry/pressure", AddedChoice::LeaveOut}},
                 .removed = {{"legacy", RemovedChoice::Drop}, {"entry/oldField", RemovedChoice::Keep}}});
    // `legacy` is dropped, `note` sits after the intro as in the base, `extra` had no choice and stays out.
    CHECK(childIds(updated) == std::vector<std::string>{"intro", "note", "mine", "entry"});
    morph::ui::Json const json = parsed(updated);
    CHECK(json.get_object().at("knownVersion").get_string() == "2.0");
    auto const& form = json.get_object().at("view").get_object().at("children").get_array().at(3).get_object();
    auto const& fields = form.at("fields").get_object();
    CHECK(fields.at("pressure").get_object().at("omit").get_boolean());
    CHECK(fields.contains("oldField"));

    auto const after = changesOf(kBaseTwo, updated);
    CHECK_FALSE(after.needsUpdate());
    CHECK(after.added.empty());
    // The kept overlay still names a field the action lost.
    CHECK(after.needsAttention());
    REQUIRE(after.removed.size() == 1);
    CHECK(after.removed.front().key == "entry/oldField");
}

TEST_CASE("custom screen versions: an element taken over brings what it holds to the end of a shorter list",
          "[ui][custom-screens][versions]") {
    std::string const updated =
        applied(kBaseTwo, kLayout,
                {.added = {{"extra", AddedChoice::TakeOver}}, .removed = {{"legacy", RemovedChoice::Drop}}});
    // The base puts `extra` fourth; without `legacy` and `note` the copy's list has three, so it goes at the end.
    CHECK(childIds(updated) == std::vector<std::string>{"intro", "mine", "entry", "extra"});
    CHECK(childIds(updated, "/view/children/3") == std::vector<std::string>{"inner"});
    // With no choice, a field is left out: its overlay is written.
    morph::ui::Json const json = parsed(updated);
    auto const& form = json.get_object().at("view").get_object().at("children").get_array().at(2).get_object();
    CHECK(form.at("fields").get_object().contains("pressure"));

    // A list two places shorter than the base's index still takes it, at its end.
    std::string const shortList =
        R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
        R"("view":{"kind":"column","id":"main","children":[{"kind":"text","id":"a","text":"x"}]}})";
    std::string const appended =
        applied(kBaseTwo, shortList, {.added = {{"extra", AddedChoice::TakeOver}}, .removed = {}});
    CHECK(childIds(appended) == std::vector<std::string>{"a", "extra"});
}

TEST_CASE("custom screen versions: a field taken over needs nothing and a form without overlays gets them",
          "[ui][custom-screens][versions]") {
    std::string const schema = R"({"properties":{"pressure":{"x-since":"2.0"},"humidity":{"x-since":"2.0"}}})";
    auto const schemas = schemasOf({{"Save", schema}});
    std::string const layout = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                               R"("models":{"sample":{"type":"S"}},)"
                               R"("view":{"kind":"column","id":"main","children":[)"
                               R"({"kind":"form","id":"entry","model":"sample","action":"Save"}]}})";
    std::string const left = applied(kBaseOne, layout, {}, schemas);
    morph::ui::Json const leftJson = parsed(left);
    auto const& form = leftJson.get_object().at("view").get_object().at("children").get_array().at(0);
    auto const& fields = form.get_object().at("fields").get_object();
    CHECK(fields.contains("pressure"));
    CHECK(fields.contains("humidity"));

    std::string const taken =
        applied(kBaseOne, layout,
                {.added = {{"entry/pressure", AddedChoice::TakeOver}, {"entry/humidity", AddedChoice::TakeOver}},
                 .removed = {}},
                schemas);
    morph::ui::Json const takenJson = parsed(taken);
    auto const& plain = takenJson.get_object().at("view").get_object().at("children").get_array().at(0);
    CHECK_FALSE(plain.get_object().contains("fields"));
    CHECK(changesOf(kBaseOne, taken, schemas).added.empty());
}

TEST_CASE("custom screen versions: removals in one list are dropped without moving each other",
          "[ui][custom-screens][versions]") {
    std::string children;
    for (int index = 0; index < 12; ++index) {
        bool const removed = index == 2 || index == 10;
        children += (index == 0 ? "" : ",") + std::string{R"({"kind":"text","id":"e)"} + std::to_string(index) +
                    (removed ? R"(","since":"1.0)" : "") + R"(","text":"x"})";
    }
    // A removed container that holds a removed element: both go.
    children += R"(,{"kind":"column","id":"box","since":"1.0","children":[)"
                R"({"kind":"text","id":"boxed","since":"1.0","text":"x"}]})";
    std::string const layout =
        R"({"vocab":"ui/1","id":"custom:x","view":{"kind":"column","id":"main","children":[)" + children + "]}}";
    std::string const base = R"({"vocab":"ui/1","id":"b","view":{"kind":"column","id":"main","children":[]}})";
    auto const changes = changesOf(base, layout, {});
    REQUIRE(changes.removed.size() == 4);
    morph::ui::LayoutChoices choices;
    for (auto const& removal : changes.removed) {
        choices.removed.emplace(removal.key, RemovedChoice::Drop);
    }
    std::string const updated = applied(base, layout, choices, {});
    CHECK(childIds(updated) == std::vector<std::string>{"e0", "e1", "e3", "e4", "e5", "e6", "e7", "e8", "e9", "e11"});
}

TEST_CASE("custom screen versions: an element of the head or held as a member keeps its place",
          "[ui][custom-screens][versions]") {
    std::string const base = R"({"vocab":"ui/1","id":"b","actions":[{"id":"one","label":"One"},)"
                             R"({"id":"two","since":"2.0","label":"Two"}],)"
                             R"("view":{"kind":"column","id":"main","since":"2.0","children":[]}})";
    std::string const layout = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                               R"("view":{"kind":"text","id":"own","text":"x"}})";
    auto const changes = changesOf(base, layout, {});
    REQUIRE(changes.added.size() == 2);
    CHECK(changes.added.at(0).key == "two");
    CHECK(changes.added.at(0).place.parent.empty());
    CHECK(changes.added.at(0).place.path == "/actions");
    CHECK(changes.added.at(0).place.index == std::optional<std::size_t>{1});
    CHECK(changes.added.at(1).key == "main");
    CHECK(changes.added.at(1).place.path == "/view");
    CHECK_FALSE(changes.added.at(1).place.index.has_value());

    // The head's list does not exist in the copy: it is made, holding the element.
    std::string const updated = applied(base, layout, {.added = {{"two", AddedChoice::TakeOver}}, .removed = {}}, {});
    morph::ui::Json const updatedJson = parsed(updated);
    auto const& actions = updatedJson.get_object().at("actions").get_array();
    REQUIRE(actions.size() == 1);
    CHECK(actions.front().get_object().at("id").get_string() == "two");

    // The copy holds something else where the base holds `main` itself.
    auto const occupied = refusalOf(base, layout, {.added = {{"main", AddedChoice::TakeOver}}, .removed = {}}, {});
    CHECK(occupied.path == "/view");
    CHECK(occupied.reason.contains("has something else here"));
}

TEST_CASE("custom screen versions: an element whose place the copy lacks is refused",
          "[ui][custom-screens][versions]") {
    // The copy has no `main`, which holds `note` in the base.
    std::string const noParent = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                                 R"("view":{"kind":"column","id":"other","children":[]}})";
    auto const missing = refusalOf(kBaseTwo, noParent, {.added = {{"note", AddedChoice::TakeOver}}, .removed = {}});
    CHECK(missing.reason.contains("sits inside `main`"));

    // The copy's `main` holds one child where the base's holds a list.
    std::string const single = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                               R"("view":{"kind":"column","id":"main","children":{"kind":"text","text":"x"}}})";
    auto const notList = refusalOf(kBaseTwo, single, {.added = {{"note", AddedChoice::TakeOver}}, .removed = {}});
    CHECK(notList.path == "/view/children");
    CHECK(notList.reason.contains("is not one"));

    // The copy's `main` has no children: the list is made.
    std::string const empty = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                              R"("view":{"kind":"column","id":"main"}})";
    std::string const updated = applied(kBaseTwo, empty, {.added = {{"note", AddedChoice::TakeOver}}, .removed = {}});
    CHECK(childIds(updated) == std::vector<std::string>{"note"});
}

TEST_CASE("custom screen versions: a choice for a key that names no change is refused",
          "[ui][custom-screens][versions]") {
    CHECK(refusalOf(kBaseTwo, kLayout, {.added = {{"nothing", AddedChoice::TakeOver}}, .removed = {}})
              .reason.contains("no addition is named `nothing`"));
    CHECK(refusalOf(kBaseTwo, kLayout, {.added = {}, .removed = {{"note", RemovedChoice::Drop}}})
              .reason.contains("no removal is named `note`"));
}

TEST_CASE("custom screen versions: a result that does not load is refused", "[ui][custom-screens][versions]") {
    // `note` reads a state the copy does not declare.
    std::string const base = R"({"vocab":"ui/1","id":"b","state":{"count":{"init":1}},)"
                             R"("view":{"kind":"column","id":"main","children":[)"
                             R"({"kind":"text","id":"note","since":"2.0","text":{"ref":"count"}}]}})";
    std::string const layout = R"({"vocab":"ui/1","id":"custom:x","knownVersion":"1.0",)"
                               R"("view":{"kind":"column","id":"main","children":[]}})";
    auto const refusal = refusalOf(base, layout, {.added = {{"note", AddedChoice::TakeOver}}, .removed = {}}, {});
    CHECK(refusal.reason.contains("count"));
}

TEST_CASE("custom screen versions: withKnownVersion sets the member", "[ui][custom-screens][versions]") {
    auto const set = morph::ui::withKnownVersion(R"({"id":"x","knownVersion":"1"})", version("2.1"));
    REQUIRE(set.has_value());
    CHECK(parsed(*set).get_object().at("knownVersion").get_string() == "2.1");
    // A control byte in a string stays escaped, so the text is still JSON.
    auto const escaped = morph::ui::withKnownVersion(R"({"text":"a\u000bb"})", version("1"));
    REQUIRE(escaped.has_value());
    CHECK(morph::ui::parseJson(*escaped).has_value());
    CHECK_FALSE(morph::ui::withKnownVersion("[]", version("1")).has_value());
    CHECK_FALSE(morph::ui::withKnownVersion("{", version("1")).has_value());
}
