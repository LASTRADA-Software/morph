// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/dto/analysis_dto.hpp"
#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
#include "lims/dto/verification_dto.hpp"

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <string>

#ifndef Q_MOC_RUN
#include <morph/util/quantity.hpp>
#endif

/// @file
/// DTO → `QVariantMap` translation shared by this rung's two QML bridges.
///
/// Extracted rather than duplicated in each bridge's anonymous namespace
/// (kanban's shape) because both surfaces render a `SampleView`, and two
/// copies of the same conversion is exactly how two screens end up disagreeing
/// about what "state" is spelled.
///
/// @par Conventions, stated once
/// - An id is a plain number, `-1` when unengaged. A real key is never `-1`
///   (`ServerSideAutoIncrement` starts at 1), so the sentinel is unambiguous.
/// - A `Quantity` crosses as **two** keys: `valueText`, the exact decimal
///   rendered at its own precision, and `hasValue`. Never as a `double` — a
///   lab result that a binding could round on its way to a label is the one
///   thing this rung exists to prevent. Arithmetic on it belongs in the
///   model, so QML has no reason to want a number.
/// - An enum crosses as its stable ascii name, not its integer, for the same
///   reason the journal records names.

namespace lims::gui {

/// @brief An id as the plain number QML rows and invokables carry.
/// @tparam Id A strong id type exposing `hasValue()`/`operator*`.
/// @param id The id to convert.
/// @return Its number, or `-1` when unengaged.
template <typename Id>
[[nodiscard]] inline qlonglong idNumber(const Id& id) {
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief A `string_view` as a `QString`.
/// @param text The text to convert.
/// @return The Qt string.
[[nodiscard]] inline QString qtext(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// @brief A timestamp as epoch milliseconds, or `-1` when unengaged.
/// @param stamp The timestamp to convert.
/// @return Epoch milliseconds, or `-1`.
[[nodiscard]] inline qlonglong millis(const ::morph::time::Timestamp& stamp) {
    return stamp.hasValue() ? static_cast<qlonglong>((*stamp.value).value.time_since_epoch().count()) : -1;
}

/// @brief A quantity's unit, as its display text (`"mg/L"`).
/// @tparam Q The `Quantity` specialisation.
/// @return The unit's display string.
template <typename Q>
[[nodiscard]] inline QString unitText() {
    return qtext(Q::unitMeta().display);
}

/// @brief A quantity's exact decimal text **without** its unit, or an empty
///        string when the quantity is empty.
///
/// `morph::units::toDecimalString` renders the exact decimal with no unit
/// suffix; `unitText<Q>()` renders the unit. The two are separate keys here so
/// a view can place them independently — a column header, a right-aligned
/// suffix, or nothing at all for a single-unit table — rather than being handed
/// one pre-joined string.
///
/// The blank for an empty quantity is this layer's choice, not the value
/// type's: `toDecimalString` reports an absent value as `"N/A"` — so that
/// `toString == toDecimalString + display` holds for every value — and a QML
/// view showing an unfilled measurement wants an empty cell, not the letters
/// `N/A` baked into its text. See morph#199.
/// @tparam Q The `Quantity` specialisation.
/// @param quantity The value to render.
/// @return Its shortest exact decimal at its own precision, or `""`.
template <typename Q>
[[nodiscard]] inline QString valueText(const Q& quantity) {
    if (!quantity.hasValue()) {
        return QString{};
    }
    return QString::fromStdString(::morph::units::toDecimalString(quantity));
}

/// @brief One `SampleView` as the property bag both surfaces bind against.
/// @param view The sample.
/// @return Its property bag.
[[nodiscard]] inline QVariantMap toVariantMap(const SampleView& view) {
    return QVariantMap{
        {"id", idNumber(view.id)},
        {"clientId", idNumber(view.clientId)},
        {"reference", QString::fromStdString(view.reference)},
        {"state", qtext(stateName(view.state))},
        {"version", static_cast<qlonglong>(*view.version)},
        {"registeredAtMs", millis(view.registeredAt)},
    };
}

/// @brief One `AnalysisVersionView` as the analysis picker's row.
/// @param view The analysis version.
/// @return Its property bag.
[[nodiscard]] inline QVariantMap toVariantMap(const AnalysisVersionView& view) {
    return QVariantMap{
        {"versionId", idNumber(view.id)},
        {"analysisId", idNumber(view.analysisId)},
        {"name", QString::fromStdString(view.name)},
        {"version", static_cast<qlonglong>(view.version)},
        {"canonicalUnit", QString::fromStdString(view.canonicalUnit)},
        {"decimalPlaces", static_cast<qlonglong>(view.decimalPlaces)},
    };
}

/// @brief One `ResultView` as the result table's row.
/// @param view The result.
/// @return Its property bag.
[[nodiscard]] inline QVariantMap toVariantMap(const ResultView& view) {
    return QVariantMap{
        {"id", idNumber(view.id)},
        {"sampleId", idNumber(view.sampleId)},
        {"analysisVersionId", idNumber(view.analysisVersionId)},
        // The qualifier's own name, and the value as exact text. A row is a
        // reading when `hasValue` is true and one of the three "no number"
        // claims otherwise — the same distinction the wire keeps, carried
        // through to the screen rather than collapsed into a blank cell.
        {"qualifier", qtext(qualifierName(view.qualifier))},
        {"valueText", valueText(view.value)},
        {"unit", unitText<Concentration>()},
        {"hasValue", view.value.hasValue()},
        {"capturedBy", QString::fromStdString(view.capturedBy)},
        {"capturedAtMs", millis(view.capturedAt)},
    };
}

/// @brief One `VerificationView` as the verification table's row.
/// @param view The verification.
/// @return Its property bag.
[[nodiscard]] inline QVariantMap toVariantMap(const VerificationView& view) {
    return QVariantMap{
        {"id", idNumber(view.id)},
        {"resultId", idNumber(view.resultId)},
        {"sampleId", idNumber(view.sampleId)},
        {"capturedBy", QString::fromStdString(view.capturedBy)},
        {"verifiedBy", QString::fromStdString(view.verifiedBy)},
        {"verifiedAtMs", millis(view.verifiedAt)},
    };
}

/// @brief One `ConflictView` as the conflict list's row.
/// @param view The conflict.
/// @return Its property bag.
[[nodiscard]] inline QVariantMap toVariantMap(const ConflictView& view) {
    return QVariantMap{
        {"id", idNumber(view.id)},
        {"sampleId", idNumber(view.sampleId)},
        {"baseVersion", static_cast<qlonglong>(*view.baseVersion)},
        {"serverVersion", static_cast<qlonglong>(*view.serverVersion)},
        {"reason", qtext(conflictReasonName(view.reason))},
        {"status", qtext(conflictStatusName(view.status))},
        {"detectedBy", QString::fromStdString(view.detectedBy)},
        {"resolvedBy", QString::fromStdString(view.resolvedBy)},
        {"resolutionNote", QString::fromStdString(view.resolutionNote)},
    };
}

/// @brief Every row in @p rows as a `QVariantList` of property bags.
/// @tparam Rows A range of DTO rows.
/// @param rows The rows to convert.
/// @return The list.
template <typename Rows>
[[nodiscard]] inline QVariantList toVariantList(const Rows& rows) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows.size()));
    for (const auto& row : rows) {
        out.append(toVariantMap(row));
    }
    return out;
}

}  // namespace lims::gui
