// SPDX-License-Identifier: Apache-2.0
#include "paste_qml_bridges.hpp"

#include <QString>
#include <exception>
#include <string>
#include <utility>

#include "gui/error_text.hpp"
#include "paste_schemas.hpp"

namespace pastebin::gui {

namespace {

/// @brief Renders an optional instant as ISO-8601, or an empty string.
[[nodiscard]] QString isoOrEmpty(const ::morph::time::Timestamp& instant) {
    return instant.hasValue() ? QString::fromStdString((*instant).toIso8601()) : QString{};
}

/// @brief Renders a read count via `morph::units::toString` (`"N/A"` when the
///        quantity is empty, i.e. "no burn limit").
///
/// `morph::units::toString`, not `std::format("{}", reads)`: the two produce
/// identical text (the `std::formatter<Quantity>` specialization delegates to
/// the same function), but Emscripten's bundled libc++ fails to compile the
/// `std::format` call outright — see `toString`'s own doc comment
/// (`include/morph/util/quantity.hpp`) for why.
[[nodiscard]] QString readsText(const pastebin::Reads& reads) {
    return QString::fromStdString(morph::units::toString(reads));
}

/// @brief `PasteId` as plain text (empty when unengaged).
[[nodiscard]] QString idText(const pastebin::PasteId& id) {
    return id.hasValue() ? QString::fromStdString(*id) : QString{};
}

/// @brief A `PasteView` as the property bag `PasteView.qml` binds against.
[[nodiscard]] QVariantMap toVariantMap(const pastebin::PasteView& view) {
    return QVariantMap{
        {"id", idText(view.id)},
        {"content", QString::fromStdString(view.content)},
        {"syntax", QString::fromStdString(view.syntax)},
        {"createdAt", isoOrEmpty(view.createdAt)},
        {"expiresAt", isoOrEmpty(view.expiresAt)},
        {"burnAfterReads", readsText(view.burnAfterReads)},
        {"readCount", readsText(view.readCount)},
        {"visibility",
         view.visibility == pastebin::Visibility::Private ? QStringLiteral("Private") : QStringLiteral("Public")},
        {"editability", view.editability == pastebin::Editability::Editable ? QStringLiteral("Editable")
                                                                            : QStringLiteral("Immutable")},
    };
}

/// @brief One `ListPastes` row as the property bag the list delegate binds
///        against. Narrower than `toVariantMap` because `PasteSummary` is
///        narrower than `PasteView` on purpose — a listing must not leak
///        paste content (`pastebin/dto/paste_dto.hpp`).
[[nodiscard]] QVariantMap toVariantMap(const pastebin::PasteSummary& summary) {
    return QVariantMap{
        {"id", idText(summary.id)},
        {"syntax", QString::fromStdString(summary.syntax)},
        {"createdAt", isoOrEmpty(summary.createdAt)},
        {"visibility",
         summary.visibility == pastebin::Visibility::Private ? QStringLiteral("Private") : QStringLiteral("Public")},
    };
}

}  // namespace

FormsBridge::FormsBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _controller{bridge, executor, pasteSchemasJson()} {}

QString FormsBridge::schemasJson() const { return QString::fromStdString(_controller.schemasJson()); }

void FormsBridge::submitIfValid(const QString& actionType, const QString& bodyJson) {
    _controller.submitIfValid(
        actionType.toStdString(), bodyJson.toStdString(),
        [this, actionType](std::string resultJson) {
            emit replyReceived(actionType, true, QString::fromStdString(resultJson));
        },
        [this, actionType](const std::exception_ptr& err) {
            emit replyReceived(actionType, false, ::morph::ladder::gui::errorText(err));
        });
}

PasteBridge::PasteBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    // Direct (same-thread) connections throughout — see this header's
    // "Threading" note for why no meta-type registration is involved.
    connect(&_presenter, &PastePresenter::bound, this, &PasteBridge::bound);
    connect(&_presenter, &PastePresenter::listed, this, [this](const pastebin::ListPastesResult& result) {
        QVariantList rows;
        rows.reserve(static_cast<qsizetype>(result.pastes.size()));
        for (const auto& summary : result.pastes) {
            rows.append(toVariantMap(summary));
        }
        emit listed(rows);
    });
    connect(&_presenter, &PastePresenter::loaded, this,
            [this](const pastebin::PasteView& view) { emit loaded(toVariantMap(view)); });
    // `PastePresenter::created`/`edited` are deliberately not relayed:
    // creating goes through the schema-driven form (FormsBridge above), so
    // its reply arrives on `replyReceived`, and this rung's shell ships no
    // edit screen. Relaying a signal nothing binds to would be a stub.
    connect(&_presenter, &PastePresenter::removed, this, &PasteBridge::removed);
    connect(&_presenter, &PastePresenter::failed, this, &PasteBridge::failed);
}

void PasteBridge::refresh() { _presenter.list(pastebin::ListPastes{}); }

void PasteBridge::open(const QString& id) {
    _presenter.get(pastebin::GetPaste{.id = pastebin::PasteId{id.toStdString()}});
}

void PasteBridge::remove(const QString& id) {
    _presenter.remove(pastebin::DeletePaste{.id = pastebin::PasteId{id.toStdString()}});
}

}  // namespace pastebin::gui
