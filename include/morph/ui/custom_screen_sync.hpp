// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../attributes.hpp"
#include "../core/callback_scope.hpp"
#include "../core/wire.hpp"
#include "custom_screens.hpp"
#include "document.hpp"
#include "interpreter.hpp"
#include "value.hpp"

/// @file
/// @brief The client of the custom screens model: discovery, the documents a client has fetched, and the calls the
///        source view and the draft bar make.
///
/// Specified in `docs/spec/ui/custom_screens.md`, "The client".

namespace morph::ui {

/// @brief Whether @p screenId names a custom screen.
/// @param screenId The screen id.
/// @return Whether it starts with `kCustomScreenIdPrefix`.
[[nodiscard]] inline bool isCustomScreenId(std::string_view screenId) noexcept {
    return screenId.starts_with(kCustomScreenIdPrefix);
}

/// @brief The screen a route names.
/// @param route The route.
/// @return The route up to its `?`, or the whole route without one.
[[nodiscard]] inline std::string_view routeScreen(std::string_view route) noexcept {
    return route.substr(0, route.find('?'));
}

/// @brief The custom documents a client has fetched, by id: what `DocumentScreenHostOptions::documents` answers for a
///        `custom:` id.
///
/// The documents lookup is synchronous, so whatever opens a custom screen (a navigator entry, **Open as**, a restored
/// tab) has its document fetched into the cache first. A list that shows a kept screen with another hash, or a whole
/// list that no longer shows it, drops it, so the next open fetches the stored document again.
class MORPH_API CustomDocuments {
public:
    /// @brief One fetched screen.
    struct Entry {
        /// @brief Its summary, as `get` answered it.
        CustomScreenSummary summary;
        /// @brief Its document's JSON text.
        std::string text;
        /// @brief Its document, loaded.
        std::shared_ptr<Document const> document;
    };

    /// @brief Loads what `get` answered and keeps it under its id, replacing what was kept.
    /// @param found   The reply.
    /// @param options How documents load: the catalog facts.
    /// @return The loaded document, or why it does not load in this client, why the screen is broken, or that it names
    ///         another id; nothing is kept then: a broken screen is never kept.
    std::expected<std::shared_ptr<Document const>, Refusal> put(CustomScreenFound found, LoadOptions const& options);

    /// @brief The loaded document of a kept screen.
    /// @param id The screen's id.
    /// @return It, or null when the screen is not kept.
    [[nodiscard]] std::shared_ptr<Document const> find(std::string_view id) const;

    /// @brief A kept screen.
    /// @param id The screen's id.
    /// @return It, valid until the cache next changes; null when the screen is not kept.
    [[nodiscard]] Entry const* entry(std::string_view id) const;

    /// @brief Drops each kept screen @p listed shows with another hash and, unless the list was truncated, each it
    ///        does not show.
    /// @param listed What `list` answered.
    /// @return The ids dropped.
    std::vector<std::string> refresh(CustomScreensListed const& listed);

    /// @brief Drops a kept screen.
    /// @param id The screen's id; one not kept changes nothing.
    void forget(std::string_view id);

    /// @brief Drops every kept screen: what a change of session does.
    void clear() noexcept { _entries.clear(); }

private:
    std::map<std::string, Entry, std::less<>> _entries;
};

/// @brief Whether a server offers custom screens, as far as a client knows.
enum class CustomScreensOffer : std::uint8_t {
    Unknown,  ///< Not asked yet, or asked under a session that has since changed.
    Offered,  ///< `list` answered.
    Off,      ///< The registration of `morph.customScreens`, or `list`, was refused.
};

/// @brief What an open or a fetch answers: the loaded document, or why there is none.
using CustomDocumentDone = std::function<void(std::expected<std::shared_ptr<Document const>, std::string>)>;

/// @brief The client of a server's custom screens model (`morph.customScreens`) for one application, through the
///        interpreter's own `Dispatcher`, so it travels the same bridge, session and backend as the screens' models.
///
/// No call names a person: the server keys every screen by the verified session's principal. Every reply arrives on
/// the runtime's owner. Replies still in flight when the sync is destroyed or `reset` are dropped.
class MORPH_API CustomScreenSync {
public:
    /// @brief A sync for @p application.
    /// @param dispatcher  Where model calls go: the interpreter's own. Borrowed: it must outlive the sync.
    /// @param application The application's id, the `app` of every call.
    /// @param catalog     The catalog facts a fetched document loads with, read at each load.
    /// @param session     The session whose watch set carries the record `morph.customScreens` once the server offers
    ///                    custom screens, so a save from anywhere relists and every watch set the session sends,
    ///                    `reconnected`'s included, names it; null for a source whose server sends no events, such as
    ///                    an in-process one. Borrowed: it must outlive the sync.
    CustomScreenSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, std::string application,
                     std::function<Catalog()> catalog, Session* session = nullptr);
    CustomScreenSync(CustomScreenSync const&) = delete;
    CustomScreenSync& operator=(CustomScreenSync const&) = delete;
    CustomScreenSync(CustomScreenSync&&) = delete;
    CustomScreenSync& operator=(CustomScreenSync&&) = delete;
    ~CustomScreenSync();

    /// @brief Asks whether the server offers custom screens, by calling `list`.
    ///
    /// The list answering makes the offer `Offered`, keeps the list, tells `setOnListed`'s handler and has the session
    /// watch the record. Any refusal makes it `Off`: a server that never registered the model refuses its registration
    /// with `unknown model type: morph.customScreens`, which is how a client learns custom screens are not offered;
    /// any other failure is logged at warn too, worded by its cause (refused, unreadable reply, or the transport),
    /// since it is a misconfiguration or an outage rather than an absence.
    /// @param done Called once with whether they are offered; may be empty.
    void probe(std::function<void(bool offered)> done = {});

    /// @brief What the last probe found.
    /// @return The offer.
    [[nodiscard]] CustomScreensOffer offer() const noexcept { return _offer; }

    /// @brief Why the offer is `Off`.
    /// @return The refusal's text; empty unless the offer is `Off`.
    [[nodiscard]] std::string const& offRefusal() const noexcept { return _offRefusal; }

    /// @brief Sets what is told each list the sync receives, from a probe or a relist.
    /// @param onListed The handler: the list, and the ids of the kept documents that list dropped (changed on the
    ///                 server, or gone); empty tells nothing.
    void setOnListed(
        std::function<void(CustomScreensListed const&, std::vector<std::string> const& dropped)> onListed) {
        _onListed = std::move(onListed);
    }

    /// @brief The last list received.
    /// @return It; empty before the first.
    [[nodiscard]] CustomScreensListed const& listed() const noexcept { return _listed; }

    /// @brief Calls `list` again, while the offer is `Offered`: the cache drops what the list shows changed, and
    ///        `setOnListed`'s handler is told. A refusal is logged at warn and keeps the last list.
    ///
    ///        The replies of one sync's calls arrive in the order the calls were made, because every call goes through
    ///        the one private channel; a list never overtakes a later one.
    void relist();

    /// @brief The document of a custom screen: the kept one, or `fetch`ed.
    /// @param id   The screen's id.
    /// @param done Called once with the document, or why there is none. With a kept document it is called before
    ///             `open` returns; otherwise later, on the runtime's owner.
    void open(std::string const& id, CustomDocumentDone done);

    /// @brief Calls `get` and keeps the document it answers, replacing a kept one.
    /// @param id   The screen's id.
    /// @param done Called once with the document, or why there is none: the store's refusal as it gave it, why the
    ///             document does not load in this client, or that the screen is broken, with the checks' reason. A
    ///             broken screen is never kept.
    void fetch(std::string const& id, CustomDocumentDone done);

    /// @brief Calls `get` and keeps nothing: what a source view of a broken screen reads.
    /// @param id   The screen's id.
    /// @param done Called once with the summary and the document, or the refusal.
    void get(std::string const& id, std::function<void(std::expected<CustomScreenFound, std::string>)> done);

    /// @brief Asks the server whether @p document would be saved as a custom screen based on @p basedOn.
    /// @param basedOn  The base screen's id.
    /// @param document The document's JSON text.
    /// @param done     Called once with the first refusal, or none, or why the call failed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the `check` action's order.
    void check(std::string basedOn, std::string document,
               std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)> done);

    /// @brief Saves a custom screen; its `app` is set to the sync's. A save that is answered relists.
    /// @param request The screen to overwrite, if any, its title, base and document.
    /// @param done    Called once with the screen's id, or the refusal as the store or the checks gave it.
    void save(CustomScreensSave request, std::function<void(std::expected<std::string, std::string>)> done);

    /// @brief Fetches the document of every custom screen @p routes name that is not kept, so a start that reopens
    ///        them can read the routes back.
    /// @param routes The routes, a workspace's record or a history's.
    /// @param done   Called once every fetch has answered, with the ids no document came for, in the routes' order.
    void prefetchRoutes(std::vector<std::string> const& routes, std::function<void(std::vector<std::string>)> done);

    /// @brief The connection was made again: while offered, relists; while off for a cause other than the server
    ///        lacking the model (a refusal, an unreadable reply, an outage), probes again. The watch on the record is
    ///        the session's: its own `reconnected` sends it.
    void reconnected();

    /// @brief A server event: a `changes` naming the record `morph.customScreens` relists, and a `resync` relists
    ///        (the session sends the watch again itself). Anything else, or anything before the offer is `Offered`,
    ///        is ignored.
    /// @param event The event.
    void deliverEvent(wire::EventBody const& event);

    /// @brief Forgets everything learnt under the session that is ending: the offer, the list, the cache and the
    ///        binding. Replies still in flight are dropped.
    void reset();

    /// @brief The fetched documents.
    /// @return The cache.
    [[nodiscard]] CustomDocuments& documents() noexcept { return _documents; }

    /// @brief The fetched documents.
    /// @return The cache.
    [[nodiscard]] CustomDocuments const& documents() const noexcept { return _documents; }

private:
    // What turned the offer off; only an absent model is not reported.
    enum class OffCause : std::uint8_t { Absent, Refused, Unreadable, Transport };

    // Sends one action through the binding, made at the first call; @p ok gets the reply's JSON, @p failed the reason
    // and the failure's kind (`detail::defaultErrorKind`).
    void call(std::string_view action, std::string const& body, std::function<void(std::string const&)> ok,
              std::function<void(std::string const&, std::string const&)> const& failed);
    void off(OffCause cause, std::string reason);
    void listedNow(CustomScreensListed listed);

    Dispatcher* _dispatcher;
    std::string _application;
    std::function<Catalog()> _catalog;
    Session* _session;
    std::shared_ptr<ModelChannel> _channel;
    CustomScreensOffer _offer = CustomScreensOffer::Unknown;
    std::string _offRefusal;
    OffCause _offCause = OffCause::Absent;
    CustomScreensListed _listed;
    std::function<void(CustomScreensListed const&, std::vector<std::string> const&)> _onListed;
    CustomDocuments _documents;
    // Last, so it stops every delivery before anything a reply would touch goes; `reset` replaces it.
    std::unique_ptr<async::CallbackScope> _scope = std::make_unique<async::CallbackScope>();
};

}  // namespace morph::ui
