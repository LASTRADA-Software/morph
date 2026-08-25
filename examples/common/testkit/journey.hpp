// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace morph::ladder::testkit {

/// @brief A named, ordered sequence of user intents, with assertions between
///        the steps.
///
/// Every other layer of the ladder's suite verifies one slice: a model test
/// proves an action's result, a presenter test proves one call wires through,
/// the stress harness proves invariants hold under concurrency. All of them
/// assume the surrounding sequence away, and authentication in particular is
/// always a *precondition* -- rigs arrive already authenticated -- rather than
/// a step that can fail and be retried.
///
/// A journey is the missing layer: it describes what a user actually does, in
/// order, so that the suite can catch what only a sequence exposes -- state
/// leaking between steps, a failed step corrupting what follows, a retry that
/// half-works, an error path that leaves the client wedged, a session that
/// outlives sign-out.
///
/// Failures report *which step* failed and the trail that led there, rather
/// than a bare assertion 200 lines into a long test body:
///
/// ```cpp
/// Journey{"sign-in"}
///     .step("acting with no session is rejected", [&] { ... })
///     .step("signing in with an invalid username is rejected", [&] { ... })
///     .step("signing in as alice establishes a session", [&] { ... })
///     .run();
/// ```
///
/// A step that throws is reported as that step's failure with the exception's
/// message, rather than escaping to Catch2 as an unhandled exception naming
/// only the test case.
class Journey {
public:
    /// @param name Human name for the whole journey, used in failure output.
    explicit Journey(std::string name) : _name{std::move(name)} {}

    /// @brief Appends a step.
    /// @param title What the user is doing, phrased as an outcome.
    /// @param body  The step, including its own assertions.
    /// @return `*this`, for chaining.
    Journey& step(std::string title, std::function<void()> body) {
        _steps.push_back(Step{std::move(title), std::move(body)});
        return *this;
    }

    /// @brief Runs every step in order, stopping at the first that throws.
    ///
    /// Each step's title is registered with Catch2 before the step runs, so a
    /// failed assertion anywhere inside it prints the journey's progress up to
    /// that point.
    void run() {
        for (std::size_t idx = 0; idx < _steps.size(); ++idx) {
            const auto& step = _steps[idx];
            UNSCOPED_INFO(_name << " -- step " << (idx + 1) << "/" << _steps.size() << ": " << step.title);
            try {
                step.body();
            } catch (const std::exception& exc) {
                FAIL(_name << ": step " << (idx + 1) << " (" << step.title << ") threw: " << exc.what());
            } catch (...) {
                FAIL(_name << ": step " << (idx + 1) << " (" << step.title << ") threw a non-std exception");
            }
        }
    }

private:
    struct Step {
        std::string title;
        std::function<void()> body;
    };

    std::string _name;
    std::vector<Step> _steps;
};

}  // namespace morph::ladder::testkit
