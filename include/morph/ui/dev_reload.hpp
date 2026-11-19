// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bundle.hpp"
#include "chat/panel.hpp"
#include "document.hpp"
#include "frontend.hpp"
#include "host.hpp"
#include "value.hpp"

/// @file
/// @brief Development reload (spec 5 §12): which documents a changed bundle changed, and remounting the mounted
///        screens that show them, keeping their state.
///
/// A development client polls with `UiClient::poll` and hands each changed bundle to `reloadDocuments`. Specified in
/// `docs/spec/ui/service.md`.

namespace morph::ui {

/// @brief The loaded documents a host mounts from, by screen id.
using DocumentMap = std::map<std::string, std::shared_ptr<Document const>, std::less<>>;

/// @brief The ids of the documents (screens and the app document) whose files differ between two manifests: changed,
///        added or removed. Themes, message catalogues and components are not documents; a document that names a
///        changed component names it by its new hash, so it changes too.
/// @param before The manifest the client had.
/// @param after  The manifest it has now.
/// @return The ids, sorted and each once.
[[nodiscard]] inline std::vector<std::string> changedDocuments(Manifest const& before, Manifest const& after) {
    auto const documents = [](Manifest const& manifest) {
        std::map<std::string, std::string, std::less<>> hashes;
        for (ManifestFile const& file : manifest.files) {
            if (file.kind == kScreenFile || file.kind == kAppFile) {
                hashes.insert_or_assign(file.id, file.hash);
            }
        }
        return hashes;
    };
    auto const old = documents(before);
    auto const now = documents(after);
    std::vector<std::string> changed;
    for (auto const& [document, hash] : now) {
        auto const found = old.find(document);
        if (found == old.end() || found->second != hash) {
            changed.push_back(document);
        }
    }
    for (auto const& [document, hash] : old) {
        static_cast<void>(hash);
        if (!now.contains(document)) {
            changed.push_back(document);
        }
    }
    std::ranges::sort(changed);
    return changed;
}

/// @brief Why a server's catalog entry may not be mounted as a screen, when it may not.
///
/// The chat panel's id carries host values and commands no other document may reach, so the panel is the client's
/// own document and never a server's.
/// @param screenId The catalog entry's id.
/// @return The refusal when @p screenId is the chat panel's, otherwise nothing.
[[nodiscard]] inline std::optional<Refusal> servedScreenRefusal(std::string const& screenId) {
    if (screenId == chat::kChatPanelId) {
        return Refusal{.document = screenId,
                       .path = {},
                       .reason = "the catalog lists the id `" + std::string{chat::kChatPanelId} +
                                 "`, which is the chat panel's; the entry is refused"};
    }
    return std::nullopt;
}

/// @brief Why a loaded server document may not be mounted under its catalog entry, when it may not.
///
/// Host names are scoped by the id a mounted document carries, so a document that names another id than its entry
/// could take that id's scope.
/// @param screenId   The catalog entry's id.
/// @param documentId The id the loaded document names.
/// @return The refusal when the ids differ, otherwise nothing.
[[nodiscard]] inline std::optional<Refusal> servedDocumentRefusal(std::string const& screenId,
                                                                  std::string const& documentId) {
    if (documentId != screenId) {
        return Refusal{.document = screenId,
                       .path = {},
                       .reason = "the document of `" + screenId + "` names the id `" + documentId +
                                 "`; it is refused"};
    }
    return std::nullopt;
}

/// @brief A mounted screen a reload may remount.
struct MountedScreen {
    /// @brief The screen id it was mounted from.
    std::string screenId;
    /// @brief Its handle, mounted by the host the reload is given. Borrowed for the call.
    ScreenHandle* handle = nullptr;
};

/// @brief What `reloadDocuments` did.
struct ReloadOutcome {
    /// @brief The screen id of each screen remounted on its new document, once per screen.
    std::vector<std::string> remounted;
    /// @brief Each new document that did not load, and each remount the interpreter refused; the screen and the
    ///        document map keep the old document for both.
    std::vector<Refusal> refused;
};

/// @brief Loads the changed documents of a new bundle and remounts the mounted screens that show them.
///
/// Each changed screen document is loaded with @p options. A document that does not load is refused and keeps the old
/// document, in @p documents and on every screen showing it. A document that loads is mounted, through
/// `DocumentScreenHost::remount`, on every screen of @p mounted showing it, which keeps each state of the screen's own
/// scope declared under the same name and type. A screen showing a draft (`DocumentScreenHost::mountDraft`) is left as
/// it is. A remount the interpreter refuses keeps that screen on the old document, and keeps the old document in @p
/// documents. A changed id that is the chat panel's, and a document that names another id than its screen's, are
/// refused the same way. A changed id the bundle no longer holds, and the app document, which is the session's root scope and not
/// a screen (`DocumentScreenHost::mountApp`), change nothing here.
/// @param changed   The ids of the changed documents (`changedDocuments`).
/// @param bundle    The new bundle's documents.
/// @param options   How the documents load: the catalog facts.
/// @param documents The host's documents, updated in place.
/// @param host      The host that mounted every screen of @p mounted.
/// @param mounted   The mounted screens.
/// @return The screens remounted, and the refusals.
[[nodiscard]] inline ReloadOutcome reloadDocuments(std::span<std::string const> changed, Bundle const& bundle,
                                                   LoadOptions const& options, DocumentMap& documents,
                                                   DocumentScreenHost& host, std::span<MountedScreen const> mounted) {
    ReloadOutcome outcome;
    for (std::string const& screenId : changed) {
        auto const text = bundle.documents.find(screenId);
        if (screenId == "app" || text == bundle.documents.end()) {
            continue;
        }
        if (auto refusal = servedScreenRefusal(screenId)) {
            outcome.refused.push_back(std::move(*refusal));
            continue;
        }
        auto loaded = loadDocumentText(text->second, options);
        if (loaded) {
            if (auto refusal = servedDocumentRefusal(screenId, loaded->id)) {
                outcome.refused.push_back(std::move(*refusal));
                continue;
            }
        }
        if (!loaded) {
            Refusal refusal = std::move(loaded.error());
            if (refusal.document.empty()) {
                refusal.document = screenId;
            }
            outcome.refused.push_back(std::move(refusal));
            continue;
        }
        auto const document = std::make_shared<Document const>(std::move(*loaded));
        bool kept = true;
        for (MountedScreen const& screen : mounted) {
            // A draft stays mounted: it is checked against the new bundle when the person previews or saves it, and
            // discarding it mounts the document the map holds then.
            if (screen.screenId != screenId || screen.handle == nullptr ||
                DocumentScreenHost::draftedNow(*screen.handle)) {
                continue;
            }
            if (auto remounted = host.remount(*screen.handle, document); !remounted) {
                outcome.refused.push_back(
                    Refusal{.document = screenId, .path = {}, .reason = std::move(remounted.error().reason)});
                kept = false;
                continue;
            }
            if (std::ranges::find(outcome.remounted, screenId) == outcome.remounted.end()) {
                outcome.remounted.push_back(screenId);
            }
        }
        if (kept) {
            documents.insert_or_assign(screenId, document);
        }
    }
    return outcome;
}

}  // namespace morph::ui
