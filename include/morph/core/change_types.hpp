// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// The plain data a derived change is made of. This header includes nothing of the registry or the
/// forms layer, so the wire types can use it.

namespace morph::effects {

/// @brief The most keys one `RecordChange` carries.
///
/// The wire readers refuse a record entry with more keys, so a producer never emits one: a change that
/// would name more rows is reported as a whole-type change (keys absent), which over-notifies but never
/// misses. The bound keeps one peer-supplied list from forcing an unbounded allocation.
inline constexpr std::size_t kMaxChangeKeys = 4096;

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

/// @brief Reports a change to rows of a record that no execute made: what a server gives an attached observer.
///
/// The first argument is the record's wire name; the second the changed rows' keys as raw JSON, absent for the
/// whole type. Callable from any thread; it holds the server weakly and does nothing once the server is gone.
using AnnounceSink = std::function<void(std::string_view record, std::optional<std::vector<glz::raw_json>> keys)>;

/// @brief Something a server tells of every change an execute or a time fact derived, before the change's events
///        are queued: a server-side cache that must drop what the change made wrong.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class IChangeObserver {
public:
    virtual ~IChangeObserver() = default;

    /// @brief Takes one fan-out's changes. Runs on the server strand, and must not load, wait or call out.
    /// @param changes What the execute or the time fact changed.
    virtual void changed(Changes const& changes) = 0;

    /// @brief Takes the sink through which the observer reports changes of its own.
    /// @param sink The server's announce sink.
    virtual void bind(AnnounceSink sink) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

}  // namespace morph::effects
