// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <morph/export.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../core/completion.hpp"
#include "../core/executor.hpp"
#include "../core/owner_strand.hpp"
#include "../session/session.hpp"
#include "../util/datetime.hpp"
#include "document.hpp"
#include "service.hpp"
#include "value.hpp"

/// @file
/// @brief A person's own screens: the store seam, the checks a custom document passes, and the server model that
///        serves them under the verified person.
///
/// Specified in `docs/spec/ui/custom_screens.md`.

namespace morph::ui {

/// @brief Where one custom screen is stored.
struct CustomScreenKey {
    /// @brief The person, derived on the server from the verified principal; never taken from a request.
    std::string person;
    /// @brief The application.
    std::string app;
    /// @brief The screen's id, `custom:` and 32 hexadecimal digits.
    std::string id;
};

/// @brief A stored custom screen.
struct CustomScreen {
    /// @brief The id, `custom:` and 32 hexadecimal digits, issued by the model.
    std::string id;
    /// @brief The person's name for it, which names its entry.
    std::string title;
    /// @brief The id of the application screen it was made from.
    std::string basedOn;
    /// @brief The `ui/1` JSON text, its screen id already the custom screen's.
    std::string document;
    /// @brief When it was saved.
    time::DateTime savedAt;
    /// @brief The principal that saved it.
    std::string savedBy;
};

/// @brief A store's refusal, shown to the person as it is.
struct CustomScreenRefusal {
    /// @brief The reason, in one sentence.
    std::string reason;
};

/// @brief Stores custom screens. The application's implementation is where every policy over them lives.
///
/// `list` answers the screens a person may open, which may include screens others saved. `put` and `remove` refuse
/// what the person may not do, with a reason for the person. The model calls a store only with a person the server
/// derived and with a document that passed every check. A store owns the executor its completions are delivered on.
class ICustomScreenStore {
public:
    ICustomScreenStore() = default;
    ICustomScreenStore(ICustomScreenStore const&) = delete;
    ICustomScreenStore& operator=(ICustomScreenStore const&) = delete;
    ICustomScreenStore(ICustomScreenStore&&) = delete;
    ICustomScreenStore& operator=(ICustomScreenStore&&) = delete;
    virtual ~ICustomScreenStore() = default;

    /// @brief The custom screens @p person may open in @p app.
    /// @param person The person.
    /// @param app    The application.
    /// @return The screens, in the store's order.
    [[nodiscard]] virtual async::Completion<std::vector<CustomScreen>> list(std::string person, std::string app) = 0;

    /// @brief One custom screen the person may open.
    /// @param key The entry.
    /// @return The screen, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<CustomScreen, CustomScreenRefusal>> get(
        CustomScreenKey key) = 0;

    /// @brief Stores a custom screen, replacing one of the same key.
    /// @param key    The entry.
    /// @param screen The screen.
    /// @return Nothing, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<void, CustomScreenRefusal>> put(CustomScreenKey key,
                                                                                          CustomScreen screen) = 0;

    /// @brief Forgets a custom screen.
    /// @param key The entry.
    /// @return Nothing, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<void, CustomScreenRefusal>> remove(CustomScreenKey key) = 0;
};

/// @brief The framework's store: a map held in memory, on its own strand, each person seeing and changing only
///        their own screens.
///
/// Its entries last as long as it does. Each call is a task on the strand, and its completion is delivered there.
class MemoryCustomScreenStore final : public ICustomScreenStore {
public:
    /// @brief A store whose strand runs on @p base.
    /// @param base Where the strand's tasks run. Borrowed: it must outlive the store.
    explicit MemoryCustomScreenStore(exec::IExecutor& base MORPH_LIFETIMEBOUND) : _strand{base} {}

    /// @brief The custom screens @p person saved in @p app.
    /// @param person The person.
    /// @param app    The application.
    /// @return The screens, ordered by id.
    [[nodiscard]] async::Completion<std::vector<CustomScreen>> list(std::string person, std::string app) override {
        auto [completion, promise] = async::Completion<std::vector<CustomScreen>>::makeSettleable(&_strand);
        _strand.postTask([this, who = std::move(person), where = std::move(app), settle = std::move(promise)] mutable {
            std::vector<CustomScreen> out;
            for (auto const& [key, screen] : _entries) {
                if (std::get<0>(key) == who && std::get<1>(key) == where) {
                    out.push_back(screen);
                }
            }
            settle.resolve(std::move(out));
        });
        return std::move(completion);
    }

    /// @brief One of the person's own screens.
    /// @param key The entry.
    /// @return The screen, or `no such custom screen`.
    [[nodiscard]] async::Completion<std::expected<CustomScreen, CustomScreenRefusal>> get(
        CustomScreenKey key) override {
        auto [completion, promise] =
            async::Completion<std::expected<CustomScreen, CustomScreenRefusal>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            auto const found = _entries.find(entry);
            if (found == _entries.end()) {
                settle.resolve(std::unexpected(CustomScreenRefusal{.reason = "no such custom screen"}));
                return;
            }
            settle.resolve(found->second);
        });
        return std::move(completion);
    }

    /// @brief Stores a screen under the person's key.
    /// @param key    The entry.
    /// @param screen The screen.
    /// @return Nothing: the memory store refuses no write.
    [[nodiscard]] async::Completion<std::expected<void, CustomScreenRefusal>> put(CustomScreenKey key,
                                                                                  CustomScreen screen) override {
        auto [completion, promise] =
            async::Completion<std::expected<void, CustomScreenRefusal>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), kept = std::move(screen), settle = std::move(promise)] mutable {
            _entries.insert_or_assign(std::move(entry), std::move(kept));
            settle.resolve(std::expected<void, CustomScreenRefusal>{});
        });
        return std::move(completion);
    }

    /// @brief Forgets one of the person's own screens.
    /// @param key The entry.
    /// @return Nothing, or `no such custom screen`.
    [[nodiscard]] async::Completion<std::expected<void, CustomScreenRefusal>> remove(CustomScreenKey key) override {
        auto [completion, promise] =
            async::Completion<std::expected<void, CustomScreenRefusal>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            if (_entries.erase(entry) == 0) {
                settle.resolve(std::unexpected(CustomScreenRefusal{.reason = "no such custom screen"}));
                return;
            }
            settle.resolve(std::expected<void, CustomScreenRefusal>{});
        });
        return std::move(completion);
    }

private:
    using Key = std::tuple<std::string, std::string, std::string>;

    [[nodiscard]] static Key tie(CustomScreenKey const& key) { return Key{key.person, key.app, key.id}; }

    std::map<Key, CustomScreen> _entries;
    /// After the map, so it is destroyed first: a task running on it reads the map.
    exec::OwnerStrand _strand;
};

/// @brief Checks that @p custom uses nothing its base screen does not: the base rule.
///
/// Compared: the actions it names, wherever declared and component fallbacks included, each on its model type and in
/// a position of the same kind (a query, a mutation, a model-backed table, a collection, a wizard, or a form by its
/// literal `submit`: none, `explicit` or `auto`); the commands run without a press (from a lifecycle list, a watch,
/// a component's `on` event, a form whose `submit` is not `explicit`, or the `onSuccess` and `onError` of a mutation
/// that a `run`, an `upload` or a `signIn` among them reaches, each name resolved in the scope that declares the
/// mutation), by trigger. A form with no `submit` keeps its action schema's mode, which this check cannot see and
/// which is automatic unless the schema says otherwise, so it counts as submitting itself.
///
/// Also compared: the client-side commands `upload`, `download`, `switchBackend`, `signIn`, `signOut` and
/// `resetPrefs`; every `host` name, whether a host command, a host value or a host component; each `host` command
/// as a whole, name and arguments, which must be identical to one of the base's; the delivered components, by name
/// and content hash; and the params, which must be the base's exactly. Every `upload` of the custom document must
/// send exactly `{"ref": "event"}` and sit directly in the `onPick` of a `filePicker` that opens files (no `mode`, or
/// `open`) or in a `dropZone`'s `onDrop`, so it is always a file the person just chose. A host command's arguments
/// must be literals. Text, layout, styles, states and expressions are not compared.
/// @param base       The base screen, loaded.
/// @param baseText   The base screen's JSON text.
/// @param custom     The custom document, loaded.
/// @param customText The text @p custom was loaded from, which the `host` names are read from.
/// @return The first use the base does not have, with its JSON Pointer, or nothing.
/// @throws std::logic_error When a document names a model alias it does not declare, which only a document that
///         skipped load-time validation can.
[[nodiscard]] MORPH_API std::optional<Refusal> checkBaseRule(Document const& base, std::string_view baseText,
                                                             Document const& custom, std::string_view customText);

/// @brief The custom screens' bounds.
struct CustomScreensOptions {
    /// @brief The longest custom document, in bytes. At most `kMaxFileBytes`, so every stored screen can be served.
    std::size_t maxDocumentBytes = std::size_t{256} * 1024;
    /// @brief The most verdicts `VerdictCache` keeps.
    std::size_t verdictCacheSize = 1024;
};

/// @brief The checks' verdicts by `verdictKey`, bounded, dropping the least recently used.
///
/// A list of custom screens runs every stored document through the checks, and every save refetches every list, so
/// an unchanged document under unchanged permissions is answered from here. Safe to share between threads.
class MORPH_API VerdictCache {
public:
    /// @brief An empty cache.
    /// @param capacity The most verdicts kept; at least one is kept whatever it says.
    explicit VerdictCache(std::size_t capacity) : _capacity{capacity == 0 ? 1 : capacity} {}

    VerdictCache(VerdictCache const&) = delete;
    VerdictCache& operator=(VerdictCache const&) = delete;
    VerdictCache(VerdictCache&&) = delete;
    VerdictCache& operator=(VerdictCache&&) = delete;
    ~VerdictCache() = default;

    /// @brief The verdict kept under @p key, which becomes the most recently used.
    /// @param key The key.
    /// @return Nothing when no verdict is kept; otherwise the verdict, itself nothing for a document that passed.
    [[nodiscard]] std::optional<std::optional<Refusal>> find(std::string const& key);

    /// @brief Keeps @p verdict under @p key, dropping the least recently used verdict past the capacity.
    /// @param key     The key.
    /// @param verdict The refusal, or nothing for a document that passed.
    void store(std::string key, std::optional<Refusal> verdict);

    /// @brief How many verdicts are kept.
    /// @return The count.
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        std::optional<Refusal> verdict;
        std::list<std::string>::iterator order;
    };

    std::size_t _capacity;
    mutable std::mutex _mutex;
    /// The keys, most recently used first.
    std::list<std::string> _order;
    std::map<std::string, Entry, std::less<>> _entries;
};

/// @brief The key of a verdict: what the checks of a document read, apart from the document's own bounds.
/// @param basedOn   The base screen's id.
/// @param document  The custom document's text.
/// @param digest    The bundle's digest.
/// @param permitted The person's permitted actions, as `permittedActions` lists them.
/// @return The key.
[[nodiscard]] MORPH_API std::string verdictKey(std::string_view basedOn, std::string_view document,
                                               std::string_view digest, std::vector<std::string> const& permitted);

/// @brief Runs every check a custom document passes, in order, and answers the first refusal.
///
/// 1. The bounds: `maxDocumentBytes`, and nesting within `kMaxJsonDepth`.
/// 2. The base: a screen document of @p bundle that the person may open (`screenAdmitted`).
/// 3. The application screen checks: `loadDocumentText` with the bundle's catalog, and a screen, not the app
///    document.
/// 4. The accessibility rules (`author::checkAccessibility`).
/// 5. The base rule (`checkBaseRule`).
/// 6. Permission: every action the document names is in `permittedActions`.
///
/// Check 2 reads the bundle and the authoriser on every call: the bundle's digest does not cover whether a screen
/// entry is available, so a cached verdict cannot vouch for the base. Checks 3 to 6 are answered from @p cache when
/// it holds the verdict, and stored in it otherwise.
/// @param bundle     The served bundle.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @param basedOn    The base screen's id.
/// @param document   The custom document's text.
/// @param options    The bounds.
/// @param cache      The verdict cache; null checks every time.
/// @return The first refusal, with its JSON Pointer, or nothing when the document passes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the document, at every call.
[[nodiscard]] MORPH_API std::optional<Refusal> checkCustomScreen(ServedBundle const& bundle,
                                                                 ::morph::session::Context const& session,
                                                                 ::morph::session::IAuthorizer const& authorizer,
                                                                 std::string_view basedOn, std::string_view document,
                                                                 CustomScreensOptions const& options,
                                                                 VerdictCache* cache = nullptr);

/// @brief `checkCustomScreen` with the person's permitted actions already computed, so a list of many screens
///        computes them once.
/// @param bundle     The served bundle.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @param permitted  `permittedActions(bundle, session, authorizer)`.
/// @param basedOn    The base screen's id.
/// @param document   The custom document's text.
/// @param options    The bounds.
/// @param cache      The verdict cache; null checks every time.
/// @return The first refusal, with its JSON Pointer, or nothing when the document passes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, then the document, at every call.
[[nodiscard]] MORPH_API std::optional<Refusal> checkCustomScreen(ServedBundle const& bundle,
                                                                 ::morph::session::Context const& session,
                                                                 ::morph::session::IAuthorizer const& authorizer,
                                                                 std::vector<std::string> const& permitted,
                                                                 std::string_view basedOn, std::string_view document,
                                                                 CustomScreensOptions const& options,
                                                                 VerdictCache* cache = nullptr);

/// @brief @p document with its screen id replaced by @p id, the id a custom screen is stored and served under.
/// @param document The custom document's JSON text.
/// @param id       The custom screen's id.
/// @return The rewritten JSON text, or the refusal of text that is not a JSON object.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the document, then its new id.
[[nodiscard]] MORPH_API std::expected<std::string, Refusal> withScreenId(std::string_view document,
                                                                         std::string_view id);

}  // namespace morph::ui
