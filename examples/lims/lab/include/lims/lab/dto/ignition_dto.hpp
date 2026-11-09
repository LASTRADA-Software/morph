// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <morph/core/records.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

namespace lims {

/// @brief One ignition test: the masses a loss-on-ignition determination is worked out from, each absent until
///        recorded. Exact decimals, since the UI refuses a number with a fraction as a double.
struct IgnitionTestView {
    std::int64_t id = 0;
    std::optional<::morph::math::Rational> container;
    std::optional<::morph::math::Rational> before;
    std::optional<::morph::math::Rational> after;
    /// ((before - after) - container) / (before - container), to four places. Absent until every mass is known, and
    /// for a sample of no mass, where it is undefined.
    std::optional<::morph::math::Rational> result;
};

struct ListIgnitionTests {};

struct ListIgnitionTestsResult {
    std::vector<IgnitionTestView> tests;
};

struct AddIgnitionTest {};

/// @brief Records some of a test's masses. One left absent keeps its value, so they can be given one at a time.
struct RecordIgnitionValues {
    std::int64_t id = 0;
    std::optional<::morph::math::Rational> container;
    std::optional<::morph::math::Rational> before;
    std::optional<::morph::math::Rational> after;

    /// Saved on a button press: as you type would store and journal each digit of a mass.
    static constexpr bool explicitSubmit = true;

    static constexpr std::array<::morph::forms::FieldMeta, 4> fieldMetadata{
        ::morph::forms::FieldMeta{.field = "id", .label = "Test", .help = "Which ignition test on this screen."},
        ::morph::forms::FieldMeta{.field = "container",
                                  .label = "Container",
                                  .help = "The empty container's mass, in grams.",
                                  .decimals = ::morph::math::DecimalPlaces{3}},
        ::morph::forms::FieldMeta{.field = "before",
                                  .label = "Before",
                                  .help = "The container and sample before ignition, in grams.",
                                  .decimals = ::morph::math::DecimalPlaces{3}},
        ::morph::forms::FieldMeta{.field = "after",
                                  .label = "After",
                                  .help = "The container and residue after ignition, in grams.",
                                  .decimals = ::morph::math::DecimalPlaces{3}},
    };
};

struct DeleteIgnitionTest {
    std::int64_t id = 0;
};

struct DeleteIgnitionTestResult {
    std::int64_t id = 0;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::IgnitionTestView, "Lims_IgnitionTest")
