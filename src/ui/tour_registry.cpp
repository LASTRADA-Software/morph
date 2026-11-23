// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/core/records.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/tour_registry.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tour_checks.hpp"

namespace morph::ui {
namespace {

// The keys of the `{"t": key}` texts of a tour: its title and summary, each step's title and body; with their
// pointers.
std::vector<std::pair<std::string, std::string>> textKeys(Json const& tour) {
    std::vector<std::pair<std::string, std::string>> keys;
    auto const keyOf = [&keys](Json const* text, std::string path) {
        auto const* const object = text == nullptr ? nullptr : std::get_if<Json::object_t>(&text->data);
        if (object == nullptr) {
            return;
        }
        if (auto const found = object->find("t"); found != object->end()) {
            if (auto const* const key = std::get_if<std::string>(&found->second.data)) {
                keys.emplace_back(std::move(path), *key);
            }
        }
    };
    auto const member = [](Json::object_t const& object, std::string_view key) -> Json const* {
        auto const found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    };
    auto const& fields = std::get<Json::object_t>(tour.data);
    keyOf(member(fields, "title"), "/title");
    keyOf(member(fields, "summary"), "/summary");
    if (Json const* const steps = member(fields, "steps")) {
        if (auto const* const list = std::get_if<Json::array_t>(&steps->data)) {
            for (std::size_t index = 0; index < list->size(); ++index) {
                if (auto const* const step = std::get_if<Json::object_t>(&list->at(index).data)) {
                    std::string const path = pointerAppend("/steps", index);
                    keyOf(member(*step, "title"), pointerAppend(path, "title"));
                    keyOf(member(*step, "body"), pointerAppend(path, "body"));
                }
            }
        }
    }
    return keys;
}

// The first `t` key of @p tour that no catalogue of @p bundle has and the server's texts do not answer in any of the
// bundle's locales (English when it has no catalogue), with its pointer.
std::optional<std::pair<std::string, std::string>> missingTextKey(Json const& tour, ServedBundle const& bundle) {
    std::vector<std::pair<std::string, std::string>> const keys = textKeys(tour);
    if (keys.empty()) {
        return std::nullopt;
    }
    std::set<std::string, std::less<>> known;
    std::set<std::string, std::less<>> locales;
    for (auto const& [hash, file] : bundle.files) {
        static_cast<void>(hash);
        for (ServedFile::Holder const& holder : file.holders) {
            if (holder.kind != kMessagesFile) {
                continue;
            }
            if (std::optional<std::string> locale = catalogueLocale(holder.id)) {
                locales.insert(std::move(*locale));
            }
            if (auto catalogue = parseCatalogue(file.bytes)) {
                for (auto const& [key, message] : catalogue->messages) {
                    static_cast<void>(message);
                    known.insert(key);
                }
            }
        }
    }
    if (bundle.fieldTexts) {
        if (locales.empty()) {
            locales.insert("en");
        }
        for (std::string const& locale : locales) {
            try {
                for (auto const& [key, text] : bundle.fieldTexts(locale)) {
                    static_cast<void>(text);
                    known.insert(key);
                }
            } catch (std::exception const& error) {
                ::morph::log::logWarn("[tours] the server's texts for `{}` could not be read: {}", locale,
                                      error.what());
            }
        }
    }
    for (auto const& entry : keys) {
        if (!known.contains(entry.second)) {
            return entry;
        }
    }
    return std::nullopt;
}

}  // namespace

TourRegistry::TourRegistry(std::function<std::shared_ptr<ServedBundle const>()> bundle) : _bundle{std::move(bundle)} {
    if (!_bundle) {
        throw std::invalid_argument{"TourRegistry: the bundle getter is empty"};
    }
}

std::expected<TourRegistry::Held, TourRefusal> TourRegistry::checked(std::string_view tour,
                                                                     ServedBundle const& bundle) const {
    auto json = parseJson(tour);
    auto const* const object = json ? std::get_if<Json::object_t>(&json->data) : nullptr;
    std::string id;
    if (object != nullptr) {
        if (auto const found = object->find("id"); found != object->end()) {
            if (auto const* const text = std::get_if<std::string>(&found->second.data)) {
                id = *text;
            }
        }
    }
    auto const refuse = [&id](std::string path, std::string reason) {
        return std::unexpected(TourRefusal{.tourId = id, .path = std::move(path), .reason = std::move(reason)});
    };
    if (object == nullptr) {
        return refuse({}, "a tour is a JSON object, as one entry of the app document's `tours`");
    }
    std::string const appText = detail::appTextOf(bundle);
    auto app = parseJson(appText);
    auto* const appObject = app ? std::get_if<Json::object_t>(&app->data) : nullptr;
    if (appObject == nullptr) {
        return refuse({}, "the bundle has no app document, and a runtime tour's texts are read in the app scope");
    }
    // The app document with this tour appended: the loader checks the tour's shape, its texts in the app scope, and
    // that its id is none of the app document's tours, with the same words as for the app document's own.
    auto const [tours, created] = appObject->try_emplace("tours", Json::array_t{});
    static_cast<void>(created);
    auto* const list = std::get_if<Json::array_t>(&tours->second.data);
    if (list == nullptr) {
        return refuse({}, "the app document's `tours` is not an array");
    }
    std::size_t const index = list->size();
    list->push_back(*json);
    std::string const prefix = pointerAppend("/tours", index);
    // A refusal's path as a pointer into the submitted tour: the part after this tour's place in the app document. A
    // path outside the tour (the bundle's own app document is at fault) is the whole tour.
    auto const local = [&prefix](std::string const& path) {
        bool const inside = path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '/');
        return inside ? path.substr(prefix.size()) : std::string{};
    };
    auto const loaded =
        loadDocumentText(glz::write_json(*app).value_or(std::string{}), LoadOptions{.catalog = bundle.catalog});
    if (!loaded) {
        return refuse(local(loaded.error().path), loaded.error().reason);
    }
    TourDecl const& decl = loaded->tours.at(index);
    std::vector<detail::TourScreen> const screens = detail::tourScreensOf(bundle);
    if (auto refused = detail::checkTour(decl, loaded->shell, detail::indexTourScreens(screens))) {
        return refuse(local(refused->path), refused->reason);
    }
    if (auto const missing = missingTextKey(*json, bundle)) {
        return refuse(missing->first,
                      "the text key `" + missing->second +
                          "` is in no message catalogue of the bundle and not among the server's texts");
    }
    Held held{.id = decl.id, .revision = 0, .text = std::string{tour}, .screens = {}};
    if (!decl.screen.empty()) {
        held.screens.push_back(decl.screen);
    }
    for (TourStepDecl const& step : decl.steps) {
        if (!step.screen.empty()) {
            held.screens.push_back(step.screen);
        }
    }
    return held;
}

std::vector<TourRefusal> TourRegistry::recheckLocked(ServedBundle const& bundle) {
    std::vector<TourRefusal> dropped;
    std::erase_if(_tours, [&](Held const& held) {
        auto const again = checked(held.text, bundle);
        if (again) {
            return false;
        }
        ::morph::log::logError("[tours] dropped the runtime tour `{}`, which the bundle now served refuses: {}",
                               held.id, again.error().message());
        dropped.push_back(again.error());
        return true;
    });
    _checkedDigest = bundle.digest;
    return dropped;
}

std::shared_ptr<ServedBundle const> TourRegistry::currentLocked(std::vector<TourRefusal>& dropped) {
    std::shared_ptr<ServedBundle const> bundle = _bundle();
    if (bundle == nullptr) {
        throw std::logic_error{"TourRegistry: the bundle getter answered no bundle"};
    }
    if (bundle->digest != _checkedDigest) {
        dropped = recheckLocked(*bundle);
    }
    return bundle;
}

std::expected<void, TourRefusal> TourRegistry::add(std::string_view tour) {
    std::vector<TourRefusal> dropped;
    std::expected<void, TourRefusal> result;
    {
        std::scoped_lock const lock{_mutex};
        std::shared_ptr<ServedBundle const> const bundle = currentLocked(dropped);
        auto held = checked(tour, *bundle);
        if (!held) {
            result = std::unexpected(std::move(held.error()));
        } else if (std::ranges::any_of(_tours, [&held](Held const& kept) { return kept.id == held->id; })) {
            result = std::unexpected(TourRefusal{
                .tourId = held->id, .path = "/id", .reason = "a runtime tour with this id is registered; replace it"});
        } else {
            held->revision = ++_revisions;
            _tours.push_back(std::move(*held));
        }
    }
    tell(result.has_value(), dropped);
    return result;
}

std::expected<void, TourRefusal> TourRegistry::replace(std::string_view tour) {
    std::vector<TourRefusal> dropped;
    std::expected<void, TourRefusal> result;
    {
        std::scoped_lock const lock{_mutex};
        std::shared_ptr<ServedBundle const> const bundle = currentLocked(dropped);
        auto held = checked(tour, *bundle);
        if (!held) {
            result = std::unexpected(std::move(held.error()));
        } else if (auto const kept = std::ranges::find(_tours, held->id, &Held::id); kept == _tours.end()) {
            result = std::unexpected(TourRefusal{
                .tourId = held->id,
                .path = "/id",
                .reason = "no runtime tour has this id; an app document's tour changes in the app document"});
        } else {
            held->revision = ++_revisions;
            *kept = std::move(*held);
        }
    }
    tell(result.has_value(), dropped);
    return result;
}

std::expected<void, TourRefusal> TourRegistry::remove(std::string_view tourId) {
    std::vector<TourRefusal> dropped;
    std::expected<void, TourRefusal> result;
    {
        std::scoped_lock const lock{_mutex};
        static_cast<void>(currentLocked(dropped));
        auto const kept = std::ranges::find(_tours, tourId, &Held::id);
        if (kept == _tours.end()) {
            result = std::unexpected(
                TourRefusal{.tourId = std::string{tourId}, .path = "/id", .reason = "no runtime tour has this id"});
        } else {
            _tours.erase(kept);
        }
    }
    tell(result.has_value(), dropped);
    return result;
}

ToursListed TourRegistry::listFor(::morph::session::Context const& session,
                                  ::morph::session::IAuthorizer const& authorizer) {
    std::vector<TourRefusal> dropped;
    ToursListed listed;
    {
        std::scoped_lock const lock{_mutex};
        std::shared_ptr<ServedBundle const> const bundle = currentLocked(dropped);
        auto const admitted = [&](std::string const& screenId) {
            auto const screen = std::ranges::find(bundle->screens, screenId,
                                                  [](ServedScreen const& served) { return served.entry.id; });
            return screen != bundle->screens.end() && screenAdmitted(*screen, session, authorizer);
        };
        for (Held const& held : _tours) {
            if (std::ranges::all_of(held.screens, admitted)) {
                listed.tours.push_back(ListedTour{.id = held.id, .revision = held.revision, .tour = held.text});
            }
        }
    }
    tell(false, dropped);
    return listed;
}

void TourRegistry::recheck() {
    std::vector<TourRefusal> dropped;
    {
        std::scoped_lock const lock{_mutex};
        std::shared_ptr<ServedBundle const> const bundle = _bundle();
        if (bundle != nullptr) {
            dropped = recheckLocked(*bundle);
        }
    }
    tell(false, dropped);
}

void TourRegistry::setOnChanged(std::function<void()> onChanged) {
    std::scoped_lock const lock{_mutex};
    _onChanged = std::move(onChanged);
}

void TourRegistry::setOnDropped(std::function<void(TourRefusal const&)> onDropped) {
    std::scoped_lock const lock{_mutex};
    _onDropped = std::move(onDropped);
}

void TourRegistry::tell(bool changed, std::vector<TourRefusal> const& dropped) {
    std::function<void()> onChanged;
    std::function<void(TourRefusal const&)> onDropped;
    bool warn = false;
    {
        std::scoped_lock const lock{_mutex};
        onChanged = _onChanged;
        onDropped = _onDropped;
        // A change nobody is told of leaves watching clients stale; say so once, not on every change.
        warn = (changed || !dropped.empty()) && !onChanged && !_warnedUnpublished;
        _warnedUnpublished = _warnedUnpublished || warn;
    }
    if (warn) {
        ::morph::log::logWarn(
            "[tours] the runtime tours changed while nothing publishes the change: call "
            "publishTourChanges (or setOnChanged), or watching clients keep their old tours");
    }
    if (onDropped) {
        for (TourRefusal const& refusal : dropped) {
            onDropped(refusal);
        }
    }
    if ((changed || !dropped.empty()) && onChanged) {
        onChanged();
    }
}

ToursModel::ToursModel(std::shared_ptr<TourRegistry> tours,
                       std::shared_ptr<::morph::session::IAuthorizer const> authorizer)
    : _tours{std::move(tours)}, _authorizer{std::move(authorizer)} {}

::core::async::Task<ToursListed> ToursModel::execute(ToursList /*request*/) {
    ::morph::session::Context const* const current = ::morph::session::current();
    ::morph::session::Context const session = current == nullptr ? ::morph::session::Context{} : *current;
    co_return _tours->listFor(session, *_authorizer);
}

void registerTours(std::shared_ptr<TourRegistry> tours,
                   std::shared_ptr<::morph::session::IAuthorizer const> authorizer,
                   model::detail::ActionDispatcher& dispatcher, model::detail::ModelRegistryFactory& registry) {
    if (tours == nullptr || authorizer == nullptr) {
        throw std::invalid_argument{"registerTours: a null registry or authorizer"};
    }
    registry.registerModel<ToursModel>(kToursModel, [tours, authorizer] {
        return std::make_unique<model::detail::ModelHolder<ToursModel>>(tours, authorizer);
    });
    dispatcher.registerAction<ToursModel, ToursList>(kToursModel, kToursList);
}

::morph::effects::Changes toursChanged() {
    ::morph::effects::Changes changes;
    changes.records.push_back(
        ::morph::effects::RecordChange{.record = std::string{kTourRecord}, .keys = std::nullopt});
    return changes;
}

void publishTourChanges(TourRegistry& tours, std::weak_ptr<::morph::backend::RemoteServer> server) {
    tours.setOnChanged([server = std::move(server)] {
        std::shared_ptr<::morph::backend::RemoteServer> const live = server.lock();
        if (live == nullptr) {
            return;
        }
        live->strand().post([weak = std::weak_ptr<::morph::backend::RemoteServer>{live}] {
            if (std::shared_ptr<::morph::backend::RemoteServer> const held = weak.lock()) {
                held->fanOut(toursChanged(), 0, {});
            }
        });
    });
}

}  // namespace morph::ui

// The record and the result slot that lets `list` be found holding it are registered here, at static initialisation,
// in the translation unit a server links by calling registerTours.
BRIDGE_REGISTER_RECORD(morph::ui::ListedTour, "morph.tours")
#ifndef MORPH_CLIENT_ONLY
namespace {
// NOLINTNEXTLINE(cert-err58-cpp): the registrar is noexcept.
[[maybe_unused]] bool const tourResults =
    morph::model::detail::registerActionResultOnce<morph::ui::ToursList>(morph::ui::kToursList);
}  // namespace
#endif
