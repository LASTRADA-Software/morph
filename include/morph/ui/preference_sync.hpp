// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../core/callback_scope.hpp"
#include "../core/completion.hpp"
#include "document.hpp"
#include "interpreter.hpp"
#include "preferences.hpp"
#include "value.hpp"

/// @file
/// @brief `PreferenceSync`: the client side of the preferences model. It loads a mounted screen's stored choices
///        into `Screen::setPreference`, and writes what `InterpreterHost::preferenceChanged` and `resetPrefs` report.
///
/// Specified in `docs/spec/ui/preferences.md`.

namespace morph::ui {

/// @brief What loading a screen's choices did.
struct PreferencesLoaded {
    /// @brief Stored choices the screen took.
    std::size_t applied = 0;
    /// @brief Stored choices the screen ignored: text that is not a value of the point's type, or a value the point
    ///        no longer admits. The point keeps the document's default.
    std::size_t ignored = 0;
};

/// @brief Talks to the server's preferences model (`morph.preferences`) for one application, through the
///        interpreter's `Dispatcher`.
///
/// An application's `InterpreterHost` holds one, calls `load` after it mounts a screen, and forwards
/// `preferenceChanged` to `changed` and `resetPrefs` to `reset`. No call names a person: the server keys every entry
/// by the verified session's principal. All calls are made on the runtime's owner, where their replies arrive.
class PreferenceSync {
public:
    /// @brief A sync for @p application.
    /// @param dispatcher  Where model calls go. Borrowed: it must outlive the sync.
    /// @param application The application's id, the `app` of every key.
    /// @param onError     Told the reason when a write fails; empty to ignore failures.
    PreferenceSync(Dispatcher& dispatcher MORPH_LIFETIMEBOUND, std::string application,
                   std::function<void(std::string const&)> onError = {})
        : _dispatcher{&dispatcher}, _application{std::move(application)}, _onError{std::move(onError)} {}

    /// @brief Loads @p screen's stored choices, in one call for all its points, and sets each that fits.
    ///
    /// A point with no stored entry keeps the document's default. A stored entry that does not parse as the point's
    /// type, or that the point refuses (`Screen::setPreference`), is ignored, so a new document version never breaks
    /// on an old choice. A screen with no points makes no call and reports at once.
    /// @param screen The mounted screen. Borrowed until the reply is delivered or @p scope stops.
    /// @param scope  Gates the delivery: stop or destroy it with the screen, and nothing touches the screen after.
    /// @param done   Called once with what was applied and ignored, or the reason the call failed; may be empty.
    void load(Screen& screen, async::CallbackScope const& scope,
              std::function<void(std::expected<PreferencesLoaded, std::string>)> done = {}) {
        Document const& doc = screen.document();
        PrefLoad request{.app = _application, .scope = doc.id, .points = {}};
        for (auto const& [name, point] : doc.customize) {
            static_cast<void>(point);
            request.points.push_back(name);
        }
        _points.insert_or_assign(doc.id, request.points);
        if (request.points.empty()) {
            if (done) {
                done(PreferencesLoaded{});
            }
            return;
        }
        auto const shared =
            std::make_shared<std::function<void(std::expected<PreferencesLoaded, std::string>)>>(std::move(done));
        channel()
            ->execute("load", model::ActionTraits<PrefLoad>::toJson(request), CallOptions{})
            .then(scope,
                  [&screen, shared](std::string const& reply) {
                      auto applied = apply(screen, reply);
                      if (*shared) {
                          (*shared)(std::move(applied));
                      }
                  })
            .onError(scope, [shared](std::exception_ptr const& error) {
                if (*shared) {
                    (*shared)(std::unexpected(detail::failureText(error)));
                }
            });
    }

    /// @brief Stores the user's choice at a point.
    /// @param screen The screen's id.
    /// @param point  The point.
    /// @param value  The choice.
    void changed(std::string const& screen, std::string const& point, Value const& value) {
        PrefSet const request{.app = _application,
                              .scope = screen,
                              .point = point,
                              .value = glz::write_json(valueToJson(value)).value_or("null")};
        report(channel()->execute("set", model::ActionTraits<PrefSet>::toJson(request), CallOptions{}));
    }

    /// @brief Forgets the stored choice at a point, or at every point of a screen this sync has loaded.
    /// @param screen The screen's id.
    /// @param point  The point, or `all`.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): InterpreterHost::resetPrefs's order.
    void reset(std::string const& screen, std::string const& point) {
        std::vector<std::string> points;
        if (point == "all") {
            if (auto const found = _points.find(screen); found != _points.end()) {
                points = found->second;
            }
        } else {
            points.push_back(point);
        }
        for (std::string& each : points) {
            PrefRemove const request{.app = _application, .scope = screen, .point = std::move(each)};
            report(channel()->execute("remove", model::ActionTraits<PrefRemove>::toJson(request), CallOptions{}));
        }
    }

private:
    /// @brief The binding, made at the first call.
    /// @return It.
    [[nodiscard]] std::shared_ptr<ModelChannel> const& channel() {
        if (_channel == nullptr) {
            _channel = _dispatcher->bind(std::string{kPreferencesModel}, bridge::BindSharing::Private, {});
        }
        return _channel;
    }

    /// @brief Tells `onError` when a write fails.
    /// @param call The write.
    void report(async::Completion<std::string> call) const {
        call.thenDetached([](std::string const&) {})
            .onErrorDetached([onError = _onError](std::exception_ptr const& error) {
                if (onError) {
                    onError(detail::failureText(error));
                }
            });
    }

    /// @brief Sets each stored choice of a `load` reply that fits its point.
    /// @param screen The screen.
    /// @param reply  The reply's JSON text.
    /// @return What was applied and ignored, or the reason the reply is not one.
    [[nodiscard]] static std::expected<PreferencesLoaded, std::string> apply(Screen& screen,
                                                                             std::string const& reply) {
        PrefValues values;
        try {
            values = model::ActionTraits<PrefLoad>::resultFromJson(reply);
        } catch (std::exception const& error) {
            return std::unexpected(std::string{"not a preferences reply: "} + error.what());
        }
        PreferencesLoaded loaded;
        auto const& points = screen.document().customize;
        for (auto& [name, text] : values.values) {
            auto const point = points.find(name);
            if (point == points.end() || !take(screen, point->second, text)) {
                ++loaded.ignored;
                continue;
            }
            ++loaded.applied;
        }
        return loaded;
    }

    /// @brief Parses a stored choice as its point's value and gives it to the screen.
    /// @param screen The screen.
    /// @param point  The point.
    /// @param text   The stored JSON text.
    /// @return Whether the screen took it.
    [[nodiscard]] static bool take(Screen& screen, CustomizePoint const& point, std::string const& text) {
        auto const json = parseJson(text);
        if (!json) {
            return false;
        }
        bool const typed = point.kind == "choice" || point.kind == "value";
        auto value = typed ? valueFromJson(*json, point.type) : valueFromJson(*json);
        return value && screen.setPreference(point.name, std::move(*value));
    }

    Dispatcher* _dispatcher;
    std::string _application;
    std::function<void(std::string const&)> _onError;
    std::shared_ptr<ModelChannel> _channel;
    /// @brief The points of each screen loaded so far, by screen id, so a reset of `all` knows what to remove.
    std::map<std::string, std::vector<std::string>, std::less<>> _points;
};

}  // namespace morph::ui
