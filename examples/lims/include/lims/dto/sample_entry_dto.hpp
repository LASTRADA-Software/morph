// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/core/types.hpp"

/// @file
/// Sample entry: the record-editing side of `SampleModel`. A model instance holds one private working copy of a
/// sample's entry data; the section actions edit it, the snapshot queries read it, and `SaveSample` commits it.
/// Nothing a section edits reaches the database before the save.

namespace lims {

/// @brief A turbidity reading, in nephelometric turbidity units.
using Turbidity = ::morph::units::Quantity<LimsUnit::scalar, 2>;

/// @brief A recovery, in percent of the nominal concentration.
using RecoveryPercent = ::morph::units::Quantity<LimsUnit::scalar, 1>;

/// @brief The dilution steps of the series, as the factor each step divides the spike by.
inline constexpr std::array<std::int64_t, 4> kDilutionFactors{1, 2, 4, 8};

/// @brief The lowest recovery, in percent, a dilution step may show and stay within limits.
inline constexpr std::int64_t kRecoveryMinPercent = 80;

/// @brief The highest recovery, in percent, a dilution step may show and stay within limits.
inline constexpr std::int64_t kRecoveryMaxPercent = 120;

/// @brief The optional measurement named `turbidity`.
inline constexpr std::string_view kTurbidity = "turbidity";

/// @brief The optional measurement named `nitrate`.
inline constexpr std::string_view kNitrate = "nitrate";

/// @brief Starts a new, empty working copy; nothing is stored until it is saved.
struct NewSample {
    /// @brief Always well-formed.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief Loads a stored sample's entry data into the working copy, and attaches the handler to the sample.
struct LoadSample {
    /// @brief The sample to load.
    SampleId id;

    /// @brief Whether a sample is named.
    /// @return `true` when `id` holds a value.
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

/// @brief Why a save was refused; `none` when it was not.
enum class SaveError : std::uint8_t {
    none,           ///< Saved.
    noWorkingCopy,  ///< No sample was created or loaded.
    noClient,       ///< The header names no client.
    noReference,    ///< The header has no reference.
    noProject,      ///< No project was chosen.
};

/// @brief Commits the working copy: registers the sample when it is new, then stores its entry data.
struct SaveSample {
    /// @brief Always well-formed: what the working copy lacks is the reply's `error`.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief What a save did.
struct SaveSampleResult {
    /// @brief Why the save was refused, or `none`.
    SaveError error = SaveError::none;
    /// @brief The saved sample; empty when the save was refused.
    SampleId sampleId;
};

/// @brief Deletes the stored sample the working copy was loaded from, and discards the working copy.
struct DeleteSample {
    /// @brief Always well-formed.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief What a delete removed.
struct DeleteSampleResult {
    /// @brief The deleted sample.
    SampleId sampleId;
};

/// @brief The header section: who the sample is for and when it arrived.
struct HeaderSection {
    /// @brief The client the sample belongs to.
    ClientId clientId;
    /// @brief The lab's container reference.
    std::string reference;
    /// @brief The day the sample was received, in days since the epoch (UTC).
    std::int64_t receivedOn = 0;
    /// @brief Free-text remarks.
    std::string remarks;

    /// @brief Members a client may leave out.
    static constexpr std::array optionalFields{std::string_view{"receivedOn"}, std::string_view{"remarks"}};

    /// @brief Always well-formed: an incomplete header is refused at save, not while it is edited.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }

    /// @brief A section is submitted with its own button.
    static constexpr bool explicitSubmit = true;
};

/// @brief The sampling section: the spike, and what each dilution step measured.
struct SamplingSection {
    /// @brief The spiked concentration the undiluted step starts from.
    Concentration spike;
    /// @brief The concentration measured at each dilution step, in `kDilutionFactors` order; a step not measured yet
    ///        is empty.
    Concentration measured1;
    /// @brief The second step's measurement.
    Concentration measured2;
    /// @brief The third step's measurement.
    Concentration measured3;
    /// @brief The fourth step's measurement.
    Concentration measured4;

    /// @brief Members a client may leave out.
    static constexpr std::array optionalFields{std::string_view{"spike"}, std::string_view{"measured1"},
                                               std::string_view{"measured2"}, std::string_view{"measured3"},
                                               std::string_view{"measured4"}};

    /// @brief Always well-formed: a step may stay unmeasured.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }

    /// @brief A section is submitted with its own button.
    static constexpr bool explicitSubmit = true;
};

/// @brief The optional turbidity measurement.
struct TurbiditySection {
    /// @brief The reading.
    Turbidity ntu;

    /// @brief Whether a reading is given.
    /// @return `true` when `ntu` holds a value.
    [[nodiscard]] bool validate() const noexcept { return ntu.hasValue(); }

    /// @brief A section is submitted with its own button.
    static constexpr bool explicitSubmit = true;
};

/// @brief The optional nitrate measurement.
struct NitrateSection {
    /// @brief The reading.
    Concentration nitrate;

    /// @brief Whether a reading is given.
    /// @return `true` when `nitrate` holds a value.
    [[nodiscard]] bool validate() const noexcept { return nitrate.hasValue(); }

    /// @brief A section is submitted with its own button.
    static constexpr bool explicitSubmit = true;
};

/// @brief Removes an optional measurement's values from the working copy.
struct ClearOptionalMeasurement {
    /// @brief `turbidity` or `nitrate`.
    std::string which;

    /// @brief Whether an optional measurement is named.
    /// @return `true` for `turbidity` and `nitrate`.
    [[nodiscard]] bool validate() const noexcept { return which == kTurbidity || which == kNitrate; }
};

/// @brief Reads the working copy's header.
struct GetHeader {
    /// @brief Always well-formed.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The working copy's header.
struct HeaderView {
    /// @brief The stored sample, or empty for one not saved yet.
    SampleId sampleId;
    /// @brief The sample's number, `S-<id>`, once it is saved.
    std::optional<std::string> sampleNumber;
    /// @brief The client.
    ClientId clientId;
    /// @brief The container reference.
    std::string reference;
    /// @brief The day received, in days since the epoch.
    std::int64_t receivedOn = 0;
    /// @brief The remarks.
    std::string remarks;
    /// @brief The chosen project's id, or 0 for none.
    std::int64_t projectId = 0;
    /// @brief The chosen project's name, or empty.
    std::string projectName;
};

/// @brief Reads the working copy's optional measurements.
struct GetMeasurements {
    /// @brief Always well-formed.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The working copy's optional measurements, and which of them hold data.
struct MeasurementsView {
    /// @brief The turbidity reading, or empty.
    Turbidity ntu;
    /// @brief The nitrate reading, or empty.
    Concentration nitrate;
    /// @brief Whether a turbidity reading is held.
    bool hasTurbidity = false;
    /// @brief Whether a nitrate reading is held.
    bool hasNitrate = false;
};

/// @brief Reads the dilution series the working copy's sampling section implies.
struct GetDilutionSeries {
    /// @brief Always well-formed.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief One dilution step, computed by the model.
struct SeriesRow {
    /// @brief The step, from 1.
    std::int64_t step = 0;
    /// @brief The factor the spike is divided by.
    std::int64_t factor = 1;
    /// @brief The concentration the step should show: the spike over the factor.
    Concentration nominal;
    /// @brief The concentration it showed, or empty.
    Concentration measured;
    /// @brief The measured share of the nominal, in percent; empty until measured.
    RecoveryPercent recoveryPct;
    /// @brief The lowest recovery within limits.
    RecoveryPercent limitMin;
    /// @brief The highest recovery within limits.
    RecoveryPercent limitMax;
    /// @brief Whether a measured step's recovery falls outside the limits.
    bool outsideLimits = false;
};

/// @brief The dilution series.
struct DilutionSeries {
    /// @brief The steps, in order; none until a spike is entered.
    std::vector<SeriesRow> rows;
};

/// @brief One project a sample can be filed under.
struct ProjectRow {
    /// @brief The project's id.
    std::int64_t id = 0;
    /// @brief Its code.
    std::string code;
    /// @brief Its name.
    std::string name;
};

/// @brief Lists the projects, through the table engine's query.
struct ListProjects {
    /// @brief The sort, filters and page.
    ::morph::table::TableQuery query;

    /// @brief Always well-formed; the table engine bounds the query.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief Files the working copy under a project.
struct SelectProject {
    /// @brief The project's id.
    std::int64_t projectId = 0;

    /// @brief Whether a project is named.
    /// @return `true` for a positive id.
    [[nodiscard]] bool validate() const noexcept { return projectId > 0; }
};

/// @brief A working copy: what the section actions edit and `SaveSample` stores, as the entry row's JSON.
struct SampleDraft {
    /// @brief The header section.
    HeaderSection header;
    /// @brief The sampling section.
    SamplingSection sampling;
    /// @brief The turbidity reading, or empty.
    Turbidity ntu;
    /// @brief The nitrate reading, or empty.
    Concentration nitrate;
    /// @brief The chosen project's id, or 0 for none.
    std::int64_t projectId = 0;
};

}  // namespace lims

template <>
struct glz::meta<lims::SaveError> {
    using enum lims::SaveError;

    static constexpr auto value = glz::enumerate(none, noWorkingCopy, noClient, noReference, noProject);
};
