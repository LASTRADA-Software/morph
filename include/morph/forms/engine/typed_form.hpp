// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/engine/typed_form.hpp
/// @brief `Form<A>`: a `FormSession` over `schemaJson<A>()`, with typed access at the edges.
///
/// Specified in `docs/spec/forms/engine.md`, "Typed forms".

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../../attributes.hpp"
#include "../../core/bridge.hpp"
#include "../../core/callback_scope.hpp"
#include "../../core/completion.hpp"
#include "../../core/registry.hpp"
#include "../../reactive/runtime.hpp"
#include "../../reactive/signal.hpp"
#include "../forms.hpp"
#include "field_model.hpp"
#include "form_session.hpp"

namespace morph::forms {

template <class A>
FormModel FormModel::forAction() {
    std::string_view const typeId = model::ActionTraits<A>::typeId();
    auto parsed = fromSchema(typeId, schemaJson<A>());
    if (!parsed) {
        throw std::logic_error{"FormModel::forAction: the schema of '" + std::string{typeId} +
                               "' does not read as a form (" + parsed.error().path + ": " + parsed.error().message +
                               ")"};
    }
    return *std::move(parsed);
}

/// @brief A form over action `A`: a `FormSession` built from `schemaJson<A>()`, submitting
///        through a `BridgeHandler`, with typed `set<>`, `value()` and `lastResult()`.
///
/// Non-copyable and non-movable: its session and in-flight callbacks point into it.
/// @tparam A The action type (registered with `BRIDGE_REGISTER_ACTION`).
/// @tparam M The model type the handler talks to.
/// @tparam S The handler's sharing policy.
template <class A, class M, class S = bridge::NoSharing>
class Form {
public:
    /// @brief The action's registered result type.
    using Result = typename model::ActionTraits<A>::Result;

    /// @brief The type of the member @p Member points to.
    /// @tparam Member A pointer to a data member of `A`.
    template <auto Member>
    using MemberValue = typename bridge::detail::MemberPointerTraits<decltype(Member)>::ValueType;

    /// @param runtime The runtime; its owner must be @p handler's callback executor. Borrowed.
    /// @param handler Executes the action. Borrowed: it must outlive the form.
    /// @param choices Fetches Choice options; may be empty when `A` has no Choice member.
    /// @param options Locale, translations, display zone; an `accepts` gate is kept and joined
    ///                with `ActionValidator<A>`.
    /// @throws std::logic_error from `FormModel::forAction<A>()` when `schemaJson<A>()` does not
    ///         read as a form model.
    Form(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, bridge::BridgeHandler<M, S>& handler MORPH_LIFETIMEBOUND,
         ChoiceFetcher choices, FormSessionOptions options = {})
        : _rt{&runtime},
          _handler{&handler},
          _session{runtime, FormModel::forAction<A>(),
                   [this](std::string_view, std::string body) { return execute(std::move(body)); }, std::move(choices),
                   withValidator(std::move(options))},
          _value{runtime,
                 [this]() -> std::optional<A> {
                     std::optional<std::string> const body = _session.body();
                     return body.has_value() ? decode(*body) : std::nullopt;
                 }},
          _lastResult{runtime, [this]() -> std::optional<Result> {
                          std::optional<std::string> const& reply = _session.lastReply();
                          if (!reply.has_value()) {
                              return std::nullopt;
                          }
                          try {
                              return model::ActionTraits<A>::resultFromJson(*reply);
                          } catch (...) {
                              return std::nullopt;
                          }
                      }} {}

    ~Form() = default;
    Form(Form const&) = delete;
    Form& operator=(Form const&) = delete;
    Form(Form&&) = delete;
    Form& operator=(Form&&) = delete;

    /// @brief The session: fields, readiness, body, submission.
    /// @return The session.
    [[nodiscard]] FormSession& session() noexcept { return _session; }

    /// @brief Sets one member, encoded with its field's encoder; a programmatic change, so
    ///        nothing is submitted.
    /// @tparam Member A pointer to a data member of `A`, e.g. `&Deposit::amount`.
    /// @param value The member's value.
    template <auto Member>
    void set(MemberValue<Member> value) {
        static_assert(std::is_same_v<typename bridge::detail::MemberPointerTraits<decltype(Member)>::ClassType, A>,
                      "Form<A>::set<>: the member must belong to A");
        _session.assign(detail::memberWireName<Member>(), glz::write_json(value).value_or(std::string{"null"}));
    }

    /// @brief The body decoded into `A`. Tracked.
    /// @return The action while the form is ready, else `std::nullopt`.
    [[nodiscard]] std::optional<A> value() const { return _value.get(); }

    /// @brief Whether the form can submit: the session is ready, which includes `A`'s own
    ///        `validate()`. Tracked.
    /// @return `true` when ready.
    [[nodiscard]] bool ready() const { return _session.ready(); }

    /// @brief The last successful result: the session's last reply, decoded. Tracked.
    /// @return The typed result, or `std::nullopt` before the first success.
    [[nodiscard]] std::optional<Result> const& lastResult() const { return _lastResult.get(); }

    /// @brief Whether a submission is in flight. Tracked.
    /// @return `true` while one is.
    [[nodiscard]] bool pending() const { return _session.pending(); }

    /// @brief The last failure. Tracked.
    /// @return The exception, or null.
    [[nodiscard]] std::exception_ptr error() const { return _session.lastError(); }

private:
    [[nodiscard]] static std::optional<A> decode(std::string const& body) {
        try {
            return model::ActionTraits<A>::fromJson(body);
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] static FormSessionOptions withValidator(FormSessionOptions options) {
        options.accepts = [own = std::move(options.accepts)](std::string_view body) {
            if (own && !own(body)) {
                return false;
            }
            std::optional<A> const action = decode(std::string{body});
            return action.has_value() && model::ActionValidator<A>::ready(*action);
        };
        return options;
    }

    [[nodiscard]] async::Completion<std::string> execute(std::string const& body) {
        auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_rt->owner());
        auto relay = std::make_shared<async::Completion<std::string>::Promise>(std::move(promise));
        std::optional<A> action = decode(body);
        if (!action) {
            relay->reject(std::make_exception_ptr(std::invalid_argument{"Form<A>: the body does not decode"}));
            return std::move(completion);
        }
        _handler->execute(*std::move(action))
            .then(_relay,
                  [relay](Result const& result) {
                      try {
                          std::string reply = model::ActionTraits<A>::resultToJson(result);
                          relay->resolve(std::move(reply));
                      } catch (...) {
                          relay->reject(std::current_exception());
                      }
                  })
            .onError(_relay, [relay](std::exception_ptr error) { relay->reject(std::move(error)); });
        return std::move(completion);
    }

    reactive::Runtime* _rt;
    bridge::BridgeHandler<M, S>* _handler;
    // Before the session: a session ready as constructed submits from its constructor, and the
    // handler callbacks that submission attaches are gated by this scope.
    async::CallbackScope _relay;
    FormSession _session;
    reactive::Computed<std::optional<A>> _value;
    // The session's last reply, decoded: the typed result and the JSON reply are one value, seen at once.
    reactive::Computed<std::optional<Result>> _lastResult;
};

}  // namespace morph::forms
