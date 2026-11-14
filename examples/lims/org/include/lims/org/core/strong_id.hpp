// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/payload_shape_tag.hpp>
#include <optional>
#include <string_view>

/// @file
/// The strong-id boilerplate every lims module defines its entity ids with. Shared, so each module's ids have the
/// same shape on the wire and in a journal fingerprint. The macros stay defined for every includer.

/// @brief Strong id boilerplate, one struct per identity role
///        (`std::optional<std::int64_t>` payload, `hasValue()`,
///        `fromOptional()`, `operator*()`, total ordering).
#define LIMS_DEFINE_STRONG_ID(Name)                                                \
    struct Name {                                                                  \
        std::optional<std::int64_t> value{};                                       \
        Name() = default;                                                          \
        explicit Name(std::int64_t v) : value{v} {}                                \
        [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); } \
        [[nodiscard]] std::int64_t operator*() const { return *value; }            \
        static Name fromOptional(std::optional<std::int64_t> v) {                  \
            Name id;                                                               \
            id.value = v;                                                          \
            return id;                                                             \
        }                                                                          \
        auto operator<=>(const Name&) const = default;                             \
    }

/// @brief On the wire each strong id is its nullable underlying integer —
///        same rationale as `ledger`'s identical block: glaze has no
///        reflection for a non-aggregate whose only member is an optional,
///        so every DTO carrying one would fail to compile inside glaze's
///        `to`/`from` templates without this.
#define LIMS_DEFINE_STRONG_ID_WIRE(Name)                  \
    template <>                                           \
    struct glz::meta<lims::Name> {                        \
        static constexpr auto value = &lims::Name::value; \
        static constexpr std::string_view name = #Name;   \
    }

/// @brief Stable payload-shape tag for each `LIMS_DEFINE_STRONG_ID` type, so a
///        journal fingerprint can tell one id from another.
///
/// The `glz::meta` specialisations above are what make these types
/// serialisable at all, and they are also what makes them *opaque* to
/// `morph::model::payloadShape`: a type whose meta names a value rather than an
/// object has no reflected members to decompose, so it renders as the bare `x`
/// and every id in a payload looks like every other one
/// (`morph/core/payload_shape_tag.hpp`; `docs/spec/journal/journal.md`,
/// "Custom-codec types name themselves").
///
/// This is the rung whose journal *is* the product: `SelfJournal`
/// (`lims/lab/core/self_journal.hpp`) stamps every entry it appends precisely so
/// the regulatory trail survives a rebuild. Every id here is
/// `std::optional<std::int64_t>` on the wire, so a retype between two of them
/// -- `AnalysisId` for the `AnalysisVersionId` a reading was captured under,
/// `ClientId` for the `SampleId` a queued capture names -- produces
/// byte-identical JSON that no decode, on any path, can catch. Absent declared
/// tags the fingerprint was byte-identical too, so `journal::replay()`'s
/// mismatch gate had nothing to fire on and the recorded integer decoded into
/// the wrong slot: a reading attributed to the wrong analysis version is a
/// wrong result with a valid-looking audit trail behind it, which is worse
/// than no trail.
///
/// The tag text is spelled here rather than derived from `glz::name_v`, which
/// is compiler-dependent -- a journal readable only by the build that wrote it
/// is the worse failure. It is part of the on-disk fingerprint of every entry
/// this rung records, so it is an interface: renaming a tag invalidates every
/// retained journal entry carrying that id, exactly as renaming a field does.
/// `lims.`-namespaced so it cannot collide with another rung's tag.
///
/// One specialisation per id type, generated the same way the structs and
/// their `glz::meta`s are.
#define LIMS_DEFINE_STRONG_ID_SHAPE_TAG(Name, Tag)                        \
    template <>                                                           \
    struct morph::model::PayloadShapeTag<lims::Name> {                    \
        /** @brief This id's stable shape name. @return The tag. */       \
        static constexpr std::string_view name() noexcept { return Tag; } \
    }
