// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../reactive/runtime.hpp"
#include "document.hpp"
#include "interpreter.hpp"
#include "value.hpp"

/// @file
/// @brief `morph::ui::Inspector`: the development inspector's read-only view of a mounted screen (spec 6 §21).
///
/// An inspector exists only for a screen whose session is in development mode (`DevelopmentTools::On`). Specified in
/// `docs/spec/ui/inspector.md`.

namespace morph::ui {

/// @brief A mounted node as the inspector lists it.
struct InspectedNode {
    /// @brief The node kind: `text`, `button`, `column`, …
    std::string kind;
    /// @brief The element id; empty when the node has none.
    std::string id;
    /// @brief The `testId`; empty when the node has none.
    std::string testId;
    /// @brief The node's JSON Pointer in the document.
    std::string path;
    /// @brief Each expression property, evaluated in the scope the node was lowered in, by its document key.
    ValueRecord properties;
};

/// @brief A query or mutation as the inspector lists it.
struct InspectedQuery {
    /// @brief The declared name.
    std::string name;
    /// @brief Where it is declared: `screen`, `nested` (a dialog's, drawer's, switch case's or row's scope) or `app`.
    std::string scope;
    /// @brief Whether it is a mutation.
    bool mutation = false;
    /// @brief The model type.
    std::string model;
    /// @brief The action it runs.
    std::string action;
    /// @brief The cache key: model type, instance, action and canonical body; empty for an idle query and a
    ///        mutation.
    std::string key;
    /// @brief `idle`, `pending`, `fresh`, `stale` or `error`.
    std::string state;
    /// @brief How long ago the shown value arrived, on the session's clock; empty before any reply and for a
    ///        mutation.
    std::optional<std::chrono::milliseconds> age;
    /// @brief The last failure's text; empty when none.
    std::string lastError;
};

/// @brief The development inspector over one mounted screen: its mounted nodes, its queries and mutations, the command
///        lists it ran, and the reactive runtime's counters.
///
/// Every read returns a copy, so nothing it hands out writes the screen. The one thing it does to the screen is
/// `refetch`, which runs the document command `{"refetch": "<query>"}` as a user's event would. Valid while the
/// screen lives; use it on the runtime's owner.
class Inspector {
public:
    /// @brief An inspector over @p screen, when its session records what an inspector reads.
    /// @param screen The screen; it outlives the inspector.
    /// @return The inspector; nothing when the screen's session has `DevelopmentTools::Off`, which records nothing.
    [[nodiscard]] static std::optional<Inspector> over(Screen& screen MORPH_LIFETIMEBOUND) {
        if (!screen._core->recording) {
            return std::nullopt;
        }
        return Inspector{screen};
    }

    /// @brief The screen's document id.
    /// @return It.
    [[nodiscard]] std::string const& screenId() const noexcept { return _screen->id(); }

    /// @brief Every node of the screen whose scope is alive, in the order it was lowered (a parent before its
    ///        children), each with its expression properties evaluated now.
    ///
    /// A node is listed once per scope it was lowered in, so a row's nodes are listed once per row. A switch's
    /// `default` and a custom node's fallback are lowered in their enclosing scope, so they are listed whether or not
    /// they are shown. An evaluation the inspector makes reports no diagnostic.
    /// @return The nodes.
    [[nodiscard]] std::vector<InspectedNode> nodes() const {
        detail::ScreenCore& core = *_screen->_core;
        Quiet const quiet{core};
        std::vector<InspectedNode> out;
        core.runtime().untracked([&] {
            for (detail::ScreenCore::LoweredNode const& entry : core.loweredNodes) {
                std::shared_ptr<detail::RtScope> const scope = entry.scope.lock();
                std::shared_ptr<detail::Bindings const> const bindings = entry.bindings.lock();
                if (!scope || !bindings) {
                    continue;
                }
                DocNode const& node = *entry.node;
                InspectedNode inspected{.kind = node.kind,
                                        .id = node.id,
                                        .testId = std::string{node.name("testId")},
                                        .path = node.path,
                                        .properties = {}};
                for (auto const& [key, expression] : node.props) {
                    inspected.properties.insert_or_assign(key, detail::evalIn(*scope, bindings.get(), expression));
                }
                out.push_back(std::move(inspected));
            }
        });
        return out;
    }

    /// @brief Every query and mutation of the screen's live scopes, the screen's own first, then those of the app
    ///        document, with their state now.
    ///
    /// A query is `idle` with no cache entry (its `when` is not `true`, its alias is unbound or its body is `null`),
    /// and with an entry that has neither a value nor a request; `pending` while a request is in flight; `error`
    /// when the last request failed; `stale` when its entry was invalidated or its value is at least `staleTime`
    /// old; `fresh` otherwise. A mutation is `pending` while a run is in flight or queued, `error` when its last run
    /// failed, `fresh` once a run succeeded, and `idle` before any.
    /// @return The queries and mutations.
    [[nodiscard]] std::vector<InspectedQuery> queries() const {
        detail::ScreenCore& core = *_screen->_core;
        std::vector<InspectedQuery> out;
        core.runtime().untracked([&] {
            for (detail::RtScope const* const scope : core.scopes) {
                collect(*scope, scope == _screen->_scope.get() ? "screen" : "nested", out);
            }
            if (detail::RtScope const* const app = core.session.appScope()) {
                collect(*app, "app", out);
            }
        });
        return out;
    }

    /// @brief The latest command lists the screen ran, oldest first, at most `detail::kCommandLogSize`.
    /// @return Each list's event, its commands and its batch.
    [[nodiscard]] std::vector<CommandLogEntry> commandLog() const {
        auto const& log = _screen->_core->commandLog;
        return {log.begin(), log.end()};
    }

    /// @brief The reactive runtime's counters: flushes, Effects per flush, batches and misuses.
    /// @return They.
    [[nodiscard]] reactive::RuntimeStats counters() const { return _screen->runtime().stats(); }

    /// @brief Refetches a query by running the document command `{"refetch": "<query>"}` in the scope that declares
    ///        it, as a user's event runs a command list: the same path, the same batch, the same log entry.
    /// @param query The query's name, as `queries()` lists it.
    /// @return Whether a live scope of the screen declares a query of that name.
    bool refetch(std::string_view query) {
        detail::ScreenCore& core = *_screen->_core;
        for (detail::RtScope* const scope : core.scopes) {
            auto const found = scope->names.find(query);
            if (found == scope->names.end() || found->second.category != detail::NameCategory::Query) {
                continue;
            }
            Commands const commands{
                Command{.kind = CommandKind::Refetch, .path = "/inspector/0", .target = std::string{query}}};
            detail::CommandCtx const ctx{.scope = scope->shared_from_this(),
                                         .bindings = std::make_shared<detail::Bindings>(),
                                         .cancelClose = std::make_shared<bool>(false),
                                         .boundary = nullptr};
            core.session.asUser([&] { core.runtime().batch([&] { core.runList(commands, ctx, {}); }); });
            return true;
        }
        return false;
    }

private:
    /// Turns off the screen's diagnostics while the inspector evaluates.
    class Quiet {
    public:
        explicit Quiet(detail::ScreenCore& core) : _core{&core}, _was{core.inspecting} { core.inspecting = true; }
        ~Quiet() { _core->inspecting = _was; }
        Quiet(Quiet const&) = delete;
        Quiet& operator=(Quiet const&) = delete;
        Quiet(Quiet&&) = delete;
        Quiet& operator=(Quiet&&) = delete;

    private:
        detail::ScreenCore* _core;
        bool _was;
    };

    [[nodiscard]] static std::string text(Value const& value) {
        std::string const* const string = value.asString();
        return string == nullptr ? std::string{} : *string;
    }

    void collect(detail::RtScope const& scope, std::string const& where, std::vector<InspectedQuery>& out) const {
        detail::ScreenCore const& core = *_screen->_core;
        std::chrono::milliseconds const now = core.session.now();
        for (auto const& query : scope.queries) {
            InspectedQuery inspected{.name = query->decl().name,
                                     .scope = where,
                                     .mutation = false,
                                     .model = query->typeId(),
                                     .action = query->decl().action,
                                     .key = {},
                                     .state = "idle",
                                     .age = std::nullopt,
                                     .lastError = {}};
            if (std::shared_ptr<detail::CacheEntry> const& entry = query->entry()) {
                inspected.key = entry->key;
                inspected.lastError = entry->error.peek();
                if (entry->fetchedAt) {
                    inspected.age = now - *entry->fetchedAt;
                }
                bool const old = inspected.age && *inspected.age >= std::chrono::milliseconds{query->decl().staleTime};
                if (entry->pending.peek()) {
                    inspected.state = "pending";
                } else if (!inspected.lastError.empty()) {
                    inspected.state = "error";
                } else if (!entry->hasValue.peek()) {
                    inspected.state = "idle";
                } else {
                    inspected.state = entry->stale || old ? "stale" : "fresh";
                }
            }
            out.push_back(std::move(inspected));
        }
        for (auto const& mutation : scope.mutations) {
            InspectedQuery inspected{.name = mutation->decl().name,
                                     .scope = where,
                                     .mutation = true,
                                     .model = mutation->typeId(),
                                     .action = mutation->decl().action,
                                     .key = {},
                                     .state = "idle",
                                     .age = std::nullopt,
                                     .lastError = text(mutation->error())};
            if (mutation->pending()) {
                inspected.state = "pending";
            } else if (!inspected.lastError.empty()) {
                inspected.state = "error";
            } else if (!mutation->result().isNull()) {
                inspected.state = "fresh";
            }
            out.push_back(std::move(inspected));
        }
    }

    explicit Inspector(Screen& screen) : _screen{&screen} {}

    Screen* _screen;
};

}  // namespace morph::ui
