// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The demo model and its actions. Everything a client needs to render these
/// as forms is generated from the declarations below:
///   - field types/structure  -> glaze reflection
///   - descriptions/bounds    -> glz::json_schema<A>
///   - units + decimals       -> the Quantity types (UnitTraits)
///   - required fields        -> member types + `optionalFields` opt-out
///   - readiness (validate)   -> morph::forms::allRequiredEngaged

#include <morph/choice.hpp>
#include <morph/datetime.hpp>
#include <morph/forms.hpp>
#include <morph/registry.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lab_units.hpp"

namespace lab {

/// @brief Compute dry density from oven-dry mass and mould volume.
struct ComputeDryDensity {
    Mass massDry{};

    /// Declared-precision override: this field is specified to 4 decimals,
    /// while the m³ unit default is 3 — the schema advertises
    /// `x-decimalPlaces: 4` and the form input adapts automatically.
    Quantity<Unit::m3, 4> volume{};

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

/// @brief One selectable sample (a row of `ListSamples`' result).
struct SampleInfo {
    std::int64_t id = 0;
    std::string name;
};

/// @brief Result of `ListSamples`.
struct SampleList {
    std::vector<SampleInfo> samples;
};

/// @brief Options provider for `RecordMeasurement.sampleId` — a pure query
///        the form renderers execute to populate the sample combo box.
struct ListSamples {};

/// @brief Record a measured density (moisture and note are optional).
struct RecordMeasurement {
    /// Not free input: options come from executing `ListSamples` — the
    /// schema carries x-optionsAction/x-optionValue/x-optionLabel and the
    /// renderers build a combo box from the result rows.
    morph::forms::Choice<std::int64_t, "ListSamples"> sampleId;

    /// When the measurement was taken (ISO-8601 UTC on the wire; renderers
    /// key on the schema's "format": "date-time").
    morph::time::Timestamp measuredAt;

    Density density{};
    Percent moisture{};
    std::optional<std::string> note{};

    static constexpr std::array optionalFields{std::string_view{"moisture"}};

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

/// @brief Result of `RecordMeasurement`.
struct MeasurementAck {
    std::int64_t sampleId = 0;
    std::string summary;
};

/// @brief The business model behind the generated forms.
class LabModel {
public:
    /// @brief The unit algebra runs in real model code: kg / m³ -> kg/m³.
    Density execute(const ComputeDryDensity& action) { return action.massDry / action.volume; }

    /// @brief Serves the sample combo-box options (a pure query).
    SampleList execute(const ListSamples& action) {
        static_cast<void>(action);
        return SampleList{.samples = {{.id = 1, .name = "Proctor A"},
                                      {.id = 2, .name = "Proctor B"},
                                      {.id = 7, .name = "Crushed aggregate 0/32"}}};
    }

    MeasurementAck execute(const RecordMeasurement& action) {
        // The dispatcher does not run validators server-side (clients gate on
        // validate() before submitting) — a model that dereferences required
        // fields must enforce its own precondition. Same predicate as the
        // GUI, so schema, form, and server agree by construction.
        if (!action.validate()) {
            throw std::invalid_argument{"RecordMeasurement: sampleId, measuredAt and density are required"};
        }
        auto summary = std::format("sample {} at {}: density {:.1f} {}", *action.sampleId, *action.measuredAt,
                                   *action.density, Density::unitMeta().display);
        if (action.moisture.hasValue()) {
            summary += std::format(", moisture {:.1f} %", *action.moisture);
        }
        if (action.note && !action.note->empty()) {
            summary += std::format(" ({})", *action.note);
        }
        return {.sampleId = *action.sampleId, .summary = std::move(summary)};
    }
};

}  // namespace lab

template <>
struct glz::json_schema<lab::ComputeDryDensity> {
    schema massDry{.description = "Oven-dry mass of the specimen"};
    schema volume{.description = "Mould volume"};
};

template <>
struct glz::json_schema<lab::RecordMeasurement> {
    schema sampleId{.description = "Sample under test"};
    schema measuredAt{.description = "When the measurement was taken (UTC)"};
    schema density{.description = "Dry density"};
    schema moisture{.description = "Moisture content", .minimum = 0, .maximum = 100};
    schema note{.description = "Free-text remark"};
};

using lab::ComputeDryDensity;
using lab::LabModel;
using lab::ListSamples;
using lab::RecordMeasurement;

BRIDGE_REGISTER_MODEL(LabModel, "LabModel")
BRIDGE_REGISTER_ACTION(LabModel, ComputeDryDensity, "ComputeDryDensity")
BRIDGE_REGISTER_ACTION(LabModel, RecordMeasurement, "RecordMeasurement")
BRIDGE_REGISTER_ACTION(LabModel, ListSamples, "ListSamples", morph::model::Loggable::No)
