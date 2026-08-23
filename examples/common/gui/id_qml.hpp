// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QtGlobal>
#include <cstdint>

/// @file
/// Strong id <-> QML scalar conversion, once.
///
/// Every ladder QML bridge that puts an entity id in a `QVariantMap` row, or
/// takes one back through a `Q_INVOKABLE`, needs the same unwrapping, and
/// nothing about it is per-model: a strong id in, a number or a text out, and
/// back. `docs/analysis/qml-bridge-boilerplate.md` measured the rest of the
/// bridge duplication as O(QML surface) and largely irreducible; this is the
/// second of the two pieces it identified as genuinely model-independent and
/// worth extracting (morph#169), after `errorText()` (morph#168).
///
/// @par What an "id" is here
/// Nothing in this header names a model type. It works on anything with
/// `hasValue()` and `operator*()` and a `T{std::int64_t}` construction, which
/// covers both id shapes the ladder actually has — and they are **not** the
/// same shape:
///
/// - **Optional-backed** (`ledger::LedgerId`, `bookmarks::BookmarkId`,
///   `kanban::TaskId`, `lims::SampleId`, …): payload is
///   `std::optional<std::int64_t>`, `hasValue()` is `has_value()`. Here a
///   disengaged id and an id holding `0` are genuinely different values.
/// - **Zero-sentinel** (`polls::OptionId`, `polls::PollEventId`,
///   `kanban::BoardEventId`): payload is a plain `std::int64_t` whose own
///   documented "not entered" value is `0`, so `hasValue()` is `value != 0`.
///   Here `Id{0}` *is* the empty state, in the model type — one layer below
///   this header. No QML-boundary conversion can reintroduce a distinction
///   the id type does not carry, and these functions do not pretend to.
///
/// So the guarantee below is stated in terms of `hasValue()`, which is the
/// only thing that is true of both: **whatever the id calls empty maps to the
/// empty representation, and everything the id calls engaged — `0` included —
/// maps to its own payload.** For an optional-backed id that is exactly the
/// unset-vs-zero distinction morph#169 asks for; for a zero-sentinel id it is
/// the strongest statement that is not a lie.

namespace morph::ladder::gui {

/// @brief The number a QML row or invokable carries in place of an id whose
///        `hasValue()` is `false`.
///
/// `-1`, not `0`: a ladder id is a Lightweight `ServerSideAutoIncrement`
/// surrogate key and those start at `1`, so `-1` names no row, while `0` is a
/// real value for an optional-backed id and the *empty* value for a
/// zero-sentinel one — either way `0` cannot also mean "no id" without
/// collapsing two states into one.
///
/// It is a named constant rather than a literal spelled out at nine call
/// sites because the QML half of the contract depends on it too
/// (`ProjectListView.qml`'s `membersProjectId: -1`, `BoardView.qml`'s
/// "-1 means accept any", `SampleView.qml`'s fallback), and a sentinel that
/// only exists as a literal is a sentinel each side is free to re-pick.
inline constexpr qlonglong kNoId = -1;

/// @brief An id as the plain number QML rows and invokables carry.
/// @tparam Id A strong id type exposing `hasValue()` and `operator*()`.
/// @param id The id to render.
/// @return `*id` when @p id `hasValue()`, `kNoId` otherwise. An engaged id
///         holding `0` returns `0`, which is distinct from `kNoId`.
template <typename Id>
[[nodiscard]] qlonglong idNumber(const Id& id) {
    return id.hasValue() ? static_cast<qlonglong>(*id) : kNoId;
}

/// @brief An id as the decimal text QML rows and invokables carry, for the
///        surfaces whose ids are `QString`-shaped rather than number-shaped
///        (kanban's board invokables, ledger's budget/rule views).
/// @tparam Id A strong id type exposing `hasValue()` and `operator*()`.
/// @param id The id to render.
/// @return The payload's decimal digits when @p id `hasValue()`, an empty
///         `QString` otherwise. An engaged id holding `0` returns `"0"`,
///         which is distinct from the empty string.
template <typename Id>
[[nodiscard]] QString idText(const Id& id) {
    return id.hasValue() ? QString::number(static_cast<qlonglong>(*id)) : QString{};
}

/// @brief Parses QML's id text back into a strong id.
///
/// Deliberately does not throw or otherwise reject: every caller hands the
/// result straight to a model action whose own `validate()` refuses an
/// unengaged id with a typed error, which the presenter relays as `failed`.
/// Diagnosing "you did not type an id" twice, in two vocabularies, is worse
/// than diagnosing it once where the message is already written.
///
/// @tparam Id A strong id type constructible from `std::int64_t` and
///         default-constructible into its empty state.
/// @param text The id as QML passed it.
/// @return `Id{value}` when @p text parses as a whole number, a
///         default-constructed (empty) `Id` otherwise — which is what an
///         empty `QString`, the output of `idText()` on an empty id, gives.
template <typename Id>
[[nodiscard]] Id idFromText(const QString& text) {
    bool ok = false;
    const qlonglong value = text.toLongLong(&ok);
    return ok ? Id{static_cast<std::int64_t>(value)} : Id{};
}

}  // namespace morph::ladder::gui
