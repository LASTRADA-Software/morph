// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/tour_registry.hpp>
#include <morph/ui/tour_sync.hpp>
#include <string>
#include <utility>
#include <vector>

namespace morph::ui {

TourSync::TourSync(Dispatcher& dispatcher, Session* session) : _dispatcher{&dispatcher}, _session{session} {}

TourSync::~TourSync() {
    if (_offered && _session != nullptr) {
        _session->removeExtraWatch(std::string{kTourRecord});
    }
}

void TourSync::call(std::function<void(std::string const&)> onReply,
                    std::function<void(std::string const&, std::string const&)> const& failed) {
    try {
        if (_channel == nullptr) {
            _channel = _dispatcher->bind(std::string{kToursModel}, bridge::BindSharing::Private, {});
        }
        _channel->execute(std::string{kToursList}, model::ActionTraits<ToursList>::toJson(ToursList{}), CallOptions{})
            .then(*_scope, [onReply = std::move(onReply)](std::string const& reply) { onReply(reply); })
            .onError(*_scope, [failed](std::exception_ptr const& error) {
                failed(detail::failureText(error), detail::defaultErrorKind(error));
            });
    } catch (std::exception const& error) {
        failed(error.what(), "error");
    }
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the continuations the chain registers are owned by the scope.

void TourSync::listed(std::string const& reply) {
    ToursListed answer;
    try {
        answer = model::ActionTraits<ToursList>::resultFromJson(reply);
    } catch (std::exception const& error) {
        ::morph::log::logWarn("runtime tours: the reply to `{}` is not a tour list: {}", kToursList, error.what());
        return;
    }
    std::vector<TourDecl> tours;
    for (ListedTour const& listedTour : answer.tours) {
        auto loaded = loadTourText(listedTour.tour);
        if (!loaded) {
            ::morph::log::logWarn("runtime tours: `{}` does not load in this client, so it is left out: {}",
                                  listedTour.id, loaded.error().message());
            continue;
        }
        loaded->revision = listedTour.revision;
        tours.push_back(std::move(*loaded));
    }
    _tours = std::move(tours);
    if (_onListed) {
        _onListed(_tours);
    }
}

void TourSync::list(std::function<void(bool offered)> done) {
    auto const shared = std::make_shared<std::function<void(bool)>>(std::move(done));
    call(
        [this, shared](std::string const& reply) {
            if (!_offered) {
                _offered = true;
                _absent = false;
                if (_session != nullptr) {
                    _session->addExtraWatch(std::string{kTourRecord});
                }
            }
            listed(reply);
            if (*shared) {
                (*shared)(true);
            }
        },
        [this, shared](std::string const& reason, std::string const& /*kind*/) {
            _absent = reason.contains("unknown model type: " + std::string{kToursModel});
            if (_absent) {
                // A server without runtime tours is ordinary: the app document's tours are offered alone.
                if (!_absentLogged) {
                    _absentLogged = true;
                    ::morph::log::logDebug("runtime tours: the server offers none ({})", reason);
                }
            } else {
                ::morph::log::logWarn("runtime tours: `{}` failed, the last list is kept: {}", kToursList, reason);
            }
            if (*shared) {
                (*shared)(false);
            }
        });
}

void TourSync::relist(std::function<void()> done) {
    auto const shared = std::make_shared<std::function<void()>>(std::move(done));
    if (!_offered) {
        if (*shared) {
            (*shared)();
        }
        return;
    }
    call(
        [this, shared](std::string const& reply) {
            listed(reply);
            if (*shared) {
                (*shared)();
            }
        },
        [shared](std::string const& reason, std::string const& /*kind*/) {
            ::morph::log::logWarn("runtime tours: `{}` failed, the last list is kept: {}", kToursList, reason);
            if (*shared) {
                (*shared)();
            }
        });
}

void TourSync::reconnected() {
    if (_offered) {
        relist();
    } else if (!_absent) {
        list();
    }
}

void TourSync::deliverEvent(wire::EventBody const& event) {
    if (!_offered) {
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
    if (changes &&
        std::ranges::any_of(changes->records, [](auto const& change) { return change.record == kTourRecord; })) {
        relist();
    }
}

void TourSync::reset() {
    if (_offered && _session != nullptr) {
        _session->removeExtraWatch(std::string{kTourRecord});
    }
    _scope = std::make_unique<async::CallbackScope>();
    _channel.reset();
    _offered = false;
    _absent = false;
    _tours.clear();
}

}  // namespace morph::ui
