// SPDX-License-Identifier: Apache-2.0

#include "qt_quick/relay.hpp"

#include <exception>
#include <morph/core/logger.hpp>
#include <utility>

#include "qt_quick/keys.hpp"

namespace morph::qt_quick::detail {

Relay::Relay(QObject* parent) : QObject{parent} {}

void Relay::on(QString const& event, Handler handler) {
    if (handler) {
        _handlers.insert_or_assign(event, std::move(handler));
    } else {
        _handlers.erase(event);
    }
}

void Relay::onAny(std::function<void(QString const&, QVariant const&)> handler) { _any = std::move(handler); }

void Relay::answer(QString const& question, Question handler) {
    if (handler) {
        _questions.insert_or_assign(question, std::move(handler));
    } else {
        _questions.erase(question);
    }
}

void Relay::kill() noexcept { _dead = true; }

void Relay::fire(QString const& event, QVariant const& payload) {
    if (_dead) {
        return;
    }
    auto const found = _handlers.find(event);
    // A copy: the handler may destroy the widget, and with it this map.
    Handler handler;
    if (found != _handlers.end()) {
        handler = found->second;
    } else if (_any) {
        handler = [any = _any, event](QVariant const& value) { any(event, value); };
    } else {
        return;
    }
    try {
        handler(payload);
    } catch (std::exception const& error) {
        morph::log::logError("morph::qt_quick: a '" + toUtf8(event) + "' handler threw: " + error.what());
    } catch (...) {
        morph::log::logError("morph::qt_quick: a '" + toUtf8(event) + "' handler threw");
    }
}

bool Relay::ask(QString const& question, QVariant const& payload) {
    if (_dead) {
        return false;
    }
    auto const found = _questions.find(question);
    if (found == _questions.end()) {
        return false;
    }
    Question const handler = found->second;
    try {
        return handler(payload);
    } catch (std::exception const& error) {
        morph::log::logError("morph::qt_quick: answering '" + toUtf8(question) + "' threw: " + error.what());
    } catch (...) {
        morph::log::logError("morph::qt_quick: answering '" + toUtf8(question) + "' threw");
    }
    return false;
}

}  // namespace morph::qt_quick::detail
