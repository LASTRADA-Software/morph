// SPDX-License-Identifier: Apache-2.0

// Mounting a document: the interpreter builds a screen or an app from it.

#include "interpreter_table_detail.hpp"

namespace morph::ui {

Screen::Screen() = default;

Screen::~Screen() {
    try {
        if (Commands const* const commands = lifecycleList("onUnmount")) {
            runCommands(*commands);
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
    }
    _core->focusElement = {};
    _core->clipboard = {};
    _core->pasteboard = {};
    _mounted.reset();
    _headActions.clear();
    _headStatus.clear();
    _headMenu.clear();
    _headTools.clear();
    _scope.reset();
}

Widget& Screen::root() const {
    if (!_mounted) {
        throw std::logic_error{"the screen is not mounted"};
    }
    return _mounted->root();
}

std::vector<TextSpan> Screen::headSpans(ExprPtr const& expr) const {
    if (expr == nullptr) {
        return {};
    }
    bool dropped = false;
    bool badTone = false;
    std::vector<TextSpan> spans =
        detail::spansOf(detail::evalIn(*_scope, nullptr, expr), _scope->formatting(), dropped, &badTone);
    if (dropped) {
        _core->report(expr->path, detail::kSpansReason);
    }
    if (badTone) {
        _core->report(expr->path, detail::kSpanToneReason);
    }
    return spans;
}

DocNode const* Screen::shownRoot() const {
    DocNode const* node = _core->doc().view.get();
    while (node != nullptr && node->kind == "switch") {
        Value const value = detail::evalIn(*_scope, nullptr, node->prop("on"));
        std::span<DocNodePtr const> shown = node->nodes("default");
        for (DocNodePtr const& part : node->partList("cases")) {
            if (!(detail::evalIn(*_scope, nullptr, part->prop("when")) == value)) {
                continue;
            }
            if (part->scope.has_value() && detail::declaresNames(*part->scope)) {
                return nullptr;
            }
            shown = part->nodes("children");
            break;
        }
        node = shown.size() == 1 ? shown.front().get() : nullptr;
    }
    return node;
}

bool Screen::pageEdge() const {
    DocNode const* const node = shownRoot();
    return node != nullptr && node->flag("pageEdge");
}

bool Screen::headJoined() const {
    DocNode const* const node = shownRoot();
    return node != nullptr && node->flag("headJoined");
}

LeadPlacement Screen::leadPlacement() const {
    ExprPtr const& placement = _core->doc().leadPlacement;
    if (placement == nullptr) {
        return LeadPlacement::Below;
    }
    Value const value = detail::evalIn(*_scope, nullptr, placement);
    if (std::string const* const name = value.asString()) {
        if (*name == "inline") {
            return LeadPlacement::Inline;
        }
        if (*name == "end") {
            return LeadPlacement::End;
        }
        if (*name == "below") {
            return LeadPlacement::Below;
        }
    }
    if (!value.isNull()) {
        _core->report(placement->path, "`leadPlacement` is `below`, `inline` or `end`");
    }
    return LeadPlacement::Below;
}

TextRole Screen::titleRole() const {
    ExprPtr const& role = _core->doc().titleRole;
    if (role == nullptr) {
        return TextRole::Heading;
    }
    Value const value = detail::evalIn(*_scope, nullptr, role);
    if (std::string const* const name = value.asString()) {
        if (*name == "headline") {
            return TextRole::Headline;
        }
        if (*name == "heading") {
            return TextRole::Heading;
        }
    }
    if (!value.isNull()) {
        _core->report(role->path, "`titleRole` is `heading` or `headline`");
    }
    return TextRole::Heading;
}

std::string Screen::headSurface() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().headSurface);
    std::string const* const name = value.asString();
    return name == nullptr ? std::string{} : *name;
}

std::string Screen::pageSurface() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().pageSurface);
    std::string const* const name = value.asString();
    return name == nullptr ? std::string{} : *name;
}

std::vector<TextSpan> Screen::subtitleSpans() const { return headSpans(_core->doc().subtitleSpans); }

std::vector<TextSpan> Screen::titleSpans() const { return headSpans(_core->doc().titleSpans); }

std::vector<TextSpan> Screen::eyebrowSpans() const { return headSpans(_core->doc().eyebrowSpans); }

BorrowedModel Screen::lend(std::string_view alias) const {
    detail::NameRef const* const found = _scope->find(alias);
    if (found == nullptr || found->category != detail::NameCategory::Model) {
        return {};
    }
    detail::ModelAlias& model = *found->model;
    return BorrowedModel{
        .channel = detail::channelOf(model, *_core),
        .token = _core->runtime().untracked([&] { return model.token->get(); }),
    };
}

std::expected<std::unique_ptr<Screen>, Refusal> Interpreter::mount(std::shared_ptr<Document const> doc,
                                                                   ValueRecord const& params, IViewBackend& backend,
                                                                   MountOptions const& options) {
    auto const refuse = [&](std::string path, std::string reason) {
        return std::unexpected(Refusal{.document = doc->id, .path = std::move(path), .reason = std::move(reason)});
    };
    if (!doc->view) {
        return refuse("", "the document has no view");
    }
    ValueRecord given;
    for (auto const& [name, type] : doc->params) {
        auto const found = params.find(name);
        Value value = found == params.end() ? Value{} : found->second;
        if (!type.admits(value)) {
            return refuse(pointerAppend("/params", name), value.isNull()
                                                              ? "param `" + name + "` is missing"
                                                              : "param `" + name + "` is not a " + type.name());
        }
        given.insert_or_assign(name, std::move(value));
    }
    for (ModelDecl const& model : doc->scope.models) {
        if (model.instance == InstanceMode::Borrowed && !options.borrowed.contains(model.name)) {
            return refuse(model.path, "alias `" + model.name + "` borrows a model its opener did not lend");
        }
    }
    reactive::Runtime& runtime = _session->runtime();
    std::unique_ptr<Screen> screen{new Screen{}};
    screen->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, options);
    detail::ScreenCore& core = *screen->_core;
    if (options.keepStateOf != nullptr) {
        core.keptStates = options.keepStateOf->ownState();
    }
    screen->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, options.depth);
    core.rootScope = screen->_scope;
    detail::RtScope& scope = *screen->_scope;
    scope.submitted = &core.submitted;
    scope.showErrors = doc->showErrors;
    for (auto const& [name, value] : given) {
        detail::NameRef ref;
        ref.category = detail::NameCategory::Param;
        ref.param = value;
        scope.names.insert_or_assign(name, std::move(ref));
    }
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    for (auto const& [chord, commands] : doc->keys) {
        static_cast<void>(chord);
        lists.push_back(&commands);
    }
    detail::headCommandLists(*doc, lists);
    Node root = runtime.untracked([&] {
        core.declare(scope, &doc->scope, {doc->view.get()}, lists);
        detail::LowerCtx const ctx{
            .scope = screen->_scope,
            .bindings = std::make_shared<detail::Bindings const>(),
            .boundary = nullptr,
        };
        Node lowered = core.lower(*doc->view, ctx);
        screen->_headStatus = core.lowerBadgeParts(doc->status, ctx);
        screen->_headActions = core.lowerActionParts(doc->actions, ctx);
        screen->_headMenu = core.lowerHeadMenu(doc->menu, ctx);
        screen->_headTools = core.lowerHeadTools(doc->tools, ctx);
        if (!doc->keys.empty()) {
            std::vector<KeyBinding> keys;
            for (auto const& [chord, commands] : doc->keys) {
                keys.push_back(KeyBinding{
                    .chord = chord,
                    .onPress = [&core, ctx, list = &commands] { core.runEvent(list, ctx, Value{}); },
                });
            }
            Column column{.children = {std::move(lowered)}};
            column.common.keys = std::move(keys);
            lowered = ui::column(std::move(column));
        }
        return lowered;
    });
    screen->_mounted = std::make_unique<Mounted>(runtime, backend, std::move(root), options.parent, options.depth + 1);
    screen->_core->focusElement = [mounted = screen->_mounted.get()](std::string_view element) {
        return mounted->focus(element);
    };
    screen->_core->clipboard = [&backend](std::string_view text) { return backend.copyText(text); };
    screen->_core->pasteboard = [&backend] { return backend.pasteText(); };
    screen->runLifecycle("onMount");
    return screen;
}

namespace {

// The texts of a tour evaluated in the app scope.
Tour lowerTour(detail::RtScope& scope, TourDecl const& declared) {
    auto const textOf = [&scope](ExprPtr const& expr) {
        return expr == nullptr ? std::string{}
                               : detail::textOf(detail::evalIn(scope, nullptr, expr), scope.formatting());
    };
    Tour tour{.id = declared.id,
              .screen = declared.screen,
              .title = textOf(declared.title),
              .summary = textOf(declared.summary),
              .steps = {},
              .revision = declared.revision};
    for (TourStepDecl const& step : declared.steps) {
        tour.steps.push_back(TourStep{.title = textOf(step.title),
                                      .body = textOf(step.body),
                                      .screen = step.screen,
                                      .params = step.params,
                                      .target = step.target,
                                      .advanceOn = step.advanceOn});
    }
    return tour;
}

// The app document's tours, then the runtime ones. A runtime tour with one of the app document's ids is left out,
// since the server refuses one and an old list may still hold it.
std::vector<Tour> lowerTours(Document const& doc, detail::RtScope& scope, std::vector<TourDecl> const& runtime) {
    std::vector<Tour> out;
    out.reserve(doc.tours.size());
    for (TourDecl const& declared : doc.tours) {
        out.push_back(lowerTour(scope, declared));
    }
    for (TourDecl const& declared : runtime) {
        if (std::ranges::none_of(out, [&declared](Tour const& kept) { return kept.id == declared.id; })) {
            out.push_back(lowerTour(scope, declared));
        }
    }
    return out;
}

}  // namespace

std::expected<std::unique_ptr<AppRoot>, Refusal> Interpreter::mountApp(std::shared_ptr<Document const> doc) {
    if (doc->id != "app") {
        return std::unexpected(
            Refusal{.document = doc->id, .path = "/id", .reason = "the app document's id is `app`"});
    }
    if (_session->appScope() != nullptr) {
        return std::unexpected(
            Refusal{.document = doc->id, .path = {}, .reason = "an app document is already mounted on the session"});
    }
    std::unique_ptr<AppRoot> root{new AppRoot{}};
    root->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, MountOptions{});
    detail::ScreenCore& core = *root->_core;
    root->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, 0);
    core.rootScope = root->_scope;
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    _session->runtime().untracked([&] {
        core.declare(*root->_scope, &doc->scope, {}, lists);
        detail::LowerCtx const ctx{
            .scope = root->_scope,
            .bindings = std::make_shared<detail::Bindings const>(),
            .boundary = nullptr,
        };
        std::vector<detail::BadgeModel> badges = core.lowerBadgeParts(doc->navigation, ctx);
        for (std::size_t index = 0; index < badges.size(); ++index) {
            root->_navigation.push_back(AppRoot::PlacedBadge{
                .screen = std::string{doc->navigation.at(index)->name("screen")},
                .badge = std::move(badges.at(index)),
            });
        }
    });
    root->_navigationBadges = std::make_unique<reactive::Computed<std::vector<NavigationBadge>>>(
        _session->runtime(), [placed = &root->_navigation] {
            std::vector<NavigationBadge> shown;
            for (AppRoot::PlacedBadge const& entry : *placed) {
                if (entry.badge.visible.evaluate()) {
                    shown.push_back(NavigationBadge{
                        .screen = entry.screen,
                        .text = entry.badge.text.evaluate(),
                        .tone = entry.badge.tone.evaluate(),
                    });
                }
            }
            return shown;
        });
    // One computed value for every tour: the texts are few, and a change of locale changes all of them at once.
    root->_runtimeTours = std::make_unique<reactive::Signal<std::shared_ptr<std::vector<TourDecl> const>>>(
        _session->runtime(), std::make_shared<std::vector<TourDecl> const>());
    root->_tours = std::make_unique<reactive::Computed<std::vector<Tour>>>(
        _session->runtime(), [doc, scope = root->_scope.get(), runtime = root->_runtimeTours.get()] {
            return lowerTours(*doc, *scope, *runtime->get());
        });
    // The app document's date patterns are the application's: every screen of the bundle shows and reads dates so.
    if (!doc->datePattern.empty() || !doc->dateTimePattern.empty()) {
        _session->setDatePatterns(doc->datePattern, doc->dateTimePattern);
    }
    _session->setAppScope(root->_scope.get());
    if (auto const found = doc->lifecycle.find("onMount"); found != doc->lifecycle.end()) {
        root->runCommands(found->second);
    }
    return root;
}

}  // namespace morph::ui
