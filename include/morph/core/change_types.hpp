// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <vector>

/// @file
/// The plain data a derived change is made of. This header includes nothing of the registry or the
/// forms layer, so the wire types can use it.

namespace morph::effects {

/// @brief A change to the rows of one record type.
struct RecordChange {
    /// The record's wire name.
    std::string record;
    /// The keys of the rows that changed, each as raw JSON; absent when the whole type changed.
    std::optional<std::vector<glz::raw_json>> keys;
};

/// @brief A change to the state of one model instance.
struct InstanceChange {
    /// The model's registered type name.
    std::string modelType;
    /// The instance id.
    std::uint64_t instance = 0;
    /// The directory key's text for a keyed shared instance; empty for a private one.
    std::string primary;
};

/// @brief What one successful execute changed.
struct Changes {
    /// Record topics, one entry per record type.
    std::vector<RecordChange> records;
    /// Instance topics.
    std::vector<InstanceChange> instances;

    /// @brief Whether nothing changed.
    /// @return True when there are no record and no instance changes.
    [[nodiscard]] bool empty() const { return records.empty() && instances.empty(); }
};

}  // namespace morph::effects
