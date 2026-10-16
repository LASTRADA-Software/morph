// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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
#include "route.hpp"
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

    /// @brief Whether the screen can save its changes: a document screen that declares `onSave`, or a host screen
    ///        whose handle implements `save`. A close guard offers *save and close* only when this is `true`.
    /// @return It; `false` unless a derived handle says otherwise.
    [[nodiscard]] virtual bool canSave() const { return false; }

    /// @brief Saves the screen's changes (spec 6 §15) and answers whether it is then clean.
    ///
    /// A document screen runs `onSave`, waits until nothing pending is left, and answers `true` when `dirty` is then
    /// not `true`. A save while one waits joins it. A screen that cannot save answers `false` at once. @p done is not
    /// called when the screen closes before the save has finished.
    /// @param done Called with whether the screen is clean.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): the base only calls it; overrides keep it.
    virtual void save(std::function<void(bool clean)> done) {
        if (done) {
            done(false);
        }
    }
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

/// @brief Makes a host screen (spec 6 §15): the application's native screen, mounted into a shell's page with
///        @p params. It returns the screen's handle, whose `title`, `dirty`, `identity` and close protocol the shell
///        reads as it does a document screen's, or why it could not be mounted.
using HostScreenFactory = std::function<std::expected<std::unique_ptr<ScreenHandle>, MountError>(
    ParamMap const& params, ContainerWidget& page)>;

/// @brief The host screens an application registers by name: what a catalog entry's `host` names instead of a
///        document.
class ScreenHostRegistry {
public:
    /// @brief Registers a host screen, replacing one of the same name.
    /// @param name    The name a catalog entry's `host` gives.
    /// @param factory What mounts it.
    void add(std::string name, HostScreenFactory factory) {
        _factories.insert_or_assign(std::move(name), std::move(factory));
    }

    /// @brief The factory registered under @p name.
    /// @param name The name.
    /// @return It, or null when none is registered.
    [[nodiscard]] HostScreenFactory const* find(std::string_view name) const {
        auto const found = _factories.find(name);
        return found == _factories.end() ? nullptr : &found->second;
    }

private:
    std::map<std::string, HostScreenFactory, std::less<>> _factories;
};

/// @brief What the user chose for a dirty screen that is being closed (spec 6 §15).
enum class CloseChoice : std::uint8_t {
    Save,     ///< Run the screen's save, and close it only if it is then clean.
    Discard,  ///< Close it without saving.
    Cancel,   ///< Keep it open.
};

/// @brief What decides a close the document does not decide itself: a dirty screen that declares no `onClose`.
///
/// It asks the user, typically with a dialog, and calls `choose` once. It offers *save and close* only when
/// `screen.canSave()`; `Save` on a screen that cannot save keeps it open. The handle carries the choice out, so
/// `choose` may be called after the handle is gone, and then does nothing.
using CloseGuard = std::function<void(ScreenHandle& screen, std::function<void(CloseChoice choice)> choose)>;

/// @brief How a `DocumentScreenHost` finds documents and mounts them.
struct DocumentScreenHostOptions {
    /// @brief The loaded document of a screen id, or null for an unknown one.
    std::function<std::shared_ptr<Document const>(std::string_view)> documents;
    /// @brief The view backend screens mount on. Borrowed: it outlives every screen.
    IViewBackend* backend = nullptr;
    /// @brief Decides the close of a dirty screen without `onClose`; empty keeps every such screen open.
    CloseGuard guard;
    /// @brief Called when the guard's *save and close* left the screen dirty, before it is kept open, so the host
    ///        shows the failure. Optional.
    std::function<void(ScreenHandle& screen)> saveFailed;
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
          _session{&session},
          _dispatcher{&dispatcher},
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
        return mountAt(screenId, params, nullptr);
    }

    /// @brief Mounts a screen as `mount` does, with its root widget appended to @p parent instead of made a backend
    ///        root: how a shell puts a screen in its own container, such as a tab's page.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @param parent   The container; it outlives the handle.
    /// @return Its handle, or why it could not be mounted.
    [[nodiscard]] std::expected<std::unique_ptr<ScreenHandle>, MountError> mountInto(
        std::string_view screenId, ParamMap const& params, ContainerWidget& parent MORPH_LIFETIMEBOUND) {
        return mountAt(screenId, params, &parent);
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

    /// @brief Starts the requests of a screen's `prefetch` queries before it mounts (spec 5 §5), so the screen mounts
    ///        on a warm cache: what a shell does when the pointer rests on an entry for a screen that is not open.
    ///
    /// Each named query's body is evaluated over @p params alone and its entry warmed through the session's cache,
    /// under the token the mounted screen's alias will have. A keyed alias's token follows from its key, and a
    /// stateless model's is shared. A private alias gets a fresh token when the screen mounts, so nothing could read
    /// what a prefetch warmed: its queries are skipped, as are evaluations, a key or body that reads more than the
    /// params, and an entry that is already fresh or in flight.
    /// @param screenId The screen id; an unknown one does nothing.
    /// @param params   The params it would mount with.
    void prefetch(std::string_view screenId, ParamMap const& params) {
        std::shared_ptr<Document const> const document = _options.documents(screenId);
        if (!document) {
            return;
        }
        for (std::string const& name : document->prefetch) {
            auto const query = std::ranges::find(document->scope.queries, name, &QueryDecl::name);
            if (query == document->scope.queries.end() || query->evaluate) {
                continue;
            }
            auto const model = std::ranges::find(document->scope.models, query->model, &ModelDecl::name);
            if (model == document->scope.models.end()) {
                continue;
            }
            std::optional<Binding> const binding = bindingOf(*model, params);
            if (!binding) {
                continue;
            }
            detail::host::ParamsEnv const env{params};
            Value const body = query->body ? evaluate(*query->body, env) : Value{ValueRecord{}};
            if (env.unknown()) {
                continue;
            }
            std::shared_ptr<ModelChannel> const channel =
                _dispatcher->bind(model->type, binding->sharing, binding->instanceKey);
            _session->prefetch(channel, binding->token, model->type, query->action, body);
        }
    }

    /// @brief The route of a screen instance (spec 6 §20): `routeOf` over the screen's declared params.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return The route, or why it has none: an unknown screen, or a param with no route form.
    [[nodiscard]] std::expected<std::string, std::string> routeOf(std::string_view screenId,
                                                                  ParamMap const& params) const {
        std::shared_ptr<Document const> const document = _options.documents(screenId);
        if (!document) {
            return std::unexpected("unknown screen `" + std::string{screenId} + "`");
        }
        return ::morph::ui::routeOf(*document, params);
    }

    /// @brief Reads a route back against the screen it names (`parseRoute` over this host's documents).
    /// @param route The route.
    /// @return The screen and its params, or why the route is refused.
    [[nodiscard]] std::expected<Route, std::string> parseRoute(std::string_view route) const {
        return ::morph::ui::parseRoute(route, _options.documents);
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

    /// @brief Mounts a screen this host mounted again, on a changed document (spec 5 §12, development reload).
    ///
    /// The new screen is mounted with the same params in the same place, and each state of its own scope starts from
    /// the old screen's value of the state of the same name and type; any other state starts from its init. The old
    /// screen is then destroyed, which runs its `onUnmount`, and the handle shows the new one: its `title`, `dirty`
    /// and `identity` follow it, and a pending call stays tied to it. A refused mount keeps the old screen.
    /// @param handle   A handle this host mounted.
    /// @param document The changed document of the same screen, loaded.
    /// @return Nothing, or why the old screen was kept: a foreign or closed handle, a close or save in progress, or
    ///         the interpreter's refusal of the new document.
    [[nodiscard]] std::expected<void, MountError> remount(ScreenHandle& handle,
                                                          std::shared_ptr<Document const> document) {
        auto* const own = dynamic_cast<Handle*>(&handle);
        if (own == nullptr || !document) {
            return std::unexpected(MountError{.screenId = document ? document->id : std::string{},
                                              .reason = "not a document screen of this host"});
        }
        return own->remount(_interpreter, std::move(document), *_options.backend);
    }

    /// @brief The root widget of a handle this host mounted, which a host places in its own layout.
    /// @param handle A handle this host mounted.
    /// @return Its root widget, or `nullptr` for a closed or foreign handle.
    [[nodiscard]] static Widget* rootOf(ScreenHandle const& handle) {
        auto const* const own = dynamic_cast<Handle const*>(&handle);
        return own == nullptr ? nullptr : own->root();
    }

private:
    // How a prefetch reaches the instance the mounted screen's alias will bind.
    struct Binding {
        std::string token;
        bridge::BindSharing sharing = bridge::BindSharing::Private;
        std::string instanceKey;
    };

    // The binding of @p model over @p params, or nothing for an instance the mounted screen will not share.
    [[nodiscard]] std::optional<Binding> bindingOf(ModelDecl const& model, ParamMap const& params) const {
        InstanceMode mode = model.instance;
        if (mode == InstanceMode::Default) {
            Catalog const& catalog = _session->options().catalog;
            std::optional<ModelInfo> const info = catalog.model ? catalog.model(model.type) : std::nullopt;
            mode = info && info->stateless ? InstanceMode::Shared : InstanceMode::Private;
        }
        if (mode == InstanceMode::Shared) {
            return Binding{.token = "shared", .sharing = bridge::BindSharing::Private, .instanceKey = {}};
        }
        if (mode != InstanceMode::Keyed || !model.key) {
            return std::nullopt;
        }
        detail::host::ParamsEnv const env{params};
        std::string const key = detail::instanceKeyText(evaluate(*model.key, env));
        if (env.unknown() || key.empty()) {
            return std::nullopt;
        }
        return Binding{.token = "shared:" + key, .sharing = bridge::BindSharing::Shared, .instanceKey = key};
    }

    // The mount itself, under @p parent or as a backend root.
    std::expected<std::unique_ptr<ScreenHandle>, MountError> mountAt(std::string_view screenId, ParamMap const& params,
                                                                     ContainerWidget* parent) {
        std::shared_ptr<Document const> document = _options.documents(screenId);
        if (!document) {
            return std::unexpected(MountError{.screenId = std::string{screenId}, .reason = "unknown screen"});
        }
        auto screen =
            _interpreter.mount(std::move(document), params, *_options.backend, MountOptions{.parent = parent});
        if (!screen) {
            return std::unexpected(mountErrorOf(screenId, screen.error()));
        }
        auto handle = std::make_unique<Handle>(std::move(*screen), _options.guard, _options.saveFailed);
        handle->placed(params, parent);
        adoptCall(*handle, screenId, params);
        return handle;
    }

    // The interpreter's refusal of a mount, as the host reports it.
    [[nodiscard]] static MountError mountErrorOf(std::string_view screenId, Refusal const& refusal) {
        std::string reason = refusal.reason;
        if (!refusal.path.empty()) {
            reason = refusal.path + ": " + reason;
        }
        return MountError{.screenId = std::string{screenId}, .reason = std::move(reason)};
    }

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
        Handle(std::unique_ptr<Screen> screen, CloseGuard guard, std::function<void(ScreenHandle&)> saveFailed)
            : _state{std::make_shared<State>(std::move(screen), std::move(guard), std::move(saveFailed))} {
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

        [[nodiscard]] bool canSave() const override { return _state->canSave(); }

        void save(std::function<void(bool clean)> done) override { State::save(_state, std::move(done)); }

        [[nodiscard]] Widget* root() const { return _state->screen ? &_state->screen->root() : nullptr; }

        /// Records what the screen was mounted with, so a remount mounts the same instance in the same place.
        void placed(ParamMap params, ContainerWidget* parent) {
            _state->params = std::move(params);
            _state->parent = parent;
        }

        /// Mounts @p document with the screen's params, keeping its state by name and type, and replaces the
        /// screen with it; on a refusal the screen stays as it was.
        std::expected<void, MountError> remount(Interpreter& interpreter, std::shared_ptr<Document const> document,
                                                IViewBackend& backend) {
            State& state = *_state;
            std::string const screenId = document->id;
            if (!state.screen) {
                return std::unexpected(MountError{.screenId = screenId, .reason = "the screen is closed"});
            }
            if (state.closing || state.saved.effect) {
                return std::unexpected(MountError{.screenId = screenId, .reason = "the screen is closing or saving"});
            }
            auto next = interpreter.mount(std::move(document), state.params, backend,
                                          MountOptions{.parent = state.parent, .keepStateOf = state.screen.get()});
            if (!next) {
                return std::unexpected(mountErrorOf(screenId, next.error()));
            }
            // The old screen goes after the new one has mounted, so its root widget is never missing in between;
            // destroying it runs its `onUnmount`.
            std::unique_ptr<Screen> const replaced = std::exchange(state.screen, std::move(*next));
            state.runtime->batch([&state] { state.generation.set(state.generation.peek() + 1); });
            return {};
        }

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
            State(std::unique_ptr<Screen> mounted, CloseGuard closeGuard,
                  std::function<void(ScreenHandle&)> onSaveFailed)
                : screen{std::move(mounted)},
                  runtime{&screen->runtime()},
                  guard{std::move(closeGuard)},
                  saveFailed{std::move(onSaveFailed)},
                  closed{*runtime, false},
                  generation{*runtime, 0},
                  title{*runtime,
                        [this] {
                            if (!closed.get() && generation.get() >= 0) {
                                lastTitle = detail::host::titleText(screen->title());
                            }
                            return lastTitle;
                        }},
                  dirty{*runtime, [this] { return !closed.get() && generation.get() >= 0 && screen->dirty(); }},
                  identity{*runtime, [this] {
                               if (!closed.get() && generation.get() >= 0) {
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
                        state->guard(*state->self, [weak](CloseChoice choice) {
                            if (auto const held = weak.lock()) {
                                choose(held, choice);
                            }
                        });
                    } else {
                        state->decide(false);
                    }
                    return;
                }
                // Closes once onClose and everything it started has ended, unless it ran cancelClose.
                whenSettled(state, &State::settled,
                            [cancelled](State& settledState) { settledState.decide(!*cancelled); });
            }

            /// A wait for the screen to have nothing pending; the id tells a posted task whether it is still current.
            struct Watch {
                /// @brief The effect that watches `pending()`; null when nothing is watched.
                std::unique_ptr<reactive::Effect> effect;
                /// @brief Which watch this is, among every watch the screen made.
                std::uint64_t id = 0;
            };

            /// Runs @p then once the screen has nothing pending, in a task posted to the owner, so the commands that
            /// settled the work (a mutation's `onSuccess`) have run. @p slot holds the watch; resetting it, or closing
            /// the screen, cancels it.
            static void whenSettled(std::shared_ptr<State> const& state, Watch State::* slot,
                                    std::function<void(State&)> then) {
                std::weak_ptr<State> const weak = state;
                std::uint64_t const watchId = ++state->watches;
                Watch& watch = state.get()->*slot;
                watch.id = watchId;
                watch.effect =
                    std::make_unique<reactive::Effect>(*state->runtime, [weak, slot, watchId, then = std::move(then)] {
                        auto const held = weak.lock();
                        if (!held || !held->screen || held->screen->pending()) {
                            return;
                        }
                        held->runtime->owner().post([weak, slot, watchId, then] {
                            auto const again = weak.lock();
                            if (!again || !again->screen || (again.get()->*slot).id != watchId) {
                                return;
                            }
                            again.get()->*slot = Watch{};
                            then(*again);
                        });
                    });
            }

            /// Whether the screen is open and declares `onSave`.
            [[nodiscard]] bool canSave() const { return screen && screen->declares("onSave"); }

            /// Runs `onSave`, or joins the save in progress, and answers once nothing is pending.
            static void save(std::shared_ptr<State> const& state, std::function<void(bool)> done) {
                if (!state->canSave()) {
                    if (done) {
                        done(false);
                    }
                    return;
                }
                if (state->saved.effect) {
                    state->saving.push_back(std::move(done));
                    return;
                }
                state->screen->runLifecycle("onSave");
                state->saving.push_back(std::move(done));
                // Answered in the settled task itself rather than posted again, so an answer never reaches a handle
                // that closed in between; an answer that closes the screen ends the answering.
                whenSettled(state, &State::saved, [](State& savedState) {
                    bool const clean = !savedState.screen->dirty();
                    for (auto const& answer : std::exchange(savedState.saving, {})) {
                        if (!savedState.screen) {
                            return;
                        }
                        if (answer) {
                            answer(clean);
                        }
                    }
                });
            }

            /// Carries out the guard's choice for the close in progress.
            static void choose(std::shared_ptr<State> const& state, CloseChoice choice) {
                if (!state->closing) {
                    return;
                }
                if (choice != CloseChoice::Save || !state->canSave()) {
                    state->decide(choice == CloseChoice::Discard);
                    return;
                }
                std::weak_ptr<State> const weak = state;
                save(state, [weak](bool clean) {
                    auto const held = weak.lock();
                    if (!held) {
                        return;
                    }
                    if (!clean && held->saveFailed && held->self != nullptr) {
                        held->saveFailed(*held->self);
                    }
                    held->decide(clean);
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
                settled = {};
                saved = {};
                saving.clear();  // a save outlived by its screen is never answered
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
            std::function<void(ScreenHandle&)> saveFailed;
            Watch settled;
            std::vector<std::function<void(bool)>> saving;
            Watch saved;
            std::uint64_t watches = 0;
            std::string lastTitle;
            Value lastIdentity;
            // What the screen was mounted with, for a remount.
            ParamMap params;
            ContainerWidget* parent = nullptr;
            reactive::Signal<bool> closed;
            // Bumped when a remount replaces the screen, so the values below read the new one.
            reactive::Signal<int> generation;
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
    bool hostCommand(std::string const& name, Value const& args) override {
        return _options.application != nullptr && _options.application->hostCommand(name, args);
    }
    bool switchBackend(std::string const& name, std::function<void(std::expected<void, std::string>)> done) override {
        return _options.application != nullptr && _options.application->switchBackend(name, std::move(done));
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
    bool uploadFor(UploadRequest const& request, std::function<void(Value)> progress,
                   std::function<void(std::expected<Value, std::string>)> done) override {
        return _options.application != nullptr &&
               _options.application->uploadFor(request, std::move(progress), std::move(done));
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
    Session* _session;
    Dispatcher* _dispatcher;
    std::vector<std::shared_ptr<Call>> _unclaimed;
    std::vector<OpenCall> _open;
    Interpreter _interpreter;
};

}  // namespace morph::ui
