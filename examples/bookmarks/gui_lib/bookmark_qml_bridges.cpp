// SPDX-License-Identifier: Apache-2.0
#include "bookmark_qml_bridges.hpp"
#include "gui/error_text.hpp"

#include "bookmark_schemas.hpp"

#include <morph/session/session.hpp>

#include <QString>
#include <QVariant>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bookmarks::gui {

namespace {

/// @brief Renders an optional instant as ISO-8601, or an empty string.
[[nodiscard]] QString isoOrEmpty(const ::morph::time::Timestamp& instant) {
    return instant.hasValue() ? QString::fromStdString((*instant).toIso8601()) : QString{};
}

/// @brief A count rendered via `morph::units::toString` (`"N/A"` when empty).
///
/// `morph::units::toString`, not `std::format("{}", count)`: see
/// `pastebin::gui::readsText`'s identical note (`paste_qml_bridges.cpp`) —
/// Emscripten's bundled libc++ fails to compile the `std::format` call for
/// this `Quantity`-family type outright.
[[nodiscard]] QString countText(const Count& count) {
    return QString::fromStdString(morph::units::toString(count));
}

/// @brief A `BookmarkId` as the plain number QML rows carry, or `-1` when
///        unengaged. `-1` is never a real surrogate key (Lightweight's
///        `ServerSideAutoIncrement` starts at 1), so it is unambiguous, and a
///        number — not a string — is what `open`/`archive`/`remove` take.
[[nodiscard]] qlonglong idNumber(const BookmarkId& id) {
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief A `TagId` as the plain number tag rows carry. See `idNumber`.
[[nodiscard]] qlonglong idNumber(const TagId& id) {
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief Tag names as a QML string list.
[[nodiscard]] QVariantList tagList(const std::vector<std::string>& tags) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(tags.size()));
    for (const auto& tag : tags) {
        out.append(QString::fromStdString(tag));
    }
    return out;
}

[[nodiscard]] QString visibilityText(Visibility visibility) {
    return visibility == Visibility::Shared ? QStringLiteral("Shared") : QStringLiteral("Private");
}

[[nodiscard]] QString readStateText(ReadState state) {
    return state == ReadState::Read ? QStringLiteral("Read") : QStringLiteral("Unread");
}

[[nodiscard]] QString archiveStateText(ArchiveState state) {
    return state == ArchiveState::Archived ? QStringLiteral("Archived") : QStringLiteral("Active");
}

/// @brief A `BookmarkView` as the property bag the detail pane binds against.
[[nodiscard]] QVariantMap toVariantMap(const BookmarkView& view) {
    return QVariantMap{
        {"id", idNumber(view.id)},
        {"url", QString::fromStdString(view.url)},
        {"title", QString::fromStdString(view.title)},
        {"description", QString::fromStdString(view.description)},
        {"notes", QString::fromStdString(view.notes)},
        {"tags", tagList(view.tags)},
        {"createdAt", isoOrEmpty(view.createdAt)},
        {"updatedAt", isoOrEmpty(view.updatedAt)},
        {"readState", readStateText(view.readState)},
        {"archiveState", archiveStateText(view.archiveState)},
        {"visibility", visibilityText(view.visibility)},
    };
}

/// @brief One listing row as the property bag a list delegate binds against.
///        Narrower than `toVariantMap(const BookmarkView&)` because
///        `BookmarkSummary` is narrower than `BookmarkView` on purpose — a
///        listing must not leak `notes` (`bookmarks/dto/bookmark_dto.hpp`).
[[nodiscard]] QVariantMap toVariantMap(const BookmarkSummary& summary) {
    return QVariantMap{
        {"id", idNumber(summary.id)},
        {"url", QString::fromStdString(summary.url)},
        {"title", QString::fromStdString(summary.title)},
        {"tags", tagList(summary.tags)},
        {"createdAt", isoOrEmpty(summary.createdAt)},
        {"updatedAt", isoOrEmpty(summary.updatedAt)},
        {"readState", readStateText(summary.readState)},
        {"archiveState", archiveStateText(summary.archiveState)},
        {"visibility", visibilityText(summary.visibility)},
    };
}

/// @brief One `ListTags` row as the property bag the tag list binds against.
[[nodiscard]] QVariantMap toVariantMap(const TagSummary& summary) {
    return QVariantMap{
        {"id", idNumber(summary.id)},
        {"name", QString::fromStdString(summary.name)},
        {"bookmarkCount", countText(summary.bookmarkCount)},
    };
}

/// @brief Every summary in @p rows as a `QVariantList` of property bags.
template <typename Summaries>
[[nodiscard]] QVariantList toVariantList(const Summaries& rows) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows.size()));
    for (const auto& row : rows) {
        out.append(toVariantMap(row));
    }
    return out;
}

}  // namespace

std::optional<LoginResult> decodeLoginResult(const std::string& resultJson) {
    // The same glaze reflection the wire used, so nothing here parses JSON by
    // hand. `read_json` returns a truthy error context on failure.
    LoginResult result;
    if (glz::read_json(result, resultJson)) {
        return std::nullopt;
    }
    return result;
}

// ── FormsBridge ─────────────────────────────────────────────────────────────

FormsBridge::FormsBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _bridge{bridge}, _controller{bridge, executor, bookmarkSchemasJson()} {}

QString FormsBridge::schemasJson() const {
    return QString::fromStdString(_controller.schemasJson());
}

void FormsBridge::onLoginSucceeded(const LoginResult& result) {
    ::morph::session::Context session;
    session.principal = result.principal;
    session.token = result.token.hasValue() ? *result.token : std::string{};
    _bridge.setDefaultSession(session);
    emit loggedIn(QString::fromStdString(result.principal));
}

void FormsBridge::submitIfValid(const QString& actionType, const QString& bodyJson) {
    _controller.submitIfValid(
        actionType.toStdString(), bodyJson.toStdString(),
        [this, actionType](std::string resultJson) {
            // A successful Login is the one reply this client reads rather
            // than merely displays: the token has to be installed before
            // anything else dispatches. See `decodeLoginResult` for why the
            // decode is a named function.
            if (actionType == QLatin1String("Login")) {
                const auto result = decodeLoginResult(resultJson);
                if (!result) {
                    emit replyReceived(actionType, false,
                                       QStringLiteral("login succeeded but its reply could not be decoded"));
                    return;
                }
                onLoginSucceeded(*result);
                // The token has already done its one job -- installed onto
                // the session above -- so it has no further reason to leave
                // this function. `replyReceived` is broadcast to *every*
                // bound QML handler, and a future handler that renders
                // `payload` unconditionally would otherwise put a live
                // bearer credential on screen (and into any screenshot or
                // recording of it). Re-encoding a redacted copy keeps the
                // signal's shape unchanged (still `(actionType, ok,
                // payload)`) — only `Login`'s own payload stops carrying the
                // token, rather than a public QML surface change.
                LoginResult redacted = *result;
                redacted.token = AuthToken{};
                emit replyReceived(actionType, true, QString::fromStdString(glz::write_json(redacted).value_or("{}")));
                return;
            }
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        },
        [this, actionType](const std::exception_ptr& err) {
            emit replyReceived(actionType, false, ::morph::ladder::gui::errorText(err));
        });
}

// ── BookmarkBridge ──────────────────────────────────────────────────────────

BookmarkBridge::BookmarkBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    // Direct (same-thread) connections throughout — see
    // paste_qml_bridges.hpp's "Threading" note for why no meta-type
    // registration is involved.
    connect(&_presenter, &BookmarkPresenter::bound, this, &BookmarkBridge::bound);
    connect(&_presenter, &BookmarkPresenter::listed, this,
            [this](const ListBookmarksResult& result) { emit listed(toVariantList(result.bookmarks)); });
    connect(&_presenter, &BookmarkPresenter::loaded, this,
            [this](const BookmarkView& view) { emit loaded(toVariantMap(view)); });
    connect(&_presenter, &BookmarkPresenter::archived, this, &BookmarkBridge::archived);
    connect(&_presenter, &BookmarkPresenter::unarchived, this, &BookmarkBridge::unarchived);
    connect(&_presenter, &BookmarkPresenter::removed, this, &BookmarkBridge::removed);
    connect(&_presenter, &BookmarkPresenter::bulkEdited, this,
            [this](const BulkEditResult& result) { emit bulkEdited(countText(result.affected)); });
    connect(&_presenter, &BookmarkPresenter::failed, this, &BookmarkBridge::failed);
}

void BookmarkBridge::refresh() {
    _presenter.list(ListBookmarks{});
}

void BookmarkBridge::refreshIncludingArchived() {
    // Every member without a default initializer is named explicitly:
    // -Wmissing-designated-field-initializers is on under
    // MORPH_ENABLE_STRICT_COMPILATION. `.cursor = {}` is an empty cursor,
    // i.e. the first page; empty `tag`/`searchText` mean "no filter".
    _presenter.list(
        ListBookmarks{.cursor = {}, .archiveFilter = ArchiveFilter::Any, .tag = {}, .searchText = {}});
}

void BookmarkBridge::open(qlonglong id) {
    _presenter.get(GetBookmark{.id = BookmarkId{static_cast<std::int64_t>(id)}});
}

void BookmarkBridge::archive(qlonglong id) {
    _presenter.archive(ArchiveBookmark{.id = BookmarkId{static_cast<std::int64_t>(id)}});
}

void BookmarkBridge::unarchive(qlonglong id) {
    _presenter.unarchive(UnarchiveBookmark{.id = BookmarkId{static_cast<std::int64_t>(id)}});
}

void BookmarkBridge::remove(qlonglong id) {
    _presenter.remove(DeleteBookmark{.id = BookmarkId{static_cast<std::int64_t>(id)}});
}

void BookmarkBridge::bulkArchive(const QVariantList& ids, bool archive) {
    BulkEdit action;
    action.ids.reserve(static_cast<std::size_t>(ids.size()));
    for (const auto& id : ids) {
        action.ids.emplace_back(static_cast<std::int64_t>(id.toLongLong()));
    }
    action.archive = archive ? BulkArchiveOp::Archive : BulkArchiveOp::Unarchive;
    _presenter.bulkEdit(std::move(action));
}

// ── TagBridge ───────────────────────────────────────────────────────────────

TagBridge::TagBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &TagPresenter::bound, this, &TagBridge::bound);
    connect(&_presenter, &TagPresenter::listed, this,
            [this](const ListTagsResult& result) { emit listed(toVariantList(result.tags)); });
    connect(&_presenter, &TagPresenter::failed, this, &TagBridge::failed);
}

void TagBridge::refresh() {
    _presenter.list(ListTags{});
}

// ── SharedFeedBridge ────────────────────────────────────────────────────────

SharedFeedBridge::SharedFeedBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                    QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &SharedFeedPresenter::bound, this, &SharedFeedBridge::bound);
    connect(&_presenter, &SharedFeedPresenter::listed, this,
            [this](const ListSharedFeedResult& result) { emit listed(toVariantList(result.bookmarks)); });
    connect(&_presenter, &SharedFeedPresenter::failed, this, &SharedFeedBridge::failed);
}

void SharedFeedBridge::refresh() {
    _presenter.list(ListSharedFeed{});
}

}  // namespace bookmarks::gui
