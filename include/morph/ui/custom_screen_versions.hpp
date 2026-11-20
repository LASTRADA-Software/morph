// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../forms/version.hpp"
#include "value.hpp"

/// @file
/// @brief The versions of a custom screen: the base version a person's copy knows, what its base gained or lost since,
///        and the update that takes the person's choices over.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "Versions".

namespace morph::ui {

/// @brief The schema JSON of an action by its id, as `forms::schemaJson<A>()` emits it; nothing for an action it does
///        not know.
using ActionSchemaLookup = std::function<std::optional<std::string>(std::string_view actionId)>;

/// @brief The member of a custom screen's document that records the base version it was last saved or updated
///        against.
inline constexpr std::string_view kKnownVersionKey = "knownVersion";

/// @brief What a change is about.
enum class LayoutItemKind : std::uint8_t {
    Element,  ///< An element of the document: a node or a part with an `id`.
    Field,    ///< A field of the action a `form` binds.
};

/// @brief Where an element sits in the base.
struct LayoutPlace {
    /// @brief The id of the nearest element that holds it; empty when no element does and it sits in the document
    ///        itself.
    std::string parent;
    /// @brief The JSON Pointer, from that element (or the document), of the member that holds it: `/children`,
    ///        `/tabs`, `/view/children`, `/actions`.
    std::string path;
    /// @brief Its index in that member's array; absent when the member holds the element itself.
    std::optional<std::size_t> index;
};

/// @brief Something the base has that the person's copy does not, newer than the version the copy knows.
struct LayoutAddition {
    /// @brief An element, or a form's field.
    LayoutItemKind kind = LayoutItemKind::Element;
    /// @brief What a choice names it by (`LayoutChoices`): an element's id, or `<form>/<field>`, where `<form>` is
    ///        the form's id, or its action for a form without one.
    std::string key;
    /// @brief The element's id, or the field's name.
    std::string id;
    /// @brief The version it first ships in.
    std::string since;
    /// @brief An element's place in the base; empty for a field.
    LayoutPlace place;
    /// @brief A field's form: its id, empty for a form without one.
    std::string form;
    /// @brief A field's form: the action it binds.
    std::string action;
};

/// @brief Something the person's copy refers to that the base no longer has.
struct LayoutRemoval {
    /// @brief An element, or a form's field overlay.
    LayoutItemKind kind = LayoutItemKind::Element;
    /// @brief What a choice names it by, as `LayoutAddition::key`.
    std::string key;
    /// @brief The element's id, or the field's name.
    std::string id;
    /// @brief A field's form: its id, empty for a form without one.
    std::string form;
    /// @brief A field's form: the action it binds.
    std::string action;
};

/// @brief What changed in a base screen since the version a person's copy of it knows.
struct LayoutChanges {
    /// @brief The base's version: the highest `since` among its elements and the fields of the actions its forms bind.
    forms::FieldVersion baseVersion;
    /// @brief The version the copy knows, its `knownVersion`; the first version when it records none.
    forms::FieldVersion knownVersion;
    /// @brief What the base gained, in the base's document order, forms' fields after the elements.
    std::vector<LayoutAddition> added;
    /// @brief What the copy refers to that the base lost: elements in the copy's document order, then forms' field
    ///        overlays.
    std::vector<LayoutRemoval> removed;

    /// @brief Whether the copy knows an older version than its base's.
    /// @return `knownVersion < baseVersion`.
    [[nodiscard]] bool needsUpdate() const noexcept { return knownVersion < baseVersion; }

    /// @brief Whether the copy refers to something the base no longer has.
    /// @return Whether `removed` is not empty.
    [[nodiscard]] bool needsAttention() const noexcept { return !removed.empty(); }
};

/// @brief What the person chose for one addition.
enum class AddedChoice : std::uint8_t {
    TakeOver,  ///< Take it over: an element at its place in the base, a field shown as the form shows it.
    LeaveOut,  ///< Leave it out: an element stays absent, a field gets an `omit` overlay.
};

/// @brief What the person chose for one removal.
enum class RemovedChoice : std::uint8_t {
    Drop,  ///< Drop the entry from the copy.
    Keep,  ///< Keep it as it is.
};

/// @brief The person's choices over a `LayoutChanges`, by `LayoutAddition::key` and `LayoutRemoval::key`. An addition
///        without a choice is left out and a removal without one is kept, so the copy changes only where the person
///        said; a key that names no change is refused.
struct LayoutChoices {
    /// @brief The choice for each addition, by key.
    std::map<std::string, AddedChoice, std::less<>> added;
    /// @brief The choice for each removal, by key.
    std::map<std::string, RemovedChoice, std::less<>> removed;
};

/// @brief The version of a base screen: the highest `since` among its elements and the `x-since` of the fields of the
///        actions its forms bind.
/// @param base    The base screen's JSON text.
/// @param schemas The action schemas; an action it does not know adds no fields.
/// @return The version, or the refusal: a base that does not load, or a schema whose `x-since` is not a version.
[[nodiscard]] MORPH_API std::expected<forms::FieldVersion, Refusal> baseVersion(std::string_view base,
                                                                                ActionSchemaLookup const& schemas);

/// @brief What changed in @p base since the version @p layout knows.
///
/// Added: each element of the base with a `since` above the copy's `knownVersion` whose id the copy does not have,
/// with its place (an element inside another added element comes with it and is not listed), and each field of a
/// base form's action with an `x-since` above it, for a form the copy has (the same id, or for a form without one the
/// first form of the same action) and gives no overlay for that field. Removed: each element of the copy with a
/// `since` whose id the base no longer has (an element a person adds has none), and each field overlay of a copy's
/// form naming a field its action no longer has.
/// @param base        The base screen's JSON text.
/// @param baseSchemas The action schemas; a form whose action it does not know is not compared.
/// @param layout      The person's copy's JSON text.
/// @return The changes, or the refusal: a document that does not load, or a schema whose `x-since` is not a version.
[[nodiscard]] MORPH_API std::expected<LayoutChanges, Refusal> layoutChanges(std::string_view base,
                                                                            ActionSchemaLookup const& baseSchemas,
                                                                            std::string_view layout);

/// @brief @p document with its `knownVersion` set to @p version.
/// @param document A document's JSON text.
/// @param version  The version to record.
/// @return The rewritten text, or the refusal of text that is not a JSON object.
[[nodiscard]] MORPH_API std::expected<std::string, Refusal> withKnownVersion(std::string_view document,
                                                                             forms::FieldVersion const& version);

/// @brief The person's copy updated by their choices, recording the base's version as `knownVersion`.
///
/// Each removal chosen `Drop` is removed from the copy; each field addition left out gets `"omit": true` in its form's
/// `fields`; then each element addition taken over is inserted at its place: into the copy's element of the same id
/// as its parent (the document itself for none), in the same member, at the base's index or at the end when the
/// member is shorter. The result is loaded as a save loads it, so what comes back is a document; the server's checks
/// still decide when it is saved.
/// @param base        The base screen's JSON text.
/// @param baseSchemas The action schemas, as `layoutChanges` takes them.
/// @param layout      The person's copy's JSON text.
/// @param choices     The person's choices.
/// @return The updated copy, or the refusal: one `layoutChanges` gives, a choice for a key that names no change, an
///         element whose place the copy does not have, or a result that does not load.
[[nodiscard]] MORPH_API std::expected<std::string, Refusal> applyLayoutChoices(std::string_view base,
                                                                               ActionSchemaLookup const& baseSchemas,
                                                                               std::string_view layout,
                                                                               LayoutChoices const& choices);

}  // namespace morph::ui
