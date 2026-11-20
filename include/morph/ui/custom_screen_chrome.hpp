// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "custom_screen_controller.hpp"
#include "custom_screen_sync.hpp"
#include "custom_screens.hpp"
#include "document.hpp"
#include "host.hpp"
#include "mount.hpp"
#include "navigator.hpp"
#include "view.hpp"
#include "workspace_model.hpp"

/// @file
/// @brief The window chrome of custom screens: the draft bar and the **Source** button a shell mounts on each tab's
///        page, the source view, and the save dialog.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief What a `CustomScreenChrome` works with.
struct CustomScreenChromeOptions {
    /// @brief The runtime the chrome's bindings belong to. Borrowed.
    reactive::Runtime* runtime = nullptr;
    /// @brief The backend its overlays are made with. Borrowed.
    IViewBackend* backend = nullptr;
    /// @brief The host that mounts the screens it is for, which mounts and discards drafts. Borrowed.
    DocumentScreenHost* host = nullptr;
    /// @brief The client of the server's custom screens. Borrowed; it outlives the chrome.
    CustomScreenSync* sync = nullptr;
    /// @brief The loaded document of an application screen id, or null: the bundle's. A screen with none (a host
    ///        screen) gets no chrome. Required.
    std::function<std::shared_ptr<Document const>(std::string_view)> documents;
    /// @brief The JSON text of an application screen's document as the bundle has it; empty for an unknown id.
    ///        Required.
    std::function<std::string(std::string_view)> sourceOf;
    /// @brief The catalog facts a draft loads with. Required.
    std::function<Catalog()> catalog;
    /// @brief The title of an application screen, as its navigator entry shows it; empty shows its id.
    std::function<std::string(std::string_view)> titleOf;
    /// @brief Opens a screen as a tab, as the shell's own navigation does, answering why when the mount was refused.
    ///        Required.
    std::function<std::expected<void, std::string>(std::string const& screenId, ParamMap const& params)> openTab;
    /// @brief Tells the person something the window does not show, with its tone; may be empty.
    std::function<void(std::string const& text, Tone tone)> notify;
    /// @brief Called after the custom screens listed, or whether they are offered, changed, so the shell lists its
    ///        navigator's Custom section again; may be empty.
    std::function<void()> onListChanged;
    /// @brief Where the overlays are made; null makes each a backend root.
    ContainerWidget* overlayParent = nullptr;
};

/// @brief The window chrome of custom screens over one workspace, built from backend nodes so every frontend shows it.
///
/// For each document screen's tab, `chromeFor` gives a draft bar, shown while the tab holds a draft (Save for a draft
/// of the person's own custom screen, Save as…, Source and Discard, and the last refusal of a save), and a row with
/// the **Source** button, shown while it does not. **Source** opens the source view, an overlay with the document's
/// JSON pretty-printed: read only while custom screens are not offered. **Preview** loads the text in the client, asks
/// the server's `check`, and mounts what passes as the tab's draft (`DocumentScreenHost::mountDraft`); a refusal is
/// shown with the line its pointer names, and choosing it moves the cursor there; the editor is read only while the
/// check runs, so the draft is exactly the text previewed. **Save as…** asks a name, saves, and opens the saved screen
/// in the tab's place. A reply to a save acts only on the draft that was saved: a tab that holds a newer draft, or
/// none, keeps it and the person is told. **Remove**, on a screen of the person's own, asks once and removes it; a tab
/// that showed it opens its base.
///
/// It is a view over a `CustomScreenController`, which holds every tab's draft and runs every command: the chrome
/// draws the controller's state, and words its outcomes and refusals. Its fixed words are English; they are the
/// view's, so localising them is the view's too. An application with controls of its own draws them over a controller
/// of its own instead. Everything runs on the runtime's owner. Neither copyable nor movable.
class MORPH_API CustomScreenChrome {
public:
    /// @brief The chrome.
    /// @param options What it works with.
    /// @throws std::invalid_argument for a null runtime, backend, host or sync, or an empty `documents`, `sourceOf`,
    ///         `catalog` or `openTab`.
    explicit CustomScreenChrome(CustomScreenChromeOptions options);

    /// @brief Closes the overlays; replies still in flight do nothing once it is gone.
    ~CustomScreenChrome();

    CustomScreenChrome(CustomScreenChrome const&) = delete;
    CustomScreenChrome& operator=(CustomScreenChrome const&) = delete;
    CustomScreenChrome(CustomScreenChrome&&) = delete;
    CustomScreenChrome& operator=(CustomScreenChrome&&) = delete;

    /// @brief The workspace whose tabs the chrome is for. Call it with null before that workspace goes, and with the
    ///        new one once it is built: the overlays close, and the tabs' state is forgotten with a new workspace.
    ///        Call it before any tab opens: it clears the per-tab state, which a tab already open would lose (a
    ///        draft's text included).
    /// @param model The workspace's model, or null. Borrowed while attached.
    void attach(WorkspaceModel* model);

    /// @brief Whether the server offers custom screens, as the sync's probe found: without them the source view is
    ///        read only and nothing can be saved.
    /// @param offered Whether they are offered.
    void setOffered(bool offered);

    /// @brief A list the sync received: what Save, **Open as** and the navigator read. Custom screens are then
    /// offered.
    /// @param listed The list.
    void listed(CustomScreensListed const& listed);

    /// @brief The chrome of one tab, for `WorkspaceOptions::chrome`.
    /// @param tab      The tab.
    /// @param screenId Its screen's id.
    /// @return The draft bar and the tools row; null for a screen with no document, such as a host screen.
    [[nodiscard]] Node chromeFor(WorkspaceTabId tab, std::string_view screenId);

    /// @brief Opens the source view of a tab's screen: its draft's text while it holds one, a custom screen's stored
    ///        text, or an application screen's text as the bundle has it.
    /// @param tab The tab; a closed one does nothing.
    void showSource(WorkspaceTabId tab);

    /// @brief The source view's **Preview**: loads its text in the client, checks it on the server, and mounts it as
    ///        the tab's draft, closing the view; or shows the first refusal.
    /// @param done Called once with whether a draft was mounted; may be empty.
    void preview(std::function<void(bool drafted)> const& done = {});

    /// @brief The text of the draft a tab holds, as the next chat turn sends it as the screen being edited.
    /// @param tab The tab.
    /// @return The draft's document text, or nothing for a tab with no draft or a closed one.
    [[nodiscard]] std::optional<std::string> draftOf(WorkspaceTabId tab) const;

    /// @brief Mounts a document the assistant proposed as @p tab's draft, as **Preview** mounts one: the document is
    ///        loaded in the client and mounted in the tab's place, so the draft bar, the generation and the unsaved
    ///        state are the same, and a second draft replaces the first. The server has checked the document before
    ///        it was proposed, so it is not asked again. Does nothing, and tells nobody, when custom screens are not
    ///        offered, the tab is closed, or the tab no longer shows @p screenId; a document the client cannot load
    ///        or mount is reported through `notify` and the tab stays as it was.
    /// @param tab      The tab the turn was started on.
    /// @param screenId The screen the turn was started on.
    /// @param text     The document as JSON text.
    /// @return Whether the draft was mounted.
    bool proposeDraft(WorkspaceTabId tab, std::string const& screenId, std::string const& text);

    /// @brief Opens the save dialog for a tab's draft, or for the source view's text when @p fromSource holds, which
    ///        is previewed first so nothing is saved the person has not seen mounted.
    /// @param tab        The tab.
    /// @param fromSource Whether the text is the source view's.
    void askSaveAs(WorkspaceTabId tab, bool fromSource);

    /// @brief The save dialog's **Save**: saves under the name typed, then opens the saved screen in the tab's place,
    ///        unless the tab holds a newer draft by then. A refusal stays in the dialog and in the draft's bar.
    void confirmSaveAs();

    /// @brief The draft bar's **Save**: overwrites the person's own custom screen with the tab's draft, which then is
    ///        the screen as stored. A refusal shows in the bar and the draft stays.
    /// @param tab The tab.
    void save(WorkspaceTabId tab);

    /// @brief The draft bar's **Discard**: mounts the screen's own document again, fetching a custom screen's first
    ///        when the cache no longer holds it.
    /// @param tab The tab.
    void discard(WorkspaceTabId tab);

    /// @brief The navigator's Custom section: the listed screens whose base takes no params, so they open with none,
    ///        broken ones included with their reason. Empty while custom screens are not offered.
    /// @return The entries, in the list's order.
    [[nodiscard]] std::vector<NavigatorCustomEntry> navigatorEntries() const;

    /// @brief Opens a custom screen: fetches its document into the cache when it is not there, then opens it as a tab
    ///        through `openTab`. A broken one opens its source view instead, since it is never mounted.
    /// @param screenId The custom screen's id.
    /// @param params   Its params.
    /// @param done     Called once with nothing, or why it did not open (a broken screen, which opened its source
    ///                 view only, or a refused mount); may be empty, and then a failure is told through `notify`.
    void openCustom(std::string const& screenId, ParamMap params,
                    std::function<void(std::expected<void, std::string>)> const& done = {});

    /// @brief Whether the list holds @p screenId as a broken custom screen, which is never mounted.
    /// @param screenId The screen.
    /// @return Whether it is listed and broken.
    [[nodiscard]] bool isBroken(std::string_view screenId) const;

    /// @brief Opens the source view of a stored custom screen, as `get` answers it, with its breakage shown as the
    ///        refusal: editable while custom screens are offered and the screen is the person's own, when
    ///        **Check and save** checks the text on the server and saves it over the screen.
    /// @param screenId The custom screen's id.
    void showStoredSource(std::string const& screenId);

    /// @brief **Open as**: opens @p screenId in @p tab's place with the tab's params, fetching a custom screen's
    ///        document first; a broken one opens its source view instead.
    /// @param tab      The tab.
    /// @param screenId The base screen, or a custom screen based on it.
    void openAs(WorkspaceTabId tab, std::string const& screenId);

    /// @brief The tools row's **Remove**: asks the person to confirm removing the tab's custom screen, which is
    ///        offered only for a listed screen of the person's own while custom screens are offered.
    /// @param tab The tab; one that shows another screen, or none, does nothing.
    void askRemove(WorkspaceTabId tab);

    /// @brief The confirmation's **Remove**: removes the screen through the sync, then opens its base in the place of
    ///        every tab that shows it, with the tab's params. A refusal stays in the confirmation, as the store gave
    ///        it.
    void confirmRemove();

    /// @brief The controller the chrome draws.
    /// @return It, owned by the chrome.
    [[nodiscard]] CustomScreenController& controller() noexcept { return _controller; }

    /// @brief An outcome in the chrome's words, as `notify` is told it.
    /// @param outcome The outcome.
    /// @return The sentence.
    [[nodiscard]] static std::string describe(CustomScreenOutcome const& outcome);

private:
    // A refusal and the text it was made for: its line is read against that text.
    struct ShownRefusal {
        std::string pointer;
        std::string reason;
        std::string text;
        bool operator==(ShownRefusal const&) const = default;
    };

    // The source view: its text, its refusal, its status line, and the mount of its dialog, destroyed first.
    struct Source {
        bool stored = false;
        std::optional<WorkspaceTabId> tab;
        std::string screenId;
        bool readOnly = false;
        // The text the view opened with: what differs from it is the person's own edit.
        std::string opened;
        std::unique_ptr<reactive::Signal<std::string>> text;
        std::unique_ptr<reactive::Signal<std::optional<ShownRefusal>>> refusal;
        std::unique_ptr<reactive::Signal<std::string>> status;
        std::unique_ptr<reactive::Signal<bool>> busy;
        std::optional<Mounted> view;
    };

    // The save dialog.
    struct SaveAs {
        WorkspaceTabId tab;
        bool fromSource = false;
        std::unique_ptr<reactive::Signal<std::string>> title;
        std::unique_ptr<reactive::Signal<std::string>> message;
        std::unique_ptr<reactive::Signal<bool>> busy;
        std::optional<Mounted> view;
    };

    // The confirmation of a removal.
    struct RemoveConfirm {
        std::uint64_t generation = 0;
        std::string screenId;
        std::unique_ptr<reactive::Signal<std::string>> message;
        std::unique_ptr<reactive::Signal<bool>> busy;
        std::optional<Mounted> view;
    };

    // The Open as menu, keyed so it is drawn again whenever an entry changes.
    struct OpenAsShape {
        std::string key;
        std::vector<CustomScreenChoice> choices;
        bool operator==(OpenAsShape const&) const = default;
    };

    // Wraps @p fn so it does nothing once the chrome is gone; posted closes and dialog presses can outlive it.
    template <typename F>
    [[nodiscard]] auto guarded(F fn) const {
        return [alive = std::weak_ptr<bool>{_alive}, fn = std::move(fn)](auto&&... args) mutable {
            if (auto const live = alive.lock(); live != nullptr && *live) {
                fn(std::forward<decltype(args)>(args)...);
            }
        };
    }

    [[nodiscard]] Node draftBar(WorkspaceTabId tab, std::string const& screenId);
    [[nodiscard]] Node toolsRow(WorkspaceTabId tab, std::string const& screenId);
    [[nodiscard]] Node openAsMenu(WorkspaceTabId tab, std::string const& screenId);
    void openSource(CustomScreenSource const& shown, std::optional<WorkspaceTabId> tab);
    void saveFromSource();
    void refuse(std::string pointer, std::string reason);
    void moveToRefusal();
    void closeSourceLater();
    void closeSaveAsLater();
    void closeRemoveLater();
    void failSaveAs(WorkspaceTabId tab, std::string const& reason);
    void notify(std::string const& text, Tone tone) const;

    CustomScreenChromeOptions _options;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    CustomScreenController _controller;
    std::uint64_t _lastRemove = 0;
    // The overlays, last: their bindings read everything above.
    std::unique_ptr<Source> _source;
    std::unique_ptr<SaveAs> _saveAs;
    std::unique_ptr<RemoveConfirm> _remove;
};

}  // namespace morph::ui
