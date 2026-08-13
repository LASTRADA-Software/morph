// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <string>

// Guarded exactly like bookmark_presenter.hpp's own includes: AUTOMOC runs
// moc over this header, and moc must not be pointed at morph's template-heavy
// bridge.hpp or at the model headers, which pull in Lightweight's DataMapper
// machinery — moc is not a C++ front end and mis-parses it, emitting the rest
// of the file inside a namespace it wrongly believes is still open. moc needs
// nothing from these headers: the macros, signals and `Q_INVOKABLE`
// signatures below are all it reads.
#ifndef Q_MOC_RUN
#include "bookmark_forms_controller.hpp"
#include "bookmark_presenter.hpp"
#include "shared_feed_presenter.hpp"
#include "tag_presenter.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

/// @file
/// The four QML-facing adapters bookmarks' shells put in front of the Task 17
/// presenters and this rung's forms controller. They live in `gui_lib` — not
/// in a shell's `main.cpp` — because every shell needs them and they must all
/// be the same program: `gui/main.cpp` (desktop) and a future
/// `gui_wasm/main_wasm.cpp` (browser) are to differ only in how they choose a
/// deployment mode, per `examples/TESTING.md`'s "same client code"
/// requirement. Same rationale, same shape and the same Qt6::Core-only bound
/// as `pastebin::gui`'s `FormsBridge`/`PasteBridge`
/// (`examples/pastebin/gui_lib/paste_qml_bridges.hpp`) — read that file's
/// "Why these adapters exist at all", "Qt6::Core only" and "Threading"
/// sections, which apply here verbatim and are not repeated.
///
/// @par Why there is no separate `AuthBridge`
/// The login step is folded into `FormsBridge` rather than given a class of
/// its own, and that is a deliberate deviation from this task's brief. A
/// standalone `AuthBridge` taking `(Bridge&, IExecutor*)` — the presenter
/// rule-2 constructor every adapter here has — would have to own a second
/// `BookmarkFormsController`, and therefore a second `BridgeHandler` for
/// *each* of this rung's three form-serving models: six registered instances
/// per client where four is the number `bookmarks::app::App`'s own
/// `kMaxLiveModels` comment budgets for. The alternative (handing one
/// controller to two adapters) breaks that constructor rule instead. Login is
/// a schema-driven form submission like every other in this rung, so the
/// class that already submits schema-driven forms is where it belongs; the
/// one thing that makes it special — installing the returned token as the
/// bridge's default session — is `onLoginSucceeded` below, and it is the only
/// place in the whole client that touches a session.

namespace bookmarks::gui {

#ifndef Q_MOC_RUN
/// @brief Decodes a `Login` reply body into a `LoginResult`, or reports that
///        it could not be decoded.
///
/// A named function rather than four lines inside `FormsBridge::submitIfValid`
/// for one reason: its failure arm is otherwise untestable. The reply that
/// reaches `submitIfValid`'s success callback is always produced by
/// `ActionTraits<Login>::resultToJson` — glaze writing the *same* reflected
/// type this reads back — on every backend the ladder ships (`LocalBackend`,
/// `SimulatedRemoteBackend`, `QtWebSocketBackend`), so no test driving a real
/// client can make that decode fail. The branch is still worth having and
/// still worth testing: the peer is a separate process that a real deployment
/// can have upgraded, downgraded or replaced independently of the client, and
/// the alternative to reporting a failed decode is installing a
/// default-constructed (tokenless) session and announcing an empty principal
/// as if login had worked. Splitting the decision out makes both arms
/// reachable from `tests/test_bookmark_qml_bridges.cpp` without a fake
/// backend, and leaves the caller with a single unambiguous branch.
///
/// @param resultJson The reply body, verbatim as the dispatch resolved it.
/// @return The decoded result, or `std::nullopt` if @p resultJson is not a
///         readable `LoginResult`.
[[nodiscard]] std::optional<LoginResult> decodeLoginResult(const std::string& resultJson);
#endif

/// @brief QML-facing face of `bookmarks::gui::BookmarkFormsController`, plus
///        this client's one session-installing seam.
///
/// Same surface `DynamicForm.qml` expects of a controller — a `schemasJson`
/// property, `submitIfValid(actionType, bodyJson)`, and a `replyReceived`
/// signal — so the shipped renderer needs no bookmarks-specific knowledge,
/// and one instance serves the login screen and every domain form alike.
class FormsBridge : public QObject {
    Q_OBJECT

    /// @brief `{actionType: schema}` JSON — everything the QML renderer needs.
    Q_PROPERTY(QString schemasJson READ schemasJson CONSTANT)

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    FormsBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief The schema document supplied to the wrapped controller
    ///        (`bookmark_schemas.hpp`).
    /// @return `{actionType: schema}` JSON.
    [[nodiscard]] QString schemasJson() const;

    /// @brief Dispatches @p bodyJson as @p actionType's body, emitting
    ///        `replyReceived` when the reply (or the error) arrives — and,
    ///        for a successful `Login`, `loggedIn` after the returned token
    ///        has been installed as the bridge's default session.
    /// @param actionType Registered action type id.
    /// @param bodyJson   Fully-assembled JSON body, as `DynamicForm` builds it.
    Q_INVOKABLE void submitIfValid(const QString& actionType, const QString& bodyJson);

  signals:
    /// @brief Emitted once per `submitIfValid`. @p payload is the result JSON
    ///        when @p ok, otherwise the error message.
    /// @param actionType The action the reply belongs to.
    /// @param ok         Whether the dispatch succeeded.
    /// @param payload    Result JSON, or the error message.
    void replyReceived(const QString& actionType, bool ok, const QString& payload);

    /// @brief Emitted after a successful `Login` has been *applied* — i.e.
    ///        after the token is installed, so a slot may dispatch straight
    ///        away. Ordered before the corresponding `replyReceived`.
    /// @param principal The verified username the server echoed back.
    void loggedIn(const QString& principal);

  private:
#ifndef Q_MOC_RUN
    /// @brief Installs @p result's token as the shared `Bridge`'s default
    ///        session, so every subsequent action from every adapter carries
    ///        it, then announces the new identity.
    ///
    /// The whole of this client's authentication handling, and deliberately
    /// so: this is infrastructure wiring, not business logic
    /// (`examples/IMPLEMENTATION.md` rule 2's "(b) pure glue" clause). It
    /// decides nothing — the token is the server's, minted and signed by it,
    /// and `principal` is the server's echo of the identity it verified, not
    /// the client's claim (`bookmarks/dto/auth_dto.hpp`).
    /// @param result The decoded `LoginResult` the server returned.
    void onLoginSucceeded(const LoginResult& result);

    ::morph::bridge::Bridge& _bridge;
    BookmarkFormsController _controller;
#endif
};

/// @brief QML-facing face of `bookmarks::gui::BookmarkPresenter`.
///
/// Turns the presenter's DTO-carrying signals into `QVariantMap`/
/// `QVariantList` property bags and its typed calls into id invokables. No
/// decisions: ownership, tag diffing, archive filtering and pagination are
/// all the model's, and this only relays what the server computed.
///
/// `create`/`edit`/`import` are absent on purpose: those are the
/// schema-driven forms `FormsBridge` submits, so their replies arrive on
/// `replyReceived`, and relaying a presenter signal nothing binds to would be
/// a stub (the same exclusion `pastebin::gui::PasteBridge` documents for
/// `created`/`edited`). `getChangesSince`/`exportAll` are absent for the same
/// reason — this rung's shell shows neither a poll view nor an export
/// screen.
class BookmarkBridge : public QObject {
    Q_OBJECT

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    BookmarkBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Fetches the first page of the caller's own active bookmarks.
    Q_INVOKABLE void refresh();

    /// @brief Fetches the first page including archived bookmarks.
    Q_INVOKABLE void refreshIncludingArchived();

    /// @brief Reads one bookmark in full. Emits `loaded`, or `failed`.
    /// @param id The bookmark to read.
    Q_INVOKABLE void open(qlonglong id);

    /// @brief Archives one bookmark.
    /// @param id The bookmark to archive.
    Q_INVOKABLE void archive(qlonglong id);

    /// @brief Unarchives one bookmark.
    /// @param id The bookmark to unarchive.
    Q_INVOKABLE void unarchive(qlonglong id);

    /// @brief Deletes one bookmark.
    /// @param id The bookmark to delete.
    Q_INVOKABLE void remove(qlonglong id);

    /// @brief Archives or unarchives several bookmarks in one atomic
    ///        `BulkEdit` (all-or-nothing, README).
    ///
    /// Driven from the list's multi-selection rather than a form: `BulkEdit`'s
    /// required `ids` member is a JSON array, which the shipped `DynamicForm`
    /// has no control for — see `BookmarkFormsController`'s class comment. No
    /// text is typed here at all; the ids come from rows the user ticked.
    /// @param ids     The bookmarks to affect, as list-row ids.
    /// @param archive `true` to archive, `false` to unarchive.
    Q_INVOKABLE void bulkArchive(const QVariantList& ids, bool archive);

  signals:
    /// @brief One page of `ListBookmarks` rows, each an
    ///        `{id, url, title, tags, createdAt, updatedAt, readState,
    ///        archiveState, visibility}` map.
    /// @param rows The page's rows.
    void listed(const QVariantList& rows);
    /// @brief A fetched bookmark, as a property bag.
    /// @param bookmark The bookmark's fields, rendered as display strings.
    void loaded(const QVariantMap& bookmark);
    /// @brief An `ArchiveBookmark` succeeded.
    void archived();
    /// @brief An `UnarchiveBookmark` succeeded.
    void unarchived();
    /// @brief A `DeleteBookmark` succeeded.
    void removed();
    /// @brief A `BulkEdit` succeeded.
    /// @param affected How many rows the server reported changed.
    void bulkEdited(const QString& affected);
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

  private:
#ifndef Q_MOC_RUN
    BookmarkPresenter _presenter;
#endif
};

/// @brief QML-facing face of `bookmarks::gui::TagPresenter`.
///
/// Listing only. `RenameTag`/`MergeTags` are schema-driven forms submitted
/// through `FormsBridge`, so their outcomes arrive on `replyReceived` and the
/// presenter's `renamed`/`merged` signals are deliberately not relayed —
/// relaying a signal nothing binds to would be a stub.
class TagBridge : public QObject {
    Q_OBJECT

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    TagBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Fetches every tag the caller owns, with bookmark counts.
    Q_INVOKABLE void refresh();

  signals:
    /// @brief Every tag the caller owns, each an `{id, name, bookmarkCount}` map.
    /// @param rows The tag rows.
    void listed(const QVariantList& rows);
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

  private:
#ifndef Q_MOC_RUN
    TagPresenter _presenter;
#endif
};

/// @brief QML-facing face of `bookmarks::gui::SharedFeedPresenter`.
///
/// The one cross-user view in this rung: every `Shared`, non-archived
/// bookmark from every owner. Same row shape as `BookmarkBridge::listed`,
/// because the model returns the same `BookmarkSummary` (and the same
/// non-leak rule applies — no `notes`).
class SharedFeedBridge : public QObject {
    Q_OBJECT

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    SharedFeedBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Fetches the first page of the shared feed.
    Q_INVOKABLE void refresh();

  signals:
    /// @brief One page of the shared feed, in `BookmarkBridge::listed`'s row shape.
    /// @param rows The page's rows.
    void listed(const QVariantList& rows);
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

  private:
#ifndef Q_MOC_RUN
    SharedFeedPresenter _presenter;
#endif
};

}  // namespace bookmarks::gui
