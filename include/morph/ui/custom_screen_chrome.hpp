// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "custom_screen_sync.hpp"
#include "custom_screens.hpp"
#include "document.hpp"
#include "host.hpp"
#include "mount.hpp"
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
    /// @brief Opens a screen as a tab, as the shell's own navigation does. Required.
    std::function<void(std::string const& screenId, ParamMap const& params)> openTab;
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
/// the
/// **Source** button, shown while it does not. **Source** opens the source view, an overlay with the document's JSON
/// pretty-printed: read only while custom screens are not offered. **Preview** loads the text in the client, asks the
/// server's `check`, and mounts what passes as the tab's draft (`DocumentScreenHost::mountDraft`); a refusal is shown
/// with the line its pointer names, and choosing it moves the cursor there; the editor is read only while the check
/// runs, so the draft is exactly the text previewed. **Save as…** asks a name, saves, and opens the saved screen in
/// the tab's place. A reply to a save acts only on the draft that was saved: a tab that holds a newer draft, or none,
/// keeps it and the person is told. Everything runs on the runtime's owner. Neither copyable nor movable.
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
    void preview(std::function<void(bool drafted)> done = {});

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

private:
    enum class SourceMode : std::uint8_t { Draft, Stored };

    // A refusal and the text it was made for: its line is read against that text.
    struct ShownRefusal {
        std::string pointer;
        std::string reason;
        std::string text;
        bool operator==(ShownRefusal const&) const = default;
    };

    // What the chrome keeps per tab: the bar's last refusal, and the draft's text while one is mounted.
    // @c generation changes with every draft mounted or ended on the tab, so a reply can tell whether the tab still
    // holds the draft it was asked about. @c pending holds while a save is in flight. @c seen holds once the model
    // held the tab, so a state made while the tab was opening is not taken for a closed tab's.
    struct TabState {
        std::unique_ptr<reactive::Signal<std::string>> message;
        std::unique_ptr<reactive::Signal<bool>> pending;
        std::string draftText;
        std::uint64_t generation = 0;
        bool seen = false;
    };

    // The source view: its text, its refusal, its status line, and the mount of its dialog, destroyed first.
    struct Source {
        SourceMode mode = SourceMode::Draft;
        // Distinguishes this view from any other opened since, so a reply to a check made for one does nothing to the
        // next.
        std::uint64_t generation = 0;
        std::optional<WorkspaceTabId> tab;
        std::string screenId;
        bool readOnly = false;
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

    // Wraps @p fn so it does nothing once the chrome is gone: replies arrive through the sync, which may outlive it.
    template <typename F>
    [[nodiscard]] auto guarded(F fn) const {
        // Tab ids are issued per workspace, so a reply made under a workspace that has been replaced does nothing too.
        return
            [this, epoch = _epoch, alive = std::weak_ptr<bool>{_alive}, fn = std::move(fn)](auto&&... args) mutable {
                if (auto const live = alive.lock(); live != nullptr && *live && _epoch == epoch) {
                    fn(std::forward<decltype(args)>(args)...);
                }
            };
    }

    [[nodiscard]] Node draftBar(WorkspaceTabId tab, std::string const& screenId,
                                std::shared_ptr<TabState> const& state);
    [[nodiscard]] Node toolsRow(WorkspaceTabId tab, std::string const& screenId);
    // The tab's state, made with its chrome.
    std::shared_ptr<TabState> ensureState(WorkspaceTabId tab);
    [[nodiscard]] std::shared_ptr<TabState> stateOf(WorkspaceTabId tab) const;
    // Drops the state of tabs that closed and have no save in flight.
    void prune();
    // Ends a save in flight on @p tab.
    void settled(WorkspaceTabId tab);
    // Whether the tab is open and still holds the draft of @p generation.
    [[nodiscard]] bool holds(WorkspaceTabId tab, std::uint64_t generation) const;
    // Shows @p text in the tab's bar, when the tab still has one.
    void tell(WorkspaceTabId tab, std::string text);
    // Tells the person a reply found its draft gone or replaced: what was saved, and that the tab was left as it is.
    void saveOutcome(WorkspaceTabId tab, std::uint64_t generation, std::string const& saved);
    [[nodiscard]] ScreenHandle* handleOf(WorkspaceTabId tab) const;
    // A tracked read of the model's tabs first, so a binding made while the tab is opening reads the handle once the
    // model holds it.
    [[nodiscard]] ScreenHandle* trackedHandleOf(WorkspaceTabId tab) const;
    [[nodiscard]] bool draftedTracked(WorkspaceTabId tab) const;
    [[nodiscard]] bool canSave(std::string const& screenId) const;
    [[nodiscard]] std::optional<CustomScreenSummary> summaryOf(std::string_view screenId) const;
    [[nodiscard]] std::string baseOf(std::string const& screenId) const;
    [[nodiscard]] std::string currentText(std::string const& screenId) const;
    [[nodiscard]] std::string titleOf(std::string const& screenId) const;
    void openSource(SourceMode mode, std::optional<WorkspaceTabId> tab, std::string screenId, std::string text,
                    bool readOnly);
    void refuse(std::string pointer, std::string reason);
    void moveToRefusal();
    void closeSourceLater();
    void closeSaveAsLater();
    void saveAs(WorkspaceTabId tab, std::string const& title);
    void failSaveAs(WorkspaceTabId tab, std::uint64_t generation, std::string const& reason);
    void openSaved(WorkspaceTabId tab, std::uint64_t generation, std::string const& id, std::string const& title);
    void notify(std::string const& text, Tone tone) const;

    CustomScreenChromeOptions _options;
    WorkspaceModel* _model = nullptr;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::unique_ptr<reactive::Signal<bool>> _offered;
    // Bumped on each list, so a binding that reads `_listed` follows it.
    std::unique_ptr<reactive::Signal<std::uint64_t>> _listVersion;
    CustomScreensListed _listed;
    std::uint64_t _epoch = 0;
    std::uint64_t _lastSource = 0;
    std::map<std::uint64_t, std::shared_ptr<TabState>> _tabs;
    // The overlays, last: their bindings read everything above.
    std::unique_ptr<Source> _source;
    std::unique_ptr<SaveAs> _saveAs;
};

}  // namespace morph::ui
