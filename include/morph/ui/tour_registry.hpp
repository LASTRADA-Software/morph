// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../core/change_types.hpp"
#include "../core/coroutine.hpp"
#include "../core/model.hpp"
#include "../core/registry.hpp"
#include "../session/session.hpp"
#include "service.hpp"

/// @file
/// @brief Guided tours registered while the server runs: `TourRegistry`, the `morph.tours` model that lists them, and
///        the wiring that tells watching clients when they change. Specified in `docs/spec/ui/tours.md`.

namespace morph::backend {
class RemoteServer;
}  // namespace morph::backend

namespace morph::ui {

/// @brief The runtime tours model's type id.
inline constexpr std::string_view kToursModel = "morph.tours";
/// @brief Its one action, `list`.
inline constexpr std::string_view kToursList = "morph.tours.list";
/// @brief The record every change of the runtime tours touches.
inline constexpr std::string_view kTourRecord = "morph.tours";

/// @brief Why a runtime tour was refused or dropped.
struct TourRefusal {
    /// @brief The tour's id, as far as it could be read; empty when it could not.
    std::string tourId;
    /// @brief A JSON Pointer into the tour, such as `/steps/2/target`; empty for the whole tour.
    std::string path;
    /// @brief The reason, in one sentence, naming the step where there is one.
    std::string reason;

    /// @brief The refusal as one line.
    /// @return `the tour `<id>` at <path>: <reason>`.
    [[nodiscard]] std::string message() const {
        return "the tour `" + tourId + "` at " + (path.empty() ? std::string{"/"} : path) + ": " + reason;
    }

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(TourRefusal const&) const = default;
};

/// @brief `list`: the runtime tours the caller may see. It names nothing: a server has one registry.
struct ToursList {};

/// @brief One runtime tour as `list` answers it: the record `morph.tours`.
struct ListedTour {
    /// @brief The tour's id, the record's key.
    std::string id;
    /// @brief Grows by one with each `add` and `replace` on the server, so a client tells a replaced tour from a
    ///        running one with new texts.
    std::uint64_t revision = 0;
    /// @brief The tour's JSON text, one entry of the app document's `tours` in form.
    std::string tour;
};

/// @brief What `list` answers.
struct ToursListed {
    /// @brief The tours, in registration order.
    std::vector<ListedTour> tours;
};

/// @brief The tours added while the server runs, beside the app document's own.
///
/// `add` and `replace` take one tour's JSON, the form of an entry of the app document's `tours` (`author::tourJson`
/// writes it), and check it against the bundle served now exactly as the bundle's build checks the app document's
/// tours, and that its `t` keys are in a catalogue of the bundle or among the server's texts. The bundle is read
/// through the getter at every call; when its digest differs from the one last checked, every held tour is checked
/// again first, and one that fails is dropped: logged at error and told to `setOnDropped`'s handler. Every change, a
/// drop included, is told to `setOnChanged`'s handler after the registry's lock is released (`publishTourChanges`
/// turns it into a change of the record `morph.tours`). The first change made while no handler is set is logged as a
/// warning, so a server that never called `publishTourChanges` hears of it. A refusal names the runtime tour by its id
/// and points into the submitted tour's JSON, never into the app document. The tours are kept in memory only. Safe to
/// call from any thread.
class MORPH_API TourRegistry {
public:
    /// @brief A registry with no tour.
    /// @param bundle The bundle served now; called at every use, from any thread, so a development reload is seen.
    explicit TourRegistry(std::function<std::shared_ptr<ServedBundle const>()> bundle);

    /// @brief Adds a tour.
    /// @param tour The tour's JSON text.
    /// @return Nothing, or the refusal: a malformed tour, a name the bundle does not have, a text key nobody has, an
    /// id
    ///         the app document or another runtime tour uses, or a bundle with no app document.
    std::expected<void, TourRefusal> add(std::string_view tour);

    /// @brief Replaces a runtime tour with new content under the same id.
    /// @param tour The tour's JSON text.
    /// @return Nothing, or the refusal: `add`'s, or an id the registry does not hold.
    std::expected<void, TourRefusal> replace(std::string_view tour);

    /// @brief Removes a runtime tour.
    /// @param tourId Its id.
    /// @return Nothing, or the refusal of an id the registry does not hold.
    std::expected<void, TourRefusal> remove(std::string_view tourId);

    /// @brief The runtime tours a person may see, in registration order: those whose `screen` and every step's
    ///        `screen` the person may open (`screenAdmitted`). A tour is offered whole or not at all.
    /// @param session    The caller's session.
    /// @param authorizer The server's authoriser.
    /// @return The tours.
    [[nodiscard]] ToursListed listFor(::morph::session::Context const& session,
                                      ::morph::session::IAuthorizer const& authorizer);

    /// @brief Checks every held tour against the bundle served now, dropping those that fail, whether or not its
    /// digest
    ///        changed: what an application calls right after it swaps the bundle.
    void recheck();

    /// @brief Sets what is told after every change.
    /// @param onChanged The handler; empty tells nothing.
    void setOnChanged(std::function<void()> onChanged);

    /// @brief Sets what is told of each tour a bundle change dropped.
    /// @param onDropped The handler, given the refusal; empty tells nothing.
    void setOnDropped(std::function<void(TourRefusal const&)> onDropped);

private:
    struct Held {
        std::string id;
        std::uint64_t revision = 0;
        std::string text;
        std::vector<std::string> screens;
    };

    [[nodiscard]] std::expected<Held, TourRefusal> checked(std::string_view tour, ServedBundle const& bundle) const;
    // Checks every held tour against @p bundle and drops those that fail; answers the drops. Under the lock.
    [[nodiscard]] std::vector<TourRefusal> recheckLocked(ServedBundle const& bundle);
    // The bundle now, every held tour checked against it; drops into @p dropped. Under the lock.
    [[nodiscard]] std::shared_ptr<ServedBundle const> currentLocked(std::vector<TourRefusal>& dropped);
    void tell(bool changed, std::vector<TourRefusal> const& dropped);

    std::function<std::shared_ptr<ServedBundle const>()> _bundle;
    mutable std::mutex _mutex;
    std::vector<Held> _tours;
    std::string _checkedDigest;
    std::uint64_t _revisions = 0;
    bool _warnedUnpublished = false;
    std::function<void()> _onChanged;
    std::function<void(TourRefusal const&)> _onDropped;
};

/// @brief The runtime tours model: `list`, for the caller's session.
class MORPH_API ToursModel {
public:
    /// @brief A model over the registry.
    /// @param tours      The registry; never null.
    /// @param authorizer The server's authoriser, for `screenAdmitted`; never null.
    ToursModel(std::shared_ptr<TourRegistry> tours, std::shared_ptr<::morph::session::IAuthorizer const> authorizer);

    /// @brief The runtime tours the caller may see.
    /// @param request Nothing.
    /// @return The tours.
    ::core::async::Task<ToursListed> execute(ToursList request);

private:
    std::shared_ptr<TourRegistry> _tours;
    std::shared_ptr<::morph::session::IAuthorizer const> _authorizer;
};

}  // namespace morph::ui

/// @cond detail
// The traits the registration macros write, without their static-init registrars: registerTours registers the model
// and its action; the record and the result slot are registered in tour_registry.cpp.
// NOLINTBEGIN(misc-const-correctness): inside the macros' codec bodies.
BRIDGE_DETAIL_MODEL_TRAITS_ONLY_BODY(morph::ui::ToursModel, "morph.tours")
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::ToursModel, morph::ui::ToursList, morph::ui::ToursListed,
                                      "morph.tours.list", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
/// @endcond

namespace morph::ui {

/// @brief Registers the runtime tours model and its `list` action, with a factory that gives every instance the one
///        registry. A server calls it once, before it serves. A server that does not answers `list` with the
///        dispatcher's unknown-model error, which a client reads as "the app document's tours alone".
/// @param tours      The registry.
/// @param authorizer The server's authoriser.
/// @param dispatcher The server's action dispatcher.
/// @param registry   The server's model registry.
/// @throws std::invalid_argument for a null registry or authoriser.
MORPH_API void registerTours(std::shared_ptr<TourRegistry> tours,
                             std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                             model::detail::ActionDispatcher& dispatcher,
                             model::detail::ModelRegistryFactory& registry);

/// @brief The change every add, replace, remove and drop makes: the record `morph.tours` as a whole.
/// @return It.
[[nodiscard]] MORPH_API ::morph::effects::Changes toursChanged();

/// @brief Fans each change of @p tours out to every connection of @p server watching `morph.tours`, from a task on the
///        server's strand. Replaces the registry's `setOnChanged` handler.
/// @param tours  The registry.
/// @param server The server; a change after it is gone is not sent.
MORPH_API void publishTourChanges(TourRegistry& tours, std::weak_ptr<::morph::backend::RemoteServer> server);

}  // namespace morph::ui
