// SPDX-License-Identifier: Apache-2.0

// The saved views of a table against a store that answers as a script says: what each failure of loading and saving
// reports, and which default view a mount starts from.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/table_views.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using namespace std::chrono_literals;
using morph::ui::TableStates;
using morph::ui::TableViews;
using morph::ui::testing::RowsFixture;

/// What one call of the store answers: a result, or a failure.
struct Reply {
    table::SaveResult result;
    std::exception_ptr failure;
};

/// A store whose loads and saves answer as the test scripted them.
class ScriptStore final : public table::ITableStateStore {
public:
    explicit ScriptStore(morph::exec::IExecutor& executor, bool sharing) : _executor{&executor}, _sharing{sharing} {}

    [[nodiscard]] morph::async::Completion<std::optional<table::StoredTableState>> load(
        table::TableStateKey const& key) override {
        auto [completion, promise] =
            morph::async::Completion<std::optional<table::StoredTableState>>::makeSettleable(_executor);
        ++loads;
        if (auto const failing = loadFailures.find(key.part); failing != loadFailures.end()) {
            promise.reject(failing->second);
        } else if (auto const found = stored.find(key.part); found != stored.end()) {
            promise.resolve(found->second);
        } else {
            promise.resolve(std::nullopt);
        }
        return std::move(completion);
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an override of the store interface; the types are fixed.
    [[nodiscard]] morph::async::Completion<table::SaveResult> save(table::TableStateKey const& key, std::string json,
                                                                   std::string expected = {}) override {
        savedExpected.push_back(std::move(expected));
        static_cast<void>(key);
        static_cast<void>(json);
        return answer();
    }

    [[nodiscard]] morph::async::Completion<bool> remove(table::TableStateKey const& /*key*/) override {
        auto [completion, promise] = morph::async::Completion<bool>::makeSettleable(_executor);
        promise.resolve(false);
        return std::move(completion);
    }

    [[nodiscard]] bool shares() const override { return _sharing; }

    [[nodiscard]] morph::async::Completion<table::SaveResult> publish(table::TableStateKey const& /*key*/,
                                                                      table::ViewOperation const& /*operation*/,
                                                                      std::string expected) override {
        savedExpected.push_back(std::move(expected));
        return answer();
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test steers directly.
    std::map<std::string, std::optional<table::StoredTableState>> stored;
    std::map<std::string, std::exception_ptr> loadFailures;
    std::deque<Reply> replies;
    std::vector<std::string> savedExpected;
    int loads = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    morph::async::Completion<table::SaveResult> answer() {
        auto [completion, promise] = morph::async::Completion<table::SaveResult>::makeSettleable(_executor);
        Reply reply{.result = {.outcome = table::SaveOutcome::Saved, .revision = "r", .reason = {}}, .failure = {}};
        if (!replies.empty()) {
            reply = std::move(replies.front());
            replies.pop_front();
        }
        if (reply.failure) {
            promise.reject(reply.failure);
        } else {
            promise.resolve(reply.result);
        }
        return std::move(completion);
    }

    morph::exec::IExecutor* _executor;
    bool _sharing;
};

table::TableStateKey keyOfTable() {
    return table::TableStateKey{.application = "lims",
                                .model = "Lims_SampleModel",
                                .table = "samples",
                                .qualifier = "samples",
                                .part = "state"};
}

table::SavedView viewNamed(std::string name) {
    return table::SavedView{.name = std::move(name),
                            .layout = {},
                            .sort = {table::SortKey{.column = "name", .dir = table::SortDirection::Ascending}},
                            .filters = {}};
}

/// A window over a script store.
struct Window {
    explicit Window(bool sharing)
        : store{std::make_shared<ScriptStore>(fixture.owner, sharing)},
          states{store, fixture.owner, &fixture.scheduler, 500ms},
          views{states, keyOfTable(), fixture.owner} {}

    template <typename T>
    T settle(morph::async::Completion<T> completion) {
        std::optional<T> value;
        completion.then([&value](T const& got) { value = got; });
        fixture.settle();
        REQUIRE(value.has_value());
        return std::move(*value);
    }

    RowsFixture fixture;
    std::shared_ptr<ScriptStore> store;
    TableStates states;
    TableViews views;
};

std::string errorOf(TableViews::Result const& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

}  // namespace

TEST_CASE("covi views: a load that fails says why, the first failing part's words winning",
          "[covi][ui][table][state]") {
    struct Case {
        char const* what;
        bool sharing;
        std::exception_ptr own;
        std::exception_ptr shared;
        std::string expected;
    };
    for (Case const& item : std::vector<Case>{
             {.what = "a store failure with a reason",
              .sharing = false,
              .own = std::make_exception_ptr(std::runtime_error{"disk gone"}),
              .shared = {},
              .expected = "The saved views could not be loaded: disk gone"},
             {.what = "a failure that carries no words",
              .sharing = false,
              .own = std::make_exception_ptr(7),
              .shared = {},
              .expected = "The saved views could not be loaded"},
             {.what = "both parts failing keep the first",
              .sharing = true,
              .own = std::make_exception_ptr(std::runtime_error{"own gone"}),
              .shared = std::make_exception_ptr(std::runtime_error{"shared gone"}),
              .expected = "The saved views could not be loaded: own gone"},
             {.what = "only the shared part failing",
              .sharing = true,
              .own = {},
              .shared = std::make_exception_ptr(std::runtime_error{"shared gone"}),
              .expected = "The saved views could not be loaded: shared gone"},
         }) {
        INFO(item.what);
        Window w{item.sharing};
        if (item.own) {
            w.store->loadFailures.insert_or_assign("views", item.own);
        }
        if (item.shared) {
            w.store->loadFailures.insert_or_assign("shared", item.shared);
        }
        CHECK(errorOf(w.settle(w.views.load())) == item.expected);
    }
}

TEST_CASE("covi views: stored views that do not read fail the load and name what could not be read",
          "[covi][ui][table][state]") {
    Window w{false};
    w.store->stored.insert_or_assign("views", table::StoredTableState{.json = "not json", .revision = "1"});
    std::string const text = errorOf(w.settle(w.views.load()));
    CHECK(text.starts_with("The saved views could not be read: "));
}

TEST_CASE("covi views: a save the store refuses says so, with its reason when it gave one",
          "[covi][ui][table][state]") {
    struct Case {
        char const* what;
        bool shared;
        std::string reason;
        std::string expected;
    };
    for (Case const& item : std::vector<Case>{
             {.what = "own, no reason", .shared = false, .reason = "", .expected = "Views could not be saved"},
             {.what = "own, a reason",
              .shared = false,
              .reason = "quota",
              .expected = "Views could not be saved: quota"},
             {.what = "shared, no reason",
              .shared = true,
              .reason = "",
              .expected = "This view could not be published"},
             {.what = "shared, a reason",
              .shared = true,
              .reason = "quota",
              .expected = "This view could not be published: quota"},
         }) {
        INFO(item.what);
        Window w{true};
        REQUIRE(w.settle(w.views.load()).has_value());
        w.store->replies.push_back(Reply{
            .result = {.outcome = table::SaveOutcome::Refused, .revision = {}, .reason = item.reason}, .failure = {}});
        auto const result = item.shared ? w.settle(w.views.publish(table::AddView{viewNamed("A")}))
                                        : w.settle(w.views.change(table::AddView{viewNamed("A")}));
        CHECK(errorOf(result) == item.expected);
        CHECK(w.views.own().views.empty());
        CHECK(w.views.shared().views.empty());
    }
}

TEST_CASE("covi views: a save that fails with an exception says why", "[covi][ui][table][state]") {
    Window w{false};
    REQUIRE(w.settle(w.views.load()).has_value());
    w.store->replies.push_back(Reply{.result = {}, .failure = std::make_exception_ptr(std::runtime_error{"offline"})});
    CHECK(errorOf(w.settle(w.views.change(table::AddView{viewNamed("A")}))) == "Views could not be saved: offline");
    w.store->replies.push_back(Reply{.result = {}, .failure = std::make_exception_ptr(3)});
    CHECK(errorOf(w.settle(w.views.change(table::AddView{viewNamed("A")}))) == "Views could not be saved");
}

TEST_CASE("covi views: a second conflict gives up, a conflict then a save succeeds after one reload",
          "[covi][ui][table][state]") {
    SECTION("two conflicts in a row") {
        Window w{true};
        REQUIRE(w.settle(w.views.load()).has_value());
        for (int round = 0; round < 2; ++round) {
            w.store->replies.push_back(Reply{
                .result = {.outcome = table::SaveOutcome::Conflict, .revision = "9", .reason = {}}, .failure = {}});
        }
        CHECK(errorOf(w.settle(w.views.change(table::AddView{viewNamed("A")}))) ==
              "Views could not be saved: another window changed them at the same time");
        w.store->replies.push_back(
            Reply{.result = {.outcome = table::SaveOutcome::Conflict, .revision = "9", .reason = {}}, .failure = {}});
        w.store->replies.push_back(
            Reply{.result = {.outcome = table::SaveOutcome::Conflict, .revision = "9", .reason = {}}, .failure = {}});
        CHECK(errorOf(w.settle(w.views.publish(table::AddView{viewNamed("A")}))) ==
              "This view could not be published: another window changed them at the same time");
    }
    SECTION("one conflict") {
        Window w{false};
        REQUIRE(w.settle(w.views.load()).has_value());
        w.store->replies.push_back(
            Reply{.result = {.outcome = table::SaveOutcome::Conflict, .revision = "9", .reason = {}}, .failure = {}});
        CHECK(w.settle(w.views.change(table::AddView{viewNamed("A")})).has_value());
        CHECK(w.views.own().views.size() == 1);
    }
}

TEST_CASE("covi views: a reload after a conflict that finds the stored views unreadable fails the change",
          "[covi][ui][table][state]") {
    struct Case {
        bool shared;
        std::string expected;
    };
    for (Case const& item :
         std::vector<Case>{{.shared = false, .expected = "The saved views could not be read: "},
                           {.shared = true,
                            .expected = "This view could not be published: The saved views could not be "
                                        "read: "}}) {
        INFO(item.shared);
        Window w{true};
        REQUIRE(w.settle(w.views.load()).has_value());
        w.store->replies.push_back(
            Reply{.result = {.outcome = table::SaveOutcome::Conflict, .revision = "9", .reason = {}}, .failure = {}});
        w.store->stored.insert_or_assign(item.shared ? "shared" : "views",
                                         table::StoredTableState{.json = "{{{", .revision = "9"});
        auto const result = item.shared ? w.settle(w.views.publish(table::AddView{viewNamed("A")}))
                                        : w.settle(w.views.change(table::AddView{viewNamed("A")}));
        CHECK(errorOf(result).starts_with(item.expected));
    }
}

TEST_CASE("covi views: a reload after a conflict that fails to load reports the failure", "[covi][ui][table][state]") {
    Window w{false};
    REQUIRE(w.settle(w.views.load()).has_value());
    w.store->replies.push_back(
        Reply{.result = {.outcome = table::SaveOutcome::Conflict, .revision = "9", .reason = {}}, .failure = {}});
    w.store->loadFailures.insert_or_assign("views", std::make_exception_ptr(std::runtime_error{"gone"}));
    CHECK(errorOf(w.settle(w.views.change(table::AddView{viewNamed("A")}))) == "Views could not be saved: gone");
}

TEST_CASE("covi views: an operation the document refuses is reported as the part says", "[covi][ui][table][state]") {
    Window w{true};
    REQUIRE(w.settle(w.views.load()).has_value());
    CHECK(errorOf(w.settle(w.views.change(table::RemoveView{"Nope"}))) ==
          table::viewErrorText(table::ViewError::NoSuchView, table::RemoveView{"Nope"}));
    CHECK(errorOf(w.settle(w.views.publish(table::RemoveView{"Nope"}))) ==
          "This view could not be published: " +
              table::viewErrorText(table::ViewError::NoSuchView, table::RemoveView{"Nope"}));
    CHECK(w.store->savedExpected.empty());
}

TEST_CASE("covi views: the default view a mount starts from", "[covi][ui][table][state]") {
    table::ViewsDocument own;
    own.views = {viewNamed("A")};
    SECTION("no default: the stored state as it is") {
        auto const out = morph::ui::stateAtMount(std::nullopt, own);
        CHECK_FALSE(out.has_value());
    }
    SECTION("a default no view bears: the stored state as it is") {
        own.defaultView = "Gone";
        CHECK_FALSE(morph::ui::stateAtMount(std::nullopt, own).has_value());
        table::StateDocument stored;
        stored.view = "Mine";
        auto const out = morph::ui::stateAtMount(stored, own);
        REQUIRE(out.has_value());
        CHECK(out->view == std::optional<std::string>{"Mine"});
        CHECK_FALSE(out->sort.has_value());
    }
    SECTION("a default view and no stored state: the view's arrangement") {
        own.defaultView = "A";
        auto const out = morph::ui::stateAtMount(std::nullopt, own);
        REQUIRE(out.has_value());
        CHECK(out->view == std::optional<std::string>{"A"});
        REQUIRE(out->sort.has_value());
        CHECK(out->sort->front().column == "name");
    }
    SECTION("a stored state keeps its layout; the view's sort and filters only with default-on-mount") {
        own.defaultView = "A";
        table::StateDocument stored;
        stored.view = "Mine";
        auto const kept = morph::ui::stateAtMount(stored, own);
        REQUIRE(kept.has_value());
        CHECK_FALSE(kept->sort.has_value());
        own.defaultOnMount = true;
        auto const taken = morph::ui::stateAtMount(stored, own);
        REQUIRE(taken.has_value());
        REQUIRE(taken->sort.has_value());
        CHECK(taken->sort->front().column == "name");
        CHECK(taken->view == std::optional<std::string>{"Mine"});
    }
}
