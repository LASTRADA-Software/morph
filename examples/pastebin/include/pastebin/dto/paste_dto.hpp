// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "pastebin/core/types.hpp"
#include "pastebin/units.hpp"

/// @file
/// Pastebin's one entity's wire DTOs. GetPaste is the one client-visible,
/// journaled mutation (README "Journal" design decision — not split into an
/// unlogged read + RecordRead). ExpirePaste is dispatched only by the
/// app-layer sweep's internal client (Task 6), never by a GUI client.

namespace pastebin {

enum class Visibility { Public, Private };
enum class Editability { Immutable, Editable };

/// @brief Longest `syntax` label, in bytes, that `CreatePaste`/`EditPaste`
///        accept.
///
/// This is the storage column's exact width, not a policy number pulled from
/// the air: `PasteRecord::syntax` is a
/// `Light::SqlAnsiString<32>` (`pastebin/db/paste_entity.hpp`), and
/// Lightweight's `SqlFixedString` constructor is
/// `_size{std::min(N, s.size())}` with **no throw and no diagnostic** — a
/// 33-byte label is silently cut to 32 on the way into the row, and the
/// client is told the create succeeded. Two concrete harms follow, which is
/// why this is validated rather than tolerated:
///
/// 1. **Silent data loss.** `GetPaste` returns the truncated label, so the
///    round trip is lossy without anything reporting it.
/// 2. **Ill-formed UTF-8.** The cut is at a byte offset, not a codepoint
///    boundary, so a multi-byte label can be severed mid-sequence — putting
///    invalid UTF-8 into the `TEXT` column *and* into the JSON text frame
///    that carries the resulting `PasteView` back to the client. That is the
///    same class of wire-level hostile-content bug this rung already found
///    and fixed in the action/result codec (commit `f2ad662`,
///    `morph::model::detail::EscapingWriteOpts`), arriving by a different
///    door.
///
/// The bound is the column width **exactly**, with no safety margin
/// deliberately: any margin would be an arbitrary second number to keep in
/// sync, and the invariant that matters is simply "everything accepted is
/// stored whole". `src/models/paste_model.cpp` carries a `static_assert`
/// tying this constant to the entity's real capacity, so widening the column
/// without widening this (or vice versa) fails the build rather than
/// silently reopening the gap.
///
/// `content` needs no equivalent bound: it is a `Light::Field<std::string>`,
/// a variable-length column with no fixed capacity to overflow. The
/// server's own message-size limit is what bounds it, and this rung already
/// tests that path ("An oversized CreatePaste is refused by the transport
/// with a typed, readable error").
inline constexpr std::size_t kMaxSyntaxBytes = 32;

struct CreatePaste {
    std::string content;
    std::string syntax;                  // free-form label, e.g. "plaintext", "cpp"
    ::morph::time::Timestamp expiresAt;  // empty = never expires
    Reads burnAfterReads;                // empty = no burn limit
    Visibility visibility = Visibility::Public;
    Editability editability = Editability::Immutable;

    /// @brief Opts the generated form out of auto-submit-on-validity
    ///        (docs/spec/forms/forms.md, "Explicit submit mode"). Without
    ///        this, a form bound directly to a live controller would store
    ///        one paste per typed character.
    static constexpr bool explicitSubmit = true;

    /// @brief Members `schemaJson<CreatePaste>()` must leave out of the derived
    ///        `required` array (`morph::forms`' `optionalFields` convention —
    ///        see `include/morph/forms/forms.hpp`).
    ///
    /// `schemaJson<A>()` marks *every* reflected member required unless it is a
    /// `std::optional` or is named here, and the schema-driven create form
    /// (`gui/qml/Main.qml`) gates submission on exactly that array. Without
    /// this list no paste could be created without both an expiry instant and
    /// a burn budget — contradicting the two members' own documented "empty =
    /// never expires" / "empty = no burn limit" semantics above — and the two
    /// enums, which already carry defaults here, would have to be typed out by
    /// hand on every create. Discovered by this rung's first schema-driven
    /// consumer (the desktop GUI shell), not by the model tests, which
    /// construct `CreatePaste` in C++ and never see the schema.
    static constexpr std::array<std::string_view, 4> optionalFields{"expiresAt", "burnAfterReads", "visibility",
                                                                    "editability"};

    [[nodiscard]] bool validate() const noexcept {
        if (content.empty() || syntax.empty() || syntax.size() > kMaxSyntaxBytes) {
            return false;
        }
        // Reads' own doc comment (units.hpp) puts the whole-number constraint
        // on this DTO to enforce, not on the type — `Quantity` requires
        // `DeclaredDecimals >= 1`, so `Reads` represents tenths exactly and
        // `2.5` is a perfectly ordinary value of it, one that survives the
        // wire codec (a `Rational` travels as its own num/den pair) and
        // arrives here intact. This is where the premise stops being an
        // aspiration: without the `isInteger()` test below, a fractional
        // budget was accepted and then quietly rewritten into a *different*
        // budget by `paste_model.cpp`'s `countOf`, whose `math::floor` turned
        // 5/2 into 2 on the way into the row — the create reported success and
        // `GetPaste` reported the budget as 2. That is the same silent
        // data-loss class `kMaxSyntaxBytes` exists to prevent (see its doc
        // comment) reaching the row through a different door, and it gets the
        // same answer: refuse the input rather than rewrite it. A rung that
        // ever *wants* "2.5 means 2" must say so in `Reads` (whose declared
        // precision is the only honest place for it), not by letting a
        // conversion helper decide.
        //
        // A budget of 0 (or negative) is a third guise of "accepted, then
        // behaves as something else": both are whole numbers, but
        // PasteModel::execute(GetPaste)'s burn check
        // (`readCount >= *burnAfterReads`) is already true before the first
        // read ever happens, so the paste is born unreadable — accepted by
        // `validate()`, then permanently `Burned` on the very first `GetPaste`.
        if (burnAfterReads.hasValue()) {
            const auto& budget = *burnAfterReads.value();
            if (!budget.isInteger() || budget.isZero() || budget.isNegative()) {
                return false;
            }
        }
        return true;
    }
};

struct CreatePasteResult {
    PasteId id;
};

struct GetPaste {
    PasteId id;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct PasteView {
    PasteId id;
    std::string content;
    std::string syntax;
    ::morph::time::Timestamp createdAt;
    ::morph::time::Timestamp expiresAt;
    Reads burnAfterReads;
    Reads readCount;
    Visibility visibility = Visibility::Public;
    Editability editability = Editability::Immutable;
};

struct EditPaste {
    PasteId id;
    std::string content;
    std::string syntax;

    [[nodiscard]] bool validate() const noexcept {
        return id.hasValue() && !content.empty() && !syntax.empty() && syntax.size() <= kMaxSyntaxBytes;
    }
};

struct DeletePaste {
    PasteId id;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

/// @brief One row of `ListPastes`' result — deliberately narrower than
///        `PasteView`: a listing must not leak full paste content.
struct PasteSummary {
    PasteId id;
    std::string syntax;
    ::morph::time::Timestamp createdAt;
    Visibility visibility = Visibility::Public;
};

struct ListPastes {
    PasteCursor cursor;  // empty = first page
};

struct ListPastesResult {
    std::vector<PasteSummary> pastes;
    PasteCursor nextCursor;  // empty = no further page
};

/// @brief Internal-only: dispatched exclusively by the app-layer expiry
///        sweep's internal client (Task 6), never by a GUI client. Payload
///        is just the id — never `now()` — so replaying this entry is
///        trivially deterministic (README "How does expiry replay?").
struct ExpirePaste {
    PasteId id;

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

}  // namespace pastebin

/// @brief Reflects `Visibility` as the strings `"Public"`/`"Private"` rather
///        than its underlying `0`/`1`.
///
/// Same rationale (and same `glz::enumerate` shape) as
/// `glz::meta<morph::journal::Outcome>`: a journal line, a wire envelope, and
/// the JSON body a schema-driven form assembles all stay readable and
/// hand-writable without cross-referencing the enum. Without a `glz::meta`
/// glaze emits the bare ordinal *and* the schema writer degrades the field's
/// `$defs` entry to the any-type union `{"type":["number","string",...]}`,
/// which tells a renderer nothing at all. Persistence is unaffected: the
/// `pastes` table stores visibility as the boolean `is_private` column
/// (`src/models/paste_model.cpp`), never as this JSON form.
template <>
struct glz::meta<pastebin::Visibility> {
    using enum pastebin::Visibility;
    static constexpr auto value = glz::enumerate(Public, Private);
};

/// @brief Reflects `Editability` as the strings `"Immutable"`/`"Editable"` —
///        see `glz::meta<pastebin::Visibility>` for the full rationale.
template <>
struct glz::meta<pastebin::Editability> {
    using enum pastebin::Editability;
    static constexpr auto value = glz::enumerate(Immutable, Editable);
};
