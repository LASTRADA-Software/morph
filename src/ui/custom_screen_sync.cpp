// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

std::expected<std::shared_ptr<Document const>, Refusal> CustomDocuments::put(CustomScreenFound found,
                                                                             LoadOptions const& options) {
    auto loaded = loadDocumentText(found.document, options);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (loaded->id != found.summary.id) {
        return std::unexpected(Refusal{.document = found.summary.id,
                                       .path = "/id",
                                       .reason = "the stored document names another screen, " + loaded->id});
    }
    auto document = std::make_shared<Document const>(std::move(*loaded));
    std::string const id = found.summary.id;
    _entries.insert_or_assign(
        id, Entry{.summary = std::move(found.summary), .text = std::move(found.document), .document = document});
    return document;
}

std::shared_ptr<Document const> CustomDocuments::find(std::string_view id) const {
    auto const found = _entries.find(id);
    return found == _entries.end() ? nullptr : found->second.document;
}

CustomDocuments::Entry const* CustomDocuments::entry(std::string_view id) const {
    auto const found = _entries.find(id);
    return found == _entries.end() ? nullptr : &found->second;
}

std::vector<std::string> CustomDocuments::refresh(CustomScreensListed const& listed) {
    std::vector<std::string> dropped;
    for (auto kept = _entries.begin(); kept != _entries.end();) {
        auto const shown = std::ranges::find(listed.screens, kept->first, &CustomScreenSummary::id);
        bool const gone = shown == listed.screens.end() ? !listed.truncated : shown->hash != kept->second.summary.hash;
        if (gone) {
            dropped.push_back(kept->first);
            kept = _entries.erase(kept);
        } else {
            ++kept;
        }
    }
    return dropped;
}

void CustomDocuments::forget(std::string_view id) {
    if (auto const found = _entries.find(id); found != _entries.end()) {
        _entries.erase(found);
    }
}

CustomScreenSync::CustomScreenSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, std::string application,
                                   std::function<Catalog()> catalog, Watcher watcher)
    : _dispatcher{&dispatcher},
      _application{std::move(application)},
      _catalog{std::move(catalog)},
      _watcher{std::move(watcher)} {}

void CustomScreenSync::call(std::string_view action, std::string const& body,
                            std::function<void(std::string const&)> ok,
                            std::function<void(std::string const&)> const& failed) {
    try {
        if (_channel == nullptr) {
            _channel = _dispatcher->bind(std::string{kCustomScreensModel}, bridge::BindSharing::Private, {});
        }
        _channel->execute(std::string{action}, body, CallOptions{})
            .then(*_scope, [ok = std::move(ok)](std::string const& reply) { ok(reply); })
            .onError(*_scope, [failed](std::exception_ptr const& error) { failed(detail::failureText(error)); });
    } catch (std::exception const& error) {
        failed(error.what());
    }
}

void CustomScreenSync::probe(std::function<void(bool offered)> done) {
    auto const shared = std::make_shared<std::function<void(bool)>>(std::move(done));
    call(
        kCustomScreensList, model::ActionTraits<CustomScreensList>::toJson(CustomScreensList{.app = _application}),
        [this, shared](std::string const& reply) {
            CustomScreensListed listed;
            try {
                listed = model::ActionTraits<CustomScreensList>::resultFromJson(reply);
            } catch (std::exception const& error) {
                off(std::string{"not a custom screens list: "} + error.what());
                if (*shared) {
                    (*shared)(false);
                }
                return;
            }
            _offer = CustomScreensOffer::Offered;
            _offRefusal.clear();
            watch();
            listedNow(std::move(listed));
            if (*shared) {
                (*shared)(true);
            }
        },
        [this, shared](std::string const& reason) {
            off(reason);
            if (*shared) {
                (*shared)(false);
            }
        });
}

void CustomScreenSync::off(std::string reason) {
    _offer = CustomScreensOffer::Off;
    // A server without the model is the ordinary way to say custom screens are not offered; anything else is a
    // server that has the model and refuses it, which someone has to hear about.
    if (!reason.contains("unknown model type")) {
        ::morph::log::logWarn("custom screens are off: the server refused `{}`: {}", kCustomScreensList, reason);
    }
    _offRefusal = std::move(reason);
}

void CustomScreenSync::relist() {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    call(
        kCustomScreensList, model::ActionTraits<CustomScreensList>::toJson(CustomScreensList{.app = _application}),
        [this](std::string const& reply) {
            try {
                listedNow(model::ActionTraits<CustomScreensList>::resultFromJson(reply));
            } catch (std::exception const& error) {
                ::morph::log::logWarn("custom screens: not a list from `{}`: {}", kCustomScreensList, error.what());
            }
        },
        [](std::string const& reason) {
            ::morph::log::logWarn("custom screens: `{}` was refused, the last list is kept: {}", kCustomScreensList,
                                  reason);
        });
}

void CustomScreenSync::listedNow(CustomScreensListed listed) {
    static_cast<void>(_documents.refresh(listed));
    _listed = std::move(listed);
    if (_onListed) {
        _onListed(_listed);
    }
}

void CustomScreenSync::open(std::string const& id, CustomDocumentDone done) {
    if (std::shared_ptr<Document const> kept = _documents.find(id)) {
        done(std::move(kept));
        return;
    }
    fetch(id, std::move(done));
}

void CustomScreenSync::fetch(std::string const& id, CustomDocumentDone done) {
    auto const shared = std::make_shared<CustomDocumentDone>(std::move(done));
    get(id, [this, shared](std::expected<CustomScreenFound, std::string> found) {
        if (!found) {
            (*shared)(std::unexpected(std::move(found.error())));
            return;
        }
        auto loaded = _documents.put(std::move(*found), LoadOptions{.catalog = _catalog ? _catalog() : Catalog{}});
        if (!loaded) {
            (*shared)(std::unexpected(loaded.error().message()));
            return;
        }
        (*shared)(std::move(*loaded));
    });
}

void CustomScreenSync::get(std::string const& id,
                           std::function<void(std::expected<CustomScreenFound, std::string>)> done) {
    auto const shared =
        std::make_shared<std::function<void(std::expected<CustomScreenFound, std::string>)>>(std::move(done));
    call(
        kCustomScreensGet,
        model::ActionTraits<CustomScreensGet>::toJson(CustomScreensGet{.app = _application, .id = id}),
        [shared](std::string const& reply) {
            try {
                (*shared)(model::ActionTraits<CustomScreensGet>::resultFromJson(reply));
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a custom screen: "} + error.what()));
            }
        },
        [shared](std::string const& reason) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::check(std::string basedOn, std::string document,
                             std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)> done) {
    auto const shared = std::make_shared<std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)>>(
        std::move(done));
    call(
        kCustomScreensCheck,
        model::ActionTraits<CustomScreensCheck>::toJson(
            CustomScreensCheck{.app = _application, .basedOn = std::move(basedOn), .document = std::move(document)}),
        [shared](std::string const& reply) {
            try {
                (*shared)(model::ActionTraits<CustomScreensCheck>::resultFromJson(reply).refusal);
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a check: "} + error.what()));
            }
        },
        [shared](std::string const& reason) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::save(CustomScreensSave request,
                            std::function<void(std::expected<std::string, std::string>)> done) {
    auto const shared =
        std::make_shared<std::function<void(std::expected<std::string, std::string>)>>(std::move(done));
    request.app = _application;
    call(
        kCustomScreensSave, model::ActionTraits<CustomScreensSave>::toJson(request),
        [this, shared](std::string const& reply) {
            CustomScreenSaved saved;
            try {
                saved = model::ActionTraits<CustomScreensSave>::resultFromJson(reply);
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a save: "} + error.what()));
                return;
            }
            relist();
            (*shared)(std::move(saved.id));
        },
        [shared](std::string const& reason) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::prefetchRoutes(std::vector<std::string> const& routes,
                                      std::function<void(std::vector<std::string>)> done) {
    std::vector<std::string> wanted;
    for (std::string const& route : routes) {
        std::string const id{routeScreen(route)};
        if (isCustomScreenId(id) && _documents.find(id) == nullptr && std::ranges::find(wanted, id) == wanted.end()) {
            wanted.push_back(id);
        }
    }
    if (wanted.empty()) {
        done({});
        return;
    }
    struct Pending {
        std::vector<std::string> ids;
        std::vector<bool> missing;
        std::size_t left = 0;
        std::function<void(std::vector<std::string>)> done;
    };
    auto const pending = std::make_shared<Pending>(Pending{.ids = wanted,
                                                           .missing = std::vector<bool>(wanted.size(), false),
                                                           .left = wanted.size(),
                                                           .done = std::move(done)});
    for (std::size_t place = 0; place < wanted.size(); ++place) {
        fetch(wanted.at(place),
              [pending, place](std::expected<std::shared_ptr<Document const>, std::string> const& loaded) {
                  pending->missing.at(place) = !loaded.has_value();
                  if (--pending->left > 0) {
                      return;
                  }
                  std::vector<std::string> missing;
                  for (std::size_t each = 0; each < pending->ids.size(); ++each) {
                      if (pending->missing.at(each)) {
                          missing.push_back(pending->ids.at(each));
                      }
                  }
                  pending->done(std::move(missing));
              });
    }
}

void CustomScreenSync::watch() {
    if (!_watcher) {
        return;
    }
    wire::WatchBody const body{.records = {std::string{kCustomScreenRecord}}, .instances = {}, .replace = false};
    _watcher(std::string{wire::kWatchKind}, glz::write_json(body).value_or(std::string{}))
        .then(*_scope,
              [](std::string const& reply) {
                  auto const read = wire::readWatchReply(reply);
                  if (read && std::ranges::find(read->refusedRecords, std::string{kCustomScreenRecord}) !=
                                  read->refusedRecords.end()) {
                      ::morph::log::logWarn(
                          "custom screens: the server refused a watch on `{}`; lists refresh only on sign-in, "
                          "reconnect and this client's own saves",
                          kCustomScreenRecord);
                  }
              })
        .onError(*_scope, [](std::exception_ptr const& error) {
            ::morph::log::logWarn("custom screens: the watch on `{}` failed: {}", kCustomScreenRecord,
                                  detail::failureText(error));
        });
}

void CustomScreenSync::reconnected() {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    watch();
    relist();
}

void CustomScreenSync::deliverEvent(wire::EventBody const& event) {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    if (event.topic == wire::kResyncTopic) {
        watch();
        relist();
        return;
    }
    if (event.topic != wire::kChangesTopic) {
        return;
    }
    auto const changes = wire::readChangesBody(event.payload.str);
    if (changes && std::ranges::any_of(changes->records,
                                       [](auto const& change) { return change.record == kCustomScreenRecord; })) {
        relist();
    }
}

void CustomScreenSync::reset() {
    _scope = std::make_unique<async::CallbackScope>();
    _channel.reset();
    _offer = CustomScreensOffer::Unknown;
    _offRefusal.clear();
    _listed = {};
    _documents.clear();
}

}  // namespace morph::ui
