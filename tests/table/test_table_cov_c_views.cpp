// SPDX-License-Identifier: Apache-2.0
//
// morph::table saved views: the document and its operations on the wire, and what each operation does to a
// document.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <morph/table/state_store.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace morph::table;

namespace {

SavedView view(std::string name) {
    SavedView out;
    out.name = std::move(name);
    out.sort = {SortKey{.column = "a", .dir = SortDirection::Descending}};
    out.layout.order = {"b", "a"};
    return out;
}

ViewsDocument twoViews() {
    ViewsDocument document;
    document.defaultView = "first";
    document.views = {view("first"), view("second")};
    return document;
}

}  // namespace

TEST_CASE("every view operation survives its own wire form", "[covc][table][views]") {
    std::vector<ViewOperation> const operations{
        AddView{view("v")},
        ReplaceView{view("v")},
        RenameView{.from = "a", .to = "b"},
        RemoveView{.name = "v"},
        SetDefaultView{.name = "v"},
        SetDefaultView{.name = std::nullopt},
        SetDefaultOnMount{.on = true},
        SetDefaultOnMount{.on = false},
    };
    for (auto const& operation : operations) {
        CAPTURE(operation.index());
        auto const text = writeViewOperation(operation);
        CAPTURE(text);
        auto const read = readViewOperation(text);
        REQUIRE(read);
        REQUIRE(read->index() == operation.index());
        CHECK(writeViewOperation(*read) == text);
    }
}

TEST_CASE("a views operation that does not read is refused with its reason", "[covc][table][views]") {
    struct Bad {
        std::string_view json;
        std::string_view message;
    };
    for (auto const& bad : {Bad{.json = "not json", .message = "a views operation does not read"},
                            Bad{.json = "{}", .message = "exactly one member"},
                            Bad{.json = R"({"add":{"name":"x"},"remove":"x"})", .message = "exactly one member"},
                            Bad{.json = R"({"add":[]})", .message = "a saved view does not read"},
                            Bad{.json = R"({"add":{"name":""}})", .message = "a saved view has a name"},
                            Bad{.json = R"({"replace":{"name":""}})", .message = "a saved view has a name"},
                            Bad{.json = R"({"rename":5})", .message = "a rename does not read"},
                            Bad{.json = R"({"remove":5})", .message = "a remove does not read"},
                            Bad{.json = R"({"default":5})", .message = "a default does not read"},
                            Bad{.json = R"({"defaultOnMount":"yes"})", .message = "a defaultOnMount does not read"},
                            Bad{.json = R"({"explode":1})", .message = "`explode` is not one this framework knows"}}) {
        CAPTURE(bad.json);
        auto const read = readViewOperation(bad.json);
        REQUIRE_FALSE(read);
        CHECK(read.error().code == TableErrorCode::InvalidSpec);
        CHECK(read.error().message.contains(bad.message));
    }
}

TEST_CASE("a views document reads once per name and refuses another version", "[covc][table][views]") {
    auto const document = readViews(R"({"version":1,"default":"a","defaultOnMount":true,
        "views":[{"name":"a"},{"name":"b"},{"name":"a","layout":{"order":["x"]}}]})");
    REQUIRE(document);
    CHECK(document->defaultView == "a");
    CHECK(document->defaultOnMount);
    REQUIRE(document->views.size() == 2);
    CHECK(document->views[0].layout.order.empty());

    auto const bare = readViews(R"({"version":1})");
    REQUIRE(bare);
    CHECK_FALSE(bare->defaultOnMount);

    auto const newer = readViews(R"({"version":2})");
    REQUIRE_FALSE(newer);
    CHECK(newer.error().message.contains("is version 2; this reads version 1"));
    auto const garbled = readViews("{");
    REQUIRE_FALSE(garbled);
    CHECK(garbled.error().message.contains("a views document does not read"));
    auto const wrongShape = readViews(R"({"version":1,"views":5})");
    REQUIRE_FALSE(wrongShape);
}

TEST_CASE("the shared part of a views document leaves out the default", "[covc][table][views]") {
    auto const document = twoViews();
    auto const own = readViews(writeViews(document, ViewsPart::Own));
    REQUIRE(own);
    CHECK(own->defaultView == "first");
    CHECK(own->views.size() == 2);
    CHECK(own->views[0].sort.front().dir == SortDirection::Descending);
    auto const shared = readViews(writeViews(document, ViewsPart::Shared));
    REQUIRE(shared);
    CHECK_FALSE(shared->defaultView);
    CHECK(shared->views.size() == 2);
}

TEST_CASE("an operation changes a views document or says why it cannot", "[covc][table][views]") {
    using Expected = std::expected<ViewsDocument, ViewError>;
    auto const apply = [](ViewOperation const& operation, ViewsPart part = ViewsPart::Own) {
        return applyViewOperation(twoViews(), operation, part);
    };
    auto const failure = [](Expected const& result) { return result ? std::optional<ViewError>{} : result.error(); };

    CHECK(apply(AddView{view("third")})->views.size() == 3);
    CHECK(failure(apply(AddView{view("first")})) == ViewError::NameTaken);

    CHECK(apply(ReplaceView{view("second")})->views.size() == 2);
    CHECK(failure(apply(ReplaceView{view("nope")})) == ViewError::NoSuchView);

    auto const renamed = apply(RenameView{.from = "first", .to = "renamed"});
    REQUIRE(renamed);
    CHECK(renamed->views[0].name == "renamed");
    CHECK(renamed->defaultView == "renamed");
    auto const renamedOther = apply(RenameView{.from = "second", .to = "other"});
    CHECK(renamedOther->defaultView == "first");
    CHECK(apply(RenameView{.from = "second", .to = "second"})->views[1].name == "second");
    CHECK(failure(apply(RenameView{.from = "nope", .to = "x"})) == ViewError::NoSuchView);
    CHECK(failure(apply(RenameView{.from = "first", .to = "second"})) == ViewError::NameTaken);

    auto const removed = apply(RemoveView{.name = "first"});
    REQUIRE(removed);
    CHECK(removed->views.size() == 1);
    CHECK_FALSE(removed->defaultView);
    CHECK(apply(RemoveView{.name = "second"})->defaultView == "first");
    CHECK(failure(apply(RemoveView{.name = "nope"})) == ViewError::NoSuchView);

    CHECK(apply(SetDefaultView{.name = "second"})->defaultView == "second");
    CHECK_FALSE(apply(SetDefaultView{.name = std::nullopt})->defaultView);
    CHECK(failure(apply(SetDefaultView{.name = "nope"})) == ViewError::NoSuchView);
    CHECK(failure(apply(SetDefaultView{.name = "second"}, ViewsPart::Shared)) == ViewError::NotShared);

    CHECK(apply(SetDefaultOnMount{.on = true})->defaultOnMount);
    CHECK(failure(apply(SetDefaultOnMount{.on = true}, ViewsPart::Shared)) == ViewError::NotShared);
}

TEST_CASE("a refused view operation is described by the name it concerns", "[covc][table][views]") {
    CHECK(viewErrorText(ViewError::NameTaken, AddView{view("v")}) == "A view named v exists");
    CHECK(viewErrorText(ViewError::NameTaken, ReplaceView{view("r")}) == "A view named r exists");
    CHECK(viewErrorText(ViewError::NameTaken, RenameView{.from = "a", .to = "b"}) == "A view named b exists");
    CHECK(viewErrorText(ViewError::NoSuchView, RenameView{.from = "a", .to = "b"}) == "No view named a");
    CHECK(viewErrorText(ViewError::NoSuchView, RemoveView{.name = "gone"}) == "No view named gone");
    CHECK(viewErrorText(ViewError::NoSuchView, SetDefaultView{.name = "d"}) == "No view named d");
    CHECK(viewErrorText(ViewError::NoSuchView, SetDefaultView{.name = std::nullopt}) == "No view named ");
    CHECK(viewErrorText(ViewError::NameTaken, SetDefaultOnMount{.on = true}) == "A view named  exists");
    CHECK(viewErrorText(ViewError::NotShared, SetDefaultView{.name = "d"}) == "The shared views have no default");
    CHECK(viewErrorText(static_cast<ViewError>(99), SetDefaultOnMount{.on = true}).empty());
}
