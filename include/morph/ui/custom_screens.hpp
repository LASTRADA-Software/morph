// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../core/completion.hpp"
#include "../core/coroutine.hpp"
#include "../core/executor.hpp"
#include "../core/model.hpp"
#include "../core/owner_strand.hpp"
#include "../core/registry.hpp"
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
    /// @brief The verified session of the call, for a store whose policy depends on more than the person: its
    ///        `principal` is @ref person, and its `token` the credential the server verified, whose claims (roles
    ///        among them) a store may read. The model copies it from the action's session; never from a request body.
    ::morph::session::Context session;
};

/// @brief Whose custom screens a `list` asks for.
struct CustomScreenScope {
    /// @brief The person, derived on the server from the verified principal; never taken from a request.
    std::string person;
    /// @brief The application.
    std::string app;
    /// @brief The verified session of the call, as `CustomScreenKey::session` carries it.
    ::morph::session::Context session;
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
    /// @brief What the store records about it, such as whether it is shared or someone's default: the store's own
    ///        record, which the model carries through `list`, `get` and `save` without reading it. Absent when the
    ///        store records nothing.
    std::optional<Json::object_t> attributes;
};

/// @brief A store's refusal, shown to the person as it is.
struct CustomScreenRefusal {
    /// @brief The reason, in one sentence.
    std::string reason;
};

/// @brief Stores custom screens. The application's implementation is where every policy over them lives.
///
/// `list` answers the screens a person may open, which may include screens others saved. `put` and `remove` refuse
/// what the person may not do, with a reason for the person. An id longer than `kMaxNameBytes` is shown to the person
/// cut, so `get` and `remove` could not be given it back: a store keeps its ids within `kMaxNameBytes` (the model's
/// own are 39 bytes). The model calls a store only with a person the server
/// derived and with a document that passed every check. Every call carries the verified session too
/// (`CustomScreenKey::session`, `CustomScreenScope::session`), so a policy may depend on more than the person, such as
/// a role the session's token grants. A store owns the executor its completions are delivered on.
///
/// An entry `list` answers whose id lacks `kCustomScreenIdPrefix` is listed broken, and `get` and `remove` refuse its
/// id before the store is called: the model cannot reach it, so cleaning it up is the store's.
///
/// The model's checks keep a person from saving a document that exceeds their permissions; they do not decide who
/// may overwrite whose screen. That is the store's: see `put`.
class ICustomScreenStore {
public:
    ICustomScreenStore() = default;
    ICustomScreenStore(ICustomScreenStore const&) = delete;
    ICustomScreenStore& operator=(ICustomScreenStore const&) = delete;
    ICustomScreenStore(ICustomScreenStore&&) = delete;
    ICustomScreenStore& operator=(ICustomScreenStore&&) = delete;
    virtual ~ICustomScreenStore() = default;

    /// @brief The custom screens a person may open in an application.
    /// @param scope The person, the application and the verified session.
    /// @return The screens, in the store's order.
    [[nodiscard]] virtual async::Completion<std::vector<CustomScreen>> list(CustomScreenScope scope) = 0;

    /// @brief One custom screen the person may open.
    /// @param key The entry.
    /// @return The screen, or the refusal.
    [[nodiscard]] virtual async::Completion<std::expected<CustomScreen, CustomScreenRefusal>> get(
        CustomScreenKey key) = 0;

    /// @brief Stores a custom screen, replacing one of the same key.
    ///
    /// The key's id may name a screen this person can open but not change (a screen another person shared with them):
    /// the store must refuse that write, since the model checks the document and not the right to replace what is
    /// stored. `screen.savedBy` is the person the model derived, the caller, not the screen's first author.
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
///        their own screens. It keys every entry by the person and reads nothing else of the session.
///
/// Its entries last as long as it does. Each call is a task on the strand, and its completion is delivered there. It
/// bounds neither the number of entries nor their total size, so it serves tests and examples, not a deployment.
class MemoryCustomScreenStore final : public ICustomScreenStore {
public:
    /// @brief A store whose strand runs on @p base.
    /// @param base Where the strand's tasks run. Borrowed: it must outlive the store.
    explicit MemoryCustomScreenStore(exec::IExecutor& base MORPH_LIFETIMEBOUND) : _strand{base} {}

    /// @brief The custom screens the person saved in the application; the session is not read.
    /// @param scope The person, the application and the verified session.
    /// @return The screens, ordered by id.
    [[nodiscard]] async::Completion<std::vector<CustomScreen>> list(CustomScreenScope scope) override {
        auto [completion, promise] = async::Completion<std::vector<CustomScreen>>::makeSettleable(&_strand);
        _strand.postTask(
            [this, who = std::move(scope.person), where = std::move(scope.app), settle = std::move(promise)] mutable {
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

namespace detail {

/// @brief What decides when the commands under a node's or part's command-list key run.
enum class EventClass : std::uint8_t {
    Press,          ///< A person's press, choice or edit runs them.
    ChosenFile,     ///< A press runs them, with the file the person just chose as `event`.
    FormOutcome,    ///< A form's own submit and its outcomes: a press when `submit` is `explicit`, else unattended.
    ComponentEvent  ///< A custom component's code decides when they run, so they are unattended.
};

/// @brief One command-list key of one node or part kind, and what runs its commands.
struct EventKey {
    /// @brief The node or part kind.
    std::string_view kind;
    /// @brief The key; for a record of command lists (`custom`'s `on`), the record's key.
    std::string_view key;
    /// @brief What runs the commands.
    EventClass eventClass;
};

/// @brief Every command-list key the document element table declares, each classified by what runs it. The base rule
///        reads each key's trigger from here; a key missing from it is treated as unattended, so a vocabulary
///        addition the table does not yet classify can only narrow what a custom screen may do.
/// @return The classified keys.
[[nodiscard]] MORPH_API std::span<EventKey const> eventKeys();

}  // namespace detail

/// @brief What the base rule lets a custom screen of @p base use, as plain text for a person or an assistant to read.
///
/// Built from the same walk as `checkBaseRule`, so it lists exactly what that check compares: the params; each action
/// with the kind of position it may be named in; each command the base runs without a press, with the trigger that
/// runs it; the client-side commands; the `host` names and each `host` command; and the delivered components. Text,
/// layout, styles and expressions are free and not listed.
/// @param base     The base screen, loaded.
/// @param baseText The base screen's JSON text.
/// @return The summary, one finding to a line; a refusal when @p baseText is not JSON.
/// @throws std::logic_error When @p base names a model alias it does not declare, which only a document that skipped
///         load-time validation can.
[[nodiscard]] MORPH_API std::expected<std::string, Refusal> describeAllowance(Document const& base,
                                                                              std::string_view baseText);

/// @brief The longest `broken.reason` of a summary, in bytes; a longer one is cut and ends with an ellipsis.
inline constexpr std::size_t kMaxBrokenReasonBytes = 1024;

/// @brief The longest `attributes` record, in bytes, as JSON. A `save` with a longer one is refused, and a stored
///        screen whose record is longer is listed broken without it.
inline constexpr std::size_t kMaxAttributesBytes = 1024;

/// @brief The most bytes one summary takes before escaping. Every field is capped when the summary is built: `id`,
///        `title`, `basedOn`, `savedBy` and `broken.pointer` at `kMaxNameBytes`, `broken.reason` at
///        `kMaxBrokenReasonBytes`, a cut one ending with an ellipsis, and `attributes` at `kMaxAttributesBytes`;
///        `hash` is 71 bytes (`sha256:` and 64 digits). That is 5 * 256 + 1024 + 1024 + 71 = 3399 bytes of text;
///        the keys, the punctuation and `savedAt` take under 300 more, and 4 KiB leaves a margin.
inline constexpr std::size_t kMaxSummaryBytes = std::size_t{4} * 1024;

/// @brief The most a byte of a summary grows to in an envelope: the reply is written with control bytes escaped, so
///        one becomes a six-byte escape sequence, and the envelope then doubles each backslash of the reply, making
///        seven.
inline constexpr std::size_t kMaxEscapedGrowth = 7;

/// @brief The bytes of a `get` reply that are not its document: one capped summary, at most
///        `kMaxEscapedGrowth * kMaxSummaryBytes` = 28 KiB once escaped, with a margin to 64 KiB.
inline constexpr std::size_t kCustomScreenReplyOverhead = std::size_t{64} * 1024;

/// @brief The custom screens' bounds.
struct CustomScreensOptions {
    /// @brief The longest custom document, in bytes. At most `kMaxFileBytes`, and small enough that a `get` reply
    ///        fits `wire::kMaxEnvelopeBytes`: the document is escaped once into the reply's JSON and the reply is
    ///        escaped again as the envelope's body, so a document of quotes and backslashes grows four-fold, and
    ///        `4 * maxDocumentBytes + kCustomScreenReplyOverhead` must not exceed the envelope bound.
    std::size_t maxDocumentBytes = std::size_t{256} * 1024;
    /// @brief The most verdicts `VerdictCache` keeps.
    std::size_t verdictCacheSize = 1024;
    /// @brief The most summaries `list` answers; it says `truncated` when the store held more. A capped summary is at
    ///        most `kMaxSummaryBytes` and grows at most `kMaxEscapedGrowth`-fold in an envelope, so
    ///        `maxListed * kMaxEscapedGrowth * kMaxSummaryBytes + kCustomScreenReplyOverhead` must not exceed
    ///        `wire::kMaxEnvelopeBytes`: at most 290. The default of 200 takes at most 5.8 MB.
    std::size_t maxListed = 200;
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
/// @param screenId  The id the document is stored under.
/// @param digest    The bundle's digest.
/// @param permitted The person's permitted actions, as `permittedActions` lists them.
/// @return The key.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, the document, its id, then the bundle's digest.
[[nodiscard]] MORPH_API std::string verdictKey(std::string_view basedOn, std::string_view document,
                                               std::string_view screenId, std::string_view digest,
                                               std::vector<std::string> const& permitted);

/// @brief Runs every check a custom document passes, in order, and answers the first refusal.
///
/// 1. The bounds: `maxDocumentBytes`, and nesting within `kMaxJsonDepth`. Neither needs the document parsed.
/// 2. The base: a screen document of @p bundle that the person may open (`screenAdmitted`).
/// 3. The application screen checks: `loadDocumentText` with the bundle's catalog, and its `id` is @p screenId, which
///    starts with `kCustomScreenIdPrefix`, so it is a screen and never the app document.
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
/// @param screenId   The id the document is stored under: its `id` member must be this.
/// @param options    The bounds.
/// @param cache      The verdict cache; null checks every time.
/// @return The first refusal, with its JSON Pointer, or nothing when the document passes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, the document, then its id, at every call.
[[nodiscard]] MORPH_API std::optional<Refusal> checkCustomScreen(
    ServedBundle const& bundle, ::morph::session::Context const& session,
    ::morph::session::IAuthorizer const& authorizer, std::string_view basedOn, std::string_view document,
    std::string_view screenId, CustomScreensOptions const& options, VerdictCache* cache = nullptr);

/// @brief `checkCustomScreen` with the person's permitted actions already computed, so a list of many screens
///        computes them once.
/// @param bundle     The served bundle.
/// @param session    The session, its principal verified.
/// @param authorizer The authoriser.
/// @param permitted  `permittedActions(bundle, session, authorizer)`, computed in this call or one that serves the
///                   same request, never kept across calls: permissions change between them.
/// @param basedOn    The base screen's id.
/// @param document   The custom document's text.
/// @param screenId   The id the document is stored under: its `id` member must be this.
/// @param options    The bounds.
/// @param cache      The verdict cache; null checks every time.
/// @return The first refusal, with its JSON Pointer, or nothing when the document passes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the base, the document, then its id, at every call.
[[nodiscard]] MORPH_API std::optional<Refusal> checkCustomScreen(
    ServedBundle const& bundle, ::morph::session::Context const& session,
    ::morph::session::IAuthorizer const& authorizer, std::vector<std::string> const& permitted,
    std::string_view basedOn, std::string_view document, std::string_view screenId,
    CustomScreensOptions const& options, VerdictCache* cache = nullptr);

/// @brief @p document with its screen id replaced by @p screenId, the id a custom screen is stored and served under.
/// @param document The custom document's JSON text.
/// @param screenId The custom screen's id.
/// @return The rewritten JSON text, or the refusal of text that is not a JSON object.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the document, then its new id.
[[nodiscard]] MORPH_API std::expected<std::string, Refusal> withScreenId(std::string_view document,
                                                                         std::string_view screenId);

/// @brief The type id the custom screens model is registered under.
inline constexpr std::string_view kCustomScreensModel = "morph.customScreens";
/// @brief The record type a custom screen's summary is, which `save` and `remove` change.
inline constexpr std::string_view kCustomScreenRecord = "morph.customScreens";
/// @brief The `list` action's id.
inline constexpr std::string_view kCustomScreensList = "morph.customScreens.list";
/// @brief The `get` action's id.
inline constexpr std::string_view kCustomScreensGet = "morph.customScreens.get";
/// @brief The `check` action's id.
inline constexpr std::string_view kCustomScreensCheck = "morph.customScreens.check";
/// @brief The `save` action's id.
inline constexpr std::string_view kCustomScreensSave = "morph.customScreens.save";
/// @brief The `remove` action's id.
inline constexpr std::string_view kCustomScreensRemove = "morph.customScreens.remove";

/// @brief A check's refusal on the wire.
struct CheckRefusal {
    /// @brief The JSON Pointer of the refused part; empty for the whole document.
    std::string pointer;
    /// @brief The reason, in one sentence.
    std::string reason;
};

/// @brief One custom screen as a list shows it: the record `save` and `remove` change.
struct CustomScreenSummary {
    /// @brief The id.
    std::string id;
    /// @brief The person's name for it.
    std::string title;
    /// @brief The application screen it was made from.
    std::string basedOn;
    /// @brief When it was saved.
    time::DateTime savedAt;
    /// @brief Who saved it.
    std::string savedBy;
    /// @brief Whether the caller saved it.
    bool mine = false;
    /// @brief The content hash of the stored document, which changes whenever the document does: a client holding a
    ///        copy of the document compares it to tell whether its copy is current.
    std::string hash;
    /// @brief Whether it opens from the navigator: it is not broken, and its base declares no params (a custom
    ///        screen declares exactly its base's).
    bool opensWithoutParams = false;
    /// @brief The refusal the checks give it today, or that its stored `attributes` are longer than
    ///        `kMaxAttributesBytes`; absent when it may be mounted.
    std::optional<CheckRefusal> broken;
    /// @brief What the store records about it, as the store answered it (`CustomScreen::attributes`); absent when the
    ///        store records nothing, or its record is too long to send.
    std::optional<Json::object_t> attributes;
};

/// @brief `list`: the caller's custom screens of an application.
struct CustomScreensList {
    /// @brief The application.
    std::string app;
};

/// @brief What `list` answers.
struct CustomScreensListed {
    /// @brief The screens, in the store's order, at most `CustomScreensOptions::maxListed`.
    std::vector<CustomScreenSummary> screens;
    /// @brief Whether the store held more screens than are listed.
    bool truncated = false;
};

/// @brief `get`: one custom screen.
struct CustomScreensGet {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id.
    std::string id;
};

/// @brief What `get` answers.
struct CustomScreenFound {
    /// @brief The screen as a list shows it.
    CustomScreenSummary summary;
    /// @brief Its document's JSON text.
    std::string document;
};

/// @brief `check`: whether a document would be saved.
struct CustomScreensCheck {
    /// @brief The application.
    std::string app;
    /// @brief The base screen's id.
    std::string basedOn;
    /// @brief The document's JSON text.
    std::string document;
};

/// @brief What `check` answers.
struct CustomScreenChecked {
    /// @brief The first refusal; absent when the document would be saved.
    std::optional<CheckRefusal> refusal;
};

/// @brief `save`: stores a custom screen.
struct CustomScreensSave {
    /// @brief The application.
    std::string app;
    /// @brief The screen to overwrite, one `get` answers for the caller; absent saves a new screen.
    std::optional<std::string> id;
    /// @brief The person's name for it.
    std::string title;
    /// @brief The base screen's id.
    std::string basedOn;
    /// @brief The document's JSON text.
    std::string document;
    /// @brief What the store is asked to record about the screen, at most `kMaxAttributesBytes` as JSON. The store
    ///        validates it and may refuse; absent asks for nothing.
    std::optional<Json::object_t> attributes;
};

/// @brief What `save` answers.
struct CustomScreenSaved {
    /// @brief The screen's id.
    std::string id;
};

/// @brief `remove`: forgets a custom screen.
struct CustomScreensRemove {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id.
    std::string id;
};

/// @brief What `remove` answers.
struct CustomScreenRemoved {
    /// @brief Whether it was removed.
    bool removed = false;
};

/// @brief A server's custom screens: the store, the bundle being served, the authoriser, the bounds and the verdict
///        cache that the model and anything beside it (an assistant that proposes screens) share.
///
/// `registerCustomScreens` makes it and returns it. Every member is safe to call from any thread: the verdict cache
/// is locked, the store is the application's, and the bundle is read through the getter given at registration.
class MORPH_API CustomScreens {
public:
    /// @brief The custom screens of one application.
    /// @param store      Where custom screens are kept.
    /// @param bundle     The bundle being served now. Called at every use, so a development reload is seen, and from
    ///                   action strands on any thread, so it must be thread-safe.
    /// @param authorizer The server's authoriser, for the permitted set and the base screen's admission.
    /// @param options    The bounds.
    CustomScreens(std::shared_ptr<ICustomScreenStore> store,
                  std::function<std::shared_ptr<ServedBundle const>()> bundle,
                  std::shared_ptr<::morph::session::IAuthorizer const> authorizer, CustomScreensOptions options);

    /// @brief Whether @p document would be saved, by the checks of `checkCustomScreen` and the verdict cache that the
    ///        `check` action uses.
    /// @param session  The session of the person asking, its principal verified.
    /// @param app      The application.
    /// @param basedOn  The base screen's id.
    /// @param document The custom document's JSON text, whatever its `id` is: it is checked as saved under a new id.
    /// @return The first refusal, or nothing when the document would be saved. A refusal too for a session with no
    ///         verified principal, an application this server does not serve, and a name outside its bounds.
    [[nodiscard]] ::core::async::Task<std::optional<Refusal>> check(::morph::session::Context const& session,
                                                                    std::string app, std::string basedOn,
                                                                    std::string document) const;

    /// @brief One of the person's custom screens, as the `get` action answers it.
    /// @param session The session of the person asking, its principal verified.
    /// @param app     The application.
    /// @param screenId The screen's id, which starts with `kCustomScreenIdPrefix`.
    /// @return The screen's summary and document, or the refusal: the store's, a session with no verified
    ///         principal, an application this server does not serve, an id outside its bounds, or a stored document
    ///         longer than `CustomScreensOptions::maxDocumentBytes`.
    [[nodiscard]] ::core::async::Task<std::expected<CustomScreenFound, CustomScreenRefusal>> get(
        ::morph::session::Context const& session, std::string app, std::string screenId) const;

    /// @brief What the base rule lets a custom screen based on @p basedOn use, as `describeAllowance` writes it, for a
    ///        person who may open that base. Synchronous.
    /// @param session The session of the person asking, its principal verified.
    /// @param app     The application.
    /// @param basedOn The base screen's id.
    /// @return The summary, or the refusal: a session with no verified principal, an application this server does not
    ///         serve, a @p basedOn outside its bounds or naming no screen document of the bundle being served, a base
    ///         this person may not open (`screenAdmitted`, as `check` refuses it), or a document that does not load.
    [[nodiscard]] std::expected<std::string, Refusal> allowance(::morph::session::Context const& session,
                                                                std::string const& app,
                                                                std::string_view basedOn) const;

private:
    friend class CustomScreensModel;

    /// @brief The bundle being served now, once @p app is the application it serves.
    /// @param app The application a request names.
    /// @return The bundle, or why custom screens are not served for @p app.
    [[nodiscard]] std::expected<std::shared_ptr<ServedBundle const>, std::string> servedFor(
        std::string const& app) const;

    /// @brief @p document with its `id` rewritten to @p screenId, after the checks of `checkCustomScreen` passed it as
    ///        so rewritten: the text `save` stores.
    /// @param bundle   The bundle being served.
    /// @param session  The session of the person.
    /// @param basedOn  The base screen's id.
    /// @param document The custom document's text.
    /// @param screenId The id it is stored under.
    /// @return The rewritten text, or the first refusal.
    [[nodiscard]] std::expected<std::string, Refusal> rewrittenAndChecked(ServedBundle const& bundle,
                                                                          ::morph::session::Context const& session,
                                                                          std::string_view basedOn,
                                                                          std::string_view document,
                                                                          std::string_view screenId) const;

    /// @brief What `check` answers once the bundle is known: `rewrittenAndChecked` under a placeholder id as long as
    ///        a real one, so the verdict is the one `save` would give.
    [[nodiscard]] std::optional<Refusal> checkedAsNew(ServedBundle const& bundle,
                                                      ::morph::session::Context const& session,
                                                      std::string_view basedOn, std::string_view document) const;

    /// @brief The task `check` returns; its arguments are owned by the frame.
    [[nodiscard]] ::core::async::Task<std::optional<Refusal>> checkTask(::morph::session::Context session,
                                                                        std::string app, std::string basedOn,
                                                                        std::string document) const;

    /// @brief The task `get` returns; its arguments are owned by the frame.
    [[nodiscard]] ::core::async::Task<std::expected<CustomScreenFound, CustomScreenRefusal>> getTask(
        ::morph::session::Context session, std::string app, std::string screenId) const;

    /// @brief What `opensWithoutParams` memoises, by base screen id.
    using OpensMemo = std::map<std::string, bool, std::less<>>;

    /// @brief One screen as a list shows it, checked today against @p permitted, which the caller computed once.
    [[nodiscard]] CustomScreenSummary summarize(ServedBundle const& bundle, ::morph::session::Context const& session,
                                                std::vector<std::string> const& permitted, CustomScreen const& screen,
                                                OpensMemo& opens) const;

    std::shared_ptr<ICustomScreenStore> _store;
    std::function<std::shared_ptr<ServedBundle const>()> _bundle;
    std::shared_ptr<::morph::session::IAuthorizer const> _authorizer;
    CustomScreensOptions _options;
    mutable VerdictCache _verdicts;
};

/// @brief The server's custom screens model: the caller's screens, through the application's store, checked.
///
/// The person is the principal of the verified session, read once at the start of each action, before anything is
/// awaited. No request names a person, and one that tries is ignored. With no verified principal, `list` answers
/// nothing and every other action is refused.
class MORPH_API CustomScreensModel {
public:
    /// @brief A model over @p context.
    /// @param context The store, the bundle, the authoriser, the bounds and the verdict cache. Shared by every
    ///                instance; never null.
    explicit CustomScreensModel(std::shared_ptr<CustomScreens> context);

    /// @brief The caller's custom screens, each checked today.
    /// @param request The application.
    /// @return The screens.
    ::core::async::Task<CustomScreensListed> execute(CustomScreensList request);

    /// @brief One of the caller's custom screens.
    /// @param request The application and the id.
    /// @return The screen and its document.
    ::core::async::Task<CustomScreenFound> execute(CustomScreensGet request);

    /// @brief Whether a document would be saved.
    /// @param request The application, the base and the document.
    /// @return The first refusal, or none.
    ::core::async::Task<CustomScreenChecked> execute(CustomScreensCheck request);

    /// @brief Stores a custom screen.
    /// @param request The application, the id to overwrite if any, the title, the base and the document.
    /// @return The screen's id.
    ::core::async::Task<CustomScreenSaved> execute(CustomScreensSave request);

    /// @brief Forgets a custom screen.
    /// @param request The application and the id.
    /// @return That it was removed.
    ::core::async::Task<CustomScreenRemoved> execute(CustomScreensRemove request);

private:
    std::shared_ptr<CustomScreens> _context;
};

}  // namespace morph::ui

/// @cond detail
// The traits the registration macros write, without their static-init registrars: registerCustomScreens registers
// the model and its actions; the record, the result slots and the effects are registered in custom_screens.cpp.
// NOLINTBEGIN(misc-const-correctness): inside the macros' codec bodies.
BRIDGE_DETAIL_MODEL_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, "morph.customScreens")
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensList,
                                      morph::ui::CustomScreensListed, "morph.customScreens.list",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensGet,
                                      morph::ui::CustomScreenFound, "morph.customScreens.get",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensCheck,
                                      morph::ui::CustomScreenChecked, "morph.customScreens.check",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensSave,
                                      morph::ui::CustomScreenSaved, "morph.customScreens.save",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::CustomScreensModel, morph::ui::CustomScreensRemove,
                                      morph::ui::CustomScreenRemoved, "morph.customScreens.remove",
                                      ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
/// @endcond

namespace morph::ui {

/// @brief Registers the custom screens model and its five actions, with a factory that gives every instance one
///        shared context.
///
/// A server that offers custom screens calls this once, before it serves, with the dispatcher and registry its
/// `RemoteServer` uses. Its authoriser then admits `morph.customScreens` and its actions for every signed-in
/// principal; the model keeps each person to what the store answers them. A client learns that a server offers
/// custom screens by calling `list`: a server that did not call this answers the dispatcher's unknown-model error.
/// @param store      Where custom screens are kept, and the application's policy over them.
/// @param bundle     The bundle being served now; called at every action, so a development reload is seen, from
///                   action strands on any thread, so it must be thread-safe.
/// @param authorizer The server's authoriser, for the permitted set and the base screen's admission.
/// @param dispatcher The server's action dispatcher.
/// @param registry   The server's model registry.
/// @param options    The bounds. `maxDocumentBytes` is at most `kMaxFileBytes`.
/// @return The handle the model uses, for whatever else serves custom screens on this server.
MORPH_API std::shared_ptr<CustomScreens> registerCustomScreens(
    std::shared_ptr<ICustomScreenStore> store, std::function<std::shared_ptr<ServedBundle const>()> bundle,
    std::shared_ptr<::morph::session::IAuthorizer const> authorizer, model::detail::ActionDispatcher& dispatcher,
    model::detail::ModelRegistryFactory& registry, CustomScreensOptions options = {});

}  // namespace morph::ui
