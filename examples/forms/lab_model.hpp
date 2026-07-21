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

#include <array>
#include <cstdint>
#include <format>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/choice.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
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

    // Illustrative field metadata (docs/spec/forms/forms.md, "Field
    // metadata"): density gets an in-control placeholder; note (already
    // optional) is hidden entirely — carried in the payload if some other
    // client sets it, never shown to this renderer's operator. sampleId,
    // measuredAt, and moisture keep their inferred titles and their existing
    // glz::json_schema<RecordMeasurement> descriptions untouched.
    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "density", .placeholder = "e.g. 1650.0"},
        morph::forms::FieldMeta{.field = "note", .hidden = true},
    };

    /// Visual structure (docs/spec/forms/forms.md, "Layout & grouping"): two
    /// titled sections plus a collapsible notes panel, with the free-text
    /// note spanning both grid columns. Purely additive — a renderer that
    /// ignores `x-layout`/`x-colspan` still renders every field, flat, in
    /// `x-order`.
    static constexpr std::array<std::string_view, 2> kSample{std::string_view{"sampleId"},
                                                             std::string_view{"measuredAt"}};
    static constexpr std::array<std::string_view, 2> kResult{std::string_view{"density"},
                                                             std::string_view{"moisture"}};
    static constexpr std::array<std::string_view, 1> kNote{std::string_view{"note"}};
    static constexpr std::array formLayout{
        morph::forms::FieldGroup{.title = "Sample", .fields = kSample},
        morph::forms::FieldGroup{.title = "Result", .fields = kResult},
        morph::forms::FieldGroup{.title = "Notes", .kind = morph::forms::GroupKind::Accordion, .fields = kNote},
    };
    static constexpr std::array fieldSpans{
        morph::forms::FieldSpan{.field = "note", .colspan = 2},
    };

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

/// @brief Result of `RecordMeasurement`.
struct MeasurementAck {
    std::int64_t sampleId = 0;
    std::string summary;
};

/// @brief One selectable country (a row of `ListCountries`' result).
struct CountryInfo {
    std::int64_t id = 0;
    std::string name;
};

/// @brief Result of `ListCountries`.
struct CountryList {
    std::vector<CountryInfo> countries;
};

/// @brief Options provider for `ShippingAddress.country` — independent, like
///        `ListSamples`.
struct ListCountries {};

/// @brief One selectable city (a row of `ListCities`' result).
struct CityInfo {
    std::int64_t id = 0;
    std::string name;
};

/// @brief Result of `ListCities`.
struct CityList {
    std::vector<CityInfo> cities;
};

/// @brief Options provider for `ShippingAddress.city` — filtered by the
///        sibling `country` value the renderer sends as the request body
///        (declared via `Choice`'s `DependsOn`, surfaced as
///        `x-optionsDependsOn` in the schema).
struct ListCities {
    std::int64_t country = 0;
};

/// @brief Ship to a city within a country: `city`'s options cascade from the
///        selected `country`.
struct ShippingAddress {
    /// Independent Choice: options come from `ListCountries` with an empty body.
    morph::forms::Choice<std::int64_t, "ListCountries"> country;

    /// Dependent Choice: options come from `ListCities` with `{"country": <value>}`
    /// as the body — the schema carries `x-optionsDependsOn: ["country"]`.
    morph::forms::Choice<std::int64_t, "ListCities", "id", "name", "country"> city;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

/// @brief Result of `ShippingAddress`.
struct ShippingAck {
    std::int64_t countryId = 0;
    std::int64_t cityId = 0;
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

    /// @brief Serves the country combo-box options (a pure query,
    ///        independent — like `ListSamples`).
    CountryList execute(const ListCountries& action) {
        static_cast<void>(action);
        return CountryList{.countries = {{.id = 1, .name = "Wonderland"}, {.id = 2, .name = "Narnia"}}};
    }

    /// @brief Serves the city combo-box options, filtered by the sibling
    ///        `country` value the renderer sends as the request body — a
    ///        dependent `Choice`'s options action is an ordinary registered
    ///        action whose body happens to be the parent selection.
    CityList execute(const ListCities& action) {
        if (action.country == 1) {
            return CityList{.cities = {{.id = 10, .name = "Looking-Glass City"}, {.id = 11, .name = "Tulgey Wood"}}};
        }
        if (action.country == 2) {
            return CityList{.cities = {{.id = 20, .name = "Cair Paravel"}}};
        }
        return CityList{};
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

    ShippingAck execute(const ShippingAddress& action) {
        if (!action.validate()) {
            throw std::invalid_argument{"ShippingAddress: country and city are required"};
        }
        return {.countryId = *action.country,
                .cityId = *action.city,
                .summary = std::format("ship to city {} in country {}", *action.city, *action.country)};
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

template <>
struct glz::json_schema<lab::ShippingAddress> {
    schema country{.description = "Destination country"};
    schema city{.description = "Destination city (options depend on country)"};
};

using lab::ComputeDryDensity;
using lab::LabModel;
using lab::ListCities;
using lab::ListCountries;
using lab::ListSamples;
using lab::RecordMeasurement;
using lab::ShippingAddress;

BRIDGE_REGISTER_MODEL(LabModel, "LabModel")
BRIDGE_REGISTER_ACTION(LabModel, ComputeDryDensity, "ComputeDryDensity")
BRIDGE_REGISTER_ACTION(LabModel, RecordMeasurement, "RecordMeasurement")
BRIDGE_REGISTER_ACTION(LabModel, ListSamples, "ListSamples", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(LabModel, ListCountries, "ListCountries", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(LabModel, ListCities, "ListCities", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(LabModel, ShippingAddress, "ShippingAddress")
