// SPDX-License-Identifier: Apache-2.0
#include "gui/event_poller.hpp"

namespace morph::ladder::gui::detail {

bool isClientTimeout(const std::exception_ptr& err) noexcept {
    if (!err) {
        return false;
    }
    try {
        std::rethrow_exception(err);
    } catch (const ::morph::backend::ClientTimeoutError&) {
        return true;
    } catch (...) {
        return false;
    }
}

QString describeFailure(const std::exception_ptr& err) {
    if (!err) {
        return QStringLiteral("EventPoller: dispatch failed with no exception information");
    }
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        return QString::fromStdString(ex.what());
    } catch (...) {
        return QStringLiteral("EventPoller: dispatch failed with a non-std::exception");
    }
}

}  // namespace morph::ladder::gui::detail
