// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/executor.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "document.hpp"
#include "expression.hpp"
#include "interpreter.hpp"
#include "value.hpp"

/// @file
/// @brief The host contract (spec 6 §4): how a host mounts UI documents and closes them, whether it owns its own
///        shell or is the framework's workspace shell, and `DocumentScreenHost`, the framework's host over documents.
///
/// Specified in `docs/spec/ui/host.md`.

namespace morph::ui {

/// @brief A screen's params, by name.
using ParamMap = ValueRecord;

/// @brief Why a screen could not be mounted.
struct MountError {
    /// @brief The screen id asked for.
    std::string screenId;
    /// @brief Why: an unknown screen, a refused param, a refused mount.
    std::string reason;
};

/// @brief One mounted screen, as its host sees it.
///
/// The host reads `title`, `dirty` and `identity` to show the tab and the unsaved indicator, and closes the screen
/// through `requestClose`. It never destroys a handle whose close is pending, except after `forceClose`.
class ScreenHandle {
public:
    ScreenHandle() = default;
    ScreenHandle(ScreenHandle const&) = delete;
    ScreenHandle& operator=(ScreenHandle const&) = delete;
    ScreenHandle(ScreenHandle&&) = delete;
    ScreenHandle& operator=(ScreenHandle&&) = delete;
    virtual ~ScreenHandle() = default;

    /// @brief The screen's title: the document's `title`, as text.
    /// @return It; it keeps its last value once the screen is closed.
    [[nodiscard]] virtual reactive::Computed<std::string> const& title() const = 0;

    /// @brief Whether the screen has unsaved changes: the document's `dirty`.
    /// @return It; `false` once the screen is closed.
    [[nodiscard]] virtual reactive::Computed<bool> const& dirty() const = 0;

    /// @brief The screen's identity: the document's `identity`, `null` when it declares none. A host re-keys a tab
    ///        when it changes.
    /// @return It; it keeps its last value once the screen is closed.
    [[nodiscard]] virtual reactive::Computed<Value> const& identity() const = 0;

    /// @brief Asks the screen to close (spec 6 §3).
    ///
    /// @p done is called exactly once, posted to the owner, never from inside the document's own handler: with `true`
    /// when the screen closed, `false` when it stays open. A request while one is pending joins it, and is answered
    /// with the same outcome.
    /// @param done Called with the outcome.
    virtual void requestClose(std::function<void(bool closed)> done) = 0;

    /// @brief Closes the screen without asking: skips `onClose` and the close guard, runs `onUnmount`, and answers
    ///        a pending `requestClose` with `true`. For application quit.
    virtual void forceClose() = 0;
};

/// @brief What mounts screens for a host that owns its own shell; the workspace shell is one.
class ScreenHost {
public:
    ScreenHost() = default;
    ScreenHost(ScreenHost const&) = delete;
    ScreenHost& operator=(ScreenHost const&) = delete;
    ScreenHost(ScreenHost&&) = delete;
    ScreenHost& operator=(ScreenHost&&) = delete;
    virtual ~ScreenHost() = default;

    /// @brief Mounts a screen.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return Its handle, or why it could not be mounted.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ScreenHandle>, MountError> mount(std::string_view screenId,
                                                                                         ParamMap params) = 0;

    /// @brief The identity a screen would have with @p params, without mounting it, so a host can focus an open
    ///        screen instead of opening a second.
    /// @param screenId The screen id.
    /// @param params   The params.
    /// @return Its canonical text, or `std::nullopt` for a screen that declares none or whose identity reads more
    ///         than its params.
    [[nodiscard]] virtual std::optional<std::string> identityOf(std::string_view screenId, ParamMap const& params) = 0;

    /// @brief A document asked to open another screen. The host decides whether that is a new tab, a focus or a
    ///        dialog. Implemented by the host.
    /// @param screenId The screen id.
    /// @param params   Its params.
    virtual void onNavigate(std::string_view screenId, ParamMap const& params) = 0;
};

/// @brief What decides a close the document does not decide itself: a dirty screen that declares no `onClose`. It
///        calls `decide(true)` to close the screen, `decide(false)` to keep it open, once.
using CloseGuard = std::function<void(ScreenHandle& screen, std::function<void(bool close)> decide)>;

/// @brief How a `DocumentScreenHost` finds documents and mounts them.
struct DocumentScreenHostOptions {
    /// @brief The loaded document of a screen id, or null for an unknown one.
    std::function<std::shared_ptr<Document const>(std::string_view)> documents;
    /// @brief The view backend screens mount on. Borrowed: it outlives every screen.
    IViewBackend* backend = nullptr;
    /// @brief Decides the close of a dirty screen without `onClose`; empty keeps every such screen open.
    CloseGuard guard;
    /// @brief The application's side of the commands the host does not answer itself; `nullptr` answers them "not
    ///        supported". Borrowed.
    InterpreterHost* application = nullptr;
    /// @brief Called with each diagnostic the screens report.
    std::function<void(Refusal const&)> diagnostics;
};

namespace detail::host {

/// The environment `identityOf` evaluates in: the params, and nothing else. Reading anything else marks it unknown.
class ParamsEnv final : public Environment {
public:
    /// @brief An environment over @p params.
    /// @param params The params; borrowed for the environment's lifetime.
    explicit ParamsEnv(ParamMap const& params) : _params{&params} {}

    [[nodiscard]] Value ref(std::string_view head, std::span<std::string const> path) const override {
        auto const found = _params->find(head);
        if (found == _params->end()) {
            _unknown = true;
            return Value{};
        }
        Value value = found->second;
        for (std::string const& step : path) {
            ValueRecord const* const record = value.asRecord();
            if (record == nullptr || !record->contains(step)) {
                return Value{};
            }
            value = record->at(step);
        }
        return value;
    }

    [[nodiscard]] Value row(std::span<std::string const> path) const override { return unknown(path); }

    [[nodiscard]] Value control(ExprKind form, std::string_view name) const override {
        static_cast<void>(form);
        static_cast<void>(name);
        _unknown = true;
        return Value{};
    }

    [[nodiscard]] Value pref(std::string_view point, std::span<std::string const> path) const override {
        static_cast<void>(point);
        return unknown(path);
    }

    [[nodiscard]] Value client(ExprKind form, std::string_view name) const override {
        static_cast<void>(form);
        static_cast<void>(name);
        _unknown = true;
        return Value{};
    }

    [[nodiscard]] FormatContext const& formatting() const override { return _formatting; }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): Environment's signature.
    void report(std::string_view path, std::string_view reason) const override {
        static_cast<void>(path);
        static_cast<void>(reason);
        _unknown = true;
    }

    /// @brief Whether the evaluation read anything but the params, or failed.
    /// @return It.
    [[nodiscard]] bool unknown() const noexcept { return _unknown; }

private:
    [[nodiscard]] Value unknown(std::span<std::string const> path) const {
        static_cast<void>(path);
        _unknown = true;
        return Value{};
    }

    ParamMap const* _params;
    FormatContext _formatting;
    mutable bool _unknown = false;
};

/// @brief The text a title value shows.
/// @param value The title's value.
/// @return A string as is, `null` as empty, anything else as its canonical JSON.
[[nodiscard]] inline std::string titleText(Value const& value) {
    if (std::string const* const text = value.asString()) {
        return *text;
    }
    return value.isNull() ? std::string{} : ::morph::ui::detail::canonicalJson(value);
}

}  // namespace detail::host

/// @brief The framework's `ScreenHost` over UI documents: it mounts them through an `Interpreter`, closes them by the
///        protocol of spec 6 §3, and hands a document's `navigate` to `onNavigate`, which the deriving host
///        implements.
///
/// It is the interpreter's `InterpreterHost` for `navigate`; every other command goes to the application's host, if
/// one was given. Everything runs on the runtime's owner.
class DocumentScreenHost : public ScreenHost, private InterpreterHost {
public:
    /// @brief A host over @p session and @p dispatcher.
    /// @param session    The session the screens share. Borrowed: it outlives the host.
    /// @param dispatcher Where model calls go. Borrowed: it outlives the host.
    /// @param options    The documents, the backend, the close guard and the application's host.
    /// @throws std::invalid_argument without a document lookup or a backend.
    DocumentScreenHost(Session& session MORPH_LIFETIMEBOUND, Dispatcher& dispatcher MORPH_LIFETIMEBOUND,
                       DocumentScreenHostOptions options)
        : _options{std::move(options)},
          _interpreter{
              session, dispatcher,
              InterpreterOptions{.host = static_cast<InterpreterHost*>(this), .diagnostics = _options.diagnostics}} {
        if (!_options.documents || _options.backend == nullptr) {
            throw std::invalid_argument("DocumentScreenHost: a document lookup and a backend are required");
        }
    }

    /// @brief Mounts a document screen.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return Its handle, or why it could not be mounted: an unknown screen, or the interpreter's refusal.
    [[nodiscard]] std::expected<std::unique_ptr<ScreenHandle>, MountError> mount(std::string_view screenId,
                                                                                 ParamMap params) override {
        std::shared_ptr<Document const> document = _options.documents(screenId);
        if (!document) {
            return std::unexpected(MountError{.screenId = std::string{screenId}, .reason = "unknown screen"});
        }
        auto screen = _interpreter.mount(std::move(document), params, *_options.backend);
        if (!screen) {
            std::string reason = screen.error().reason;
            if (!screen.error().path.empty()) {
                reason = screen.error().path + ": " + reason;
            }
            return std::unexpected(MountError{.screenId = std::string{screenId}, .reason = std::move(reason)});
        }
        auto handle = std::make_unique<Handle>(std::move(*screen), _options.guard);
        adoptCall(*handle, screenId, params);
        return handle;
    }

    /// @brief The identity a document screen would have with @p params: its `identity` evaluated over the params.
    /// @param screenId The screen id.
    /// @param params   The params.
    /// @return Its canonical text, or `std::nullopt` for an unknown screen, one without `identity`, or one whose
    ///         identity reads more than its params or is `null`.
    [[nodiscard]] std::optional<std::string> identityOf(std::string_view screenId, ParamMap const& params) override {
        std::shared_ptr<Document const> const document = _options.documents(screenId);
        if (!document || !document->identity) {
            return std::nullopt;
        }
        detail::host::ParamsEnv const env{params};
        Value const identity = evaluate(*document->identity, env);
        if (env.unknown() || identity.isNull()) {
            return std::nullopt;
        }
        return detail::canonicalJson(identity);
    }

    /// @brief A document asked to open another screen, with how (`as`). The default hands the screen and its params
    ///        to `onNavigate(screenId, params)`; a host that shows dialogs, drawers or windows overrides this one.
    /// @param request The request: screen, params, `as`.
    virtual void onNavigate(NavigateRequest const& request) {
        ValueRecord const* const params = request.params.asRecord();
        onNavigate(request.screen, params == nullptr ? ParamMap{} : *params);
    }

    using ScreenHost::onNavigate;

    /// @brief A screen this host mounted closed itself, by `return` or `close: self` (spec 6 §16). Called after the
    ///        screen is closed, posted to the owner; the host drops its tab or dialog. The default does nothing.
    /// @param screen The handle, closed.
    virtual void onScreenClosed(ScreenHandle& screen) { static_cast<void>(screen); }

    /// @brief The root widget of a handle this host mounted, which a host places in its own layout.
    /// @param handle A handle this host mounted.
    /// @return Its root widget, or `nullptr` for a closed or foreign handle.
    [[nodiscard]] static Widget* rootOf(ScreenHandle const& handle) {
        auto const* const own = dynamic_cast<Handle const*>(&handle);
        return own == nullptr ? nullptr : own->root();
    }

private:
    /// A screen opened to answer its opener (spec 6 §16): answered exactly once, by `return`, by `close: self`, or by
    /// the screen closing any other way, which is a cancel.
    struct Call {
        std::string screen;
        ParamMap params;
        std::function<void(Value)> onResult;
        std::function<void()> onCancel;
        bool answered = false;

        void answer(std::optional<Value> result) {
            if (answered) {
                return;
            }
            answered = true;
            if (result) {
                if (onResult) {
                    onResult(std::move(*result));
                }
            } else if (onCancel) {
                onCancel();
            }
        }
    };

    /// The handle of a document screen. Its state is shared with the posts it makes, so a post that runs after the
    /// handle is gone finds the screen already closed.
    class Handle final : public ScreenHandle {
    public:
        Handle(std::unique_ptr<Screen> screen, CloseGuard guard)
            : _state{std::make_shared<State>(std::move(screen), std::move(guard))} {
            _state->self = this;
        }

        ~Handle() override {
            _state->self = nullptr;
            try {
                _state->finish(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to; the screen is gone.
            }
        }

        Handle(Handle const&) = delete;
        Handle& operator=(Handle const&) = delete;
        Handle(Handle&&) = delete;
        Handle& operator=(Handle&&) = delete;

        [[nodiscard]] reactive::Computed<std::string> const& title() const override { return _state->title; }
        [[nodiscard]] reactive::Computed<bool> const& dirty() const override { return _state->dirty; }
        [[nodiscard]] reactive::Computed<Value> const& identity() const override { return _state->identity; }

        void requestClose(std::function<void(bool closed)> done) override { State::request(_state, std::move(done)); }

        void forceClose() override { _state->finish(true); }

        [[nodiscard]] Widget* root() const { return _state->screen ? &_state->screen->root() : nullptr; }

        /// The call this screen answers, if it was opened as one.
        void answers(std::shared_ptr<Call> call) { _state->call = std::move(call); }

        /// Closes the screen after its own `return` or `close: self` has answered: posted, since the command that
        /// asked for it is still running in the screen.
        void closeItself(std::function<void(ScreenHandle&)> closed) {
            std::weak_ptr<State> const weak = _state;
            _state->runtime->owner().post([weak, closed = std::move(closed)] {
                auto const held = weak.lock();
                if (!held || !held->screen) {
                    return;
                }
                held->finish(true);
                if (held->self != nullptr && closed) {
                    closed(*held->self);
                }
            });
        }

        /// Whether this screen answers @p call and is still open.
        [[nodiscard]] bool answering(std::shared_ptr<Call> const& call) const {
            return _state->screen && _state->call == call;
        }

    private:
        struct State {
            State(std::unique_ptr<Screen> mounted, CloseGuard closeGuard)
                : screen{std::move(mounted)},
                  runtime{&screen->runtime()},
                  guard{std::move(closeGuard)},
                  closed{*runtime, false},
                  title{*runtime,
                        [this] {
                            if (!closed.get()) {
                                lastTitle = detail::host::titleText(screen->title());
                            }
                            return lastTitle;
                        }},
                  dirty{*runtime, [this] { return !closed.get() && screen->dirty(); }},
                  identity{*runtime, [this] {
                               if (!closed.get()) {
                                   lastIdentity = screen->identity();
                               }
                               return lastIdentity;
                           }} {}

            State(State const&) = delete;
            State& operator=(State const&) = delete;
            State(State&&) = delete;
            State& operator=(State&&) = delete;
            ~State() = default;

            /// Starts a close, or joins the one in progress.
            static void request(std::shared_ptr<State> const& state, std::function<void(bool)> done) {
                if (!state->screen) {
                    state->post({std::move(done)}, true);
                    return;
                }
                state->waiting.push_back(std::move(done));
                if (state->closing) {
                    return;
                }
                state->closing = true;
                std::shared_ptr<bool const> const cancelled = state->screen->beginClose();
                if (cancelled == nullptr) {
                    bool const isDirty = state->screen->dirty();
                    if (!isDirty) {
                        state->finish(true);
                    } else if (state->guard && state->self != nullptr) {
                        std::weak_ptr<State> const weak = state;
                        state->guard(*state->self, [weak](bool close) {
                            if (auto const held = weak.lock()) {
                                held->decide(close);
                            }
                        });
                    } else {
                        state->decide(false);
                    }
                    return;
                }
                // Closes once onClose and everything it started has ended, unless it ran cancelClose.
                std::weak_ptr<State> const weak = state;
                state->settled = std::make_unique<reactive::Effect>(*state->runtime, [weak, cancelled] {
                    auto const held = weak.lock();
                    if (!held || !held->screen || held->screen->pending()) {
                        return;
                    }
                    held->runtime->owner().post([weak, cancelled] {
                        if (auto const again = weak.lock()) {
                            again->settled.reset();
                            again->decide(!*cancelled);
                        }
                    });
                });
            }

            /// Ends the close in progress: closes the screen or keeps it open, and answers every request.
            void decide(bool close) {
                if (!closing) {
                    return;
                }
                if (close) {
                    finish(true);
                    return;
                }
                closing = false;
                post(std::exchange(waiting, {}), false);
            }

            /// Closes the screen now, answering every pending request with `true`.
            void finish(bool answer) {
                settled.reset();
                closing = false;
                if (call) {
                    call->answer(std::nullopt);  // a no-op once `return` or `close: self` has answered
                }
                if (screen) {
                    // The last values a host reads once the screen is gone, whether or not it read them before.
                    lastTitle = detail::host::titleText(screen->title());
                    lastIdentity = screen->identity();
                    runtime->batch([this] { closed.set(true); });
                    screen.reset();
                }
                post(std::exchange(waiting, {}), answer);
            }

            /// Posts each answer to the owner, so none runs inside the document's own handler.
            void post(std::vector<std::function<void(bool)>> answers, bool answer) const {
                if (answers.empty()) {
                    return;
                }
                runtime->owner().post([answers = std::move(answers), answer] {
                    for (auto const& done : answers) {
                        if (done) {
                            done(answer);
                        }
                    }
                });
            }

            std::unique_ptr<Screen> screen;
            reactive::Runtime* runtime;
            CloseGuard guard;
            Handle* self = nullptr;
            std::shared_ptr<Call> call;
            bool closing = false;
            std::vector<std::function<void(bool)>> waiting;
            std::unique_ptr<reactive::Effect> settled;
            std::string lastTitle;
            Value lastIdentity;
            reactive::Signal<bool> closed;
            reactive::Computed<std::string> title;
            reactive::Computed<bool> dirty;
            reactive::Computed<Value> identity;
        };

        std::shared_ptr<State> _state;
    };

    // InterpreterHost: navigation is the host's; everything else is the application's.

    bool navigate(NavigateRequest const& request, std::function<void(Value)> onResult,
                  std::function<void()> onCancel) override {
        if (request.screen.empty()) {
            return _options.application != nullptr &&
                   _options.application->navigate(request, std::move(onResult), std::move(onCancel));
        }
        if (onResult || onCancel) {
            ValueRecord const* const params = request.params.asRecord();
            _unclaimed.push_back(std::make_shared<Call>(Call{.screen = request.screen,
                                                             .params = params == nullptr ? ParamMap{} : *params,
                                                             .onResult = std::move(onResult),
                                                             .onCancel = std::move(onCancel),
                                                             .answered = false}));
        }
        onNavigate(request);
        return true;
    }

    bool closeSelf(std::string const& screen, std::optional<Value> result) override {
        // The innermost open call of that screen answers; a screen opened without one is the application's.
        for (OpenCall const& open : std::views::reverse(_open)) {
            std::shared_ptr<Call> const call = open.call.lock();
            if (call == nullptr || call->screen != screen || call->answered || open.handle == nullptr ||
                !open.handle->answering(call)) {
                continue;
            }
            call->answer(std::move(result));
            open.handle->closeItself([this](ScreenHandle& closed) { onScreenClosed(closed); });
            return true;
        }
        return _options.application != nullptr && _options.application->closeSelf(screen, std::move(result));
    }

    /// Ties a mounted screen to the call that opened it: the oldest unclaimed call of that screen and params.
    void adoptCall(Handle& handle, std::string_view screenId, ParamMap const& params) {
        std::erase_if(_open, [](OpenCall const& open) { return open.call.expired(); });
        auto const found = std::ranges::find_if(_unclaimed, [&](std::shared_ptr<Call> const& call) {
            return call->screen == screenId && call->params == params;
        });
        if (found == _unclaimed.end()) {
            return;
        }
        std::shared_ptr<Call> const call = std::move(*found);
        _unclaimed.erase(found);
        handle.answers(call);
        _open.push_back(OpenCall{.call = call, .handle = &handle});
    }
    bool focus(std::string const& screen, std::string const& element) override {
        return _options.application != nullptr && _options.application->focus(screen, element);
    }
    bool hostCommand(std::string const& name, Value const& args) override {
        return _options.application != nullptr && _options.application->hostCommand(name, args);
    }
    bool switchBackend(std::string const& name) override {
        return _options.application != nullptr && _options.application->switchBackend(name);
    }
    void resetPrefs(std::string const& screen, std::string const& point) override {
        if (_options.application != nullptr) {
            _options.application->resetPrefs(screen, point);
        }
    }
    void setLocale(std::string const& locale) override {
        if (_options.application != nullptr) {
            _options.application->setLocale(locale);
        }
    }
    bool notify(std::string const& text, std::string const& role) override {
        return _options.application != nullptr && _options.application->notify(text, role);
    }
    bool signIn(Value const& reply) override {
        return _options.application != nullptr && _options.application->signIn(reply);
    }
    void signOut() override {
        if (_options.application != nullptr) {
            _options.application->signOut();
        }
    }
    bool setAppValue(std::string const& name, Value const& value) override {
        return _options.application != nullptr && _options.application->setAppValue(name, value);
    }
    bool upload(Value const& file, std::function<void(Value)> progress,
                std::function<void(std::expected<Value, std::string>)> done) override {
        return _options.application != nullptr &&
               _options.application->upload(file, std::move(progress), std::move(done));
    }
    bool download(Value const& file, std::string const& option, Value const& name,
                  std::function<void(std::expected<Value, std::string>)> done) override {
        return _options.application != nullptr && _options.application->download(file, option, name, std::move(done));
    }
    void preferenceChanged(std::string const& screen, std::string const& point, Value const& value) override {
        if (_options.application != nullptr) {
            _options.application->preferenceChanged(screen, point, value);
        }
    }
    void permissionsStale() override {
        if (_options.application != nullptr) {
            _options.application->permissionsStale();
        }
    }

    /// A call a mounted screen answers, newest last.
    struct OpenCall {
        std::weak_ptr<Call> call;
        Handle* handle = nullptr;
    };

    DocumentScreenHostOptions _options;
    std::vector<std::shared_ptr<Call>> _unclaimed;
    std::vector<OpenCall> _open;
    Interpreter _interpreter;
};

}  // namespace morph::ui
