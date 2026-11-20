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
#include "custom_screen_sync.hpp"
#include "custom_screens.hpp"
#include "document.hpp"
#include "host.hpp"
#include "navigator.hpp"
#include "value.hpp"
#include "view.hpp"
#include "workspace_model.hpp"

/// @file
/// @brief The headless controller of custom screens over one workspace: per-tab drafts, the commands that save,
///        discard, open and remove them, and their outcomes as data, for any view to draw.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief What happened, for the person to be told.
enum class CustomScreenEvent : std::uint8_t {
    Saved,               ///< The save completed and the tab shows the saved screen.
    SavedTabClosed,      ///< The save completed, but its tab was closed first.
    SavedDraftReplaced,  ///< The save completed, but the tab now holds another draft, which is not saved.
    SavedDraftGone,      ///< The save completed, but the draft it was saved from is gone.
    SavedNotOpened,      ///< **Save as** completed, but the saved screen could not be opened; `detail` says why.
    SavedNotReread,      ///< **Save** completed, but the stored screen could not be read again; `detail` says why.
    SavedStaysDraft,     ///< **Save as** completed, but the screen stayed open, so the tab stays a draft; `detail`
                         ///< says why, empty when the person kept it open.
    SaveRefused,         ///< A save whose asker is gone was refused; `detail` is the refusal.
    Removed,             ///< The screen was removed.
    RemovedStaysOpen,    ///< The screen was removed, but a tab showing it stayed open; `detail` says why, empty when
                         ///< the person kept it open.
    OpenAsCancelled,     ///< **Open as** was cancelled: the screen stayed open. `title` is that screen's.
    OpenFailed,          ///< A screen could not be opened; `detail` says why.
    SourceUnreadable,    ///< A tab's source could not be read; `detail` says why, and may be empty.
    StoredSourceUnreadable,  ///< A stored custom screen's source could not be read; `detail` says why.
    ProposalNotShown,        ///< The assistant's draft could not be mounted; `detail` says why.
};

/// @brief Which save an outcome is about.
enum class CustomScreenSaveKind : std::uint8_t {
    None,    ///< Not a save.
    Save,    ///< **Save**: an overwrite of the person's own screen.
    SaveAs,  ///< **Save as**: a new screen.
};

/// @brief An outcome the person should be told of, as data: the view words it.
struct CustomScreenOutcome {
    /// @brief What happened.
    CustomScreenEvent event = CustomScreenEvent::Saved;
    /// @brief The save it is about, if any.
    CustomScreenSaveKind save = CustomScreenSaveKind::None;
    /// @brief The title of the screen it is about.
    std::string title;
    /// @brief The reason or refusal behind it; empty when there is none.
    std::string detail;
    /// @brief How it reads: good news, a warning, an error or a plain fact.
    Tone tone = Tone::Ok;
};

/// @brief The text a source view opens with, and how it may be used.
struct CustomScreenSource {
    /// @brief The screen the text is of.
    std::string screenId;
    /// @brief The document, pretty-printed (`prettyJson`).
    std::string text;
    /// @brief Whether it may not be edited or saved: custom screens are not offered, or the stored screen is not the
    ///        person's own.
    bool readOnly = false;
    /// @brief Whether it is a stored custom screen's text (`storedSource`) rather than a tab's.
    bool stored = false;
    /// @brief The checks' refusal of the stored document, for a broken screen.
    std::optional<CheckRefusal> broken;
};

/// @brief How a check asked from a source view ended.
enum class CustomScreenCheckStatus : std::uint8_t {
    Done,     ///< It passed, and what it was asked for was done: the draft mounted, or the screen saved.
    Refused,  ///< The client, the server, the store or the host refused; `refusal` says where and why.
    Dropped,  ///< Its answer came after `dropSourceAnswers`, or after the tab's draft changed, so nothing was done.
};

/// @brief What a check asked from a source view answers.
struct CustomScreenCheckResult {
    /// @brief How it ended.
    CustomScreenCheckStatus status = CustomScreenCheckStatus::Done;
    /// @brief The refusal, for `Refused`: a JSON Pointer into the text checked, empty for the whole document.
    CheckRefusal refusal;
};

/// @brief One entry of **Open as**, as data.
struct CustomScreenChoice {
    /// @brief The screen it opens.
    std::string id;
    /// @brief Its title: the base's navigator title, or the custom screen's store title.
    std::string title;
    /// @brief Who saved it; empty for the base.
    std::string savedBy;
    /// @brief Whether the person saved it; true for the base.
    bool mine = true;
    /// @brief Whether it is broken, and opens its source instead.
    bool broken = false;
    /// @brief Whether the tab shows it now.
    bool current = false;
    /// @brief Whether two choices are the same.
    /// @return Whether every member is equal.
    bool operator==(CustomScreenChoice const&) const = default;
};

/// @brief What a `CustomScreenController` works with.
struct CustomScreenControllerOptions {
    /// @brief The runtime its state belongs to; its owner runs every command and every answer. Borrowed.
    reactive::Runtime* runtime = nullptr;
    /// @brief The host that mounts the screens it is for, which mounts and discards drafts. Borrowed.
    DocumentScreenHost* host = nullptr;
    /// @brief The client of the server's custom screens. Borrowed; it outlives the controller.
    CustomScreenSync* sync = nullptr;
    /// @brief The loaded document of an application screen id, or null: the bundle's. Required.
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
    /// @brief Told each outcome the person should hear of that no command's own answer carries; may be empty.
    std::function<void(CustomScreenOutcome const&)> onOutcome;
    /// @brief Called after the custom screens listed, or whether they are offered, changed; may be empty.
    std::function<void()> onListChanged;
};

/// @brief The headless controller of custom screens over one workspace: what each tab holds and the commands over it,
///        with no view of its own.
///
/// It keeps, per tab, the draft's text, a generation that changes with every draft mounted or ended, whether a save is
/// in flight and the last refusal of a save. Its commands mount a checked edit as a tab's draft, save it as a new
/// screen or over the person's own, discard it, open a screen in a tab's place, and remove a screen; an answer acts
/// only on the draft it was asked about, and one that finds the tab holding another draft, or none, or closed, leaves
/// the tab as it is and is told as an outcome. Outcomes and refusals are data (`CustomScreenOutcome`,
/// `CustomScreenCheckResult`): wording them, in whatever language, is the view's. Its state is reactive, so a view's
/// bindings follow it. Everything runs on the runtime's owner. Neither copyable nor movable.
class MORPH_API CustomScreenController {
public:
    /// @brief The controller.
    /// @param options What it works with.
    /// @throws std::invalid_argument for a null runtime, host or sync, or an empty `documents`, `sourceOf`,
    ///         `catalog` or `openTab`.
    explicit CustomScreenController(CustomScreenControllerOptions options);

    /// @brief Answers still in flight do nothing once it is gone.
    ~CustomScreenController();

    CustomScreenController(CustomScreenController const&) = delete;
    CustomScreenController& operator=(CustomScreenController const&) = delete;
    CustomScreenController(CustomScreenController&&) = delete;
    CustomScreenController& operator=(CustomScreenController&&) = delete;

    /// @brief The workspace whose tabs it is for: null before that workspace goes, the new one once it is built. The
    ///        tabs' state is forgotten, and answers asked under another workspace do nothing.
    /// @param model The workspace's model, or null. Borrowed while attached.
    void attach(WorkspaceModel* model);

    /// @brief Whether the server offers custom screens, as the sync's probe found.
    /// @param offered Whether they are offered.
    void setOffered(bool offered);

    /// @brief A list the sync received. Custom screens are then offered.
    /// @param listed The list.
    void listed(CustomScreensListed const& listed);

    /// @brief Whether custom screens are offered; a tracked read.
    /// @return Whether they are.
    [[nodiscard]] bool offered() const;

    /// @brief Starts keeping a tab's state: what a view calls when it draws the tab, before anything else of it.
    /// @param tab The tab.
    void follow(WorkspaceTabId tab);

    /// @brief Whether a tab holds a draft; a tracked read, so a binding made while the tab opens follows it.
    /// @param tab The tab.
    /// @return Whether it is open and holds a draft.
    [[nodiscard]] bool drafted(WorkspaceTabId tab) const;

    /// @brief Whether a save of the tab's draft is in flight; a tracked read.
    /// @param tab The tab.
    /// @return Whether one is.
    [[nodiscard]] bool saving(WorkspaceTabId tab) const;

    /// @brief The last refusal of a save of the tab's draft, or of its discard; a tracked read.
    /// @param tab The tab.
    /// @return The refusal; empty when there is none.
    [[nodiscard]] std::string message(WorkspaceTabId tab) const;

    /// @brief Whether @p screenId may be saved over and removed from this client: a listed custom screen of the
    ///        person's own while custom screens are offered. Whether it is allowed is still the store's. A tracked
    ///        read.
    /// @param screenId The screen.
    /// @return Whether it may.
    [[nodiscard]] bool canChange(std::string const& screenId) const;

    /// @brief The text of the draft a tab holds.
    /// @param tab The tab.
    /// @return The draft's document text, or nothing for a tab with no draft or a closed one.
    [[nodiscard]] std::optional<std::string> draftOf(WorkspaceTabId tab) const;

    /// @brief The screen a tab shows.
    /// @param tab The tab.
    /// @return Its id, or nothing for a closed tab.
    [[nodiscard]] std::optional<std::string> screenOf(WorkspaceTabId tab) const;

    /// @brief A custom screen as the list, or the cache, has it.
    /// @param screenId The screen.
    /// @return Its summary, or nothing when neither holds it.
    [[nodiscard]] std::optional<CustomScreenSummary> summaryOf(std::string_view screenId) const;

    /// @brief The application screen @p screenId is based on: itself for an application screen.
    /// @param screenId The screen.
    /// @return The base's id; empty for a custom screen that is not known.
    [[nodiscard]] std::string baseOf(std::string const& screenId) const;

    /// @brief A screen's title: a custom screen's store title, else the application screen's navigator title.
    /// @param screenId The screen.
    /// @return The title, or the id when there is none.
    [[nodiscard]] std::string titleOf(std::string const& screenId) const;

    /// @brief Whether the list holds @p screenId as a broken custom screen, which is never mounted.
    /// @param screenId The screen.
    /// @return Whether it is listed and broken.
    [[nodiscard]] bool isBroken(std::string_view screenId) const;

    /// @brief The text a source view of a tab opens with: its draft's while it holds one, a custom screen's stored
    ///        text (fetched again when the cache dropped it), or an application screen's as the bundle has it.
    /// @param tab  The tab; a closed one answers nothing.
    /// @param done Called once with the text; before `source` returns when nothing has to be fetched. When it
    ///             cannot be read, `onOutcome` is told `SourceUnreadable` and @p done is not called.
    void source(WorkspaceTabId tab, std::function<void(CustomScreenSource)> done);

    /// @brief The text of a stored custom screen as `get` answers it, editable while custom screens are offered and
    ///        the screen is the person's own, with its breakage. When it cannot be read, `onOutcome` is told
    ///        `SourceUnreadable` and @p done is not called.
    /// @param screenId The custom screen.
    /// @param done     Called once with the text.
    void storedSource(std::string const& screenId, std::function<void(CustomScreenSource)> done);

    /// @brief Loads @p text in the client, asks the server's `check` against the screen's base, and mounts what passes
    ///        as the tab's draft (`DocumentScreenHost::mountDraft`).
    /// @param tab  The tab.
    /// @param text The edited document's text.
    /// @param done Called once: `Done` with the draft mounted, `Refused` with the client's, the server's or the host's
    ///             refusal, or `Dropped` when `dropSourceAnswers` ran, or the tab's draft changed, before the answer.
    void preview(WorkspaceTabId tab, std::string const& text, std::function<void(CustomScreenCheckResult)> done);

    /// @brief Checks @p text on the server against the stored screen's base and saves it over the screen: what a
    ///        source view of the person's own stored screen does.
    /// @param screenId The person's custom screen.
    /// @param text     The edited document's text.
    /// @param checked  Called once the check passed and the save is sent; may be empty.
    /// @param done     Called once: `Done` once saved, `Refused` with the check's or the store's refusal, or `Dropped`
    ///                 when `dropSourceAnswers` ran first; a store's refusal after that is told as `SaveRefused`.
    ///                 `onOutcome` is told `Saved` too.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen, then the text saved over it.
    void checkAndSave(std::string const& screenId, std::string const& text, std::function<void()> checked,
                      std::function<void(CustomScreenCheckResult)> done);

    /// @brief Makes every check asked through `preview` or `checkAndSave` so far answer `Dropped`: what a view does
    ///        when the source view that asked closes or is replaced.
    void dropSourceAnswers() noexcept { ++_sourceSerial; }

    /// @brief Mounts a document the assistant proposed as @p tab's draft, as `preview` mounts one, without asking the
    ///        server again: it checked the document before proposing it. Does nothing when custom screens are not
    ///        offered, the tab is closed, or it no longer shows @p screenId; a document the client cannot load or
    ///        mount is told as `ProposalNotShown`.
    /// @param tab      The tab the turn was started on.
    /// @param screenId The screen the turn was started on.
    /// @param text     The document as JSON text.
    /// @return Whether the draft was mounted.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen's id and the document's text.
    bool proposeDraft(WorkspaceTabId tab, std::string const& screenId, std::string const& text);

    /// @brief Saves the tab's draft, or the screen it shows, as a new custom screen, then opens it in the tab's place
    ///        with the same params, while the tab still holds that draft. A refusal is the tab's `message` too.
    /// @param tab        The tab.
    /// @param title      The new screen's name.
    /// @param attributes What the store is asked to record about it; may be absent.
    /// @param done       Called once: nothing once the store answered, or the refusal; the outcome of opening the
    ///                   saved screen is told through `onOutcome`.
    void saveAs(WorkspaceTabId tab, std::string const& title, std::optional<Json::object_t> attributes,
                std::function<void(std::expected<void, std::string>)> done);

    /// @brief Overwrites the person's own custom screen with the tab's draft, which then is the screen as stored; the
    ///        screen's attributes go with it as listed. A refusal is the tab's `message`. Does nothing for a tab with
    ///        no draft, a save in flight, or a screen that is not the person's own.
    /// @param tab The tab.
    void save(WorkspaceTabId tab);

    /// @brief Mounts the screen's own document again in place of the tab's draft, fetching a custom screen's first
    ///        when the cache no longer holds it. A failure is the tab's `message`.
    /// @param tab The tab.
    void discard(WorkspaceTabId tab);

    /// @brief **Open as**: opens @p screenId in @p tab's place with the tab's params, fetching a custom screen's
    ///        document first. A refusal is told as `OpenFailed`, a screen the person kept open as `OpenAsCancelled`.
    ///        Opening a broken screen is the view's: it opens the source instead.
    /// @param tab      The tab.
    /// @param screenId The base screen, or a custom screen based on it.
    void openAs(WorkspaceTabId tab, std::string const& screenId);

    /// @brief Opens a custom screen as a tab through `openTab`, fetching its document into the cache first.
    /// @param screenId The custom screen's id.
    /// @param params   Its params.
    /// @param done     Called once with nothing, or why it did not open; may be empty, and then a failure is told as
    ///                 `OpenFailed`.
    void openCustom(std::string const& screenId, ParamMap params,
                    std::function<void(std::expected<void, std::string>)> const& done = {});

    /// @brief Removes a custom screen, then opens its base in the place of every tab that shows it, with the tab's
    ///        params. Told as `Removed`, and a tab that stayed open as `RemovedStaysOpen`.
    /// @param screenId The person's custom screen.
    /// @param done     Called once with nothing, or the store's refusal as it gave it.
    void remove(std::string const& screenId, std::function<void(std::expected<void, std::string>)> done);

    /// @brief The entries **Open as** offers on a screen: the base, then each listed custom screen of that base.
    ///        Empty while custom screens are not offered, for a base that takes no params, and when none is listed.
    ///        A tracked read of the list and the offer.
    /// @param screenId The screen the tab shows.
    /// @return The choices.
    [[nodiscard]] std::vector<CustomScreenChoice> openAsChoices(std::string const& screenId) const;

    /// @brief The navigator's Custom section: the listed screens whose base takes no params, broken ones included
    ///        with their reason. Empty while custom screens are not offered.
    /// @return The entries, in the list's order.
    [[nodiscard]] std::vector<NavigatorCustomEntry> navigatorEntries() const;

private:
    // What is kept per tab. @c generation changes with every draft mounted or ended, so an answer can tell whether the
    // tab still holds the draft it was asked about. @c pending holds while a save is in flight. @c seen holds once the
    // model held the tab, so a state made while the tab was opening is not taken for a closed tab's.
    struct TabState {
        std::unique_ptr<reactive::Signal<std::string>> message;
        std::unique_ptr<reactive::Signal<bool>> pending;
        std::string draftText;
        std::uint64_t generation = 0;
        bool seen = false;
    };

    // Wraps @p fn so it does nothing once the controller is gone, or under another workspace: answers arrive through
    // the sync, which may outlive it, and tab ids are issued per workspace.
    template <typename F>
    [[nodiscard]] auto guarded(F fn) const {
        return
            [this, epoch = _epoch, alive = std::weak_ptr<bool>{_alive}, fn = std::move(fn)](auto&&... args) mutable {
                if (auto const live = alive.lock(); live != nullptr && *live && _epoch == epoch) {
                    fn(std::forward<decltype(args)>(args)...);
                }
            };
    }

    [[nodiscard]] std::shared_ptr<TabState> stateOf(WorkspaceTabId tab) const;
    // Drops the state of tabs that closed and have no save in flight.
    void prune();
    // Ends a save in flight on @p tab.
    void settled(WorkspaceTabId tab);
    // Whether the tab is open and still holds the draft of @p generation.
    [[nodiscard]] bool holds(WorkspaceTabId tab, std::uint64_t generation) const;
    // Sets the tab's message, when the tab still has state.
    void tell(WorkspaceTabId tab, std::string text);
    // Tells how a save ended whose draft is gone, replaced, or whose tab closed.
    void saveOutcome(WorkspaceTabId tab, std::uint64_t generation, CustomScreenSaveKind kind,
                     std::string const& title);
    [[nodiscard]] ScreenHandle* handleOf(WorkspaceTabId tab) const;
    [[nodiscard]] std::string currentText(std::string const& screenId) const;
    // Mounts @p document as the tab's draft with @p text as its source: the generation moves on and the message
    // clears. The one place a draft is mounted.
    [[nodiscard]] std::expected<void, MountError> mountAsDraft(WorkspaceTabId tab,
                                                               std::shared_ptr<Document const> const& document,
                                                               std::string const& text);
    void openSaved(WorkspaceTabId tab, std::uint64_t generation, std::string const& screenId, std::string const& title,
                   std::function<void(std::expected<void, std::string>)> const& done);
    void fallBack(std::string const& screenId, std::string const& basedOn, std::string const& title);
    void tellOutcome(CustomScreenOutcome const& outcome) const;

    CustomScreenControllerOptions _options;
    WorkspaceModel* _model = nullptr;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::unique_ptr<reactive::Signal<bool>> _offered;
    // Bumped on each list, so a binding that reads `_listed` follows it.
    std::unique_ptr<reactive::Signal<std::uint64_t>> _listVersion;
    CustomScreensListed _listed;
    std::uint64_t _epoch = 0;
    std::uint64_t _sourceSerial = 0;
    std::map<std::uint64_t, std::shared_ptr<TabState>> _tabs;
};

}  // namespace morph::ui
