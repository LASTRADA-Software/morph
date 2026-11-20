// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/version.hpp
/// @brief `morph::forms::FieldVersion`, the ordered version a field or a screen element first ships in, and the
///        field-inventory check an application runs over its actions in its own tests.
///
/// Its own header, free of glaze and of the schema emitters, because both `forms.hpp` (`FieldMeta::since`) and the
/// ui documents (an element's `since`, a custom screen's `knownVersion`) use the same ordering.

#include <algorithm>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace morph::forms {

/// @brief A dotted numeric version such as `6.11.0`, ordered component by component.
///
/// The value means nothing to morph apart from its order. Components are compared from the left as unsigned numbers,
/// and a missing trailing component counts as 0, so `6.11` and `6.11.0` are equal and `6.11` is below `6.11.1`. A
/// default-constructed version is the **first version**, written `0`: what a field or an element without a `since`
/// belongs to, and below every version that has a non-zero component.
class FieldVersion {
public:
    /// @brief The most bytes a version's text may have, so a version quoted in a refusal stays short.
    static constexpr std::size_t kMaxTextBytes = 64;

    /// @brief The first version, `0`.
    FieldVersion() = default;

    /// @brief Parses a dotted numeric version.
    ///
    /// The text is one or more components separated by single dots. Each component is decimal digits, without a
    /// leading zero unless it is `0` itself (so `6.01` cannot silently equal `6.1`), and at most 4294967295. The
    /// whole text is at most `kMaxTextBytes` bytes. Anything else is refused.
    /// @param text The version as written, e.g. `"6.11.0"`.
    /// @return The version, or why @p text is not one, a sentence that quotes it.
    [[nodiscard]] static std::expected<FieldVersion, std::string> parse(std::string_view text) {
        if (text.empty()) {
            return std::unexpected{std::string{"a version must not be empty; write dotted numbers such as 6.11.0"}};
        }
        if (text.size() > kMaxTextBytes) {
            return std::unexpected{"a version is at most " + std::to_string(kMaxTextBytes) + " bytes; this one has " +
                                   std::to_string(text.size())};
        }
        FieldVersion version;
        version._text = std::string{text};
        std::string_view rest = text;
        while (true) {
            auto const dot = rest.find('.');
            std::string_view const component = rest.substr(0, dot);
            auto parsed = parseComponent(text, component);
            if (!parsed.has_value()) {
                return std::unexpected{std::move(parsed.error())};
            }
            version._components.push_back(*parsed);
            if (dot == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(dot + 1);
        }
        return version;
    }

    /// @brief The version as written; `0` for the first version.
    /// @return The text this version was parsed from.
    [[nodiscard]] std::string const& text() const noexcept { return _text; }

    /// @brief The numeric components, as written (trailing zeros kept); empty for the first version.
    /// @return The components, most significant first.
    [[nodiscard]] std::span<std::uint32_t const> components() const noexcept { return _components; }

    /// @brief Orders two versions component by component, a missing component counting as 0.
    /// @param lhs The left version.
    /// @param rhs The right version.
    /// @return How @p lhs orders against @p rhs.
    friend std::strong_ordering operator<=>(FieldVersion const& lhs, FieldVersion const& rhs) noexcept {
        std::size_t const count = std::max(lhs._components.size(), rhs._components.size());
        for (std::size_t index = 0; index < count; ++index) {
            std::uint32_t const left = index < lhs._components.size() ? lhs._components.at(index) : 0U;
            std::uint32_t const right = index < rhs._components.size() ? rhs._components.at(index) : 0U;
            if (left != right) {
                return left <=> right;
            }
        }
        return std::strong_ordering::equal;
    }

    /// @brief Whether two versions are the same version (`6.11` equals `6.11.0`).
    /// @param lhs The left version.
    /// @param rhs The right version.
    /// @return `true` when neither orders before the other.
    friend bool operator==(FieldVersion const& lhs, FieldVersion const& rhs) noexcept {
        return (lhs <=> rhs) == std::strong_ordering::equal;
    }

private:
    /// @brief Parses one component of @p whole.
    /// @param whole     The whole version text, quoted in a refusal.
    /// @param component The component's digits.
    /// @return The component's value, or why it is not one.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters): the whole text, then the component of it.
    [[nodiscard]] static std::expected<std::uint32_t, std::string> parseComponent(std::string_view whole,
                                                                                  std::string_view component) {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        auto const refuse = [whole](std::string_view why) {
            return std::unexpected{"'" + std::string{whole} + "' is not a version: " + std::string{why} +
                                   "; write dotted numbers such as 6.11.0"};
        };
        if (component.empty()) {
            return refuse("it has an empty component");
        }
        if (component.size() > 1 && component.front() == '0') {
            return refuse("a component has a leading zero");
        }
        std::uint32_t value = 0;
        char const* const last = std::to_address(component.end());
        auto const [end, error] = std::from_chars(std::to_address(component.begin()), last, value);
        if (error == std::errc::result_out_of_range) {
            return refuse("a component is above 4294967295");
        }
        if (error != std::errc{} || end != last) {
            return refuse("a component is not decimal digits");
        }
        return value;
    }

    std::vector<std::uint32_t> _components;
    std::string _text{"0"};
};

/// @brief Reads a `since` as written in a declaration or a document: empty is the first version.
/// @param text The text, possibly empty.
/// @return The version, or why @p text is not one.
[[nodiscard]] inline std::expected<FieldVersion, std::string> parseSince(std::string_view text) {
    if (text.empty()) {
        return FieldVersion{};
    }
    return FieldVersion::parse(text);
}

/// @brief One field of an action's inventory: its wire name and the version it first shipped in.
///
/// A plain aggregate, so an application can keep a snapshot as JSON with glaze as it is.
struct FieldInventoryEntry {
    /// @brief The field's wire name.
    std::string field;
    /// @brief The version the field first ships in, as written; empty for the first version.
    std::string since;
    /// @brief Whether the field was removed. Only a snapshot keeps removed fields, so a name is never used twice.
    bool removed = false;
};

/// @brief What is wrong with an action's fields against the snapshot of the last release.
enum class FieldInventoryProblemKind : std::uint8_t {
    /// An entry's `since` is not a version.
    InvalidSince,
    /// A field that is not in the snapshot has no `since` above the snapshot's highest.
    AddedWithoutNewerSince,
    /// A field of the snapshot declares another `since` than it shipped with.
    SinceChanged,
    /// A field the snapshot records as removed is back: a wire name is never reused.
    Reappeared,
};

/// @brief One problem `checkFieldInventory` found.
struct FieldInventoryProblem {
    /// @brief What kind of problem it is.
    FieldInventoryProblemKind kind{FieldInventoryProblemKind::InvalidSince};
    /// @brief The field it is about.
    std::string field;
    /// @brief A sentence for a test's failure output, naming the field and the versions.
    std::string message;
};

namespace detail {

/// @brief Finds the entry named @p field in @p entries.
/// @param entries The inventory to search.
/// @param field   The wire name.
/// @return The entry, or `nullptr` when none is named @p field.
[[nodiscard]] inline FieldInventoryEntry const* findInventoryEntry(std::span<FieldInventoryEntry const> entries,
                                                                   std::string_view field) noexcept {
    auto const found =
        std::ranges::find_if(entries, [field](FieldInventoryEntry const& entry) { return entry.field == field; });
    return found == entries.end() ? nullptr : &*found;
}

}  // namespace detail

/// @brief Checks an action's current fields against the snapshot of its last release.
///
/// The application keeps the snapshot (`nextFieldInventory` gives the one to keep at a release) and runs this in its
/// own tests; morph keeps none. A problem is reported for:
/// - an entry of either list whose `since` is not a version (`InvalidSince`);
/// - a current field absent from the snapshot whose `since` is not above the highest `since` the snapshot records,
///   removed fields included (`AddedWithoutNewerSince`); an empty snapshot is the first release and checks no
///   addition;
/// - a current field the snapshot holds whose `since` is another version (`SinceChanged`);
/// - a current field the snapshot records as removed (`Reappeared`).
///
/// A field of the snapshot that is gone from @p current is a removal, which is allowed. An entry of @p current marked
/// `removed` is not a field of the action and is skipped.
/// @param previous The snapshot of the last release, removed fields included.
/// @param current  The action's fields now, `fieldInventory<A>()`.
/// @return Every problem, in the order of @p current (invalid snapshot entries first); empty when there is none.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the snapshot, then the fields now, as the check reads them.
[[nodiscard]] inline std::vector<FieldInventoryProblem> checkFieldInventory(
    std::span<FieldInventoryEntry const> previous, std::span<FieldInventoryEntry const> current) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::vector<FieldInventoryProblem> problems;
    FieldVersion highest;
    for (FieldInventoryEntry const& entry : previous) {
        auto const since = parseSince(entry.since);
        if (!since.has_value()) {
            problems.push_back({.kind = FieldInventoryProblemKind::InvalidSince,
                                .field = entry.field,
                                .message = "the snapshot's '" + entry.field + "': " + since.error()});
            continue;
        }
        highest = std::max(highest, *since);
    }
    for (FieldInventoryEntry const& entry : current) {
        if (entry.removed) {
            continue;
        }
        auto const since = parseSince(entry.since);
        if (!since.has_value()) {
            problems.push_back({.kind = FieldInventoryProblemKind::InvalidSince,
                                .field = entry.field,
                                .message = "'" + entry.field + "': " + since.error()});
            continue;
        }
        FieldInventoryEntry const* const shipped = detail::findInventoryEntry(previous, entry.field);
        if (shipped == nullptr) {
            if (!previous.empty() && *since <= highest) {
                problems.push_back({.kind = FieldInventoryProblemKind::AddedWithoutNewerSince,
                                    .field = entry.field,
                                    .message = "'" + entry.field + "' is new, so its since must be above " +
                                               highest.text() + ", the highest the last release had; it is " +
                                               since->text()});
            }
            continue;
        }
        if (shipped->removed) {
            problems.push_back({.kind = FieldInventoryProblemKind::Reappeared,
                                .field = entry.field,
                                .message = "'" + entry.field +
                                           "' was removed and is back; a wire name is never reused, so give the "
                                           "field a new name"});
            continue;
        }
        auto const before = parseSince(shipped->since);
        if (before.has_value() && *before != *since) {
            problems.push_back({.kind = FieldInventoryProblemKind::SinceChanged,
                                .field = entry.field,
                                .message = "'" + entry.field + "' shipped with since " + before->text() +
                                           " and now declares " + since->text() + "; a field's since never changes"});
        }
    }
    return problems;
}

/// @brief The snapshot to keep at a release: the current fields, then every field of @p previous that @p current
///        no longer has, marked removed, so a later `checkFieldInventory` catches a removed name coming back.
/// @param previous The snapshot of the last release.
/// @param current  The action's fields now, `fieldInventory<A>()`.
/// @return The new snapshot.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the snapshot, then the fields now, as the check reads them.
[[nodiscard]] inline std::vector<FieldInventoryEntry> nextFieldInventory(
    std::span<FieldInventoryEntry const> previous, std::span<FieldInventoryEntry const> current) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    std::vector<FieldInventoryEntry> next{current.begin(), current.end()};
    for (FieldInventoryEntry const& entry : previous) {
        if (detail::findInventoryEntry(current, entry.field) == nullptr) {
            next.push_back({.field = entry.field, .since = entry.since, .removed = true});
        }
    }
    return next;
}

}  // namespace morph::forms
