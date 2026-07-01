// SPDX-License-Identifier: Apache-2.0

#include "BankController.hpp"

namespace bankgui {

QString BankController::errorText(const std::exception_ptr& err) {
    try {
        if (err) {
            std::rethrow_exception(err);
        }
    } catch (const std::exception& exc) {
        return QString::fromUtf8(exc.what());
    } catch (...) {
        return QStringLiteral("unknown error");
    }
    return {};
}

}  // namespace bankgui
