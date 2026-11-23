// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "backend.hpp"
#include "host.hpp"
#include "interpreter.hpp"

/// @file
/// @brief Guided tours on the client: `TourController` runs one of the app's tours at a time over a
///        `TourShell`, the little it needs of the shell it runs in. Specified in `docs/spec/ui/tours.md`.

namespace morph::ui {

/// @brief What a tour reaches of the shell it runs in. The generic client implements it over its workspace, its frame
///        and its navigator; a test implements it with a fake.
class TourShell {
public:
    TourShell() = default;
    virtual ~TourShell() = default;
    TourShell(TourShell const&) = delete;
    TourShell& operator=(TourShell const&) = delete;
    TourShell(TourShell&&) = delete;
    TourShell& operator=(TourShell&&) = delete;

    /// @brief Opens a screen as a navigator click does, or focuses the tab that already shows it.
    /// @param screenId The screen.
    /// @param params   Its params.
    /// @return Nothing, or why it could not be opened.
    virtual std::expected<void, MountError> openScreen(std::string const& screenId, ParamMap const& params) = 0;

    /// @brief Whether the active tab shows a screen with these params: the same screen, compared by its identity.
    /// @param screenId The screen.
    /// @param params   Its params.
    /// @return It.
    [[nodiscard]] virtual bool shows(std::string const& screenId, ParamMap const& params) const = 0;

    /// @brief The screen the active tab shows, a custom screen named by the screen it is based on.
    /// @return Its id; empty with no tab.
    [[nodiscard]] virtual std::string activeScreen() const = 0;

    /// @brief A screen's title as the navigator shows it, for the hint of a step that waits for it.
    /// @param screenId The screen.
    /// @return The title; the id when there is none.
    [[nodiscard]] virtual std::string screenTitle(std::string const& screenId) const = 0;

    /// @brief Whether the frame has its compact form.
    /// @return It.
    [[nodiscard]] virtual bool compact() const = 0;

    /// @brief Makes a navigator entry drawable in the wide form: empties the navigator's search and opens the module
    ///        that holds the entry.
    /// @param screenId The entry's screen.
    virtual void revealNavEntry(std::string const& screenId) = 0;

    /// @brief The spotlight a tour is shown in, made on first use and kept as long as the frame.
    /// @return It.
    virtual SpotlightWidget& spotlight() = 0;

    /// @brief Runs @p then once the card shows the step the controller has just set. The controller opens the
    ///        spotlight from it: opening moves focus to the card's first shown button, which is only the right one
    ///        (Next, or End tour on a waiting step) when the card has caught up with the step. The default runs it at
    ///        once, for a shell whose card is current as soon as the view changes; a shell whose card updates after
    ///        the current turn runs it after that update. It may be dropped unrun when the shell goes away.
    /// @param then What to run.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): an override may keep the callback to run it later.
    virtual void afterCardUpdated(std::function<void()> then) { then(); }

    /// @brief Tells the person something, as a toast.
    /// @param text The message.
    /// @param tone Its tone.
    virtual void notify(std::string const& text, Tone tone) = 0;
};

/// @brief What the card shows of the running step.
struct TourView {
    /// @brief The tour's id.
    std::string tourId;
    /// @brief The tour's title, the card's eyebrow.
    std::string title;
    /// @brief The step's title.
    std::string stepTitle;
    /// @brief The step's body; empty for none.
    std::string body;
    /// @brief The step's place, from 1.
    std::int64_t position = 0;
    /// @brief How many steps the tour has.
    std::int64_t count = 0;
    /// @brief Whether the step waits for a screen to become active rather than for Next. A step whose screen is
    ///        already active when it is shown does not wait.
    bool waiting = false;
    /// @brief The title of the screen a waiting step waits for; empty otherwise.
    std::string waitingFor;
    /// @brief Whether there is a step before it.
    bool canBack = false;
    /// @brief Whether it is the last step, whose Next is captioned Done.
    bool last = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(TourView const&) const = default;
};

/// @brief What a `TourController` reports.
struct TourControllerOptions {
    /// @brief Called with a sentence for the developer: a step whose target is not shown (once per step and tour run),
    ///        a tour that stopped. Empty logs at warn.
    std::function<void(std::string const&)> warn;
    /// @brief Called after the view changed, and after a tour ended; empty calls nothing.
    std::function<void()> onChanged;
};

/// @brief The tours the Tours menu offers on one screen.
struct OfferedTours {
    /// @brief The screen's own tours, in declaration order.
    std::vector<Tour> screen;
    /// @brief The general tours, in declaration order.
    std::vector<Tour> general;
};

/// @brief Splits the app's tours into those a screen offers and the general ones.
/// @param tours    The tours, as `AppRoot::tours` answers them.
/// @param screenId The active screen, a custom screen named by its base; empty for none, which offers only the
///                 general tours.
/// @return The two lists.
[[nodiscard]] MORPH_API OfferedTours offeredTours(std::vector<Tour> const& tours, std::string_view screenId);

/// @brief The test id a step rings in the frame's form: in the compact form a navigator entry and the parts that
///        moved into the navigation menu are rung as that menu, and the search as its button.
/// @param target  The step's target.
/// @param compact Whether the frame is compact.
/// @return The id to ring.
[[nodiscard]] MORPH_API std::string tourTargetFor(std::string_view target, bool compact);

/// @brief Runs at most one of the app's tours: opens each step's screen, prepares a shell target, points the
///        spotlight at the step's target, and moves on with Next, Back, a waiting step's screen, or End.
///
/// A shell makes one with each frame and destroys it with the frame, which ends a running tour. It reads the tours
/// from `AppRoot::tours()`, runtime tours included, and follows them while a tour runs, so a change of locale
/// re-evaluates the card's texts. Neither copyable nor movable; on the runtime's owner.
class MORPH_API TourController {
public:
    /// @brief A controller with no tour running.
    /// @param runtime The runtime the root's values belong to. Borrowed.
    /// @param root    The app document's root. Borrowed: it outlives the controller.
    /// @param shell   The shell the tours run in. Borrowed: it outlives the controller.
    /// @param options Where warnings and changes go.
    TourController(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, AppRoot const& root MORPH_LIFETIMEBOUND,
                   TourShell& shell MORPH_LIFETIMEBOUND, TourControllerOptions options = {});

    /// @brief Stops following the tours. The spotlight is left to the frame, which goes with the controller.
    ~TourController();

    TourController(TourController const&) = delete;
    TourController& operator=(TourController const&) = delete;
    TourController(TourController&&) = delete;
    TourController& operator=(TourController&&) = delete;

    /// @brief Ends a running tour, opens the tour's screen when it names one the active tab does not show, opens the
    ///        spotlight and shows the first step. A screen that cannot be opened ends the tour with a toast.
    /// @param tourId The tour.
    /// @return Nothing, or the refusal of an id the app offers no tour for, which starts nothing.
    std::expected<void, Refusal> start(std::string_view tourId);

    /// @brief Shows the next step, or ends the tour on the last one. A step that waits for a screen ignores it.
    void next();

    /// @brief Shows the step before. A screen an earlier step opened is not closed; that step's own `screen` opens
    ///        again if it names one.
    void back();

    /// @brief Ends the running tour: the spotlight closes, which hands focus back, and the view empties.
    void end();

    /// @brief Tells the controller the active tab changed: a step waiting for the screen now active moves on.
    void activeChanged();

    /// @brief Whether a tour runs.
    /// @return It.
    [[nodiscard]] bool running() const noexcept { return !_tourId.empty(); }

    /// @brief Whether the app offers a tour.
    /// @param tourId The tour.
    /// @return It.
    [[nodiscard]] bool declares(std::string_view tourId) const;

    /// @brief What the card shows of the running step.
    /// @return It; nothing while no tour runs.
    [[nodiscard]] std::optional<TourView> const& view() const noexcept { return _view; }

private:
    [[nodiscard]] Tour const* current() const;
    void show(std::size_t step);
    void refresh();
    void fail(std::string const& screenId, MountError const& error);
    void warn(std::string const& text) const;
    void changed() const;

    reactive::Runtime* _runtime;
    AppRoot const* _root;
    TourShell* _shell;
    TourControllerOptions _options;
    std::vector<Tour> _tours;
    std::unique_ptr<reactive::Effect> _follow;
    std::string _tourId;
    std::size_t _step = 0;
    std::uint64_t _startedRevision = 0;
    std::set<std::size_t> _warned;
    std::optional<TourView> _view;
    bool _showing = false;
    bool _waiting = false;
    // Whether the controller is alive, for a callback the shell runs after it is gone; and which start the pending
    // open belongs to.
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::uint64_t _run = 0;
};

}  // namespace morph::ui
