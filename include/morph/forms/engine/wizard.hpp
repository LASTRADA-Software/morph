// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/wizard.hpp
/// @brief Wizards, headless: a `w-*` document as one `FormSession` per step, sequenced by successful submissions,
///        with each step's prefill taken from what earlier steps resolved.
///
/// Specified in `docs/spec/forms/engine.md`, "Wizards".

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../../core/callback_scope.hpp"
#include "../../core/completion.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../../render/i18n.hpp"
#include "../i18n.hpp"
#include "collection.hpp"
#include "detail/text.hpp"
#include "field_model.hpp"
#include "form_session.hpp"
#include "rules.hpp"

namespace morph::forms {

/// @brief One `w-steps` entry.
struct WizardStepSpec {
    /// @brief The step's action type.
    std::string action{};
    /// @brief The step's title: `title`, else the action type.
    std::string title{};
    /// @brief `{field: "Action.member"}`: what entering the step fills in, in document order.
    std::vector<std::pair<std::string, std::string>> prefill{};
};

/// @brief The immutable model of a `w-*` wizard document.
class WizardModel {
public:
    /// @brief Reads a wizard document.
    /// @param wizardId   The wizard's registered id, the stem of its i18n keys.
    /// @param wizardJson The document, in the vocabulary `flows::wizardSchemaJson<W>()` emits.
    /// @return The model, or the error naming the malformed key.
    [[nodiscard]] static std::expected<WizardModel, SchemaError> fromSchema(std::string_view wizardId,
                                                                            std::string_view wizardJson) {
        using detail::engine::Json;
        auto const dom = detail::engine::parseJson(wizardJson);
        if (!dom || !dom->is_object()) {
            return std::unexpected(SchemaError{"", "a wizard document must be a JSON object"});
        }
        WizardModel model{};
        model._id = wizardId;
        auto const* const title = detail::engine::stringAt(*dom, "w-title");
        model._title = title != nullptr ? *title : std::string{wizardId};
        Json const* const steps = detail::engine::member(*dom, "w-steps");
        if (steps == nullptr || !steps->is_array()) {
            return std::unexpected(SchemaError{"w-steps", R"(a wizard document needs a "w-steps" array)"});
        }
        std::size_t index = 0;
        for (Json const& entry : steps->get_array()) {
            std::string const path = "w-steps[" + std::to_string(index++) + "]";
            auto const* const action = detail::engine::stringAt(entry, "action");
            if (action == nullptr) {
                return std::unexpected(SchemaError{path, R"(a step needs an "action")"});
            }
            WizardStepSpec step{};
            step.action = *action;
            auto const* const stepTitle = detail::engine::stringAt(entry, "title");
            step.title = stepTitle != nullptr ? *stepTitle : step.action;
            if (Json const* const prefill = detail::engine::member(entry, "prefill")) {
                if (!prefill->is_object()) {
                    return std::unexpected(SchemaError{path + ".prefill", R"("prefill" must be an object)"});
                }
                for (auto const& [field, source] : prefill->get_object()) {
                    auto const* const text = source.get_if<std::string>();
                    if (text == nullptr || text->find('.') == std::string::npos) {
                        return std::unexpected(
                            SchemaError{path + ".prefill." + field, R"(a prefill path is "<Action>.<member>")"});
                    }
                    step.prefill.emplace_back(field, *text);
                }
            }
            model._steps.push_back(std::move(step));
        }
        if (model._steps.empty()) {
            return std::unexpected(SchemaError{"w-steps", "a wizard needs at least one step"});
        }
        return model;
    }

    /// @brief The wizard's id.
    /// @return The id passed to `fromSchema`.
    [[nodiscard]] std::string_view id() const noexcept { return _id; }

    /// @brief `w-title`, or the id.
    /// @return The title.
    [[nodiscard]] std::string_view title() const noexcept { return _title; }

    /// @brief The steps, in order; never empty.
    /// @return The steps.
    [[nodiscard]] std::span<WizardStepSpec const> steps() const noexcept { return _steps; }

private:
    WizardModel() = default;

    std::string _id;
    std::string _title;
    std::vector<WizardStepSpec> _steps;
};

/// @brief The reactive state of one wizard: its steps' sessions, the position, and what each step resolved.
///
/// A step is **done** while its current body is the one whose submission last succeeded: editing a done step
/// makes it not done until that body succeeds. Next moves on from a done step without submitting; from a step that
/// is ready but not done it submits and moves on once that submission succeeds. Back always returns, keeping every
/// step's values. Entering a step applies its prefill from the resolved values as JSON values (programmatic, so it
/// submits nothing by itself); a step left ready and not done by its prefill submits once in automatic mode.
///
/// Non-copyable and non-movable: the steps' submitters point into it. Destroy a view of it before the session.
class WizardSession {
public:
    /// @param runtime The runtime. Borrowed: it must outlive the session.
    /// @param model   The wizard model.
    /// @param schemas Finds each step's schema.
    /// @param submit  Executes every step's action. Required.
    /// @param choices Fetches the steps' Choice options; may be empty.
    /// @param options Every step's session options.
    /// @throws std::invalid_argument when @p submit is empty, or a step's schema is unknown or does not read.
    WizardSession(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, WizardModel model, SchemaLookup const& schemas,
                  Submitter submit, ChoiceFetcher const& choices, FormSessionOptions const& options = {})
        : _rt{&runtime}, _model{std::move(model)}, _submit{std::move(submit)}, _current{runtime, std::size_t{0}} {
        if (!_submit) {
            throw std::invalid_argument{"WizardSession: a Submitter is required"};
        }
        for (std::size_t i = 0; i < _model.steps().size(); ++i) {
            WizardStepSpec const& spec = detail::engine::elementAt(_model.steps(), i);
            auto const schema = schemas ? schemas(spec.action) : std::nullopt;
            if (!schema) {
                throw std::invalid_argument{"WizardSession: no schema for step '" + spec.action + "'"};
            }
            auto stepModel = FormModel::fromSchema(spec.action, *schema);
            if (!stepModel) {
                throw std::invalid_argument{"WizardSession: the schema of step '" + spec.action + "' does not read: " +
                                            stepModel.error().path + ": " + stepModel.error().message};
            }
            _succeeded.push_back(
                std::make_unique<reactive::Signal<std::optional<std::string>>>(runtime, std::nullopt));
            _steps.push_back(std::make_unique<FormSession>(
                runtime, *std::move(stepModel),
                [this, i](std::string_view action, std::string body) { return relay(i, action, std::move(body)); },
                choices, options));
        }
    }

    ~WizardSession() = default;
    WizardSession(WizardSession const&) = delete;
    WizardSession& operator=(WizardSession const&) = delete;
    WizardSession(WizardSession&&) = delete;
    WizardSession& operator=(WizardSession&&) = delete;

    /// @brief The wizard model.
    /// @return The model.
    [[nodiscard]] WizardModel const& model() const noexcept { return _model; }

    /// @brief The current step. Tracked.
    /// @return Its index.
    [[nodiscard]] std::size_t current() const { return _current.get(); }

    /// @brief How many steps there are.
    /// @return The count; at least one.
    [[nodiscard]] std::size_t stepCount() const noexcept { return _steps.size(); }

    /// @brief A step's form session.
    /// @param index The step.
    /// @return Its session, alive as long as the wizard.
    /// @throws std::out_of_range for an index past the steps.
    [[nodiscard]] FormSession& step(std::size_t index) const { return *_steps.at(index); }

    /// @brief Whether a step is done: its current body is the one whose submission last succeeded. Tracked.
    /// @param index The step.
    /// @return `true` when done.
    /// @throws std::out_of_range for an index past the steps.
    [[nodiscard]] bool stepDone(std::size_t index) const {
        std::optional<std::string> const& succeeded = _succeeded.at(index)->get();
        return succeeded.has_value() && _steps.at(index)->body() == succeeded;
    }

    /// @brief Whether Back is possible. Tracked.
    /// @return `true` past the first step.
    [[nodiscard]] bool canBack() const { return current() > 0; }

    /// @brief Whether Next is possible. Tracked.
    /// @return `true` when the current step is not the last, and is done, or ready with no submission in flight.
    [[nodiscard]] bool canNext() const {
        std::size_t const at = current();
        if (at + 1 >= _steps.size()) {
            return false;
        }
        FormSession const& session = *_steps.at(at);
        return stepDone(at) || (session.ready() && !session.pending());
    }

    /// @brief Whether the current step is the last. Tracked.
    /// @return `true` on the last step.
    [[nodiscard]] bool onLastStep() const { return current() + 1 >= _steps.size(); }

    /// @brief Moves on: at once from a done step; from a ready step after submitting its body and that submission
    ///        succeeding. Does nothing while `canNext()` is false.
    void next() {
        if (!_rt->untracked([&] { return canNext(); })) {
            return;
        }
        std::size_t const at = _current.peek();
        if (_rt->untracked([&] { return stepDone(at); })) {
            enter(at + 1);
            return;
        }
        _advanceFrom = at;
        _steps.at(at)->submit();
    }

    /// @brief Returns to the previous step, its values as they were; nothing is submitted.
    void back() {
        _advanceFrom.reset();
        if (_current.peek() > 0) {
            _current.set(_current.peek() - 1);
        }
    }

    /// @brief A value an earlier successful submission resolved: its body's member, or its reply's, the reply
    ///        winning when both have one. Not tracked.
    /// @param path `<ActionType>.<member>`.
    /// @return The member's JSON, or `std::nullopt`.
    [[nodiscard]] std::optional<std::string> resolved(std::string_view path) const {
        auto const found = _resolved.find(path);
        return found == _resolved.end() ? std::nullopt : std::optional<std::string>{found->second};
    }

    /// @brief The wizard's title, resolved through the translations (`<id>.title`).
    /// @return The title.
    [[nodiscard]] std::string title() const {
        FormSessionOptions const& options = _steps.front()->options();
        return render::resolveText(options.translations, options.bcp47, std::nullopt,
                                   i18n::wizardTitleKey(_model.id()), _model.title());
    }

    /// @brief A step's title, resolved through the translations (`<id>.step.<index>.title`).
    /// @param index The step.
    /// @return The title.
    /// @throws std::out_of_range for an index past the steps.
    [[nodiscard]] std::string stepTitle(std::size_t index) const {
        FormSessionOptions const& options = _steps.at(index)->options();
        return render::resolveText(options.translations, options.bcp47, std::nullopt,
                                   i18n::wizardStepTitleKey(_model.id(), index),
                                   detail::engine::elementAt(_model.steps(), index).title);
    }

private:
    void enter(std::size_t index) {
        _advanceFrom.reset();
        FormSession& session = *_steps.at(index);
        _rt->batch([&] {
            _current.set(index);
            for (auto const& [field, path] : detail::engine::elementAt(_model.steps(), index).prefill) {
                if (auto const value = resolved(path)) {
                    session.assign(field, *value);
                }
            }
        });
        bool const submitNow = _rt->untracked(
            [&] { return session.submitMode() == SubmitMode::Automatic && session.ready() && !stepDone(index); });
        if (submitNow) {
            session.submit();
        }
    }

    [[nodiscard]] async::Completion<std::string> relay(std::size_t index, std::string_view action, std::string body) {
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_rt->owner());
        auto settle = std::make_shared<async::Completion<std::string>::Promise>(std::move(promise));
        std::string submitted = body;
        std::string type{action};
        _submit(action, std::move(body))
            .then(_callbacks,
                  [this, index, settle, submitted = std::move(submitted),
                   type = std::move(type)](std::string const& reply) {
                      _rt->batch([&] {
                          record(type, submitted);
                          record(type, reply);
                          _succeeded.at(index)->set(submitted);
                      });
                      settle->resolve(reply);
                      if (_advanceFrom == index && _current.peek() == index && index + 1 < _steps.size()) {
                          enter(index + 1);
                      }
                  })
            .onError(_callbacks, [this, index, settle](std::exception_ptr const& error) {
                _succeeded.at(index)->set(std::nullopt);
                if (_advanceFrom == index) {
                    _advanceFrom.reset();
                }
                settle->reject(error);
            });
        return std::move(completion);
    }

    void record(std::string const& action, std::string const& json) {
        auto const dom = detail::engine::parseJson(json);
        if (!dom || !dom->is_object()) {
            return;
        }
        for (auto const& [name, value] : dom->get_object()) {
            _resolved.insert_or_assign(action + "." + name, detail::engine::toJson(value));
        }
    }

    reactive::Runtime* _rt;
    WizardModel _model;
    Submitter _submit;
    reactive::Signal<std::size_t> _current;
    std::vector<std::unique_ptr<reactive::Signal<std::optional<std::string>>>> _succeeded;
    std::map<std::string, std::string, std::less<>> _resolved;
    std::optional<std::size_t> _advanceFrom;
    // Before the steps: a step ready as constructed submits from its constructor through `relay`, whose replies
    // this scope gates.
    async::CallbackScope _callbacks;
    std::vector<std::unique_ptr<FormSession>> _steps;
};

}  // namespace morph::forms
