// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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
#include "value.hpp"

/// @file
/// @brief Per-person storage of customization choices (spec 5 §12b, spec 6 §8): the `IPreferencesStore` seam, the
///        framework's in-memory store, and the server's preferences model, which keys every entry by the person the
///        verified session names.
///
/// Specified in `docs/spec/ui/preferences.md`.

namespace morph::ui {

/// @brief Where one choice is stored.
struct PrefKey {
    /// @brief The person, derived on the server from the verified principal; never taken from a request.
    std::string person;
    /// @brief The application.
    std::string app;
    /// @brief The screen's id, or `app` for the app document's points.
    std::string scope;
    /// @brief The customization point.
    std::string point;
};

/// @brief Stores customization choices as JSON text, by `PrefKey`.
///
/// Every call is asynchronous, so a store may sit on a database or a remote service. A store owns the executor its
/// completions are delivered on. The preferences model calls it only with keys whose person the server derived and
/// with values it has already bounded and parsed, so a store applies no bound of its own.
class IPreferencesStore {
public:
    IPreferencesStore() = default;
    IPreferencesStore(IPreferencesStore const&) = delete;
    IPreferencesStore& operator=(IPreferencesStore const&) = delete;
    IPreferencesStore(IPreferencesStore&&) = delete;
    IPreferencesStore& operator=(IPreferencesStore&&) = delete;
    virtual ~IPreferencesStore() = default;

    /// @brief Reads a choice.
    /// @param key The entry.
    /// @return Its JSON text, or `std::nullopt` when nothing is stored.
    [[nodiscard]] virtual async::Completion<std::optional<std::string>> get(PrefKey const& key) = 0;

    /// @brief Writes a choice, replacing what was stored.
    /// @param key  The entry.
    /// @param json Its JSON text.
    /// @return Whether it was stored; `false` is a refusal the caller reports.
    [[nodiscard]] virtual async::Completion<bool> set(PrefKey const& key, std::string json) = 0;

    /// @brief Forgets a choice, so the point is at the document's default again.
    /// @param key The entry.
    /// @return Whether an entry was there.
    [[nodiscard]] virtual async::Completion<bool> remove(PrefKey const& key) = 0;
};

/// @brief The framework's store: a map held in memory, on its own strand.
///
/// Its entries last as long as it does. Each call is a task on the strand, and its completion is delivered there.
class MemoryPreferencesStore final : public IPreferencesStore {
public:
    /// @brief A store whose strand runs on @p base.
    /// @param base Where the strand's tasks run. Borrowed: it must outlive the store.
    explicit MemoryPreferencesStore(exec::IExecutor& base MORPH_LIFETIMEBOUND) : _strand{base} {}

    /// @brief Reads a choice.
    /// @param key The entry.
    /// @return Its JSON text, or `std::nullopt` when nothing is stored.
    [[nodiscard]] async::Completion<std::optional<std::string>> get(PrefKey const& key) override {
        auto [completion, promise] = async::Completion<std::optional<std::string>>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            auto const found = _entries.find(entry);
            settle.resolve(found == _entries.end() ? std::nullopt : std::optional<std::string>{found->second});
        });
        return std::move(completion);
    }

    /// @brief Writes a choice.
    /// @param key  The entry.
    /// @param json Its JSON text.
    /// @return `true`: the memory store refuses nothing.
    [[nodiscard]] async::Completion<bool> set(PrefKey const& key, std::string json) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), text = std::move(json), settle = std::move(promise)] mutable {
            _entries.insert_or_assign(std::move(entry), std::move(text));
            settle.resolve(true);
        });
        return std::move(completion);
    }

    /// @brief Forgets a choice.
    /// @param key The entry.
    /// @return Whether an entry was there.
    [[nodiscard]] async::Completion<bool> remove(PrefKey const& key) override {
        auto [completion, promise] = async::Completion<bool>::makeSettleable(&_strand);
        _strand.postTask([this, entry = tie(key), settle = std::move(promise)] mutable {
            settle.resolve(_entries.erase(entry) != 0);
        });
        return std::move(completion);
    }

private:
    using Key = std::tuple<std::string, std::string, std::string, std::string>;

    [[nodiscard]] static Key tie(PrefKey const& key) { return Key{key.person, key.app, key.scope, key.point}; }

    std::map<Key, std::string> _entries;
    /// After the map, so it is destroyed first: a task running on it reads the map.
    exec::OwnerStrand _strand;
};

/// @brief The preferences model's bounds.
struct PreferencesOptions {
    /// @brief The longest JSON text a choice may have, in bytes.
    std::size_t maxValueBytes = std::size_t{16} * 1024;
};

/// @brief The type id the preferences model is registered under.
inline constexpr std::string_view kPreferencesModel = "morph.preferences";

/// @brief The longest application, scope or point name a preferences request may carry, in bytes.
inline constexpr std::size_t kMaxPrefNameBytes = 256;

/// @brief The most points one `load` may ask for.
inline constexpr std::size_t kMaxPrefPoints = 256;

/// @brief `load`: the stored choices of a screen's points. A request carries no person.
struct PrefLoad {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id, or `app`.
    std::string scope;
    /// @brief The points to read.
    std::vector<std::string> points;
};

/// @brief What `load` answers.
struct PrefValues {
    /// @brief The stored JSON text of each point that has an entry; a point without one is absent.
    std::map<std::string, std::string> values;
};

/// @brief `set`: stores one choice. A request carries no person.
struct PrefSet {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id, or `app`.
    std::string scope;
    /// @brief The point.
    std::string point;
    /// @brief The choice's JSON text.
    std::string value;
};

/// @brief What `set` answers.
struct PrefStored {
    /// @brief Whether the store took it.
    bool stored = false;
};

/// @brief `remove`: forgets one choice. A request carries no person.
struct PrefRemove {
    /// @brief The application.
    std::string app;
    /// @brief The screen's id, or `app`.
    std::string scope;
    /// @brief The point.
    std::string point;
};

/// @brief What `remove` answers.
struct PrefRemoved {
    /// @brief Whether an entry was there.
    bool removed = false;
};

/// @brief The server's preferences model: reads and writes the calling person's choices through a store.
///
/// The person is the principal of the verified session (`session::current()`), which `RemoteServer` stamps from the
/// authoriser before dispatch. The authoriser sees the model and the action, never the body, so the model is where
/// the key is decided: no request names a person, and one that tries reaches only its own caller's entries. With no
/// verified principal there is no key that belongs to the caller alone, so `load` answers nothing and `set` and
/// `remove` are refused.
class PreferencesModel {
public:
    /// @brief A model over @p store.
    /// @param store   Where choices are kept. Shared by every instance.
    /// @param options The bounds.
    PreferencesModel(std::shared_ptr<IPreferencesStore> store, PreferencesOptions options)
        : _store{std::move(store)}, _options{options} {
        if (_store == nullptr) {
            throw std::invalid_argument{"PreferencesModel: a null store"};
        }
    }

    /// @brief Reads the caller's stored choices of @p request's points.
    /// @param request The application, the scope and the points.
    /// @return The stored JSON text of each point that has an entry.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads the store.
    core::async::Task<PrefValues> execute(PrefLoad request) {
        checkName(request.app, "app");
        checkName(request.scope, "scope");
        if (request.points.size() > kMaxPrefPoints) {
            throw std::invalid_argument{"a load names more than " + std::to_string(kMaxPrefPoints) + " points"};
        }
        for (std::string const& point : request.points) {
            checkName(point, "point");
        }
        PrefValues reply;
        std::string const person = verifiedPerson();
        if (person.empty()) {
            co_return reply;
        }
        for (std::string& point : request.points) {
            std::optional<std::string> stored = co_await _store->get(
                PrefKey{.person = person, .app = request.app, .scope = request.scope, .point = point});
            if (stored) {
                reply.values.insert_or_assign(std::move(point), std::move(*stored));
            }
        }
        co_return reply;
    }

    /// @brief Stores the caller's choice.
    /// @param request The key's application, scope and point, and the choice's JSON text.
    /// @return Whether the store took it.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): writes the store.
    core::async::Task<PrefStored> execute(PrefSet request) {
        PrefKey const key = keyOf(request.app, request.scope, request.point);
        if (request.value.size() > _options.maxValueBytes) {
            throw std::invalid_argument{"a preference is longer than " + std::to_string(_options.maxValueBytes) +
                                        " bytes"};
        }
        if (auto const parsed = parseJson(request.value); !parsed) {
            throw std::invalid_argument{"a preference is JSON text: " + parsed.error()};
        }
        bool const stored = co_await _store->set(key, std::move(request.value));
        co_return PrefStored{.stored = stored};
    }

    /// @brief Forgets the caller's choice.
    /// @param request The key's application, scope and point.
    /// @return Whether an entry was there.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): writes the store.
    core::async::Task<PrefRemoved> execute(PrefRemove request) {
        PrefKey const key = keyOf(request.app, request.scope, request.point);
        bool const removed = co_await _store->remove(key);
        co_return PrefRemoved{.removed = removed};
    }

private:
    /// @brief The verified principal of the call being run.
    /// @return It; empty when the server verified nobody.
    [[nodiscard]] static std::string verifiedPerson() {
        session::Context const* const session = session::current();
        return session == nullptr ? std::string{} : session->principal;
    }

    /// @brief The caller's key for a write.
    /// @param app   The application.
    /// @param scope The screen's id, or `app`.
    /// @param point The point.
    /// @return The key, with the verified person.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the three parts of a key, in its order.
    [[nodiscard]] static PrefKey keyOf(std::string const& app, std::string const& scope, std::string const& point) {
        checkName(app, "app");
        checkName(scope, "scope");
        checkName(point, "point");
        std::string person = verifiedPerson();
        if (person.empty()) {
            throw std::runtime_error{"preferences are stored for a signed-in person"};
        }
        return PrefKey{.person = std::move(person), .app = app, .scope = scope, .point = point};
    }

    /// @brief Refuses an empty or overlong name.
    /// @param name The name.
    /// @param what What it names, for the refusal.
    static void checkName(std::string const& name, std::string_view what) {
        if (name.empty() || name.size() > kMaxPrefNameBytes) {
            throw std::invalid_argument{"a preference's `" + std::string{what} + "` is 1 to " +
                                        std::to_string(kMaxPrefNameBytes) + " bytes"};
        }
    }

    std::shared_ptr<IPreferencesStore> _store;
    PreferencesOptions _options;
};

}  // namespace morph::ui

/// @cond detail
// The traits the registration macros write, without their static-init registrars: registerPreferences registers.
// NOLINTBEGIN(misc-const-correctness): inside the macros' codec bodies.
BRIDGE_DETAIL_MODEL_TRAITS_ONLY_BODY(morph::ui::PreferencesModel, "morph.preferences")
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::PreferencesModel, morph::ui::PrefLoad, morph::ui::PrefValues, "load",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::PreferencesModel, morph::ui::PrefSet, morph::ui::PrefStored, "set",
                                      ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::PreferencesModel, morph::ui::PrefRemove, morph::ui::PrefRemoved,
                                      "remove", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
/// @endcond

namespace morph::ui {

/// @brief Registers the preferences model and its three actions, with a factory that gives every instance @p store.
///
/// Nothing registers it at static initialisation: a server that offers preferences calls this once, before it
/// serves, with the dispatcher and registry its `RemoteServer` uses. A deployment's authoriser then admits
/// `morph.preferences` and its actions `load`, `set` and `remove` for every signed-in principal; the model itself
/// keeps each person to their own entries.
/// @param store      Where choices are kept.
/// @param dispatcher The server's action dispatcher.
/// @param registry   The server's model registry.
/// @param options    The bounds.
inline void registerPreferences(std::shared_ptr<IPreferencesStore> store, model::detail::ActionDispatcher& dispatcher,
                                model::detail::ModelRegistryFactory& registry, PreferencesOptions options = {}) {
    if (store == nullptr) {
        throw std::invalid_argument{"registerPreferences: a null store"};
    }
    registry.registerModel<PreferencesModel>(kPreferencesModel, [store = std::move(store), options] {
        return std::make_unique<model::detail::ModelHolder<PreferencesModel>>(store, options);
    });
    dispatcher.registerAction<PreferencesModel, PrefLoad>(kPreferencesModel, "load");
    dispatcher.registerAction<PreferencesModel, PrefSet>(kPreferencesModel, "set");
    dispatcher.registerAction<PreferencesModel, PrefRemove>(kPreferencesModel, "remove");
}

}  // namespace morph::ui
