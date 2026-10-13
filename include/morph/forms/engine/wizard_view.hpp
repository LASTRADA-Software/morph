// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/wizard_view.hpp
/// @brief `wizardView`: a `WizardSession` rendered as a view tree (`ui::Node`): the title and step indicator, the
///        current step's form, and Back and Next.
///
/// Specified in `docs/spec/forms/engine.md`, "Rendering collections and wizards".

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../ui/view.hpp"
#include "form_view.hpp"
#include "wizard.hpp"

namespace morph::forms {

/// @brief Replacements for the parts of a wizard view a host draws from its own UI kit. Each one left empty keeps
///        the built-in part.
struct WizardChrome {
    /// @brief Replaces the title and the step indicator. Called once with the session, whose `title`, `current`,
    ///        `stepCount` and `stepTitle` it reads.
    std::function<ui::Node(WizardSession&)> header;
    /// @brief Replaces Back and Next. Called once with the session, whose `canBack`, `canNext`, `onLastStep`, `back`
    ///        and `next` are the same gates and moves the built-in buttons use.
    std::function<ui::Node(WizardSession&)> nav;
};

/// @brief How `wizardView` draws a wizard and labels its chrome.
struct WizardViewOptions {
    /// @brief Every step's form view options; its overrides reach every step's fields.
    FormViewOptions form{};
    /// @brief The host's replacements for the view's own parts.
    WizardChrome chrome{};
    /// @brief The Back button's caption.
    std::string backLabel = "Back";
    /// @brief The Next button's caption.
    std::string nextLabel = "Next";
    /// @brief The note shown on the last step, which has no Next.
    std::string lastStepText = "Last step — fill it in to finish";
};

namespace detail::engine {

/// @brief The built-in header: the title with the step's position (`Flow  (1 / 2)`) and a step indicator (done,
/// current,
///        pending).
/// @param session The session.
/// @return The header.
[[nodiscard]] inline ui::Node wizardHeader(WizardSession& session) {
    WizardSession* const wizard = &session;
    ui::Text title{.text =
                       [wizard] {
                           return wizard->title() + "  (" + std::to_string(wizard->current() + 1) + " / " +
                                  std::to_string(wizard->stepCount()) + ")";
                       },
                   .role = ui::TextRole::Heading};
    title.common.testId = "wizard_title";
    ui::Steps steps{
        .items =
            [wizard] {
                std::vector<ui::Step> items;
                std::size_t const at = wizard->current();
                for (std::size_t i = 0; i < wizard->stepCount(); ++i) {
                    ui::StepState state = ui::StepState::Pending;
                    if (i == at) {
                        state = ui::StepState::Active;
                    } else if (wizard->stepDone(i)) {
                        state = ui::StepState::Done;
                    }
                    items.push_back(ui::Step{.label = wizard->stepTitle(i), .state = state});
                }
                return items;
            },
        .current = [wizard] { return std::optional<std::size_t>{wizard->current()}; },
    };
    steps.common.testId = "wizard_steps";
    return ui::column(ui::Column{.children = {ui::text(std::move(title)), ui::steps(std::move(steps))}, .gap = 1});
}

/// @brief The built-in navigation: Back, Next and the last step's note.
/// @param session The session.
/// @param options The view options.
/// @return The navigation row.
[[nodiscard]] inline ui::Node wizardNav(WizardSession& session, WizardViewOptions const& options) {
    WizardSession* const wizard = &session;
    ui::Button back{.label = options.backLabel, .onClick = [wizard] { wizard->back(); }};
    back.common.enabled = [wizard] { return wizard->canBack(); };
    back.common.testId = "wizardBack";
    ui::Button next{.label = options.nextLabel, .onClick = [wizard] { wizard->next(); }};
    next.common.enabled = [wizard] { return wizard->canNext(); };
    next.common.testId = "wizardNext";
    ui::Text last{.text = options.lastStepText, .role = ui::TextRole::Muted};
    last.common.visible = [wizard] { return wizard->onLastStep(); };
    last.common.testId = "wizard_last";
    return ui::row(ui::Row{
        .children = {ui::button(std::move(back)), ui::button(std::move(next)), ui::text(std::move(last))}, .gap = 1});
}

}  // namespace detail::engine

/// @brief Renders @p session: the header (title with the step's position, a step indicator), the current step's
///        `formView` (one per step, mounted while it is current; the values live in the steps' sessions, so leaving
///        a step loses nothing), and Back and Next, enabled by `canBack` and `canNext`.
///
/// Test ids: `wizard_title`, `wizard_steps`, `wizard_step_<index>` (the current step's form), `wizardBack`,
/// `wizardNext` and `wizard_last`; the step's fields have their form ids.
///
/// The nodes bind to the session by pointer: mount the result after the session and destroy the mount before it.
/// @param session The session.
/// @param options Every step's form options, the chrome and the labels.
/// @return The view.
[[nodiscard]] inline ui::Node wizardView(WizardSession& session, WizardViewOptions const& options = {}) {
    WizardSession* const wizard = &session;
    std::vector<ui::SwitchCase> cases;
    for (std::size_t i = 0; i < session.stepCount(); ++i) {
        ui::Column step{.children = {formView(session.step(i), options.form)}};
        step.common.testId = "wizard_step_" + std::to_string(i);
        cases.push_back(
            ui::SwitchCase{.key = ui::Key{static_cast<std::int64_t>(i)}, .node = ui::column(std::move(step))});
    }
    ui::Node body = ui::switchOf(ui::Switch{
        .selector = [wizard] { return ui::Key{static_cast<std::int64_t>(wizard->current())}; },
        .cases = std::move(cases),
    });
    return ui::column(ui::Column{
        .children = {options.chrome.header ? options.chrome.header(session) : detail::engine::wizardHeader(session),
                     std::move(body),
                     options.chrome.nav ? options.chrome.nav(session) : detail::engine::wizardNav(session, options)},
        .gap = 1});
}

}  // namespace morph::forms
