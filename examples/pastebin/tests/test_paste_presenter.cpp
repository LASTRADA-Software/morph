// SPDX-License-Identifier: Apache-2.0
//
// PastePresenter's own suite (Task 11): each of the five actions
// (create/get/edit/remove/list) round-trips through the presenter's own
// signals — not the model directly — across the full BackendRig mode matrix
// (Local/LocalSingleThread/Socket, examples/TESTING.md "The dual-mode
// fixture"), plus the `failed` signal path for an unknown id. Domain rules
// (validation, burn-after-read, expiry, keyspace collisions, ...) already
// have a dedicated suite at the model level (test_paste_model.cpp); this
// file only proves the presenter wires each action to the right signal, sets
// `busy()`/`idle()` correctly, and neither crashes nor hangs — the
// "translates and routes only" contract paste_presenter.hpp's own doc
// comment states (examples/IMPLEMENTATION.md rule 2).
//
// Step 2 of Task 11 (one offscreen QML engine-load smoke test, TESTING.md
// presenter rule 6) is deliberately not attempted here: it needs Task 12's
// Main.qml to exist first, per the plan.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "paste_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <Lightweight/SqlStatement.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

[[nodiscard]] pastebin::CreatePaste makeCreate(std::string content, std::string syntax = "text") {
    pastebin::CreatePaste create;
    create.content = std::move(content);
    create.syntax = std::move(syntax);
    return create;
}

}  // namespace

TEST_CASE("PastePresenter::create then get round-trips a paste, all three backend modes",
          "[pastebin][presenter]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::PastePresenter presenter{rig.bridge(0), rig.executor()};

    pastebin::PasteId createdId;
    bool created = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::created,
                      [&](pastebin::CreatePasteResult result) {
                          createdId = result.id;
                          created = true;
                      });
    presenter.create(makeCreate("presenter round-trip"));
    REQUIRE(pumpUntil([&] { return created; }));
    REQUIRE_FALSE(presenter.busy());
    REQUIRE(createdId.hasValue());

    pastebin::PasteView loaded;
    bool gotLoaded = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::loaded, [&](pastebin::PasteView view) {
        loaded = view;
        gotLoaded = true;
    });
    presenter.get(pastebin::GetPaste{.id = createdId});
    REQUIRE(pumpUntil([&] { return gotLoaded; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(loaded.id == createdId);
    CHECK(loaded.content == "presenter round-trip");
    CHECK(loaded.syntax == "text");
}

TEST_CASE("PastePresenter::edit replaces an editable paste's content and syntax, all three backend modes",
          "[pastebin][presenter]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::PastePresenter presenter{rig.bridge(0), rig.executor()};

    pastebin::PasteId createdId;
    bool created = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::created,
                      [&](pastebin::CreatePasteResult result) {
                          createdId = result.id;
                          created = true;
                      });
    auto create = makeCreate("before edit");
    create.editability = pastebin::Editability::Editable;
    presenter.create(create);
    REQUIRE(pumpUntil([&] { return created; }));

    pastebin::PasteView edited;
    bool gotEdited = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::edited, [&](pastebin::PasteView view) {
        edited = view;
        gotEdited = true;
    });
    presenter.edit(pastebin::EditPaste{.id = createdId, .content = "after edit", .syntax = "cpp"});
    REQUIRE(pumpUntil([&] { return gotEdited; }));
    REQUIRE_FALSE(presenter.busy());
    CHECK(edited.id == createdId);
    CHECK(edited.content == "after edit");
    CHECK(edited.syntax == "cpp");

    // Persisted, not merely reflected back from the action.
    pastebin::PasteView reloaded;
    bool gotReloaded = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::loaded, [&](pastebin::PasteView view) {
        reloaded = view;
        gotReloaded = true;
    });
    presenter.get(pastebin::GetPaste{.id = createdId});
    REQUIRE(pumpUntil([&] { return gotReloaded; }));
    CHECK(reloaded.content == "after edit");
    CHECK(reloaded.syntax == "cpp");
}

TEST_CASE("PastePresenter::remove deletes a paste, and a follow-up get fails, all three backend modes",
          "[pastebin][presenter]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::PastePresenter presenter{rig.bridge(0), rig.executor()};

    pastebin::PasteId createdId;
    bool created = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::created,
                      [&](pastebin::CreatePasteResult result) {
                          createdId = result.id;
                          created = true;
                      });
    presenter.create(makeCreate("doomed"));
    REQUIRE(pumpUntil([&] { return created; }));

    bool removed = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::removed, [&] { removed = true; });
    presenter.remove(pastebin::DeletePaste{.id = createdId});
    REQUIRE(pumpUntil([&] { return removed; }));
    REQUIRE_FALSE(presenter.busy());

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.get(pastebin::GetPaste{.id = createdId});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("PastePresenter::list returns the pastes just created, all three backend modes",
          "[pastebin][presenter]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::PastePresenter presenter{rig.bridge(0), rig.executor()};

    std::vector<pastebin::PasteId> createdIds;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::created,
                      [&](pastebin::CreatePasteResult result) { createdIds.push_back(result.id); });

    constexpr int kCount = 3;
    for (int i = 0; i < kCount; ++i) {
        presenter.create(makeCreate("listed " + std::to_string(i)));
        REQUIRE(pumpUntil([&] { return static_cast<int>(createdIds.size()) == i + 1; }));
    }
    REQUIRE(createdIds.size() == static_cast<std::size_t>(kCount));

    pastebin::ListPastesResult listed;
    bool gotListed = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::listed, [&](pastebin::ListPastesResult result) {
        listed = std::move(result);
        gotListed = true;
    });
    presenter.list(pastebin::ListPastes{});
    REQUIRE(pumpUntil([&] { return gotListed; }));
    REQUIRE_FALSE(presenter.busy());

    REQUIRE(listed.pastes.size() == static_cast<std::size_t>(kCount));
    for (const auto& id : createdIds) {
        CHECK(std::ranges::find_if(listed.pastes, [&](const pastebin::PasteSummary& summary) {
                  return summary.id == id;
              }) != listed.pastes.end());
    }
}

TEST_CASE("Every PastePresenter action routes its failure to failed(), not just get()",
          "[pastebin][presenter]") {
    // `get`'s error path has its own case below; this covers the other four.
    // Not a completeness ritual: `track()`'s third argument is attached
    // per-call, and `Completion<T>::onError` keeps only the *last* handler
    // attached (docs/findings/023), so a mis-wired `onErr` on one action is
    // invisible from every other action's tests — the busy counter still
    // clears (that is `track()`'s own surviving handler) and the error simply
    // vanishes. That is precisely the failure mode finding 023 describes, and
    // it can only be caught per action.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::PastePresenter presenter{rig.bridge(0), rig.executor()};

    QString failure;
    int failures = 0;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::failed, [&](QString message) {
        failure = message;
        ++failures;
    });

    // create: empty content fails CreatePaste::validate().
    presenter.create(makeCreate(""));
    REQUIRE(pumpUntil([&] { return failures == 1; }));
    CHECK(failure.contains("CreatePaste"));
    REQUIRE_FALSE(presenter.busy());

    // edit: an id nothing was ever stored under.
    presenter.edit(pastebin::EditPaste{.id = pastebin::PasteId{"no-such-paste"}, .content = "x", .syntax = "text"});
    REQUIRE(pumpUntil([&] { return failures == 2; }));
    CHECK(failure.contains("EditPaste"));
    REQUIRE_FALSE(presenter.busy());

    // remove: a disengaged id fails DeletePaste::validate(). (An id that
    // merely does not exist is deliberately *not* an error — deleting is
    // idempotent by design, see test_paste_model.cpp.)
    presenter.remove(pastebin::DeletePaste{});
    REQUIRE(pumpUntil([&] { return failures == 3; }));
    CHECK(failure.contains("DeletePaste"));
    REQUIRE_FALSE(presenter.busy());

    // list: the one action with no validation failure at all — every
    // `ListPastes` is well-formed. Its error path is reachable only through a
    // genuine store error, so provoke one the way docs/findings/018's
    // resolution line prescribes (a real failure through the schema, not a
    // mock): drop the table out from under the query. `DbFixture` re-creates
    // the schema for the next test case, so this is contained.
    {
        ::Lightweight::SqlStatement stmt;
        (void) stmt.ExecuteDirect("DROP TABLE pastes");
    }
    presenter.list(pastebin::ListPastes{});
    REQUIRE(pumpUntil([&] { return failures == 4; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}

TEST_CASE("PastePresenter::get against an unknown id emits failed, not a crash", "[pastebin][presenter]") {
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::PastePresenter presenter{rig.bridge(0), rig.executor()};

    QString failure;
    bool failed = false;
    QObject::connect(&presenter, &pastebin::gui::PastePresenter::failed, [&](QString message) {
        failure = message;
        failed = true;
    });
    presenter.get(pastebin::GetPaste{.id = pastebin::PasteId{"no-such-paste"}});
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(failure.isEmpty());
    REQUIRE_FALSE(presenter.busy());
}
