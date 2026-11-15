// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/file_store.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_parts.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {
namespace {

/// @brief Whether @p bytes can be served: within `kMaxFileBytes`, and within the envelope bound once escaped into
///        an `ok` reply.
/// @param bytes The file.
/// @return Why it cannot, or nothing.
[[nodiscard]] std::optional<std::string> checkServable(std::string const& bytes) {
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
[[nodiscard]] std::optional<std::string> checkEntry(CatalogEntry const& entry) {
    if (entry.id.empty()) {
        return std::string{"a catalog entry has an id"};
    }
    if (auto reason = detail::checkName(entry.id, "a catalog entry id")) {
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
[[nodiscard]] bool permitsAll(::morph::session::IAuthorizer const& authorizer,
                              ::morph::session::Context const& session, std::vector<ActionRef> const& refs) {
    return std::ranges::all_of(
        refs, [&](ActionRef const& ref) { return authorizer.authorize(session, ref.modelType, ref.action); });
}

}  // namespace
}  // namespace morph::ui

namespace morph::ui {

std::expected<void, Refusal> UiRegistry::addScreen(CatalogEntry entry, std::string text) {
    std::string const screenId = entry.id;
    std::size_t const size = text.size();
    auto added = addScreenImpl(std::move(entry), std::move(text));
    if (added) {
        ::morph::log::logDebug("[ui-serve] added the screen `{}` ({} bytes)", screenId, size);
    } else {
        ::morph::log::logWarn("[ui-serve] refused a screen: {}", added.error().message());
    }
    return added;
}

std::expected<void, Refusal> UiRegistry::addEntry(CatalogEntry entry) {
    std::string const entryId = entry.id;
    auto added = addEntryImpl(std::move(entry));
    if (added) {
        ::morph::log::logDebug("[ui-serve] added the entry `{}`", entryId);
    } else {
        ::morph::log::logWarn("[ui-serve] refused an entry: {}", added.error().message());
    }
    return added;
}

std::expected<void, Refusal> UiRegistry::addFile(BundleFile file) {
    std::string const kind = file.kind;
    std::string const fileId = file.id;
    std::size_t const size = file.bytes.size();
    auto added = addFileImpl(std::move(file));
    if (added) {
        ::morph::log::logDebug("[ui-serve] added the {} `{}` ({} bytes)", kind, fileId, size);
    } else {
        ::morph::log::logWarn("[ui-serve] refused a file: {}", added.error().message());
    }
    return added;
}

std::expected<void, Refusal> UiRegistry::setShellParts(ShellParts parts, Catalog reads) {
    // A refused call leaves the parts and the catalog as they were, so the next app document is not judged by
    // them.
    Catalog previousCatalog = _catalog;
    std::optional<ShellParts> previous = std::exchange(_shellParts, std::move(parts));
    _catalog = withReads(_baseCatalog, std::move(reads));
    std::expected<void, Refusal> outcome = [&]() -> std::expected<void, Refusal> {
        if (auto named = checkShellNames()) {
            return std::unexpected(std::move(*named));
        }
        return applyShellParts(previous.has_value());
    }();
    if (!outcome) {
        _shellParts = std::move(previous);
        _catalog = std::move(previousCatalog);
    }
    return outcome;
}

std::expected<std::shared_ptr<ServedBundle const>, Refusal> UiRegistry::build() const {
    auto built = buildImpl();
    if (built) {
        logThemeProblems();
        ::morph::log::logDebug("[ui-serve] built the bundle of `{}`: {} files, digest {}", _application.id,
                               (*built)->files.size(), (*built)->digest);
    } else {
        ::morph::log::logWarn("[ui-serve] refused to build: {}", built.error().message());
    }
    return built;
}

void UiRegistry::logThemeProblems() const {
    TokenFileLookup const lookup = [this](std::string_view fileId) -> TokenSet const* {
        auto const found = _tokenFiles.find(fileId);
        return found == _tokenFiles.end() ? nullptr : &found->second;
    };
    for (auto const& [indexId, index] : _themeIndexes) {
        for (std::string const& problem : themeProblems(index, lookup)) {
            ::morph::log::logWarn("[ui-serve] theme {}: {}", indexId, problem);
        }
    }
}

std::expected<void, Refusal> UiRegistry::addScreenImpl(CatalogEntry entry, std::string text) {
    if (auto refusal = checkNewEntry(entry)) {
        return std::unexpected(std::move(*refusal));
    }
    if (entry.host) {
        return refuse(entry.id, "a host screen has no document; it is added with addEntry");
    }
    if (auto reason = checkServable(text)) {
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
    logWarnings(*loaded);
    DocumentActions const named{*loaded};
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

std::expected<void, Refusal> UiRegistry::addEntryImpl(CatalogEntry entry) {
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

std::expected<void, Refusal> UiRegistry::addFileImpl(BundleFile file) {
    if (file.kind != kAppFile && file.kind != kThemeFile && file.kind != kMessagesFile &&
        file.kind != kComponentFile && file.kind != kIconFile) {
        return refuse(file.id,
                      "a file is an app document, a theme, a message catalogue, a component or an icon, "
                      "not `" +
                          file.kind + "`");
    }
    if (file.id.empty()) {
        return refuse(file.id, "a file has an id");
    }
    if (auto reason = detail::checkName(file.id, "a file id")) {
        return refuse(file.id, std::move(*reason));
    }
    if (file.kind == kAppFile && _appSynthesised) {
        // The app document `setShellParts` made stands in until the application adds its own.
        std::erase_if(_files, [](BundleFile const& held) { return held.kind == kAppFile; });
        _appSynthesised = false;
    }
    if (std::ranges::any_of(
            _files, [&file](BundleFile const& added) { return added.kind == file.kind && added.id == file.id; })) {
        return refuse(file.id, "the " + file.kind + " `" + file.id + "` is already added");
    }
    if (auto reason = checkServable(file.bytes)) {
        return refuse(file.id, std::move(*reason));
    }
    if (file.kind == kAppFile && _shellParts) {
        auto patched = withShellParts(std::move(file), false);
        if (!patched) {
            return std::unexpected(std::move(patched.error()));
        }
        file = std::move(*patched);
    }
    if (auto added = addKind(file); !added) {
        return added;
    }
    _files.push_back(std::move(file));
    return {};
}

std::expected<std::shared_ptr<ServedBundle const>, Refusal> UiRegistry::buildImpl() const {
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

std::optional<Refusal> UiRegistry::checkThemes() const {
    TokenFileLookup const lookup = [this](std::string_view fileId) -> TokenSet const* {
        auto const found = _tokenFiles.find(fileId);
        return found == _tokenFiles.end() ? nullptr : &found->second;
    };
    for (auto const& [indexId, index] : _themeIndexes) {
        if (auto reason = checkTheme(index, lookup)) {
            return Refusal{.document = indexId, .path = {}, .reason = "the theme: " + std::move(*reason)};
        }
        for (auto const& [name, fileId] : index.icons) {
            bool const held = std::ranges::any_of(
                _files, [&fileId](BundleFile const& file) { return file.kind == kIconFile && file.id == fileId; });
            if (!held) {
                return Refusal{.document = indexId,
                               .path = {},
                               .reason = std::format("the theme index names the icon `{}` as `{}`, which is not "
                                                     "an icon file of the bundle",
                                                     name, fileId)};
            }
        }
    }
    return std::nullopt;
}

std::expected<void, Refusal> UiRegistry::addKind(BundleFile const& file) {
    if (file.kind == kMessagesFile) {
        return checkCatalogue(file);
    }
    if (file.kind == kThemeFile) {
        return addTheme(file);
    }
    if (file.kind == kAppFile) {
        return addApp(file);
    }
    if (file.kind == kIconFile) {
        return checkIcon(file);
    }
    return {};
}

std::expected<void, Refusal> UiRegistry::checkIcon(BundleFile const& file) {
    if (file.bytes.size() > kMaxIconBytes) {
        return refuse(file.id, std::format("an icon is at most {} bytes of SVG; this one is {}", kMaxIconBytes,
                                           file.bytes.size()));
    }
    if (!file.bytes.contains("<svg")) {
        return refuse(file.id, "an icon is SVG text, and this one has no <svg> element");
    }
    return {};
}

std::expected<void, Refusal> UiRegistry::checkCatalogue(BundleFile const& file) {
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
    return {};
}

std::expected<void, Refusal> UiRegistry::addTheme(BundleFile const& file) {
    if (isThemeIndex(file.bytes)) {
        auto index = parseThemeIndex(file.bytes);
        if (!index) {
            return refuse(file.id, "the theme index: " + index.error());
        }
        _themeIndexes.emplace_back(file.id, std::move(*index));
        return {};
    }
    auto tokens = parseTokens(file.bytes);
    if (!tokens) {
        return refuse(file.id, "the token file: " + tokens.error());
    }
    _tokenFiles.emplace(file.id, std::move(*tokens));
    return {};
}

std::expected<void, Refusal> UiRegistry::applyShellParts(bool replace) {
    auto found = std::ranges::find_if(_files, [](BundleFile const& file) { return file.kind == kAppFile; });
    if (found == _files.end()) {
        BundleFile file{.kind = std::string{kAppFile},
                        .id = std::string{kAppFile},
                        .bytes = R"({"vocab":"ui/1","id":"app"})",
                        .isPublic = true};
        auto patched = withShellParts(std::move(file), false);
        if (!patched) {
            return std::unexpected(std::move(patched.error()));
        }
        if (auto added = addApp(*patched); !added) {
            return added;
        }
        _files.push_back(std::move(*patched));
        _appSynthesised = true;
        return {};
    }
    auto patched = withShellParts(*found, replace);
    if (!patched) {
        return std::unexpected(std::move(patched.error()));
    }
    if (auto added = addApp(*patched); !added) {
        return added;
    }
    *found = std::move(*patched);
    return {};
}

std::expected<BundleFile, Refusal> UiRegistry::withShellParts(BundleFile file, bool replace) const {
    auto document = parseJson(file.bytes);
    auto* const object = document ? std::get_if<Json::object_t>(&document->data) : nullptr;
    auto shell = parseJson(glz::write_json(*_shellParts).value_or("{}"));
    if (object == nullptr || !shell) {
        return refuse(file.id, "the app document is a JSON object");
    }
    if (!replace && object->contains("shell")) {
        return refuse(file.id, "the app document declares its own `shell`, and the registry was given parts too");
    }
    object->insert_or_assign("shell", std::move(*shell));
    file.bytes = glz::write_json(*document).value_or(file.bytes);
    return file;
}

std::expected<void, Refusal> UiRegistry::addApp(BundleFile const& file) {
    auto loaded = loadDocumentText(file.bytes, LoadOptions{.catalog = _catalog});
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (loaded->id != kAppFile) {
        return refuse(file.id, "the app document's id is `app`");
    }
    logWarnings(*loaded);
    DocumentActions const named{*loaded};
    _appActions = named.actions();
    _appModelTypes = named.modelTypes();
    return {};
}

void UiRegistry::logWarnings(Document const& document) {
    for (LoadWarning const& warning : document.warnings) {
        morph::log::logWarn(warning.message(document.id));
    }
}

std::optional<Refusal> UiRegistry::checkNewEntry(CatalogEntry const& entry) const {
    if (auto reason = checkEntry(entry)) {
        return Refusal{.document = entry.id, .path = {}, .reason = std::move(*reason)};
    }
    if (std::ranges::any_of(_screens, [&](Screen const& screen) { return screen.served.entry.id == entry.id; })) {
        return Refusal{.document = entry.id, .path = {}, .reason = "a catalog entry with this id is registered"};
    }
    return std::nullopt;
}

Catalog UiRegistry::withReads(Catalog base, Catalog reads) {
    if (reads.model) {
        base.model = [first = std::move(base.model),
                      second = std::move(reads.model)](std::string_view typeId) -> std::optional<ModelInfo> {
            auto found = first ? first(typeId) : std::nullopt;
            return found ? found : second(typeId);
        };
    }
    if (reads.action) {
        base.action = [first = std::move(base.action),
                       second = std::move(reads.action)](std::string_view actionId) -> std::optional<ActionInfo> {
            auto found = first ? first(actionId) : std::nullopt;
            return found ? found : second(actionId);
        };
    }
    return base;
}

std::optional<Refusal> UiRegistry::checkShellNames() const {
    if (!_shellParts) {
        return std::nullopt;
    }
    auto const hasModule = [this](std::string const& module) {
        return module.empty() || std::ranges::any_of(_screens, [&module](Screen const& screen) {
                   return screen.served.entry.module == module;
               });
    };
    auto const hasScreen = [this](std::string const& id) {
        return std::ranges::any_of(_screens, [&id](Screen const& screen) { return screen.served.entry.id == id; });
    };
    auto const unknown = [](std::string path, std::string const& kind, std::string const& name) {
        return Refusal{.document = std::string{kAppFile},
                       .path = std::move(path),
                       .reason = "the shell names the " + kind + " `" + name + "`, which no screen registered has"};
    };
    for (std::size_t index = 0; index < _shellParts->modules.size(); ++index) {
        if (auto const& module = _shellParts->modules.at(index).module; !hasModule(module)) {
            return unknown("/shell/modules/" + std::to_string(index) + "/module", "module", module);
        }
    }
    for (std::size_t index = 0; index < _shellParts->search.size(); ++index) {
        if (auto const& module = _shellParts->search.at(index).module; !hasModule(module)) {
            return unknown("/shell/search/" + std::to_string(index) + "/module", "module", module);
        }
    }
    if (_shellParts->inbox && !hasScreen(_shellParts->inbox->screen)) {
        return unknown("/shell/inbox/screen", "screen", _shellParts->inbox->screen);
    }
    if (_shellParts->help && !_shellParts->help->screen.empty() && !hasScreen(_shellParts->help->screen)) {
        return unknown("/shell/help/screen", "screen", _shellParts->help->screen);
    }
    return std::nullopt;
}

std::optional<Refusal> UiRegistry::checkBuildable() const {
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

void UiRegistry::serveFiles(ServedBundle& bundle) const {
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

void UiRegistry::addFacts(ServedBundle& bundle) const {
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
    auto const documents = [&all](auto const& lookup, auto& out) {
        if (!lookup) {
            return;
        }
        auto& known = out.emplace();
        for (ActionRef const& ref : all) {
            if (std::optional<std::string> document = lookup(ref.action)) {
                known.emplace(ref.action, std::move(*document));
            }
        }
    };
    documents(_catalog.view, bundle.views);
    documents(_catalog.wizard, bundle.wizards);
}

UiService::UiService(std::shared_ptr<ServedBundle const> bundle,
                     ::morph::model::detail::ActionDispatcher& dispatcher MORPH_LIFETIMEBOUND,
                     std::shared_ptr<IFileStore> files)
    : _bundle{std::move(bundle)}, _dispatcher{&dispatcher}, _files{std::move(files)} {
    if (!_bundle) {
        throw std::invalid_argument("UiService: the bundle is null");
    }
}

bool UiService::serves(std::string_view kind) const {
    return kind == kHelloKind || kind == kCatalogKind || kind == kFetchKind || kind == kEvaluateKind ||
           (kind == kFileKind && _files != nullptr);
}

std::string UiService::handle(::morph::wire::Envelope const& request,
                              ::morph::session::IAuthorizer const& authorizer) {
    ::morph::backend::ConnectionFacts facts;
    return handle(request, authorizer, facts);
}

std::string UiService::handle(::morph::wire::Envelope const& request, ::morph::session::IAuthorizer const& authorizer,
                              ::morph::backend::ConnectionFacts& facts) {
    if (request.kind == kHelloKind) {
        return hello(request, authorizer, facts);
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

std::string UiService::hello(::morph::wire::Envelope const& request, ::morph::session::IAuthorizer const& authorizer,
                             ::morph::backend::ConnectionFacts& facts) const {
    Answer answer = helloAnswer(request, authorizer, facts);
    ::morph::log::logDebug("[ui-hello] `{}`: {}", answer.subject, answer.outcome);
    return std::move(answer.encoded);
}

UiService::Answer UiService::helloAnswer(::morph::wire::Envelope const& request,
                                         ::morph::session::IAuthorizer const& authorizer,
                                         ::morph::backend::ConnectionFacts& facts) const {
    std::string const application = _bundle->application.id;
    if (!gate(authorizer, request, kHelloAction)) {
        return Answer{.encoded = err(request, std::string{::morph::wire::kUnauthorizedMessage}),
                      .subject = application,
                      .outcome = "refused: unauthorized"};
    }
    auto client = parseHelloRequest(request.body);
    if (!client) {
        std::string const reason = std::move(client.error());
        return Answer{.encoded = err(request, reason), .subject = application, .outcome = "refused: " + reason};
    }
    std::string missing;
    for (std::string const& vocabulary : _bundle->vocabularies) {
        if (std::ranges::find(client->vocabularies, vocabulary) == client->vocabularies.end()) {
            missing += (missing.empty() ? "" : ", ") + vocabulary;
        }
    }
    if (!missing.empty()) {
        std::string const reason = "missing vocabulary: " + missing;
        return Answer{.encoded = err(request, reason), .subject = application, .outcome = "refused: " + reason};
    }
    if (client->events) {
        facts.events = true;
    }
    return Answer{.encoded = ok(request, detail::writeMessage(HelloReply{.application = _bundle->application.id,
                                                                         .version = _bundle->application.version,
                                                                         .vocabularies = _bundle->vocabularies,
                                                                         .digest = _bundle->digest,
                                                                         .events = client->events})),
                  .subject = application,
                  .outcome = "ok, digest " + _bundle->digest};
}

std::string UiService::evaluate(::morph::wire::Envelope const& request,
                                ::morph::session::IAuthorizer const& authorizer) const {
    if (request.modelType.empty() || request.actionType.empty()) {
        return err(request, "ui-evaluate names no modelType and actionType");
    }
    if (!authorizer.authorize(request.session, request.modelType, request.actionType)) {
        return err(request, std::string{::morph::wire::kUnauthorizedMessage});
    }
    auto evaluation = _dispatcher->evaluate(request.modelType, request.actionType, request.body);
    EvaluateReply reply{.body = {}, .errors = std::move(evaluation.errors), .ready = evaluation.ready};
    if (auto const error = glz::read_json(reply.body, evaluation.body)) {
        return err(request, "ui-evaluate: " + glz::format_error(error, evaluation.body));
    }
    return ok(request, detail::writeMessage(reply));
}

std::string UiService::file(::morph::wire::Envelope const& request,
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
            return err(request, std::string{::morph::wire::kUnauthorizedMessage});
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

std::string UiService::catalog(::morph::wire::Envelope const& request,
                               ::morph::session::IAuthorizer const& authorizer) const {
    Answer answer = catalogAnswer(request, authorizer);
    ::morph::log::logDebug("[ui-catalog] `{}`: {}", answer.subject, answer.outcome);
    return std::move(answer.encoded);
}

UiService::Answer UiService::catalogAnswer(::morph::wire::Envelope const& request,
                                           ::morph::session::IAuthorizer const& authorizer) const {
    std::string const application = _bundle->application.id;
    if (!gate(authorizer, request, kCatalogAction)) {
        return Answer{.encoded = err(request, std::string{::morph::wire::kUnauthorizedMessage}),
                      .subject = application,
                      .outcome = "refused: unauthorized"};
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
    // A collection's or wizard's document goes with the action that names it, to whoever may fetch that action.
    auto const documents = [&fetchable](auto const& served, auto& out) {
        if (!served) {
            return;
        }
        auto& known = out.emplace();
        for (auto const& [action, document] : *served) {
            if (fetchable.actions.contains(action)) {
                known.emplace(action, document);
            }
        }
    };
    documents(_bundle->views, reply.views);
    documents(_bundle->wizards, reply.wizards);
    return Answer{.encoded = ok(request, detail::writeMessage(reply)),
                  .subject = application,
                  .outcome = std::format("ok, {} entries", reply.entries.size())};
}

UiService::Fetchable UiService::listEntries(CatalogReply& reply, ::morph::session::Context const& session,
                                            ::morph::session::IAuthorizer const& authorizer) const {
    Fetchable fetchable{.modelTypes = {_bundle->appModelTypes.begin(), _bundle->appModelTypes.end()}, .actions = {}};
    for (ActionRef const& ref : _bundle->appActions) {
        fetchable.actions.insert(ref.action);
    }
    for (ServedScreen const& screen : _bundle->screens) {
        CatalogEntry entry = screen.entry;
        if (entry.available && !permitsAll(authorizer, session, screen.topQueries)) {
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

std::string UiService::fetch(::morph::wire::Envelope const& request,
                             ::morph::session::IAuthorizer const& authorizer) const {
    Answer answer = fetchAnswer(request, authorizer);
    ::morph::log::logDebug("[ui-fetch] {}: {}", answer.subject, answer.outcome);
    return std::move(answer.encoded);
}

UiService::Answer UiService::fetchAnswer(::morph::wire::Envelope const& request,
                                         ::morph::session::IAuthorizer const& authorizer) const {
    auto wanted = parseFetchRequest(request.body);
    if (!wanted) {
        std::string const reason = std::move(wanted.error());
        return Answer{.encoded = err(request, reason), .subject = "<unreadable>", .outcome = "refused: " + reason};
    }
    std::string const& hash = wanted->hash;
    auto const found = _bundle->files.find(hash);
    bool const isPublic =
        found != _bundle->files.end() &&
        std::ranges::any_of(found->second.holders, [](ServedFile::Holder const& holder) { return holder.isPublic; });
    if (isPublic) {
        return gate(authorizer, request, kHelloAction)
                   ? Answer{.encoded = ok(request, found->second.bytes), .subject = hash, .outcome = "ok"}
                   : Answer{.encoded = err(request, std::string{::morph::wire::kUnauthorizedMessage}),
                            .subject = hash,
                            .outcome = "refused: unauthorized"};
    }
    // The fetch gate comes before the lookup's answer, so a caller it refuses cannot learn which hashes exist.
    if (!gate(authorizer, request, kFetchAction)) {
        return Answer{.encoded = err(request, std::string{::morph::wire::kUnauthorizedMessage}),
                      .subject = hash,
                      .outcome = "refused: unauthorized"};
    }
    if (found == _bundle->files.end()) {
        return Answer{.encoded = err(request, "not found"), .subject = hash, .outcome = "refused: not found"};
    }
    bool const admitted = std::ranges::any_of(found->second.holders, [&](ServedFile::Holder const& holder) {
        return !holder.screen || screenAdmits(_bundle->screens.at(*holder.screen), request.session, authorizer);
    });
    return admitted ? Answer{.encoded = ok(request, found->second.bytes), .subject = hash, .outcome = "ok"}
                    : Answer{.encoded = err(request, std::string{::morph::wire::kUnauthorizedMessage}),
                             .subject = hash,
                             .outcome = "refused: unauthorized"};
}

bool UiService::screenAdmits(ServedScreen const& screen, ::morph::session::Context const& session,
                             ::morph::session::IAuthorizer const& authorizer) {
    return screen.entry.available && permitsAll(authorizer, session, screen.topQueries);
}

}  // namespace morph::ui
