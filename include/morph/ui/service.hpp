// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/remote.hpp"
#include "bundle.hpp"
#include "file_store.hpp"
#include "theme.hpp"

/// @file
/// @brief The server side of the UI service (spec 5 §12, §12b, §13): `UiRegistry` collects an application's screens
///        and files and builds a `ServedBundle`; `UiService` answers `ui-hello`, `ui-catalog` and `ui-fetch` from it
///        as a `RemoteServer` envelope service.
///
/// Specified in `docs/spec/ui/service.md`.

namespace morph::ui {

/// @brief The application a bundle belongs to, as `ui-hello` names it.
struct ApplicationInfo {
    /// @brief The application's id.
    std::string id;
    /// @brief The application's version.
    std::string version;
};

/// @brief An action a document names, with the model type it runs on: what the authoriser is asked about.
struct ActionRef {
    /// @brief The model's registered type id.
    std::string modelType;
    /// @brief The action id.
    std::string action;

    /// @brief Orders by model type, then action.
    /// @return The ordering.
    auto operator<=>(ActionRef const&) const = default;
};

/// @brief One file a bundle serves, with every manifest entry whose bytes it is.
struct ServedFile {
    /// @brief One manifest entry that names the file.
    struct Holder {
        /// @brief The file kind, or `manifest` for the manifest itself.
        std::string kind;
        /// @brief The file id.
        std::string id;
        /// @brief Whether the entry is public.
        bool isPublic = false;
        /// @brief For a screen, its index in `ServedBundle::screens`.
        std::optional<std::size_t> screen;
    };

    /// @brief The file's bytes.
    std::string bytes;
    /// @brief The entries that name these bytes; two entries with equal bytes share one file.
    std::vector<Holder> holders;
};

/// @brief A screen as the bundle serves it.
struct ServedScreen {
    /// @brief The catalog entry, as registered.
    CatalogEntry entry;
    /// @brief The document's content hash; empty for an entry without a document.
    std::string hash;
    /// @brief The queries the document declares at its top level: the principal may see the screen when it may run
    ///        every one (spec 5 §12b).
    std::vector<ActionRef> topQueries;
    /// @brief Every action a query, a mutation or a model-bound node names, at any scope.
    std::vector<ActionRef> actions;
    /// @brief Every model type an alias names, at any scope.
    std::vector<std::string> modelTypes;
};

/// @brief An application's bundle, built and immutable: what `UiService` serves.
struct ServedBundle {
    /// @brief The application.
    ApplicationInfo application;
    /// @brief The vocabularies the bundle needs, sorted; always `expr/1` and `ui/1`.
    std::vector<std::string> vocabularies;
    /// @brief The manifest's text.
    std::string manifest;
    /// @brief The manifest's content hash, which `ui-hello` replies with.
    std::string digest;
    /// @brief Every file by its content hash, the manifest included.
    std::map<std::string, ServedFile, std::less<>> files;
    /// @brief The catalog's screens, in registration order.
    std::vector<ServedScreen> screens;
    /// @brief The actions the app document names.
    std::vector<ActionRef> appActions;
    /// @brief The model types the app document names.
    std::vector<std::string> appModelTypes;
    /// @brief Every action any served document names, sorted and unique: the candidates of `permitted`.
    std::vector<ActionRef> allActions;
    /// @brief The model facts the registry's catalog gave, for every model type a document names; absent when it
    ///        has no model lookup.
    std::optional<std::map<std::string, ModelInfo, std::less<>>> models;
    /// @brief The action facts the registry's catalog gave, for every action a document names; absent when it has
    ///        no action lookup.
    std::optional<std::map<std::string, ActionInfo, std::less<>>> actions;
    /// @brief The view documents the registry's catalog gave, by the action of their `v-query`, for every action a
    ///        document names; absent when it has no view lookup.
    std::optional<std::map<std::string, std::string, std::less<>>> views;
    /// @brief The wizard documents the registry's catalog gave, by the action of their first step, for every action
    ///        a document names; absent when it has no wizard lookup.
    std::optional<std::map<std::string, std::string, std::less<>>> wizards;
};

/// @brief The actions of @p bundle that the principal of @p session may run: what `ui-catalog` answers as
///        `permitted`.
///
/// The candidates are `ServedBundle::allActions`, so an action no served document names is never permitted here,
/// whatever the authoriser would say about it.
/// @param bundle     The bundle.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @return The permitted action ids, in `allActions` order.
[[nodiscard]] MORPH_API std::vector<std::string> permittedActions(ServedBundle const& bundle,
                                                                  ::morph::session::Context const& session,
                                                                  ::morph::session::IAuthorizer const& authorizer);

/// @brief Whether the principal of @p session may open @p screen: it is available, and the principal may run every
///        query the screen declares at its top level (spec 5 §12b).
/// @param screen     The screen.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @return Whether the screen admits the principal.
[[nodiscard]] MORPH_API bool screenAdmitted(ServedScreen const& screen, ::morph::session::Context const& session,
                                            ::morph::session::IAuthorizer const& authorizer);

/// @brief A file of the bundle that is not a screen, as `UiRegistry::addFile` takes it.
struct BundleFile {
    /// @brief `app`, `theme`, `messages`, `component` or `icon`.
    std::string kind;
    /// @brief The file's id: `app` for the app document, or a theme's, a catalogue's or a component's name.
    std::string id;
    /// @brief The file, served byte for byte.
    std::string bytes;
    /// @brief Whether anyone the `ui-hello` gate admits may fetch it, before sign-in.
    bool isPublic = false;
};

/// @brief Collects an application's screens and files and builds the bundle the UI service serves.
///
/// Built once, before the service answers anything; `build` copies everything into an immutable `ServedBundle`.
/// A screen authored with the C++ builders is added by its emitted text, so an authoring registry feeds this one
/// through `addScreen` without either depending on the other.
class MORPH_API UiRegistry {
public:
    /// @brief An empty registry.
    /// @param application The application `ui-hello` names.
    /// @param catalog     The catalog facts: load-time validation reads them at registration, and the
    ///                    `ui-catalog` reply carries them to the client. A lookup left empty is neither checked nor
    ///                    served.
    explicit UiRegistry(ApplicationInfo application, Catalog catalog = {})
        : _application{std::move(application)}, _baseCatalog{catalog}, _catalog{std::move(catalog)} {}

    /// @brief Registers a screen: its catalog entry and its document.
    ///
    /// Refused when the entry's id is empty, longer than `kMaxNameBytes`, already registered, or not the document's
    /// id; when `whenDenied` is neither `hide` nor `disable`; when the entry names a `host` (a host screen has no
    /// document: `addEntry`); when the document is `app` (`addFile`); when the text is too large to serve; and when
    /// load-time validation refuses the document.
    /// @param entry The catalog entry.
    /// @param text  The document's JSON text, served byte for byte.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addScreen(CatalogEntry entry, std::string text);

    /// @brief Lists an entry that has no document: a host screen (`host`), or one the application has not built
    ///        (`available: false`).
    ///
    /// Refused on the same entry rules as `addScreen`, and when the entry is neither a host screen nor unavailable.
    /// @param entry The catalog entry.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addEntry(CatalogEntry entry);

    /// @brief Adds a file that is not a screen: the app document (`app`, whose id is `app`), a theme (`theme`), a
    ///        message catalogue (`messages`), a delivered component (`component`) or an icon (`icon`).
    ///
    /// Refused for another kind, an empty or over-long id, a kind and id already added, a file too large to serve,
    /// an app document that load-time validation refuses or whose id is not `app`, a message catalogue whose id is
    /// not `i18n/LOCALE.json`, that `parseCatalogue` refuses, or that has a pattern outside the `ui/1` subset, and a
    /// theme file that is neither a theme index `parseThemeIndex` takes nor a token file `parseTokens` takes, and an
    /// icon that is not SVG text or is larger than `kMaxIconBytes`.
    /// @param file The file.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addFile(BundleFile file);

    /// @brief Declares the shell's parts (`docs/spec/ui/shell_parts.md`): a scope switcher,
    ///        module counts, search, an inbox and the person's menu. They are written into the app document's `shell`
    ///        member, so they travel in the file every client already reads; an application that added no app
    ///        document gets an otherwise empty one, public.
    ///
    /// Refused when the app document with the parts does not load, and when a part names a module, a screen or an
    /// action that the registry does not hold at all: this is the check of a misconfiguration, and it is made against
    /// every screen registered, whoever may see it, so a screen hidden from some person is still a screen. The
    /// screens are therefore added before the parts; a screen added after them cannot make a name they give
    /// unknown, so `build` does not check again.
    ///
    /// The actions a part reads are not named by any screen, so the catalog does not know them yet: @p reads adds
    /// their facts, from the same `CatalogFacts` a screen's builders gather, to the registry's catalog, so the
    /// `ui-catalog` reply carries them as it carries a screen's. A later call replaces the facts of an earlier one.
    /// @param parts The parts.
    /// @param reads The facts of the models and actions the parts read; their lookups are consulted after the
    ///              registry's own. Empty: the parts read only what a screen already names.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> setShellParts(ShellParts parts, Catalog reads = {});

    /// @brief Adds a vocabulary the bundle needs beyond `ui/1` and `expr/1`, such as `forms/1` or `files/1`. A
    ///        client that does not speak it is refused at `ui-hello`.
    /// @param vocabulary The vocabulary.
    void requireVocabulary(std::string vocabulary) { _vocabularies.insert(std::move(vocabulary)); }

    /// @brief Builds the bundle.
    ///
    /// Refused when a screen names a delivered component (`qml`) whose hash no `component` file has, when the
    /// bundle holds more than `kMaxBundleFiles` files or entries, and when it needs more than `kMaxListItems`
    /// vocabularies or a vocabulary name longer than `kMaxNameBytes`. A theme file's token that nothing reads is
    /// logged at warn and does not refuse the build.
    /// @return The bundle, or the refusal.
    [[nodiscard]] std::expected<std::shared_ptr<ServedBundle const>, Refusal> build() const;

private:
    /// @brief Logs, at warn, what each theme index's files hold that nothing reads or that cannot be read
    ///        (`themeProblems`); the bundle is still built.
    void logThemeProblems() const;

    /// @brief Registers a screen; the public `addScreen` logs its outcome.
    /// @param entry The catalog entry.
    /// @param text  The document's JSON text.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addScreenImpl(CatalogEntry entry, std::string text);

    /// @brief Registers an entry that has no document; the public `addEntry` logs its outcome.
    /// @param entry The catalog entry.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addEntryImpl(CatalogEntry entry);

    /// @brief Adds a file; the public `addFile` logs its outcome.
    /// @param file The file.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addFileImpl(BundleFile file);

    /// @brief Builds the bundle; the public `build` logs its outcome.
    /// @return The bundle, or the refusal.
    [[nodiscard]] std::expected<std::shared_ptr<ServedBundle const>, Refusal> buildImpl() const;

    /// @brief Checks every theme index: each combination of its variants, densities and surfaces over `base`
    ///        resolves from the bundle's token files (`checkTheme`).
    /// @return The refusal, naming the index and the combination, or nothing.
    [[nodiscard]] std::optional<Refusal> checkThemes() const;

    /// @brief A registered screen and what `build` needs of its document.
    struct Screen {
        /// @brief What the bundle serves.
        ServedScreen served;
        /// @brief The document's text; empty for an entry without one.
        std::string text;
        /// @brief The delivered components the document names: their `qml` pointer and hash.
        std::vector<std::pair<std::string, std::string>> components;
    };

    // The checks and the indexing one kind of file adds to addFile's common ones.
    std::expected<void, Refusal> addKind(BundleFile const& file);

    // An icon is SVG text, small enough for a theme to carry a set of them.
    [[nodiscard]] static std::expected<void, Refusal> checkIcon(BundleFile const& file);

    [[nodiscard]] static std::expected<void, Refusal> checkCatalogue(BundleFile const& file);

    std::expected<void, Refusal> addTheme(BundleFile const& file);

    // Writes `_shellParts` into the app document, or into a synthesised one. @p replace says the app document may
    // already hold parts this registry wrote.
    std::expected<void, Refusal> applyShellParts(bool replace);

    // The app document with the shell's parts written into its `shell` member. A document that already declares
    // `shell` is refused unless @p replace says the registry wrote it.
    [[nodiscard]] std::expected<BundleFile, Refusal> withShellParts(BundleFile file, bool replace) const;

    std::expected<void, Refusal> addApp(BundleFile const& file);

    /// @brief Logs what load-time validation found in @p document that does not refuse it.
    /// @param document The loaded document.
    static void logWarnings(Document const& document);

    /// @brief A refusal of the registration of @p document.
    /// @param document The screen or file id.
    /// @param reason   Why.
    /// @return The refusal, as an `std::unexpected`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the id first, then the reason, at every call.
    [[nodiscard]] static std::unexpected<Refusal> refuse(std::string document, std::string reason) {
        return std::unexpected(Refusal{.document = std::move(document), .path = {}, .reason = std::move(reason)});
    }

    /// @brief Checks an entry about to be registered: its own fields, and that its id is new.
    /// @param entry The entry.
    /// @return The refusal, or nothing.
    [[nodiscard]] std::optional<Refusal> checkNewEntry(CatalogEntry const& entry) const;

    /// @brief @p base with the model and action lookups of @p reads behind its own.
    /// @param base  The catalog held.
    /// @param reads The facts to add.
    /// @return The catalog that answers from @p base first and from @p reads where @p base does not know.
    [[nodiscard]] static Catalog withReads(Catalog base, Catalog reads);

    /// @brief Checks the modules and screens the shell's parts name against every screen registered.
    ///
    /// An action a part names is checked by the app document's load, against the catalog.
    /// @return The refusal for the first name no screen has, or nothing.
    [[nodiscard]] std::optional<Refusal> checkShellNames() const;

    /// @brief What `build` refuses: a delivered component missing from the bundle, a theme some combination of which
    ///        does not resolve, and the bounds.
    /// @return The refusal, or nothing.
    [[nodiscard]] std::optional<Refusal> checkBuildable() const;

    /// @brief Fills @p bundle's screens, files, manifest and digest.
    /// @param bundle The bundle being built.
    void serveFiles(ServedBundle& bundle) const;

    /// @brief Fills @p bundle's action candidates and, from the registry's catalog, its facts.
    /// @param bundle The bundle being built, its screens already served.
    void addFacts(ServedBundle& bundle) const;

    ApplicationInfo _application;
    // The catalog given at construction, and the one the registry reads: that one behind the facts of the last
    // `setShellParts`, so a later call replaces the earlier call's facts instead of stacking on them.
    Catalog _baseCatalog;
    Catalog _catalog;
    std::vector<Screen> _screens;
    std::vector<BundleFile> _files;
    /// @brief The theme indexes added, by file id, as `addFile` parsed them.
    std::vector<std::pair<std::string, ThemeIndex>> _themeIndexes;
    /// @brief The theme token files added, by file id, as `addFile` parsed them.
    std::map<std::string, TokenSet, std::less<>> _tokenFiles;
    std::vector<ActionRef> _appActions;
    std::optional<ShellParts> _shellParts;
    bool _appSynthesised = false;
    std::vector<std::string> _appModelTypes;
    std::set<std::string, std::less<>> _vocabularies{std::string{kExprVocabulary}, std::string{kVocabulary}};
};

/// @brief Whether an authoriser is asked about one of the UI service's reads: `ui`'s `hello`, `catalog` or `fetch`.
///
/// An application that serves a login screen admits these for a session with no principal yet, so a client can greet
/// the service, read the catalog and fetch the login screen. Admitting them discloses no other screen: `ui-catalog`
/// lists, and `ui-fetch` serves, a non-public screen only to a principal who may run its queries, which the
/// authoriser still decides.
/// @param modelType  The model type asked about.
/// @param actionType The action asked about.
/// @return Whether it is one of those reads.
[[nodiscard]] constexpr bool isUiServiceRead(std::string_view modelType, std::string_view actionType) noexcept {
    return modelType == "ui" && (actionType == "hello" || actionType == "catalog" || actionType == "fetch");
}

/// @brief The UI service: answers `ui-hello`, `ui-catalog` and `ui-fetch` from one `ServedBundle`, and `ui-evaluate`
///        through the server's `ActionDispatcher`, as an envelope service of a `RemoteServer`
///        (`ServerConfig::services`).
///
/// Every request is authorised (`docs/spec/ui/service.md`, "Authorisation"):
///
/// | Kind | Gate |
/// |---|---|
/// | `ui-hello` | `authorize(session, "ui", "hello")` |
/// | `ui-catalog` | `authorize(session, "ui", "catalog")` |
/// | `ui-fetch` of the manifest or a public file | the `ui-hello` gate |
/// | `ui-fetch` of any other file | `authorize(session, "ui", "fetch")`; for a screen, also the catalog's rule |
/// | `ui-evaluate` | `authorize(session, modelType, actionType)`: as the action it evaluates |
///
/// A refusal is `err "unauthorized"`. A request body that is too large, malformed or carries a must-understand key
/// is refused with the parser's reason.
class MORPH_API UiService final : public ::morph::backend::IEnvelopeService {
public:
    /// @brief Serves @p bundle, and evaluates through @p dispatcher.
    /// @param bundle     The bundle; shared, never changed.
    /// @param dispatcher The dispatcher `ui-evaluate` reads the actions from: the server's own. Borrowed: it must
    ///                   outlive the service.
    /// @throws std::invalid_argument if @p bundle is null.
    /// @param files      The file side channel's store; `nullptr` serves no `ui-file`.
    explicit UiService(std::shared_ptr<ServedBundle const> bundle,
                       ::morph::model::detail::ActionDispatcher& dispatcher MORPH_LIFETIMEBOUND =
                           ::morph::model::detail::defaultDispatcher(),
                       std::shared_ptr<IFileStore> files = nullptr);

    /// @brief Whether @p kind is one of the service's kinds.
    /// @param kind The envelope kind.
    /// @return `true` for `ui-hello`, `ui-catalog`, `ui-fetch` and `ui-evaluate`.
    [[nodiscard]] bool serves(std::string_view kind) const override;

    /// @brief Answers one request with no connection behind it: a `ui-hello` offer of events is accepted in the
    ///        reply, and recorded nowhere.
    /// @param request    The envelope, its principal verified.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer) override;

    /// @brief Answers one request; a `ui-hello` that offers events and is answered `ok` sets `facts.events`.
    ///
    /// A hello that does not offer them leaves the facts as they are, so a later hello on the same connection, such
    /// as a development reload's poll, does not withdraw what the first negotiated.
    /// @param request    The envelope, its principal verified.
    /// @param authorizer The server's authoriser.
    /// @param facts      The facts of the connection the request arrived on.
    /// @return The encoded reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer,
                                     ::morph::backend::ConnectionFacts& facts) override;

private:
    /// @brief The model types and actions of the documents a principal may fetch, whose facts the catalog carries.
    struct Fetchable {
        std::set<std::string, std::less<>> modelTypes;
        std::set<std::string, std::less<>> actions;
    };

    [[nodiscard]] static std::string ok(::morph::wire::Envelope const& request, std::string body) {
        return ::morph::wire::encode(::morph::wire::makeOk(request.callId, std::move(body)));
    }

    [[nodiscard]] static std::string err(::morph::wire::Envelope const& request, std::string message) {
        return ::morph::wire::encode(::morph::wire::makeErr(std::move(message), request.callId));
    }

    /// @brief An encoded reply and the fields the service's log line carries: what was asked for, and the outcome.
    struct Answer {
        /// @brief The encoded reply.
        std::string encoded;
        /// @brief The hash or application the request named.
        std::string subject;
        /// @brief `ok`, or the refusal the reply carries.
        std::string outcome;
    };

    [[nodiscard]] static bool gate(::morph::session::IAuthorizer const& authorizer,
                                   ::morph::wire::Envelope const& request, std::string_view action) {
        return authorizer.authorize(request.session, kServiceModel, action);
    }

    /// @brief Answers a `ui-hello` and logs the outcome at debug.
    /// @param request    The envelope.
    /// @param authorizer The server's authoriser.
    /// @param facts      The facts of the connection the request arrived on.
    /// @return The encoded reply.
    [[nodiscard]] std::string hello(::morph::wire::Envelope const& request,
                                    ::morph::session::IAuthorizer const& authorizer,
                                    ::morph::backend::ConnectionFacts& facts) const;

    /// @brief Answers a `ui-hello` without logging it.
    /// @param request    The envelope.
    /// @param authorizer The server's authoriser.
    /// @param facts      The facts of the connection the request arrived on.
    /// @return The reply and its log fields.
    [[nodiscard]] Answer helloAnswer(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer,
                                     ::morph::backend::ConnectionFacts& facts) const;

    /// @brief Evaluates the action body @p request carries (spec 5 §7): decoded, reconciled and recomputed, never
    ///        executed, and answered with its field errors. Synchronous on the server strand, like the other kinds.
    /// @param request    The envelope: `modelType`, `actionType` and the action's JSON `body`.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply: an `EvaluateReply`, or `err` for a refusal or a body that does not decode.
    [[nodiscard]] std::string evaluate(::morph::wire::Envelope const& request,
                                       ::morph::session::IAuthorizer const& authorizer) const;

    /// @brief One step of an upload or a download through the file side channel (spec 6 §18).
    ///
    /// `begin` is authorised as the mutation that will reference the file; every later step is authorised by the
    /// upload id or reference, which belong to the principal that created them. A refusal of the store is `err` with
    /// its text; an id or reference of another principal is `not found`, like one that does not exist.
    /// @param request    The envelope; its body is a `FileRequest`.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply: a `FileReply`, or `err`.
    [[nodiscard]] std::string file(::morph::wire::Envelope const& request,
                                   ::morph::session::IAuthorizer const& authorizer) const;

    /// @brief Answers a `ui-catalog` and logs the outcome at debug.
    /// @param request    The envelope.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply.
    [[nodiscard]] std::string catalog(::morph::wire::Envelope const& request,
                                      ::morph::session::IAuthorizer const& authorizer) const;

    /// @brief Answers a `ui-catalog` without logging it.
    /// @param request    The envelope.
    /// @param authorizer The server's authoriser.
    /// @return The reply and its log fields.
    [[nodiscard]] Answer catalogAnswer(::morph::wire::Envelope const& request,
                                       ::morph::session::IAuthorizer const& authorizer) const;

    /// @brief Lists the entries the principal sees into @p reply.
    /// @param reply      The reply being built.
    /// @param session    The session, its principal verified.
    /// @param authorizer The server's authoriser.
    /// @return What the documents the principal may fetch name.
    [[nodiscard]] Fetchable listEntries(CatalogReply& reply, ::morph::session::Context const& session,
                                        ::morph::session::IAuthorizer const& authorizer) const;

    /// @brief Answers a `ui-fetch` and logs the outcome at debug.
    /// @param request    The envelope; its body is a `FetchRequest`.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply.
    [[nodiscard]] std::string fetch(::morph::wire::Envelope const& request,
                                    ::morph::session::IAuthorizer const& authorizer) const;

    /// @brief Answers a `ui-fetch` without logging it.
    /// @param request    The envelope; its body is a `FetchRequest`.
    /// @param authorizer The server's authoriser.
    /// @return The reply and its log fields.
    [[nodiscard]] Answer fetchAnswer(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer) const;

    std::shared_ptr<ServedBundle const> _bundle;
    ::morph::model::detail::ActionDispatcher* _dispatcher;
    std::shared_ptr<IFileStore> _files;
};

}  // namespace morph::ui
