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
#include <morph/ui/json_source.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

std::expected<std::shared_ptr<Document const>, Refusal> CustomDocuments::put(CustomScreenFound found,
                                                                             LoadOptions const& options) {
    if (found.summary.broken) {
        return std::unexpected(Refusal{.document = found.summary.id,
                                       .path = found.summary.broken->pointer,
                                       .reason = "the screen is broken: " + found.summary.broken->reason});
    }
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
    std::string const stored = found.summary.id;
    _entries.insert_or_assign(
        stored, Entry{.summary = std::move(found.summary), .text = std::move(found.document), .document = document});
    return document;
}

std::shared_ptr<Document const> CustomDocuments::find(std::string_view screenId) const {
    auto const found = _entries.find(screenId);
    return found == _entries.end() ? nullptr : found->second.document;
}

CustomDocuments::Entry const* CustomDocuments::entry(std::string_view screenId) const {
    auto const found = _entries.find(screenId);
    return found == _entries.end() ? nullptr : &found->second;
}

std::vector<std::string> CustomDocuments::refresh(CustomScreensListed const& listed) {
    std::vector<std::string> dropped;
    for (auto kept = _entries.begin(); kept != _entries.end();) {
        auto const shown = std::ranges::find(listed.screens, kept->first, &CustomScreenSummary::id);
        // A screen the list marks broken is dropped whatever its hash: the checks can change without the document
        // changing, and the cache answers the host's `documents` lookup, which mounts without asking.
        bool const gone = shown == listed.screens.end()
                              ? !listed.truncated
                              : shown->hash != kept->second.summary.hash || shown->broken.has_value();
        if (gone) {
            dropped.push_back(kept->first);
            kept = _entries.erase(kept);
        } else {
            if (shown != listed.screens.end()) {
                kept->second.summary = *shown;
            }
            ++kept;
        }
    }
    return dropped;
}

void CustomDocuments::forget(std::string_view screenId) {
    if (auto const found = _entries.find(screenId); found != _entries.end()) {
        _entries.erase(found);
    }
}

CustomScreenSync::CustomScreenSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, std::string application,
                                   std::function<Catalog()> catalog, Session* session)
    : _dispatcher{&dispatcher},
      _application{std::move(application)},
      _catalog{std::move(catalog)},
      _session{session} {}

CustomScreenSync::~CustomScreenSync() {
    if (_offer == CustomScreensOffer::Offered && _session != nullptr) {
        _session->removeExtraWatch(std::string{kCustomScreenRecord});
    }
}

void CustomScreenSync::call(std::string_view action, std::string const& body,
                            std::function<void(std::string const&)> onReply,
                            std::function<void(std::string const&, std::string const&)> const& failed) {
    try {
        if (_channel == nullptr) {
            _channel = _dispatcher->bind(std::string{kCustomScreensModel}, bridge::BindSharing::Private, {});
        }
        _channel->execute(std::string{action}, body, CallOptions{})
            .then(*_scope, [onReply = std::move(onReply)](std::string const& reply) { onReply(reply); })
            .onError(*_scope, [failed](std::exception_ptr const& error) {
                failed(detail::failureText(error), detail::defaultErrorKind(error));
            });
    } catch (std::exception const& error) {
        failed(error.what(), "error");
    }
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the chain registers are owned by the scope.

void CustomScreenSync::probe(std::function<void(bool offered)> done) {
    auto const shared = std::make_shared<std::function<void(bool)>>(std::move(done));
    call(
        kCustomScreensList, model::ActionTraits<CustomScreensList>::toJson(CustomScreensList{.app = _application}),
        [this, shared](std::string const& reply) {
            CustomScreensListed listed;
            try {
                listed = model::ActionTraits<CustomScreensList>::resultFromJson(reply);
            } catch (std::exception const& error) {
                off(OffCause::Unreadable, error.what());
                if (*shared) {
                    (*shared)(false);
                }
                return;
            }
            _offer = CustomScreensOffer::Offered;
            _offRefusal.clear();
            if (_session != nullptr) {
                _session->addExtraWatch(std::string{kCustomScreenRecord});
            }
            listedNow(std::move(listed));
            if (*shared) {
                (*shared)(true);
            }
        },
        [this, shared](std::string const& reason, std::string const& kind) {
            bool const transport = kind == "offline" || kind == "timeout" || kind == "backendChanged";
            bool const absent = reason.contains("unknown model type: " + std::string{kCustomScreensModel});
            OffCause cause = OffCause::Refused;
            if (absent) {
                cause = OffCause::Absent;
            } else if (transport) {
                cause = OffCause::Transport;
            }
            off(cause, reason);
            if (*shared) {
                (*shared)(false);
            }
        });
}

void CustomScreenSync::off(OffCause cause, std::string reason) {
    _offer = CustomScreensOffer::Off;
    _offCause = cause;
    // A server without the model is the ordinary way to say custom screens are not offered; anything else is a
    // misconfiguration or an outage, which someone has to hear about.
    if (cause == OffCause::Refused) {
        ::morph::log::logWarn("custom screens are off: the server refused `{}`: {}", kCustomScreensList, reason);
    } else if (cause == OffCause::Unreadable) {
        ::morph::log::logWarn("custom screens are off: the server's reply to `{}` is not a custom screens list: {}",
                              kCustomScreensList, reason);
    } else if (cause == OffCause::Transport) {
        ::morph::log::logWarn(
            "custom screens are off: `{}` did not reach the server ({}); the next reconnect asks again",
            kCustomScreensList, reason);
    }
    _offRefusal = std::move(reason);
}

void CustomScreenSync::relist() {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    // One private channel carries every call, so this list's reply arrives after the replies of the calls made before
    // it and before those made after it.
    call(
        kCustomScreensList, model::ActionTraits<CustomScreensList>::toJson(CustomScreensList{.app = _application}),
        [this](std::string const& reply) {
            try {
                listedNow(model::ActionTraits<CustomScreensList>::resultFromJson(reply));
            } catch (std::exception const& error) {
                ::morph::log::logWarn("custom screens: not a list from `{}`: {}", kCustomScreensList, error.what());
            }
        },
        [](std::string const& reason, std::string const&) {
            ::morph::log::logWarn("custom screens: `{}` was refused, the last list is kept: {}", kCustomScreensList,
                                  reason);
        });
}

void CustomScreenSync::listedNow(CustomScreensListed listed) {
    std::vector<std::string> const dropped = _documents.refresh(listed);
    _listed = std::move(listed);
    if (_onListed) {
        _onListed(_listed, dropped);
    }
}

void CustomScreenSync::open(std::string const& screenId, CustomDocumentDone done) {
    if (std::shared_ptr<Document const> kept = _documents.find(screenId)) {
        done(std::move(kept));
        return;
    }
    fetch(screenId, std::move(done));
}

void CustomScreenSync::fetch(std::string const& screenId, CustomDocumentDone done) {
    auto const shared = std::make_shared<CustomDocumentDone>(std::move(done));
    get(screenId, [this, shared](std::expected<CustomScreenFound, std::string> found) {
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

void CustomScreenSync::get(std::string const& screenId,
                           std::function<void(std::expected<CustomScreenFound, std::string>)> done) {
    auto const shared =
        std::make_shared<std::function<void(std::expected<CustomScreenFound, std::string>)>>(std::move(done));
    call(
        kCustomScreensGet,
        model::ActionTraits<CustomScreensGet>::toJson(CustomScreensGet{.app = _application, .id = screenId}),
        [shared](std::string const& reply) {
            try {
                (*shared)(model::ActionTraits<CustomScreensGet>::resultFromJson(reply));
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a custom screen: "} + error.what()));
            }
        },
        [shared](std::string const& reason, std::string const&) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::check(std::string basedOn, std::string document,
                             std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)> done) {
    auto const shared = std::make_shared<std::function<void(std::expected<std::optional<CheckRefusal>, std::string>)>>(
        std::move(done));
    call(
        kCustomScreensCheck,
        model::ActionTraits<CustomScreensCheck>::toJson(
            CustomScreensCheck{.app = _application, .basedOn = std::move(basedOn), .document = compactJson(document)}),
        [shared](std::string const& reply) {
            try {
                (*shared)(model::ActionTraits<CustomScreensCheck>::resultFromJson(reply).refusal);
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a check: "} + error.what()));
            }
        },
        [shared](std::string const& reason, std::string const&) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::save(CustomScreensSave request,
                            std::function<void(std::expected<std::string, std::string>)> done) {
    auto const shared =
        std::make_shared<std::function<void(std::expected<std::string, std::string>)>>(std::move(done));
    request.app = _application;
    request.document = compactJson(request.document);
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
        [shared](std::string const& reason, std::string const&) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::remove(std::string const& screenId,
                              std::function<void(std::expected<void, std::string>)> done) {
    auto const shared = std::make_shared<std::function<void(std::expected<void, std::string>)>>(std::move(done));
    call(
        kCustomScreensRemove,
        model::ActionTraits<CustomScreensRemove>::toJson(CustomScreensRemove{.app = _application, .id = screenId}),
        [this, shared, screenId](std::string const& reply) {
            try {
                static_cast<void>(model::ActionTraits<CustomScreensRemove>::resultFromJson(reply));
            } catch (std::exception const& error) {
                (*shared)(std::unexpected(std::string{"not a removal: "} + error.what()));
                return;
            }
            _documents.forget(screenId);
            relist();
            (*shared)(std::expected<void, std::string>{});
        },
        [shared](std::string const& reason, std::string const&) { (*shared)(std::unexpected(reason)); });
}

void CustomScreenSync::prefetchRoutes(std::vector<std::string> const& routes,
                                      std::function<void(std::vector<std::string>)> done) {
    std::vector<std::string> wanted;
    for (std::string const& route : routes) {
        std::string const screen{routeScreen(route)};
        if (isCustomScreenId(screen) && _documents.find(screen) == nullptr &&
            std::ranges::find(wanted, screen) == wanted.end()) {
            wanted.push_back(screen);
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

void CustomScreenSync::reconnected() {
    if (_offer == CustomScreensOffer::Off && _offCause != OffCause::Absent) {
        probe();
        return;
    }
    relist();
}

void CustomScreenSync::deliverEvent(wire::EventBody const& event) {
    if (_offer != CustomScreensOffer::Offered) {
        return;
    }
    if (event.topic == wire::kResyncTopic) {
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
    if (_offer == CustomScreensOffer::Offered && _session != nullptr) {
        _session->removeExtraWatch(std::string{kCustomScreenRecord});
    }
    _scope = std::make_unique<async::CallbackScope>();
    _channel.reset();
    _offer = CustomScreensOffer::Unknown;
    _offRefusal.clear();
    _offCause = OffCause::Absent;
    _listed = {};
    _documents.clear();
}

}  // namespace morph::ui
