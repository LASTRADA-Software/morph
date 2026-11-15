// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/inspector.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/route.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace morph::ui {
namespace {

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

}  // namespace

bool ScreenHead::choose(std::string_view entryId) {
    static_cast<void>(entryId);
    return false;
}

bool ScreenHead::edit(std::string_view toolId, Value const& value) {
    static_cast<void>(toolId);
    static_cast<void>(value);
    return false;
}

bool ScreenHead::submit(std::string_view toolId) {
    static_cast<void>(toolId);
    return false;
}

void ScreenHandle::save(std::function<void(bool clean)> done) {
    if (done) {
        done(false);
    }
}

HostScreenFactory const* ScreenHostRegistry::find(std::string_view name) const {
    auto const found = _factories.find(name);
    return found == _factories.end() ? nullptr : &found->second;
}
namespace detail::host {

std::string titleText(Value const& value) {
    if (std::string const* const text = value.asString()) {
        return *text;
    }
    return value.isNull() ? std::string{} : ::morph::ui::detail::canonicalJson(value);
}
}  // namespace detail::host

std::optional<std::string> identityKey(Value const& identity) {
    if (identity.isNull()) {
        return std::nullopt;
    }
    return detail::canonicalJson(identity);
}

bool sameIdentity(Value const& lhs, Value const& rhs) {
    std::optional<std::string> const left = identityKey(lhs);
    return left.has_value() && left == identityKey(rhs);
}

DocumentScreenHost::DocumentScreenHost(Session& session MORPH_LIFETIMEBOUND,
                                       Dispatcher& dispatcher MORPH_LIFETIMEBOUND, DocumentScreenHostOptions options)
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

std::expected<std::unique_ptr<AppRoot>, MountError> DocumentScreenHost::mountApp(
    std::shared_ptr<Document const> document) {
    if (!document) {
        return std::unexpected(MountError{.screenId = "app", .reason = "no app document"});
    }
    std::string const documentId = document->id;
    auto root = _interpreter.mountApp(std::move(document));
    if (!root) {
        return std::unexpected(mountErrorOf(documentId, root.error()));
    }
    return std::move(*root);
}

std::optional<std::string> DocumentScreenHost::identityOf(std::string_view screenId, ParamMap const& params) {
    std::shared_ptr<Document const> const document = _options.documents(screenId);
    if (!document || !document->identity) {
        return std::nullopt;
    }
    ParamsEnv const env{params};
    Value const identity = evaluate(*document->identity, env);
    if (env.unknown()) {
        return std::nullopt;
    }
    return identityKey(identity);
}

void DocumentScreenHost::prefetch(std::string_view screenId, ParamMap const& params) {
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
        ParamsEnv const env{params};
        Value const body = query->body ? evaluate(*query->body, env) : Value{ValueRecord{}};
        if (env.unknown()) {
            continue;
        }
        std::shared_ptr<ModelChannel> const channel =
            _dispatcher->bind(model->type, binding->sharing, binding->instanceKey);
        _session->prefetch(channel, binding->token, model->type, query->action, body);
    }
}

std::expected<std::string, std::string> DocumentScreenHost::routeOf(std::string_view screenId,
                                                                    ParamMap const& params) const {
    std::shared_ptr<Document const> const document = _options.documents(screenId);
    if (!document) {
        return std::unexpected("unknown screen `" + std::string{screenId} + "`");
    }
    return ::morph::ui::routeOf(*document, params);
}

void DocumentScreenHost::onNavigate(NavigateRequest const& request) {
    ValueRecord const* const params = request.params.asRecord();
    onNavigate(request.screen, params == nullptr ? ParamMap{} : *params);
}

std::expected<void, MountError> DocumentScreenHost::remount(ScreenHandle& handle,
                                                            std::shared_ptr<Document const> document) {
    auto* const own = dynamic_cast<Handle*>(&handle);
    if (own == nullptr || !document) {
        return std::unexpected(MountError{.screenId = document ? document->id : std::string{},
                                          .reason = "not a document screen of this host"});
    }
    return own->remount(_interpreter, std::move(document), *_options.backend);
}

std::optional<chat::TurnRequest> DocumentScreenHost::chatContext(ScreenHandle& handle) {
    auto const* const own = dynamic_cast<Handle const*>(&handle);
    Screen const* const screen = own == nullptr ? nullptr : own->screen();
    if (screen == nullptr) {
        return std::nullopt;
    }
    chat::TurnRequest context;
    context.screenId = screen->id();
    context.title = handle.title().peek();
    context.identity = identityKey(handle.identity().peek()).value_or(std::string{});
    context.paramsJson = glz::write_json(valueToJson(Value{own->params()})).value_or(std::string{"{}"});
    for (Screen::BoundAlias const& alias : screen->boundAliases()) {
        context.models.push_back(chat::ModelRef{.path = alias.decl->path,
                                                .alias = alias.decl->name,
                                                .modelType = alias.decl->type,
                                                .instanceId = alias.facts.instanceId,
                                                .primary = alias.facts.primary});
    }
    return context;
}

std::optional<Inspector> DocumentScreenHost::inspector(ScreenHandle& handle) {
    auto const* const own = dynamic_cast<Handle const*>(&handle);
    Screen* const screen = own == nullptr ? nullptr : own->screen();
    if (screen == nullptr) {
        return std::nullopt;
    }
    return Inspector::over(*screen);
}

void DocumentScreenHost::cancelUnclaimedCalls() {
    for (std::shared_ptr<Call> const& call : std::exchange(_unclaimed, {})) {
        call->answer(std::nullopt);
    }
}

Widget* DocumentScreenHost::rootOf(ScreenHandle const& handle) {
    auto const* const own = dynamic_cast<Handle const*>(&handle);
    return own == nullptr ? nullptr : own->root();
}

void DocumentScreenHost::settleCall(std::string_view screenId, ParamMap const& params) {
    auto const found = std::ranges::find_if(_unclaimed, [&](std::shared_ptr<Call> const& call) {
        return call->screen == screenId && call->params == params;
    });
    if (found == _unclaimed.end()) {
        return;
    }
    std::shared_ptr<Call> const call = std::move(*found);
    _unclaimed.erase(found);
    if (_options.diagnostics) {
        _options.diagnostics(
            Refusal{.document = std::string{screenId},
                    .path = {},
                    .reason = "navigate to " + std::string{screenId} + " mounted no screen; its call was cancelled"});
    }
    call->answer(std::nullopt);
}

std::optional<DocumentScreenHost::Binding> DocumentScreenHost::bindingOf(ModelDecl const& model,
                                                                         ParamMap const& params) const {
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
    ParamsEnv const env{params};
    std::string const key = detail::instanceKeyText(evaluate(*model.key, env));
    if (env.unknown() || key.empty()) {
        return std::nullopt;
    }
    return Binding{.token = "shared:" + key, .sharing = bridge::BindSharing::Shared, .instanceKey = key};
}

std::expected<std::unique_ptr<ScreenHandle>, MountError> DocumentScreenHost::mountAt(std::string_view screenId,
                                                                                     ParamMap const& params,
                                                                                     ContainerWidget* parent) {
    std::shared_ptr<Document const> document = _options.documents(screenId);
    if (!document) {
        return std::unexpected(MountError{.screenId = std::string{screenId}, .reason = "unknown screen"});
    }
    auto screen = _interpreter.mount(std::move(document), params, *_options.backend, MountOptions{.parent = parent});
    if (!screen) {
        return std::unexpected(mountErrorOf(screenId, screen.error()));
    }
    auto handle = std::make_unique<Handle>(std::move(*screen), _options.guard, _options.saveFailed);
    handle->placed(params, parent);
    adoptCall(*handle, screenId, params);
    return handle;
}

MountError DocumentScreenHost::mountErrorOf(std::string_view screenId, Refusal const& refusal) {
    std::string reason = refusal.reason;
    if (!refusal.path.empty()) {
        reason = refusal.path + ": " + reason;
    }
    return MountError{.screenId = std::string{screenId}, .reason = std::move(reason)};
}

void DocumentScreenHost::Call::answer(std::optional<Value> result) {
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

DocumentScreenHost::Handle::~Handle() {
    _state->self = nullptr;
    try {
        _state->finish(true);
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to; the screen is gone.
    }
}

void DocumentScreenHost::Handle::placed(ParamMap params, ContainerWidget* parent) {
    _state->params = std::move(params);
    _state->parent = parent;
}

std::expected<void, MountError> DocumentScreenHost::Handle::remount(Interpreter& interpreter,
                                                                    std::shared_ptr<Document const> document,
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

void DocumentScreenHost::Handle::closeItself(std::function<void(ScreenHandle&)> closed) {
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

void DocumentScreenHost::Handle::State::request(std::shared_ptr<State> const& state, std::function<void(bool)> done) {
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
    whenSettled(state, &State::settled, [cancelled](State& settledState) { settledState.decide(!*cancelled); });
}

void DocumentScreenHost::Handle::State::whenSettled(std::shared_ptr<State> const& state, Watch State::* slot,
                                                    std::function<void(State&)> then) {
    std::weak_ptr<State> const weak = state;
    std::uint64_t const watchId = ++state->watches;
    Watch& watch = state.get()->*slot;
    watch.id = watchId;
    watch.effect = std::make_unique<reactive::Effect>(*state->runtime, [weak, slot, watchId, then = std::move(then)] {
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

void DocumentScreenHost::Handle::State::save(std::shared_ptr<State> const& state, std::function<void(bool)> done) {
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

void DocumentScreenHost::Handle::State::choose(std::shared_ptr<State> const& state, CloseChoice choice) {
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

void DocumentScreenHost::Handle::State::decide(bool close) {
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

void DocumentScreenHost::Handle::State::finish(bool answer) {
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
        lastTitleSpans = screen->titleSpans();
        lastPageEdge = screen->pageEdge();
        lastHeadJoined = screen->headJoined();
        lastLeadPlacement = screen->leadPlacement();
        lastTitleRole = screen->titleRole();
        lastHeadSurface = screen->headSurface();
        lastPageSurface = screen->pageSurface();
        lastEyebrow = detail::host::titleText(screen->eyebrow());
        lastEyebrowSpans = screen->eyebrowSpans();
        lastSubtitle = detail::host::titleText(screen->subtitle());
        lastSubtitleSpans = screen->subtitleSpans();
        runtime->batch([this] { closed.set(true); });
        screen.reset();
    }
    post(std::exchange(waiting, {}), answer);
}

void DocumentScreenHost::Handle::State::post(std::vector<std::function<void(bool)>> answers, bool answer) const {
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

bool DocumentScreenHost::navigate(NavigateRequest const& request, std::function<void(Value)> onResult,
                                  std::function<void()> onCancel) {
    if (request.screen.empty()) {
        return _options.application != nullptr &&
               _options.application->navigate(request, std::move(onResult), std::move(onCancel));
    }
    NavigateRequest forwarded = request;
    forwarded.call = onResult || onCancel;
    if (forwarded.call) {
        ValueRecord const* const params = request.params.asRecord();
        _unclaimed.push_back(std::make_shared<Call>(Call{.screen = request.screen,
                                                         .params = params == nullptr ? ParamMap{} : *params,
                                                         .onResult = std::move(onResult),
                                                         .onCancel = std::move(onCancel),
                                                         .answered = false}));
    }
    onNavigate(forwarded);
    return true;
}

bool DocumentScreenHost::closeSelf(std::string const& screen, std::optional<Value> result) {
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

void DocumentScreenHost::adoptCall(Handle& handle, std::string_view screenId, ParamMap const& params) {
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

void DocumentScreenHost::resetPrefs(std::string const& screen, std::string const& point) {
    if (_options.application != nullptr) {
        _options.application->resetPrefs(screen, point);
    }
}

void DocumentScreenHost::setLocale(std::string const& locale) {
    if (_options.application != nullptr) {
        _options.application->setLocale(locale);
    }
}

void DocumentScreenHost::signOut() {
    if (_options.application != nullptr) {
        _options.application->signOut();
    }
}

bool DocumentScreenHost::uploadFor(UploadRequest const& request, std::function<void(Value)> progress,
                                   std::function<void(std::expected<Value, std::string>)> done) {
    return _options.application != nullptr &&
           _options.application->uploadFor(request, std::move(progress), std::move(done));
}

void DocumentScreenHost::preferenceChanged(std::string const& screen, std::string const& point, Value const& value) {
    if (_options.application != nullptr) {
        _options.application->preferenceChanged(screen, point, value);
    }
}

void DocumentScreenHost::permissionsStale() {
    if (_options.application != nullptr) {
        _options.application->permissionsStale();
    }
}
}  // namespace morph::ui
