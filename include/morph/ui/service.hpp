// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <compare>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/remote.hpp"
#include "../core/wire.hpp"
#include "../session/session.hpp"
#include "bundle.hpp"
#include "catalogue.hpp"
#include "document.hpp"
#include "file_store.hpp"
#include "theme.hpp"
#include "value.hpp"

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
};

namespace detail {

/// @brief Whether @p bytes can be served: within `kMaxFileBytes`, and within the envelope bound once escaped into
///        an `ok` reply.
/// @param bytes The file.
/// @return Why it cannot, or nothing.
[[nodiscard]] inline std::optional<std::string> checkServable(std::string const& bytes) {
    if (bytes.size() > kMaxFileBytes) {
        return "the file is larger than " + std::to_string(kMaxFileBytes) + " bytes";
    }
    if (::morph::wire::encode(::morph::wire::makeOk(0, bytes)).size() > ::morph::wire::kMaxEnvelopeBytes) {
        return std::string{"the file does not fit one envelope once escaped"};
    }
    return std::nullopt;
}

/// @brief Checks a catalog entry's own fields.
/// @param entry The entry.
/// @return Why it is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> checkEntry(CatalogEntry const& entry) {
    if (entry.id.empty()) {
        return std::string{"a catalog entry has an id"};
    }
    if (auto reason = checkName(entry.id, "a catalog entry id")) {
        return reason;
    }
    if (entry.whenDenied != "hide" && entry.whenDenied != "disable") {
        return "`whenDenied` is `hide` or `disable`, not `" + entry.whenDenied + "`";
    }
    return std::nullopt;
}

// NOLINTBEGIN(misc-no-recursion): view nodes nest, and the walk follows them; parseJson bounds the depth at 64.

/// @brief Collects the actions and model types a loaded document names, resolving each query's, mutation's and
///        model-bound node's alias through the enclosing scopes.
///
/// Load-time validation has already resolved every alias and requires every alias to name a type, so each lookup
/// here finds one.
class DocumentActions {
public:
    /// @brief Walks @p document.
    /// @param document The loaded document.
    explicit DocumentActions(Document const& document) {
        enter(document.scope, true);
        if (document.view) {
            node(*document.view);
        }
    }

    /// @brief The top-level queries.
    /// @return Them, in declaration order.
    [[nodiscard]] std::vector<ActionRef> const& topQueries() const noexcept { return _top; }

    /// @brief Every action named.
    /// @return Them, sorted and unique.
    [[nodiscard]] std::vector<ActionRef> actions() const { return {_all.begin(), _all.end()}; }

    /// @brief Every model type named.
    /// @return Them, sorted and unique.
    [[nodiscard]] std::vector<std::string> modelTypes() const { return {_models.begin(), _models.end()}; }

private:
    void enter(ScopeDecl const& scope, bool top) {
        for (ModelDecl const& model : scope.models) {
            _types.try_emplace(model.name).first->second.push_back(model.type);
            _models.insert(model.type);
        }
        for (QueryDecl const& query : scope.queries) {
            ActionRef ref{.modelType = resolve(query.model), .action = query.action};
            if (top) {
                _top.push_back(ref);
            }
            _all.insert(std::move(ref));
        }
        for (MutationDecl const& mutation : scope.mutations) {
            _all.insert(ActionRef{.modelType = resolve(mutation.model), .action = mutation.action});
        }
    }

    void leave(ScopeDecl const& scope) {
        for (ModelDecl const& model : scope.models) {
            _types.at(model.name).pop_back();
        }
    }

    void node(DocNode const& view) {
        if (view.scope) {
            enter(*view.scope, false);
        }
        if (std::string_view const model = view.name("model"); !model.empty()) {
            _all.insert(ActionRef{.modelType = resolve(model), .action = std::string{view.name("action")}});
        }
        for (auto const& [key, children] : view.children) {
            static_cast<void>(key);
            for (DocNodePtr const& child : children) {
                node(*child);
            }
        }
        for (auto const& [key, parts] : view.parts) {
            static_cast<void>(key);
            for (DocNodePtr const& part : parts) {
                node(*part);
            }
        }
        if (view.scope) {
            leave(*view.scope);
        }
    }

    /// @brief The type of the innermost alias named @p alias. Load-time validation has resolved it already, so the
    ///        lookup cannot miss; `at` throws rather than read past the end if that ever changes.
    [[nodiscard]] std::string const& resolve(std::string_view alias) const {
        std::vector<std::string> const& types = _types.at(std::string{alias});
        return types.at(types.size() - 1);
    }

    /// @brief The types an alias names in the open scopes, innermost last.
    std::map<std::string, std::vector<std::string>, std::less<>> _types;
    std::vector<ActionRef> _top;
    std::set<ActionRef> _all;
    std::set<std::string> _models;
};

// NOLINTEND(misc-no-recursion)

/// @brief Whether the principal of @p session may run every action of @p refs.
/// @param authorizer The authoriser.
/// @param session    The session, its principal verified.
/// @param refs       The actions.
/// @return Whether it may run all of them; `true` for none.
[[nodiscard]] inline bool permitsAll(::morph::session::IAuthorizer const& authorizer,
                                     ::morph::session::Context const& session, std::vector<ActionRef> const& refs) {
    return std::ranges::all_of(
        refs, [&](ActionRef const& ref) { return authorizer.authorize(session, ref.modelType, ref.action); });
}

}  // namespace detail

/// @brief A file of the bundle that is not a screen, as `UiRegistry::addFile` takes it.
struct BundleFile {
    /// @brief `app`, `theme`, `messages` or `component`.
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
class UiRegistry {
public:
    /// @brief An empty registry.
    /// @param application The application `ui-hello` names.
    /// @param catalog     The catalog facts: load-time validation reads them at registration, and the
    ///                    `ui-catalog` reply carries them to the client. A lookup left empty is neither checked nor
    ///                    served.
    explicit UiRegistry(ApplicationInfo application, Catalog catalog = {})
        : _application{std::move(application)}, _catalog{std::move(catalog)} {}

    /// @brief Registers a screen: its catalog entry and its document.
    ///
    /// Refused when the entry's id is empty, longer than `kMaxNameBytes`, already registered, or not the document's
    /// id; when `whenDenied` is neither `hide` nor `disable`; when the entry names a `host` (a host screen has no
    /// document: `addEntry`); when the document is `app` (`addFile`); when the text is too large to serve; and when
    /// load-time validation refuses the document.
    /// @param entry The catalog entry.
    /// @param text  The document's JSON text, served byte for byte.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addScreen(CatalogEntry entry, std::string text) {
        if (auto refusal = checkNewEntry(entry)) {
            return std::unexpected(std::move(*refusal));
        }
        if (entry.host) {
            return refuse(entry.id, "a host screen has no document; it is added with addEntry");
        }
        if (auto reason = detail::checkServable(text)) {
            return refuse(entry.id, std::move(*reason));
        }
        auto loaded = loadDocumentText(text, LoadOptions{.catalog = _catalog});
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        if (loaded->id != entry.id) {
            return refuse(entry.id, "the entry's id is not the document's id `" + loaded->id + "`");
        }
        if (!loaded->view) {
            return refuse(entry.id, "the app document is added with addFile");
        }
        detail::DocumentActions const named{*loaded};
        Screen screen{.served = ServedScreen{.entry = std::move(entry),
                                             .hash = contentHash(text),
                                             .topQueries = named.topQueries(),
                                             .actions = named.actions(),
                                             .modelTypes = named.modelTypes()},
                      .text = std::move(text),
                      .components = {}};
        for (auto const& [name, component] : loaded->components) {
            static_cast<void>(name);
            if (!component.qml.empty()) {
                screen.components.emplace_back(pointerAppend(component.path, "qml"), component.qml);
            }
        }
        _screens.push_back(std::move(screen));
        return {};
    }

    /// @brief Lists an entry that has no document: a host screen (`host`), or one the application has not built
    ///        (`available: false`).
    ///
    /// Refused on the same entry rules as `addScreen`, and when the entry is neither a host screen nor unavailable.
    /// @param entry The catalog entry.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addEntry(CatalogEntry entry) {
        if (auto refusal = checkNewEntry(entry)) {
            return std::unexpected(std::move(*refusal));
        }
        if (!entry.host && entry.available) {
            return refuse(entry.id, "an entry without a document is a host screen or unavailable");
        }
        _screens.push_back(Screen{
            .served =
                ServedScreen{.entry = std::move(entry), .hash = {}, .topQueries = {}, .actions = {}, .modelTypes = {}},
            .text = {},
            .components = {}});
        return {};
    }

    /// @brief Adds a file that is not a screen: the app document (`app`, whose id is `app`), a theme (`theme`), a
    ///        message catalogue (`messages`) or a delivered component (`component`).
    ///
    /// Refused for another kind, an empty or over-long id, a kind and id already added, a file too large to serve,
    /// an app document that load-time validation refuses or whose id is not `app`, a message catalogue whose id is
    /// not `i18n/LOCALE.json`, that `parseCatalogue` refuses, or that has a pattern outside the `ui/1` subset, and a
    /// theme file that is neither a theme index `parseThemeIndex` takes nor a token file `parseTokens` takes.
    /// @param file The file.
    /// @return Nothing, or the refusal.
    std::expected<void, Refusal> addFile(BundleFile file) {
        if (file.kind != kAppFile && file.kind != kThemeFile && file.kind != kMessagesFile &&
            file.kind != kComponentFile) {
            return refuse(file.id, "a file is an app document, a theme, a message catalogue or a component, not `" +
                                       file.kind + "`");
        }
        if (file.id.empty()) {
            return refuse(file.id, "a file has an id");
        }
        if (auto reason = detail::checkName(file.id, "a file id")) {
            return refuse(file.id, std::move(*reason));
        }
        if (std::ranges::any_of(
                _files, [&file](BundleFile const& added) { return added.kind == file.kind && added.id == file.id; })) {
            return refuse(file.id, "the " + file.kind + " `" + file.id + "` is already added");
        }
        if (auto reason = detail::checkServable(file.bytes)) {
            return refuse(file.id, std::move(*reason));
        }
        if (file.kind == kMessagesFile) {
            if (!catalogueLocale(file.id)) {
                return refuse(file.id, "a message catalogue's id is i18n/<locale>.json");
            }
            auto catalogue = parseCatalogue(file.bytes);
            if (!catalogue) {
                return refuse(file.id, std::move(catalogue.error()));
            }
            if (!catalogue->dropped.empty()) {
                return refuse(file.id, "a message is outside the ui/1 subset: " + catalogue->dropped.front());
            }
        }
        if (file.kind == kThemeFile) {
            if (isThemeIndex(file.bytes)) {
                if (auto index = parseThemeIndex(file.bytes); !index) {
                    return refuse(file.id, "the theme index: " + index.error());
                }
            } else if (auto tokens = parseTokens(file.bytes); !tokens) {
                return refuse(file.id, "the token file: " + tokens.error());
            }
        }
        if (file.kind == kAppFile) {
            auto loaded = loadDocumentText(file.bytes, LoadOptions{.catalog = _catalog});
            if (!loaded) {
                return std::unexpected(std::move(loaded.error()));
            }
            if (loaded->id != kAppFile) {
                return refuse(file.id, "the app document's id is `app`");
            }
            detail::DocumentActions const named{*loaded};
            _appActions = named.actions();
            _appModelTypes = named.modelTypes();
        }
        _files.push_back(std::move(file));
        return {};
    }

    /// @brief Adds a vocabulary the bundle needs beyond `ui/1` and `expr/1`, such as `forms/1` or `files/1`. A
    ///        client that does not speak it is refused at `ui-hello`.
    /// @param vocabulary The vocabulary.
    void requireVocabulary(std::string vocabulary) { _vocabularies.insert(std::move(vocabulary)); }

    /// @brief Builds the bundle.
    ///
    /// Refused when a screen names a delivered component (`qml`) whose hash no `component` file has, when the
    /// bundle holds more than `kMaxBundleFiles` files or entries, and when it needs more than `kMaxListItems`
    /// vocabularies or a vocabulary name longer than `kMaxNameBytes`.
    /// @return The bundle, or the refusal.
    [[nodiscard]] std::expected<std::shared_ptr<ServedBundle const>, Refusal> build() const {
        if (auto refusal = checkBuildable()) {
            return std::unexpected(std::move(*refusal));
        }
        auto bundle = std::make_shared<ServedBundle>();
        bundle->application = _application;
        bundle->vocabularies.assign(_vocabularies.begin(), _vocabularies.end());
        bundle->appActions = _appActions;
        bundle->appModelTypes = _appModelTypes;
        serveFiles(*bundle);
        addFacts(*bundle);
        return bundle;
    }

private:
    /// @brief Checks every theme index: each combination of its variants, densities and surfaces over `base`
    ///        resolves from the bundle's token files (`checkTheme`).
    /// @return The refusal, naming the index and the combination, or nothing.
    [[nodiscard]] std::optional<Refusal> checkThemes() const {
        std::map<std::string, TokenSet, std::less<>> tokenFiles;
        std::vector<std::pair<std::string, ThemeIndex>> indexes;
        for (BundleFile const& file : _files) {
            if (file.kind != kThemeFile) {
                continue;
            }
            if (isThemeIndex(file.bytes)) {
                if (auto index = parseThemeIndex(file.bytes)) {
                    indexes.emplace_back(file.id, std::move(*index));
                }
            } else if (auto tokens = parseTokens(file.bytes)) {
                tokenFiles.emplace(file.id, std::move(*tokens));
            }
        }
        TokenFileLookup const lookup = [&tokenFiles](std::string_view id) -> TokenSet const* {
            auto const found = tokenFiles.find(id);
            return found == tokenFiles.end() ? nullptr : &found->second;
        };
        for (auto const& [id, index] : indexes) {
            if (auto reason = checkTheme(index, lookup)) {
                return Refusal{.document = id, .path = {}, .reason = "the theme: " + std::move(*reason)};
            }
        }
        return std::nullopt;
    }

    /// @brief A registered screen and what `build` needs of its document.
    struct Screen {
        /// @brief What the bundle serves.
        ServedScreen served;
        /// @brief The document's text; empty for an entry without one.
        std::string text;
        /// @brief The delivered components the document names: their `qml` pointer and hash.
        std::vector<std::pair<std::string, std::string>> components;
    };

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
    [[nodiscard]] std::optional<Refusal> checkNewEntry(CatalogEntry const& entry) const {
        if (auto reason = detail::checkEntry(entry)) {
            return Refusal{.document = entry.id, .path = {}, .reason = std::move(*reason)};
        }
        if (std::ranges::any_of(_screens, [&](Screen const& screen) { return screen.served.entry.id == entry.id; })) {
            return Refusal{.document = entry.id, .path = {}, .reason = "a catalog entry with this id is registered"};
        }
        return std::nullopt;
    }

    /// @brief What `build` refuses: a delivered component missing from the bundle, a theme some combination of which
    ///        does not resolve, and the bounds.
    /// @return The refusal, or nothing.
    [[nodiscard]] std::optional<Refusal> checkBuildable() const {
        if (auto refusal = checkThemes()) {
            return refusal;
        }
        std::set<std::string, std::less<>> componentHashes;
        for (BundleFile const& file : _files) {
            if (file.kind == kComponentFile) {
                componentHashes.insert(contentHash(file.bytes));
            }
        }
        for (Screen const& screen : _screens) {
            for (auto const& [path, hash] : screen.components) {
                if (!componentHashes.contains(hash)) {
                    return Refusal{.document = screen.served.entry.id,
                                   .path = path,
                                   .reason = "the delivered component is not in the bundle"};
                }
            }
        }
        if (_screens.size() > kMaxBundleFiles || _screens.size() + _files.size() > kMaxBundleFiles) {
            return Refusal{
                .document = {},
                .path = {},
                .reason = "the bundle holds more than " + std::to_string(kMaxBundleFiles) + " files or entries"};
        }
        std::vector<std::string> const vocabularies{_vocabularies.begin(), _vocabularies.end()};
        if (auto reason = detail::checkNames(vocabularies, kMaxListItems, "the vocabularies", "a vocabulary")) {
            return Refusal{.document = {}, .path = {}, .reason = std::move(*reason)};
        }
        return std::nullopt;
    }

    /// @brief Fills @p bundle's screens, files, manifest and digest.
    /// @param bundle The bundle being built.
    void serveFiles(ServedBundle& bundle) const {
        Manifest manifest{.application = _application.id, .version = _application.version, .files = {}};
        auto serve = [&bundle, &manifest](BundleFile const& file, std::optional<std::size_t> screen) {
            std::string const hash = contentHash(file.bytes);
            manifest.files.push_back(ManifestFile{
                .id = file.id, .kind = file.kind, .hash = hash, .size = file.bytes.size(), .isPublic = file.isPublic});
            ServedFile& served = bundle.files.try_emplace(hash).first->second;
            served.bytes = file.bytes;
            served.holders.push_back(
                ServedFile::Holder{.kind = file.kind, .id = file.id, .isPublic = file.isPublic, .screen = screen});
        };
        for (Screen const& screen : _screens) {
            if (!screen.served.hash.empty()) {
                serve(BundleFile{.kind = std::string{kScreenFile},
                                 .id = screen.served.entry.id,
                                 .bytes = screen.text,
                                 .isPublic = screen.served.entry.isPublic},
                      bundle.screens.size());
            }
            bundle.screens.push_back(screen.served);
        }
        for (BundleFile const& file : _files) {
            serve(file, std::nullopt);
        }
        std::ranges::sort(manifest.files, [](ManifestFile const& lhs, ManifestFile const& rhs) {
            return std::tie(lhs.kind, lhs.id) < std::tie(rhs.kind, rhs.id);
        });
        bundle.manifest = manifestText(manifest);
        bundle.digest = contentHash(bundle.manifest);
        bundle.files.insert_or_assign(
            bundle.digest,
            ServedFile{.bytes = bundle.manifest,
                       .holders = {ServedFile::Holder{
                           .kind = "manifest", .id = _application.id, .isPublic = true, .screen = std::nullopt}}});
    }

    /// @brief Fills @p bundle's action candidates and, from the registry's catalog, its facts.
    /// @param bundle The bundle being built, its screens already served.
    void addFacts(ServedBundle& bundle) const {
        std::set<ActionRef> all{_appActions.begin(), _appActions.end()};
        std::set<std::string, std::less<>> modelTypes{_appModelTypes.begin(), _appModelTypes.end()};
        for (ServedScreen const& screen : bundle.screens) {
            all.insert(screen.actions.begin(), screen.actions.end());
            modelTypes.insert(screen.modelTypes.begin(), screen.modelTypes.end());
        }
        bundle.allActions.assign(all.begin(), all.end());
        if (_catalog.model) {
            auto& known = bundle.models.emplace();
            for (std::string const& type : modelTypes) {
                // Load-time validation refused every document naming a type the lookup does not know.
                known.emplace(type, _catalog.model(type).value_or(ModelInfo{}));
            }
        }
        if (_catalog.action) {
            auto& known = bundle.actions.emplace();
            for (ActionRef const& ref : all) {
                // Load-time validation refused every document naming an action the lookup does not know.
                known.emplace(ref.action, _catalog.action(ref.action).value_or(ActionInfo{}));
            }
        }
    }

    ApplicationInfo _application;
    Catalog _catalog;
    std::vector<Screen> _screens;
    std::vector<BundleFile> _files;
    std::vector<ActionRef> _appActions;
    std::vector<std::string> _appModelTypes;
    std::set<std::string, std::less<>> _vocabularies{std::string{kExprVocabulary}, std::string{kVocabulary}};
};

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
class UiService final : public ::morph::backend::IEnvelopeService {
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
                       std::shared_ptr<IFileStore> files = nullptr)
        : _bundle{std::move(bundle)}, _dispatcher{&dispatcher}, _files{std::move(files)} {
        if (!_bundle) {
            throw std::invalid_argument("UiService: the bundle is null");
        }
    }

    /// @brief Whether @p kind is one of the service's kinds.
    /// @param kind The envelope kind.
    /// @return `true` for `ui-hello`, `ui-catalog`, `ui-fetch` and `ui-evaluate`.
    [[nodiscard]] bool serves(std::string_view kind) const override {
        return kind == kHelloKind || kind == kCatalogKind || kind == kFetchKind || kind == kEvaluateKind ||
               (kind == kFileKind && _files != nullptr);
    }

    /// @brief Answers one request.
    /// @param request    The envelope, its principal verified.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply.
    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer) override {
        if (request.kind == kHelloKind) {
            return hello(request, authorizer);
        }
        if (request.kind == kCatalogKind) {
            return catalog(request, authorizer);
        }
        if (request.kind == kEvaluateKind) {
            return evaluate(request, authorizer);
        }
        if (request.kind == kFileKind) {
            return file(request, authorizer);
        }
        return fetch(request, authorizer);
    }

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

    [[nodiscard]] static bool gate(::morph::session::IAuthorizer const& authorizer,
                                   ::morph::wire::Envelope const& request, std::string_view action) {
        return authorizer.authorize(request.session, kServiceModel, action);
    }

    [[nodiscard]] std::string hello(::morph::wire::Envelope const& request,
                                    ::morph::session::IAuthorizer const& authorizer) const {
        if (!gate(authorizer, request, kHelloAction)) {
            return err(request, "unauthorized");
        }
        auto client = parseHelloRequest(request.body);
        if (!client) {
            return err(request, std::move(client.error()));
        }
        std::string missing;
        for (std::string const& vocabulary : _bundle->vocabularies) {
            if (std::ranges::find(client->vocabularies, vocabulary) == client->vocabularies.end()) {
                missing += (missing.empty() ? "" : ", ") + vocabulary;
            }
        }
        if (!missing.empty()) {
            return err(request, "missing vocabulary: " + missing);
        }
        return ok(request, detail::writeMessage(HelloReply{.application = _bundle->application.id,
                                                           .version = _bundle->application.version,
                                                           .vocabularies = _bundle->vocabularies,
                                                           .digest = _bundle->digest}));
    }

    /// @brief Evaluates the action body @p request carries (spec 5 §7): decoded, reconciled and recomputed, never
    ///        executed, and answered with its field errors. Synchronous on the server strand, like the other kinds.
    /// @param request    The envelope: `modelType`, `actionType` and the action's JSON `body`.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply: an `EvaluateReply`, or `err` for a refusal or a body that does not decode.
    [[nodiscard]] std::string evaluate(::morph::wire::Envelope const& request,
                                       ::morph::session::IAuthorizer const& authorizer) const {
        if (request.modelType.empty() || request.actionType.empty()) {
            return err(request, "ui-evaluate names no modelType and actionType");
        }
        if (!authorizer.authorize(request.session, request.modelType, request.actionType)) {
            return err(request, "unauthorized");
        }
        auto evaluation = _dispatcher->evaluate(request.modelType, request.actionType, request.body);
        EvaluateReply reply{.body = {}, .errors = std::move(evaluation.errors), .ready = evaluation.ready};
        if (auto const error = glz::read_json(reply.body, evaluation.body)) {
            return err(request, "ui-evaluate: " + glz::format_error(error, evaluation.body));
        }
        return ok(request, detail::writeMessage(reply));
    }

    /// @brief One step of an upload or a download through the file side channel (spec 6 §18).
    ///
    /// `begin` is authorised as the mutation that will reference the file; every later step is authorised by the
    /// upload id or reference, which belong to the principal that created them. A refusal of the store is `err` with
    /// its text; an id or reference of another principal is `not found`, like one that does not exist.
    /// @param request    The envelope; its body is a `FileRequest`.
    /// @param authorizer The server's authoriser.
    /// @return The encoded reply: a `FileReply`, or `err`.
    [[nodiscard]] std::string file(::morph::wire::Envelope const& request,
                                   ::morph::session::IAuthorizer const& authorizer) const {
        auto parsed = parseFileRequest(request.body);
        if (!parsed) {
            return err(request, std::move(parsed.error()));
        }
        FileRequest& step = *parsed;
        std::string const& principal = request.session.principal;
        auto const refused = [&request](FileError error) { return err(request, std::string{fileErrorText(error)}); };
        FileReply reply;
        if (step.op == "begin") {
            if (step.model.empty() || step.action.empty()) {
                return err(request, "ui-file begin names no model and action");
            }
            if (!authorizer.authorize(request.session, step.model, step.action)) {
                return err(request, "unauthorized");
            }
            auto upload = _files->begin(
                principal, FileInfo{.name = std::move(step.name), .type = std::move(step.type), .size = step.size});
            if (!upload) {
                return refused(upload.error());
            }
            reply.upload = std::move(*upload);
        } else if (step.op == "chunk") {
            auto bytes = ::morph::session::detail::base64UrlDecode(step.data);
            if (!bytes) {
                return err(request, "ui-file chunk data is not base64url");
            }
            if (bytes->size() > kMaxFileChunkBytes) {
                return err(request, "ui-file chunk is longer than " + std::to_string(kMaxFileChunkBytes) + " bytes");
            }
            auto received = _files->append(principal, step.upload, step.offset, *bytes);
            if (!received) {
                return refused(received.error());
            }
            reply.received = *received;
        } else if (step.op == "end") {
            auto ref = _files->finish(principal, step.upload);
            if (!ref) {
                return refused(ref.error());
            }
            reply.ref = std::move(*ref);
        } else {
            auto slice = _files->read(principal, step.ref, step.offset, std::min(step.length, kMaxFileChunkBytes));
            if (!slice) {
                return refused(slice.error());
            }
            reply.data = ::morph::session::detail::base64UrlEncode(slice->bytes);
            reply.total = slice->info.size;
            reply.name = std::move(slice->info.name);
            reply.type = std::move(slice->info.type);
        }
        return ok(request, detail::writeMessage(reply));
    }

    [[nodiscard]] std::string catalog(::morph::wire::Envelope const& request,
                                      ::morph::session::IAuthorizer const& authorizer) const {
        if (!gate(authorizer, request, kCatalogAction)) {
            return err(request, "unauthorized");
        }
        CatalogReply reply;
        Fetchable const fetchable = listEntries(reply, request.session, authorizer);
        for (ActionRef const& ref : _bundle->allActions) {
            if (authorizer.authorize(request.session, ref.modelType, ref.action)) {
                reply.permitted.push_back(ref.action);
            }
        }
        if (_bundle->models) {
            auto& known = reply.models.emplace();
            for (auto const& [type, info] : *_bundle->models) {
                if (fetchable.modelTypes.contains(type)) {
                    known.emplace(type, info);
                }
            }
        }
        if (_bundle->actions) {
            auto& known = reply.actions.emplace();
            for (auto const& [action, info] : *_bundle->actions) {
                if (fetchable.actions.contains(action)) {
                    known.emplace(action, info);
                }
            }
        }
        return ok(request, detail::writeMessage(reply));
    }

    /// @brief Lists the entries the principal sees into @p reply.
    /// @param reply      The reply being built.
    /// @param session    The session, its principal verified.
    /// @param authorizer The server's authoriser.
    /// @return What the documents the principal may fetch name.
    [[nodiscard]] Fetchable listEntries(CatalogReply& reply, ::morph::session::Context const& session,
                                        ::morph::session::IAuthorizer const& authorizer) const {
        Fetchable fetchable{.modelTypes = {_bundle->appModelTypes.begin(), _bundle->appModelTypes.end()},
                            .actions = {}};
        for (ActionRef const& ref : _bundle->appActions) {
            fetchable.actions.insert(ref.action);
        }
        for (ServedScreen const& screen : _bundle->screens) {
            CatalogEntry entry = screen.entry;
            if (entry.available && !detail::permitsAll(authorizer, session, screen.topQueries)) {
                if (entry.whenDenied == "hide") {
                    continue;
                }
                entry.available = false;
                entry.reason = "denied";
            }
            if (entry.available || entry.isPublic) {
                fetchable.modelTypes.insert(screen.modelTypes.begin(), screen.modelTypes.end());
                for (ActionRef const& ref : screen.actions) {
                    fetchable.actions.insert(ref.action);
                }
            }
            reply.entries.push_back(std::move(entry));
        }
        return fetchable;
    }

    [[nodiscard]] std::string fetch(::morph::wire::Envelope const& request,
                                    ::morph::session::IAuthorizer const& authorizer) const {
        auto wanted = parseFetchRequest(request.body);
        if (!wanted) {
            return err(request, std::move(wanted.error()));
        }
        auto const found = _bundle->files.find(wanted->hash);
        bool const isPublic = found != _bundle->files.end() &&
                              std::ranges::any_of(found->second.holders,
                                                  [](ServedFile::Holder const& holder) { return holder.isPublic; });
        if (isPublic) {
            return gate(authorizer, request, kHelloAction) ? ok(request, found->second.bytes)
                                                           : err(request, "unauthorized");
        }
        // The fetch gate comes before the lookup's answer, so a caller it refuses cannot learn which hashes exist.
        if (!gate(authorizer, request, kFetchAction)) {
            return err(request, "unauthorized");
        }
        if (found == _bundle->files.end()) {
            return err(request, "not found");
        }
        bool const admitted = std::ranges::any_of(found->second.holders, [&](ServedFile::Holder const& holder) {
            return !holder.screen || screenAdmits(_bundle->screens.at(*holder.screen), request.session, authorizer);
        });
        return admitted ? ok(request, found->second.bytes) : err(request, "unauthorized");
    }

    /// @brief Whether the principal may fetch @p screen's document: it is available, and the catalog lists it as
    ///        such (every top-level query permitted).
    [[nodiscard]] static bool screenAdmits(ServedScreen const& screen, ::morph::session::Context const& session,
                                           ::morph::session::IAuthorizer const& authorizer) {
        return screen.entry.available && detail::permitsAll(authorizer, session, screen.topQueries);
    }

    std::shared_ptr<ServedBundle const> _bundle;
    ::morph::model::detail::ActionDispatcher* _dispatcher;
    std::shared_ptr<IFileStore> _files;
};

}  // namespace morph::ui
