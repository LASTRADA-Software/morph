// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "../core/wire.hpp"
#include "../session/session_auth.hpp"
#include "document.hpp"
#include "value.hpp"

/// @file
/// @brief What the UI service's client and server share (spec 5 §12): content hashes, the bundle's manifest, and
///        the bodies of the `ui-hello`, `ui-catalog` and `ui-fetch` envelopes, with their bounds and their parsers.
///
/// Specified in `docs/spec/ui/service.md`.

namespace morph::ui {

/// @brief The expression vocabulary every bundle needs alongside `ui/1`.
inline constexpr std::string_view kExprVocabulary = "expr/1";

/// @brief The envelope kind of the handshake.
inline constexpr std::string_view kHelloKind = "ui-hello";
/// @brief The envelope kind of the catalog.
inline constexpr std::string_view kCatalogKind = "ui-catalog";
/// @brief The envelope kind that fetches a file by its hash.
inline constexpr std::string_view kFetchKind = "ui-fetch";
/// @brief The envelope kind that evaluates an action body without running it (spec 5 §7).
inline constexpr std::string_view kEvaluateKind = "ui-evaluate";
/// @brief The envelope kind of the file side channel (`files/1`, spec 6 §18): uploads and downloads in chunks.
inline constexpr std::string_view kFileKind = "ui-file";

/// @brief The pseudo-model the service's envelopes are authorised under: `authorize(session, "ui", <action>)`.
inline constexpr std::string_view kServiceModel = "ui";
/// @brief The pseudo-action that gates `ui-hello`, and the fetch of a public file or the manifest.
inline constexpr std::string_view kHelloAction = "hello";
/// @brief The pseudo-action that gates `ui-catalog`.
inline constexpr std::string_view kCatalogAction = "catalog";
/// @brief The pseudo-action that gates the fetch of a file that is not public.
inline constexpr std::string_view kFetchAction = "fetch";

/// @brief The prefix of a content hash.
inline constexpr std::string_view kHashPrefix = "sha256:";
/// @brief The hexadecimal digits of a SHA-256 digest.
inline constexpr std::size_t kHashDigits = 64;

/// @brief The most file bytes one `ui-file` chunk carries, before base64url.
inline constexpr std::size_t kMaxFileChunkBytes = std::size_t{256} * 1024;
/// @brief The largest `ui-file` request body a server reads: a chunk in base64url, and the rest of the request.
inline constexpr std::size_t kMaxFileRequestBytes = (kMaxFileChunkBytes / 3 * 4) + (std::size_t{4} * 1024);

/// @brief The largest request body (`ui-hello`, `ui-fetch`) a server reads.
inline constexpr std::size_t kMaxRequestBytes = std::size_t{64} * 1024;
/// @brief The largest file a bundle may hold, and a client accepts.
inline constexpr std::size_t kMaxFileBytes = std::size_t{4} * 1024 * 1024;
/// @brief The most files a manifest, and entries a catalog, may list.
inline constexpr std::size_t kMaxBundleFiles = 4096;
/// @brief The most action ids, model facts or action facts a catalog may carry.
inline constexpr std::size_t kMaxCatalogFacts = 16384;
/// @brief The most items of a list or map in a `ui-hello` (vocabularies, host components, commands, values).
inline constexpr std::size_t kMaxListItems = 256;
/// @brief The longest name: an id, a kind, a vocabulary, a host name, an application id or version.
inline constexpr std::size_t kMaxNameBytes = 256;

/// @brief The file kinds of a manifest.
inline constexpr std::string_view kScreenFile = "screen";
/// @brief The app document (spec 6 §5).
inline constexpr std::string_view kAppFile = "app";
/// @brief A theme (`ui-theme/1`, spec 6 §6).
inline constexpr std::string_view kThemeFile = "theme";
/// @brief A message catalogue.
inline constexpr std::string_view kMessagesFile = "messages";
/// @brief A delivered custom component (spec 5 §10).
inline constexpr std::string_view kComponentFile = "component";

/// @brief The content hash of @p bytes: `sha256:` and the SHA-256 digest in 64 lower-case hexadecimal digits.
/// @param bytes The bytes.
/// @return The hash.
[[nodiscard]] inline std::string contentHash(std::string_view bytes) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string const raw = ::morph::session::detail::sha256(bytes);
    std::string out{kHashPrefix};
    out.reserve(kHashPrefix.size() + kHashDigits);
    for (char const byte : raw) {
        auto const value = static_cast<unsigned char>(byte);
        out.push_back(kHex.at(static_cast<std::size_t>(value >> 4U)));
        out.push_back(kHex.at(static_cast<std::size_t>(value & 0x0FU)));
    }
    return out;
}

/// @brief Whether @p text is a well-formed content hash.
/// @param text The text.
/// @return `true` for `sha256:` followed by exactly 64 lower-case hexadecimal digits.
[[nodiscard]] inline bool isContentHash(std::string_view text) {
    if (text.size() != kHashPrefix.size() + kHashDigits || !text.starts_with(kHashPrefix)) {
        return false;
    }
    return std::ranges::all_of(text.substr(kHashPrefix.size()), [](char const digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    });
}

/// @brief One file of a manifest.
struct ManifestFile {
    /// @brief The file's id: a screen's document id, `app`, a theme's, a catalogue's or a component's name.
    std::string id;
    /// @brief `screen`, `app`, `theme`, `messages` or `component`; a client fetches no kind it does not know.
    std::string kind;
    /// @brief The file's content hash.
    std::string hash;
    /// @brief The file's size in bytes.
    std::uint64_t size = 0;
    /// @brief Whether anyone the `ui-hello` gate admits may fetch it, before sign-in (spec 6 §8 `public`).
    bool isPublic = false;

    /// @brief Field-wise equality.
    /// @return Whether every field is equal.
    bool operator==(ManifestFile const&) const = default;
};

/// @brief A bundle's manifest: every file the application serves, by its hash (spec 5 §12, "Content addressing").
struct Manifest {
    /// @brief The application's id.
    std::string application;
    /// @brief The application's version.
    std::string version;
    /// @brief The files, sorted by kind then id.
    std::vector<ManifestFile> files;

    /// @brief Finds a file.
    /// @param kind The file kind.
    /// @param fileId The file id.
    /// @return The file, or null.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a manifest is keyed by kind, then id.
    [[nodiscard]] ManifestFile const* find(std::string_view kind, std::string_view fileId) const {
        auto const found = std::ranges::find_if(
            files, [kind, fileId](ManifestFile const& file) { return file.kind == kind && file.id == fileId; });
        return found == files.end() ? nullptr : &*found;
    }
};

/// @brief The body of a `ui-hello` request: what the client speaks and carries.
struct HelloRequest {
    /// @brief The vocabularies the client speaks, such as `ui/1`, `expr/1`, `forms/1`, `ui-theme/1`, `files/1`.
    std::vector<std::string> vocabularies;
    /// @brief The client's Qt version; empty for a client without Qt.
    std::string qt;
    /// @brief Whether the client loads delivered custom components.
    bool components = false;
    /// @brief The host components the client has, by name, with their versions.
    std::map<std::string, std::int64_t, std::less<>> hostComponents;
    /// @brief The host commands the client has, by name, with their versions.
    std::map<std::string, std::int64_t, std::less<>> hostCommands;
    /// @brief The host values the client has, by name, with their versions.
    std::map<std::string, std::int64_t, std::less<>> hostValues;
};

/// @brief The body of a `ui-hello` reply.
struct HelloReply {
    /// @brief The application's id.
    std::string application;
    /// @brief The application's version.
    std::string version;
    /// @brief The vocabularies the bundle needs; a client that lacks one is refused.
    std::vector<std::string> vocabularies;
    /// @brief The manifest's content hash.
    std::string digest;
};

/// @brief The body of a `ui-evaluate` reply: what `ActionDispatcher::evaluate` answered.
struct EvaluateReply {
    /// @brief The action body, reconciled, with its computed fields filled.
    Json body;
    /// @brief The field errors, by field name.
    std::map<std::string, std::string, std::less<>> errors;
    /// @brief Whether an `execute` of this body would reach the model.
    bool ready = false;
};

/// @brief The body of a `ui-file` request: one step of an upload or a download, named by `op`.
struct FileRequest {
    /// @brief `begin`, `chunk`, `end` or `read`.
    std::string op;
    /// @brief `begin`: the file's name.
    std::string name;
    /// @brief `begin`: its media type.
    std::string type;
    /// @brief `begin`: its size in bytes.
    std::size_t size = 0;
    /// @brief `begin`: the model type of the mutation that will reference the file.
    std::string model;
    /// @brief `begin`: that mutation's action.
    std::string action;
    /// @brief `chunk`, `end`: the upload id `begin` answered.
    std::string upload;
    /// @brief `chunk`, `read`: the first byte.
    std::size_t offset = 0;
    /// @brief `chunk`: the bytes, base64url without padding.
    std::string data;
    /// @brief `read`: the file's reference.
    std::string ref;
    /// @brief `read`: at most this many bytes; capped at `kMaxFileChunkBytes`.
    std::size_t length = kMaxFileChunkBytes;
};

/// @brief The body of a `ui-file` reply: the members its `op` answers, the others absent.
struct FileReply {
    /// @brief `begin`: the upload id.
    std::optional<std::string> upload;
    /// @brief `chunk`: the bytes received so far.
    std::optional<std::size_t> received;
    /// @brief `end`: the file's reference.
    std::optional<std::string> ref;
    /// @brief `read`: the bytes, base64url without padding.
    std::optional<std::string> data;
    /// @brief `read`: the whole file's size.
    std::optional<std::size_t> total;
    /// @brief `read`: the file's name.
    std::optional<std::string> name;
    /// @brief `read`: its media type.
    std::optional<std::string> type;
};

/// @brief One catalog entry (spec 6 §8).
struct CatalogEntry {
    /// @brief The screen's id.
    std::string id;
    /// @brief The title: a string, or a message reference such as `{"t": "lims.sample.title", "fallback": "…"}`.
    Json title;
    /// @brief The navigator's module.
    std::string module;
    /// @brief The navigator's group.
    std::string group;
    /// @brief The icon name.
    std::string icon;
    /// @brief Whether the screen opens at most once.
    bool singleton = false;
    /// @brief Whether the entry can be opened; `false` lists it disabled with `reason`.
    bool available = true;
    /// @brief Why the entry is not available: the application's reason, or `denied`.
    std::optional<std::string> reason;
    /// @brief What a permission refusal does to the entry: `hide` or `disable`.
    std::string whenDenied = "hide";
    /// @brief Whether the client may fetch the screen before sign-in.
    bool isPublic = false;
    /// @brief The host screen the application registered with the client instead of a document.
    std::optional<std::string> host;
    /// @brief Whether opening the entry replaces an empty, unmodified tab of it.
    bool reuseEmpty = false;
};

/// @brief The body of a `ui-catalog` reply: the catalog filtered for the principal, and the facts a client's
///        load-time validation reads (`ui::Catalog`).
struct CatalogReply {
    /// @brief The entries the principal sees, in registration order.
    std::vector<CatalogEntry> entries;
    /// @brief The action ids the principal may run, which `{"can": …}` reads.
    std::vector<std::string> permitted;
    /// @brief The model facts by type id; absent when the server has none to give.
    std::optional<std::map<std::string, ModelInfo, std::less<>>> models;
    /// @brief The action facts by action id; absent when the server has none to give.
    std::optional<std::map<std::string, ActionInfo, std::less<>>> actions;
    /// @brief The `v-*` view documents a `collection` node builds from, by the action of their `v-query`; absent
    ///        when the server has none to give.
    std::optional<std::map<std::string, std::string, std::less<>>> views;
    /// @brief The `w-*` wizard documents a `wizard` node builds from, by the action of their first step; absent when
    ///        the server has none to give.
    std::optional<std::map<std::string, std::string, std::less<>>> wizards;

    /// @brief The facts as the lookups load-time validation takes. A lookup the reply carried no facts for is left
    ///        empty, so its rules are skipped.
    /// @return The catalog.
    [[nodiscard]] Catalog facts() const {
        Catalog catalog;
        if (models) {
            catalog.model = [known = std::make_shared<std::map<std::string, ModelInfo, std::less<>> const>(*models)](
                                std::string_view typeId) -> std::optional<ModelInfo> {
                auto const found = known->find(typeId);
                if (found == known->end()) {
                    return std::nullopt;
                }
                return found->second;
            };
        }
        if (actions) {
            catalog.action = [known = std::make_shared<std::map<std::string, ActionInfo, std::less<>> const>(
                                  *actions)](std::string_view actionId) -> std::optional<ActionInfo> {
                auto const found = known->find(actionId);
                if (found == known->end()) {
                    return std::nullopt;
                }
                return found->second;
            };
        }
        if (views) {
            catalog.view = documentLookup(*views);
        }
        if (wizards) {
            catalog.wizard = documentLookup(*wizards);
        }
        return catalog;
    }
};

/// @brief The body of a `ui-fetch` request.
struct FetchRequest {
    /// @brief The content hash of the file wanted: the manifest's digest, or a file the manifest lists.
    std::string hash;
};

}  // namespace morph::ui

/// @brief The wire form of a manifest file: `isPublic` travels as `public`.
template <>
struct glz::meta<morph::ui::ManifestFile> {
    /// @brief The type.
    using T = morph::ui::ManifestFile;
    /// @brief Its members, by wire key.
    static constexpr auto value =
        glz::object("id", &T::id, "kind", &T::kind, "hash", &T::hash, "size", &T::size, "public", &T::isPublic);
};

/// @brief The wire form of a catalog entry: `isPublic` travels as `public`.
template <>
struct glz::meta<morph::ui::CatalogEntry> {
    /// @brief The type.
    using T = morph::ui::CatalogEntry;
    /// @brief Its members, by wire key.
    static constexpr auto value =
        glz::object("id", &T::id, "title", &T::title, "module", &T::module, "group", &T::group, "icon", &T::icon,
                    "singleton", &T::singleton, "available", &T::available, "reason", &T::reason, "whenDenied",
                    &T::whenDenied, "public", &T::isPublic, "host", &T::host, "reuseEmpty", &T::reuseEmpty);
};

namespace morph::ui {

namespace detail {

/// @brief The must-understand key (`!name`) of one object, if it has one.
/// @param json  The value; anything but an object has none.
/// @param where What the object is, for the reason.
/// @return Why the message is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> mustUnderstand(Json const& json, std::string_view where) {
    auto const* const object = std::get_if<Json::object_t>(&json.data);
    if (object == nullptr) {
        return std::nullopt;
    }
    for (auto const& [key, value] : *object) {
        static_cast<void>(value);
        if (key.starts_with('!')) {
            return "the must-understand key `" + key + "` in " + std::string{where} + " is not understood";
        }
    }
    return std::nullopt;
}

/// @brief The must-understand keys of a message: its top level, every element of its listed arrays and every value
///        of its listed maps.
/// @param root   The parsed message.
/// @param what   What the message is.
/// @param arrays The keys of arrays whose elements are objects this layer defines.
/// @param maps   The keys of maps whose values are objects this layer defines.
/// @return Why the message is refused, or nothing.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the arrays' keys, then the maps', as a message lists them.
[[nodiscard]] inline std::optional<std::string> mustUnderstandIn(Json const& root, std::string_view what,
                                                                 std::initializer_list<std::string_view> arrays,
                                                                 std::initializer_list<std::string_view> maps) {
    if (auto reason = mustUnderstand(root, what)) {
        return reason;
    }
    auto const* const object = std::get_if<Json::object_t>(&root.data);
    if (object == nullptr) {
        return std::nullopt;
    }
    for (std::string_view const key : arrays) {
        auto const* const found = object->find(key);
        auto const* const items = found == object->end() ? nullptr : std::get_if<Json::array_t>(&found->second.data);
        if (items == nullptr) {
            continue;
        }
        for (Json const& item : *items) {
            if (auto reason = mustUnderstand(item, std::string{what} + " " + std::string{key})) {
                return reason;
            }
        }
    }
    for (std::string_view const key : maps) {
        auto const* const found = object->find(key);
        auto const* const items = found == object->end() ? nullptr : std::get_if<Json::object_t>(&found->second.data);
        if (items == nullptr) {
            continue;
        }
        for (auto const& [name, item] : *items) {
            if (auto reason = mustUnderstand(item, std::string{what} + " " + std::string{key} + " " + name)) {
                return reason;
            }
        }
    }
    return std::nullopt;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

/// @brief Reads a message: bounded depth, must-understand keys refused, unknown keys ignored.
/// @tparam T The message type.
/// @param text   The JSON text.
/// @param what   What the message is, for the reason.
/// @param arrays The keys of arrays whose elements are objects this layer defines.
/// @param maps   The keys of maps whose values are objects this layer defines.
/// @return The message, or why it was refused.
template <typename T>
[[nodiscard]] std::expected<T, std::string> readMessage(std::string_view text, std::string_view what,
                                                        std::initializer_list<std::string_view> arrays,
                                                        std::initializer_list<std::string_view> maps) {
    auto const tree = parseJson(text);
    if (!tree) {
        return std::unexpected(std::string{what} + ": " + tree.error());
    }
    if (auto reason = mustUnderstandIn(*tree, what, arrays, maps)) {
        return std::unexpected(std::move(*reason));
    }
    T value{};
    if (auto const error = glz::read<glz::opts{.error_on_unknown_keys = false}>(value, text)) {
        return std::unexpected(std::string{what} + ": " + glz::format_error(error, text));
    }
    return value;
}

/// @brief Writes a message as JSON, escaping control bytes as `wire::encode` does.
/// @tparam T The message type.
/// @param value The message.
/// @return The JSON text.
template <typename T>
[[nodiscard]] std::string writeMessage(T const& value) {
    std::string out;
    // A reflected struct of strings, numbers, maps and generic JSON has no write error to report.
    static_cast<void>(glz::write<::morph::wire::detail::EscapingWriteOpts{}>(value, out));
    return out;
}

/// @brief Checks a name's length.
/// @param name The name.
/// @param what What it names, for the reason.
/// @return Why it is refused, or nothing.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the name, then what it names, as the reason reads.
[[nodiscard]] inline std::optional<std::string> checkName(std::string_view name, std::string_view what) {
    if (name.size() > kMaxNameBytes) {
        return std::string{what} + " is longer than " + std::to_string(kMaxNameBytes) + " bytes";
    }
    return std::nullopt;
}

/// @brief Checks a list of names: its length and each name's.
/// @param names The names.
/// @param limit The most names allowed.
/// @param list  What the list is, for the reason.
/// @param item  What one name is, for the reason.
/// @return Why the list is refused, or nothing.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the list's description, then one item's, as the reasons read.
[[nodiscard]] inline std::optional<std::string> checkNames(std::vector<std::string> const& names, std::size_t limit,
                                                           std::string_view list, std::string_view item) {
    if (names.size() > limit) {
        return std::string{list} + " list more than " + std::to_string(limit) + " items";
    }
    for (std::string const& name : names) {
        if (auto reason = checkName(name, item)) {
            return reason;
        }
    }
    return std::nullopt;
}

/// @brief Checks a map of host names to versions: its size and each name's length.
/// @param names The map.
/// @param list  What the map holds, for the reason.
/// @param item  What one name is, for the reason.
/// @return Why the map is refused, or nothing.
[[nodiscard]] inline std::optional<std::string> checkHostNames(
    std::map<std::string, std::int64_t, std::less<>> const& names, std::string_view list, std::string_view item) {
    if (names.size() > kMaxListItems) {
        return std::string{list} + " list more than " + std::to_string(kMaxListItems) + " items";
    }
    for (auto const& [name, version] : names) {
        static_cast<void>(version);
        if (auto reason = checkName(name, item)) {
            return reason;
        }
    }
    return std::nullopt;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace detail

/// @brief The manifest's text: the JSON its digest is the hash of. Equal manifests write equal text.
/// @param manifest The manifest; its files should be sorted by kind then id.
/// @return The JSON text.
[[nodiscard]] inline std::string manifestText(Manifest const& manifest) { return detail::writeMessage(manifest); }

/// @brief Reads a manifest a server sent.
///
/// Refused: text that is not JSON or nests deeper than `kMaxJsonDepth`, a must-understand key at the top or in a
/// file, more than `kMaxBundleFiles` files, a name longer than `kMaxNameBytes`, a malformed hash, a size above
/// `kMaxFileBytes`, or two files of one kind with one id. Unknown keys are ignored.
/// @param text The manifest's text, already verified against its digest.
/// @return The manifest, or why it was refused.
[[nodiscard]] inline std::expected<Manifest, std::string> parseManifest(std::string_view text) {
    auto manifest = detail::readMessage<Manifest>(text, "the manifest", {"files"}, {});
    if (!manifest) {
        return std::unexpected(std::move(manifest.error()));
    }
    if (manifest->files.size() > kMaxBundleFiles) {
        return std::unexpected("the manifest lists more than " + std::to_string(kMaxBundleFiles) + " files");
    }
    if (auto reason = detail::checkName(manifest->application, "the application id")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkName(manifest->version, "the application version")) {
        return std::unexpected(std::move(*reason));
    }
    std::set<std::pair<std::string_view, std::string_view>> seen;
    for (ManifestFile const& file : manifest->files) {
        if (auto reason = detail::checkName(file.id, "a file id")) {
            return std::unexpected(std::move(*reason));
        }
        if (auto reason = detail::checkName(file.kind, "a file kind")) {
            return std::unexpected(std::move(*reason));
        }
        if (!isContentHash(file.hash)) {
            return std::unexpected("the manifest file `" + file.id + "` has a malformed hash");
        }
        if (file.size > kMaxFileBytes) {
            return std::unexpected("the manifest file `" + file.id + "` is larger than " +
                                   std::to_string(kMaxFileBytes) + " bytes");
        }
        if (!seen.emplace(file.kind, file.id).second) {
            return std::unexpected("the manifest lists the " + file.kind + " `" + file.id + "` twice");
        }
    }
    return manifest;
}

/// @brief Reads a `ui-hello` request body.
///
/// Refused: a body longer than `kMaxRequestBytes`, text that is not JSON or nests too deep, a must-understand key,
/// more than `kMaxListItems` vocabularies or host names of a kind, or a name longer than `kMaxNameBytes`.
/// @param text The request body; empty reads as a client that speaks nothing.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<HelloRequest, std::string> parseHelloRequest(std::string_view text) {
    if (text.size() > kMaxRequestBytes) {
        return std::unexpected("the ui-hello request is larger than " + std::to_string(kMaxRequestBytes) + " bytes");
    }
    auto request = detail::readMessage<HelloRequest>(text.empty() ? std::string_view("{}") : text,
                                                     "the ui-hello request", {}, {});
    if (!request) {
        return std::unexpected(std::move(request.error()));
    }
    if (auto reason = detail::checkNames(request->vocabularies, kMaxListItems, "the vocabularies", "a vocabulary")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkName(request->qt, "the Qt version")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkHostNames(request->hostComponents, "the host components", "a host component")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkHostNames(request->hostCommands, "the host commands", "a host command")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkHostNames(request->hostValues, "the host values", "a host value")) {
        return std::unexpected(std::move(*reason));
    }
    return request;
}

/// @brief Reads a `ui-hello` reply body.
///
/// Refused: text that is not JSON or nests too deep, a must-understand key, more than `kMaxListItems` vocabularies,
/// a name longer than `kMaxNameBytes`, or a digest that is not a content hash.
/// @param text The reply body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<HelloReply, std::string> parseHelloReply(std::string_view text) {
    auto reply = detail::readMessage<HelloReply>(text, "the ui-hello reply", {}, {});
    if (!reply) {
        return std::unexpected(std::move(reply.error()));
    }
    if (auto reason = detail::checkNames(reply->vocabularies, kMaxListItems, "the vocabularies", "a vocabulary")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkName(reply->application, "the application id")) {
        return std::unexpected(std::move(*reason));
    }
    if (auto reason = detail::checkName(reply->version, "the application version")) {
        return std::unexpected(std::move(*reason));
    }
    if (!isContentHash(reply->digest)) {
        return std::unexpected("the ui-hello reply's digest is not a content hash");
    }
    return reply;
}

/// @brief Reads a `ui-evaluate` reply body.
///
/// Refused: text that is not JSON or nests too deep, a must-understand key at the top, a body that is not an object,
/// or more than `kMaxCatalogFacts` field errors.
/// @param text The reply body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<EvaluateReply, std::string> parseEvaluateReply(std::string_view text) {
    auto reply = detail::readMessage<EvaluateReply>(text, "the ui-evaluate reply", {}, {});
    if (!reply) {
        return std::unexpected(std::move(reply.error()));
    }
    if (!reply->body.is_object()) {
        return std::unexpected("the ui-evaluate reply's body is not an object");
    }
    if (reply->errors.size() > kMaxCatalogFacts) {
        return std::unexpected("the ui-evaluate reply carries more than " + std::to_string(kMaxCatalogFacts) +
                               " field errors");
    }
    return reply;
}

/// @brief Reads a `ui-file` request body.
///
/// Refused: a body longer than `kMaxFileRequestBytes`, text that is not JSON or nests too deep, a must-understand
/// key, or an `op` that is not `begin`, `chunk`, `end` or `read`.
/// @param text The request body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<FileRequest, std::string> parseFileRequest(std::string_view text) {
    if (text.size() > kMaxFileRequestBytes) {
        return std::unexpected("the ui-file request is longer than " + std::to_string(kMaxFileRequestBytes) +
                               " bytes");
    }
    auto request = detail::readMessage<FileRequest>(text, "the ui-file request", {}, {});
    if (!request) {
        return std::unexpected(std::move(request.error()));
    }
    if (request->op != "begin" && request->op != "chunk" && request->op != "end" && request->op != "read") {
        return std::unexpected("the ui-file request's op is not begin, chunk, end or read");
    }
    return request;
}

/// @brief Reads a `ui-file` reply body.
/// @param text The reply body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<FileReply, std::string> parseFileReply(std::string_view text) {
    return detail::readMessage<FileReply>(text, "the ui-file reply", {}, {});
}

/// @brief Reads a `ui-catalog` reply body.
///
/// Refused: text that is not JSON or nests too deep, a must-understand key at the top, in an entry or in a model's
/// or action's facts, more than `kMaxBundleFiles` entries, more than `kMaxCatalogFacts` permitted actions, model
/// facts or action facts, or an entry id longer than `kMaxNameBytes`.
/// @param text The reply body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<CatalogReply, std::string> parseCatalogReply(std::string_view text) {
    auto reply = detail::readMessage<CatalogReply>(text, "the catalog", {"entries"},
                                                   {"models", "actions", "views", "wizards"});
    if (!reply) {
        return std::unexpected(std::move(reply.error()));
    }
    if (reply->entries.size() > kMaxBundleFiles) {
        return std::unexpected("the catalog lists more than " + std::to_string(kMaxBundleFiles) + " entries");
    }
    std::size_t const models = reply->models ? reply->models->size() : 0;
    std::size_t const actions = reply->actions ? reply->actions->size() : 0;
    std::size_t const views = reply->views ? reply->views->size() : 0;
    std::size_t const wizards = reply->wizards ? reply->wizards->size() : 0;
    if (std::ranges::max({reply->permitted.size(), models, actions, views, wizards}) > kMaxCatalogFacts) {
        return std::unexpected("the catalog carries more than " + std::to_string(kMaxCatalogFacts) +
                               " facts of a kind");
    }
    for (CatalogEntry const& entry : reply->entries) {
        if (auto reason = detail::checkName(entry.id, "a catalog entry id")) {
            return std::unexpected(std::move(*reason));
        }
    }
    return reply;
}

/// @brief Reads a `ui-fetch` request body.
///
/// Refused: a body longer than `kMaxRequestBytes`, text that is not JSON or nests too deep, a must-understand key,
/// or a hash that is not a content hash.
/// @param text The request body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<FetchRequest, std::string> parseFetchRequest(std::string_view text) {
    if (text.size() > kMaxRequestBytes) {
        return std::unexpected("the ui-fetch request is larger than " + std::to_string(kMaxRequestBytes) + " bytes");
    }
    auto request = detail::readMessage<FetchRequest>(text, "the ui-fetch request", {}, {});
    if (!request) {
        return std::unexpected(std::move(request.error()));
    }
    if (!isContentHash(request->hash)) {
        return std::unexpected(std::string{"the ui-fetch request's hash is not a content hash"});
    }
    return request;
}

}  // namespace morph::ui
