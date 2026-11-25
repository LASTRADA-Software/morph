// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/record_name.hpp
/// @brief A record's registered name, looked up by type.
///
/// Its own header so the table headers that write a record's name into a schema can each include it, whichever of
/// them a translation unit reaches first.

#include <algorithm>
#include <morph/core/records.hpp>
#include <string>
#include <typeindex>

namespace morph::table::detail {

/// @brief A record's registered name, by the record's type.
/// @param type The record type.
/// @return Its name in `record::detail::recordRegistry()`, or empty when none is registered.
[[nodiscard]] inline std::string recordNameOf(std::type_index type) {
    auto const& records = record::detail::recordRegistry();
    auto const found = std::ranges::find(records, type, &record::RecordInfo::type);
    return found != records.end() ? found->name : std::string{};
}

}  // namespace morph::table::detail
