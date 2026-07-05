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

#include <morph/forms.hpp>
#include <morph/registry.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

/// @brief Record a measured density (moisture and note are optional).
struct RecordMeasurement {
    std::int64_t sampleId = 0;
    Density density{};
    Percent moisture{};
    std::optional<std::string> note{};

    static constexpr std::array optionalFields{std::string_view{"moisture"}};

    [[nodiscard]] bool validate() const { return sampleId > 0 && morph::forms::allRequiredEngaged(*this); }
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

    MeasurementAck execute(const RecordMeasurement& action) {
        // The dispatcher does not run validators server-side (clients gate on
        // validate() before submitting) — a model that dereferences required
        // quantities must enforce its own precondition. Same predicate as the
        // GUI, so schema, form, and server agree by construction.
        if (!action.validate()) {
            throw std::invalid_argument{"RecordMeasurement: sampleId and density are required"};
        }
        auto summary = std::format("sample {}: density {:.1f} {}", action.sampleId, *action.density,
                                   Density::unitMeta().display);
        if (action.moisture.hasValue()) {
            summary += std::format(", moisture {:.1f} %", *action.moisture);
        }
        if (action.note && !action.note->empty()) {
            summary += std::format(" ({})", *action.note);
        }
        return {.sampleId = action.sampleId, .summary = std::move(summary)};
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
    schema sampleId{.description = "Sample id", .minimum = 1};
    schema density{.description = "Dry density"};
    schema moisture{.description = "Moisture content", .minimum = 0, .maximum = 100};
    schema note{.description = "Free-text remark"};
};

using lab::ComputeDryDensity;
using lab::LabModel;
using lab::RecordMeasurement;

BRIDGE_REGISTER_MODEL(LabModel, "LabModel")
BRIDGE_REGISTER_ACTION(LabModel, ComputeDryDensity, "ComputeDryDensity")
BRIDGE_REGISTER_ACTION(LabModel, RecordMeasurement, "RecordMeasurement")
