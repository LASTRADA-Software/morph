// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/tours.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {
namespace {

// @p pattern with its `{title}` replaced by @p title.
std::string withTitle(std::string pattern, std::string_view title) {
    if (auto const at = pattern.find("{title}"); at != std::string::npos) {
        pattern.replace(at, 7, title);
    }
    return pattern;
}

}  // namespace

OfferedTours offeredTours(std::vector<Tour> const& tours, std::string_view screenId) {
    OfferedTours offered;
    for (Tour const& tour : tours) {
        if (tour.screen.empty()) {
            offered.general.push_back(tour);
        } else if (!screenId.empty() && tour.screen == screenId) {
            offered.screen.push_back(tour);
        }
    }
    return offered;
}

std::string tourTargetFor(std::string_view target, bool compact) {
    if (!compact) {
        return std::string{target};
    }
    if (target.starts_with("nav_")) {
        return "shell_nav_menu";
    }
    if (target == "shell_search") {
        return "shell_search_open";
    }
    if (target == "shell_context" || target == "shell_help" || target == "shell_principal_menu" ||
        target == "shell_tours") {
        return "shell_nav_menu";
    }
    return std::string{target};
}

TourController::TourController(reactive::Runtime& runtime, AppRoot const& root, TourShell& shell,
                               TourControllerOptions options)
    : _runtime{&runtime}, _root{&root}, _shell{&shell}, _options{std::move(options)} {}

TourController::~TourController() = default;

bool TourController::declares(std::string_view tourId) const {
    std::vector<Tour> const& tours = _root->tours().peek();
    return std::ranges::find(tours, tourId, &Tour::id) != tours.end();
}

std::expected<void, Refusal> TourController::start(std::string_view tourId) {
    std::vector<Tour> const& tours = _root->tours().peek();
    auto const found = std::ranges::find(tours, tourId, &Tour::id);
    if (found == tours.end()) {
        std::string reason = "the app offers no tour `" + std::string{tourId} + "`";
        return std::unexpected(Refusal{.document = "app", .path = "/tours", .reason = std::move(reason)});
    }
    std::string const startedId = found->id;
    std::string const screen = found->screen;
    std::uint64_t const revision = found->revision;
    end();
    _tours = _root->tours().peek();
    _tourId = startedId;
    _startedRevision = revision;
    _step = 0;
    _warned.clear();
    _waiting = false;
    if (!screen.empty() && _shell->activeScreen() != screen) {
        _showing = true;
        auto const opened = _shell->openScreen(screen, {});
        _showing = false;
        if (!opened) {
            fail(screen, opened.error());
            return {};
        }
    }
    show(0);
    if (!running()) {
        return {};
    }
    // Opened once the card shows the first step: opening moves focus to the card's first shown button, and which one
    // that is (Next, or End tour on a waiting step) is decided by what the card shows by then. A tour that ended or
    // was replaced in between opens nothing.
    std::uint64_t const run = ++_run;
    _shell->afterCardUpdated([this, run, alive = std::weak_ptr<bool>{_alive}] {
        if (auto const live = alive.lock(); live != nullptr && running() && run == _run) {
            _shell->spotlight().setOpen(true);
        }
    });
    // The texts follow the locale and the app state while the tour runs. The steps do not: a tour that is gone, was
    // replaced (its revision changed) or lost the step on show ends, since its steps may no longer match the one
    // shown. A change of locale keeps the revision and the steps, and only re-evaluates the texts.
    _follow = std::make_unique<reactive::Effect>(*_runtime, [this] {
        std::vector<Tour> const& latest = _root->tours().get();
        _runtime->untracked([this, &latest] {
            if (!running()) {
                return;
            }
            _tours = latest;
            Tour const* const now = current();
            if (now == nullptr || now->revision != _startedRevision || _step >= now->steps.size()) {
                std::string const title = _view ? _view->title : _tourId;
                end();
                _shell->notify(withTitle(_shell->text("morph.shell.tourChanged"), title), Tone::Info);
                return;
            }
            std::optional<TourView> const before = _view;
            refresh();
            // The first run follows the view start() just showed, which has been reported already.
            if (_view != before) {
                changed();
            }
        });
    });
    return {};
}

void TourController::next() {
    Tour const* const tour = current();
    if (tour == nullptr || _waiting) {
        return;
    }
    if (_step + 1 >= tour->steps.size()) {
        end();
        return;
    }
    show(_step + 1);
}

void TourController::back() {
    if (current() == nullptr || _step == 0) {
        return;
    }
    show(_step - 1);
}

void TourController::end() {
    if (!running()) {
        return;
    }
    // `_follow` stays: it is replaced by the next `start`, does nothing while no tour runs, and `end` may be running
    // inside its own body.
    _tourId.clear();
    _step = 0;
    _waiting = false;
    _view.reset();
    SpotlightWidget& spotlight = _shell->spotlight();
    spotlight.setTarget(std::nullopt);
    spotlight.setTargetFocusable(false);
    spotlight.setOpen(false);
    changed();
}

void TourController::activeChanged() {
    Tour const* const tour = current();
    // A screen the tour itself is opening is not the person's move.
    if (_showing || tour == nullptr) {
        return;
    }
    if (!_waiting || _step >= tour->steps.size() || _shell->activeScreen() != tour->steps.at(_step).advanceOn) {
        return;
    }
    if (_step + 1 >= tour->steps.size()) {
        end();
        return;
    }
    show(_step + 1);
}

Tour const* TourController::current() const {
    if (!running()) {
        return nullptr;
    }
    auto const found = std::ranges::find(_tours, _tourId, &Tour::id);
    return found == _tours.end() ? nullptr : &*found;
}

void TourController::show(std::size_t step) {
    Tour const* const tour = current();
    if (tour == nullptr || step >= tour->steps.size()) {
        end();
        return;
    }
    TourStep const shown = tour->steps.at(step);
    if (!shown.screen.empty() && !_shell->shows(shown.screen, shown.params)) {
        _showing = true;
        auto const opened = _shell->openScreen(shown.screen, shown.params);
        _showing = false;
        if (!opened) {
            fail(shown.screen, opened.error());
            return;
        }
    }
    std::string const target = ringedTarget(shown);
    _step = step;
    // A step whose screen is already active has nothing to wait for: it is shown as an ordinary step, since no change
    // of the active screen would ever move it on.
    _waiting = !shown.advanceOn.empty() && _shell->activeScreen() != shown.advanceOn;
    SpotlightWidget& spotlight = _shell->spotlight();
    spotlight.setTargetFocusable(_waiting);
    spotlight.setTarget(target.empty() ? std::nullopt : std::optional<std::string>{target});
    if (!target.empty() && !spotlight.targetShown() && _warned.insert(step).second) {
        warn("the tour `" + _tourId + "`, step " + std::to_string(step + 1) + ": nothing shown carries the test id `" +
             target + "`, so its card is centred");
    }
    refresh();
    changed();
}

std::string TourController::ringedTarget(TourStep const& step) {
    std::string target = step.target;
    if (target.empty()) {
        return target;
    }
    bool const compact = _shell->compact();
    if (!compact && target.starts_with("nav_")) {
        _shell->revealNavEntry(target.substr(4));
    }
    return tourTargetFor(target, compact);
}

void TourController::formChanged() {
    Tour const* const tour = current();
    if (tour == nullptr || _step >= tour->steps.size()) {
        return;
    }
    std::string const target = ringedTarget(tour->steps.at(_step));
    _shell->spotlight().setTarget(target.empty() ? std::nullopt : std::optional<std::string>{target});
}

void TourController::refresh() {
    Tour const* const tour = current();
    if (tour == nullptr || _step >= tour->steps.size()) {
        _view.reset();
        return;
    }
    TourStep const& step = tour->steps.at(_step);
    _view = TourView{
        .tourId = tour->id,
        .title = tour->title,
        .stepTitle = step.title,
        .body = step.body,
        .position = static_cast<std::int64_t>(_step + 1),
        .count = static_cast<std::int64_t>(tour->steps.size()),
        .waiting = _waiting,
        .waitingFor = _waiting ? _shell->screenTitle(step.advanceOn) : std::string{},
        .canBack = _step > 0,
        .last = _step + 1 == tour->steps.size(),
    };
}

void TourController::fail(std::string const& screenId, MountError const& error) {
    std::string const title = _shell->screenTitle(screenId);
    std::string const endedId = _tourId;
    end();
    _shell->notify(withTitle(_shell->text("morph.shell.tourScreenFailed"), title), Tone::Warn);
    warn("the tour `" + endedId + "` stopped: `" + screenId + "` could not be opened: " + error.reason);
}

void TourController::warn(std::string const& text) const {
    if (_options.warn) {
        _options.warn(text);
    } else {
        morph::log::logWarn("[tours] {}", text);
    }
}

void TourController::changed() const {
    if (_options.onChanged) {
        _options.onChanged();
    }
}

}  // namespace morph::ui
