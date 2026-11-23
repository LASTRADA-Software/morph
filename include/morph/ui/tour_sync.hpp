// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <string>
#include <vector>

#include "../attributes.hpp"
#include "../core/callback_scope.hpp"
#include "../core/wire.hpp"
#include "document.hpp"
#include "interpreter.hpp"

/// @file
/// @brief The client of the runtime tours model, `morph.tours`: the list, kept current through the record watch.
///        Specified in `docs/spec/ui/tours.md`.

namespace morph::ui {

/// @brief Lists the runtime tours a server offers and keeps the list current.
///
/// `list` asks `morph.tours.list`. An answer makes the tours offered, loads each listed tour with `loadTourText` (one
/// that does not load is logged at warn and left out), tells `setOnListed`'s handler, and has the session watch the
/// record `morph.tours`. A server that never registered the model answers `unknown model type: morph.tours`: the sync
/// then offers none, which is logged once per sync at debug, since a server without runtime tours is ordinary. Any
/// other failure is logged at warn and keeps the last list. While offered, a `changes` event naming the record, a
/// `resync` and a reconnect list again. Neither copyable nor movable; on the runtime's owner.
class MORPH_API TourSync {
public:
    /// @brief A sync that has not listed yet.
    /// @param dispatcher Where model calls go: the interpreter's own. Borrowed: it must outlive the sync.
    /// @param session    The session whose watch set carries `morph.tours` once offered; null for a source whose
    ///                   server sends no events, such as an in-process one. Borrowed: it must outlive the sync.
    explicit TourSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, Session* session = nullptr);
    TourSync(TourSync const&) = delete;
    TourSync& operator=(TourSync const&) = delete;
    TourSync(TourSync&&) = delete;
    TourSync& operator=(TourSync&&) = delete;
    /// @brief Stops the watch.
    ~TourSync();

    /// @brief Asks for the list, and learns whether the server offers runtime tours.
    /// @param done Called once with whether they are offered; may be empty.
    void list(std::function<void(bool offered)> done = {});

    /// @brief Asks for the list again while offered; the replies of one sync arrive in call order.
    /// @param done Called once this list's reply was handled, whatever it held; may be empty.
    void relist(std::function<void()> done = {});

    /// @brief The connection was made again: lists again while offered; probes again when the last failure was not the
    ///        absent model. The session sends the watch itself.
    void reconnected();

    /// @brief A server event: a `changes` naming `morph.tours`, or a `resync`, lists again while offered.
    /// @param event The event.
    void deliverEvent(wire::EventBody const& event);

    /// @brief Forgets the list and the offer, stops the watch, and drops replies still in flight: a new frame's
    ///        session.
    void reset();

    /// @brief Whether the server offers runtime tours, as the last list found.
    /// @return It.
    [[nodiscard]] bool offered() const noexcept { return _offered; }

    /// @brief The tours the last list held, loaded, in the server's order.
    /// @return They; empty before the first list and without the model.
    [[nodiscard]] std::vector<TourDecl> const& tours() const noexcept { return _tours; }

    /// @brief Sets what is told after each list that answered.
    /// @param onListed The handler, given the tours; empty tells nothing.
    void setOnListed(std::function<void(std::vector<TourDecl> const&)> onListed) { _onListed = std::move(onListed); }

private:
    void call(std::function<void(std::string const&)> onReply,
              std::function<void(std::string const&, std::string const&)> const& failed);
    void listed(std::string const& reply);

    Dispatcher* _dispatcher;
    Session* _session;
    std::shared_ptr<ModelChannel> _channel;
    std::vector<TourDecl> _tours;
    std::function<void(std::vector<TourDecl> const&)> _onListed;
    bool _offered = false;
    bool _absent = false;
    bool _absentLogged = false;
    // Last, so it stops every delivery before anything a reply would touch goes; `reset` replaces it.
    std::unique_ptr<async::CallbackScope> _scope = std::make_unique<async::CallbackScope>();
};

}  // namespace morph::ui
