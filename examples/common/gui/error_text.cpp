// SPDX-License-Identifier: Apache-2.0

#include "gui/error_text.hpp"

namespace morph::ladder::gui {

QString errorText(const std::exception_ptr& err) noexcept {
    if (err == nullptr) {
        return QStringLiteral("unknown error");
    }
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& exc) {
        // fromUtf8, not fromStdString: the two are the same call in Qt 6
        // (fromStdString forwards to fromUtf8), and naming the encoding makes
        // it explicit that `what()` is treated as UTF-8 rather than as the
        // system locale. The ladder previously used both spellings
        // interchangeably; a test pins that they agree.
        return QString::fromUtf8(exc.what());
    } catch (...) {
        return QStringLiteral("unknown error");
    }
}

}  // namespace morph::ladder::gui
