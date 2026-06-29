// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QWidget>

namespace bankgui {

/// @brief Base for a navigable content page. `refresh()` is called by the main
///        window whenever the page becomes visible, so each page reloads its
///        data from the models on demand.
class Page : public QWidget {
public:
    using QWidget::QWidget;
    virtual void refresh() {}
};

}  // namespace bankgui
