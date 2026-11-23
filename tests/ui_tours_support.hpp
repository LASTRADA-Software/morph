// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/tours.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

/// @file
/// A shell a tour runs in, over a `RecordingBackend`: it opens screens by recording them and making them active, and
/// records what the tour asked of it.

namespace morph::ui::testing {

class FakeTourShell final : public TourShell {
public:
    explicit FakeTourShell(RecordingBackend& backend) : _backend{&backend} {}

    std::expected<void, MountError> openScreen(std::string const& screenId, ParamMap const& params) override {
        opened.push_back(screenId);
        if (refused.contains(screenId)) {
            return std::unexpected(MountError{.screenId = screenId, .reason = "not for this person", .pointer = {}});
        }
        active = screenId;
        activeParams = params;
        // What the workspace's change callback does in the generic client: synchronously, inside the open.
        if (onActive) {
            onActive();
        }
        return {};
    }
    [[nodiscard]] bool shows(std::string const& screenId, ParamMap const& params) const override {
        return active == screenId && activeParams == params;
    }
    [[nodiscard]] std::string activeScreen() const override { return active; }
    [[nodiscard]] std::string screenTitle(std::string const& screenId) const override {
        return "Title of " + screenId;
    }
    [[nodiscard]] bool compact() const override { return isCompact; }
    void revealNavEntry(std::string const& screenId) override { revealed.push_back(screenId); }
    SpotlightWidget& spotlight() override {
        if (_spotlight == nullptr) {
            _spotlight = _backend->createSpotlight(nullptr);
            if (onSpotlight) {
                onSpotlight(*_spotlight);
            }
        }
        return *_spotlight;
    }
    void notify(std::string const& text, Tone tone) override { notices.emplace_back(text, tone); }

    /// The spotlight's widget id; 0 before the first tour.
    [[nodiscard]] int spotlightId() const { return _spotlight == nullptr ? 0 : _backend->idOf(*_spotlight); }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): what a test sets and reads.
    std::string active;
    ParamMap activeParams;
    bool isCompact = false;
    std::set<std::string, std::less<>> refused;
    std::vector<std::string> opened;
    std::vector<std::string> revealed;
    std::vector<std::pair<std::string, Tone>> notices;
    std::function<void()> onActive;
    std::function<void(SpotlightWidget&)> onSpotlight;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    RecordingBackend* _backend;
    std::unique_ptr<SpotlightWidget> _spotlight;
};

}  // namespace morph::ui::testing
