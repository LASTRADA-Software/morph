// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "pastebin/core/errors.hpp"
#include "pastebin/dto/paste_dto.hpp"

/// @file
/// The one model this rung ships. `examples/IMPLEMENTATION.md` rule 1 —
/// models *are* the application: every pastebin business rule (id allocation,
/// expiry, burn-after-read, editability, listing/pagination) lives here and
/// nowhere else. The app bootstrap, presenters, and GUI carry no domain logic.

namespace pastebin {

/// @brief Create/read/edit/delete/list/expire over the `pastes` table.
///
/// Registered **plain** — no `BRIDGE_MODEL_KEY`, no `AllowShared` (the
/// README's resolved burn-atomicity decision): every action dispatch gets a
/// fresh instance and all real state lives in `pastes`. This model holds no
/// database state itself — each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning a connection for its own
/// lifetime. Burn-after-read atomicity therefore comes from SQL, not from a
/// shared C++ instance — see `execute(const GetPaste&)` in
/// `src/models/paste_model.cpp` for the exact mechanism and why it is safe
/// against two clients racing on the last allowed read.
class PasteModel {
public:
    /// @brief Stores a new paste under a freshly allocated animal-name id.
    /// @param action The paste to store.
    /// @return The allocated id.
    /// @throws ValidationError if the action fails `validate()`, or if no free
    ///         id could be allocated within the bounded retry budget.
    CreatePasteResult execute(const CreatePaste& action);

    /// @brief Consumes one read of a paste and returns it.
    /// @param action The paste to read.
    /// @return The paste, with its post-read `readCount`.
    /// @throws ValidationError if the action fails `validate()`.
    /// @throws NotFound if no such paste exists (or it was burned away).
    /// @throws Expired if the paste's `expiresAt` has passed.
    /// @throws Burned if the paste's burn-after-reads budget was already spent.
    PasteView execute(const GetPaste& action);

    /// @brief Replaces an editable paste's content and syntax.
    /// @param action The edit to apply.
    /// @return The paste as it now stands.
    /// @throws ValidationError if the action fails `validate()` or the paste is
    ///         immutable.
    /// @throws NotFound if no such paste exists.
    PasteView execute(const EditPaste& action);

    /// @brief Deletes a paste, whether or not it exists.
    /// @param action The paste to delete.
    /// @return An acknowledgement.
    /// @throws ValidationError if the action fails `validate()`.
    Ack execute(const DeletePaste& action);

    /// @brief Returns one page of public pastes, newest id first.
    /// @param action The page request (empty cursor = first page).
    /// @return The page, plus the cursor for the next one (empty when exhausted).
    ListPastesResult execute(const ListPastes& action);

    /// @brief Reclaims one paste whose `expiresAt` has passed.
    ///
    /// Dispatched only by the app-layer expiry sweep's internal client
    /// (Task 6) — never by a GUI client. Deliberately a no-op (still `Ack`)
    /// when the paste is absent or not actually expired yet, so a replayed or
    /// late-arriving sweep entry can never destroy a live paste.
    /// @param action The paste to reclaim.
    /// @return An acknowledgement.
    /// @throws ValidationError if the action fails `validate()`.
    Ack execute(const ExpirePaste& action);
};

}  // namespace pastebin

BRIDGE_REGISTER_MODEL(pastebin::PasteModel, "PasteModel")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::CreatePaste, "CreatePaste")
// GetPaste stays the one client-visible, journaled *mutation* (default
// Loggable::Yes) — the README's resolved journal decision; it is deliberately
// not split into an unlogged read plus a RecordRead, and must not opt out.
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::GetPaste, "GetPaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::EditPaste, "EditPaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::DeletePaste, "DeletePaste")
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::ListPastes, "ListPastes", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(pastebin::PasteModel, pastebin::ExpirePaste, "ExpirePaste")
