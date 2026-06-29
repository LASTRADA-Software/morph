// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QWidget>

#include <functional>

#include "BankClient.hpp"
#include "Page.hpp"

class QStackedWidget;
class QVBoxLayout;
class QLabel;
class QButtonGroup;

namespace bankgui {

/// @brief The signed-in shell: a sidebar of navigation buttons, a header, and a
///        stacked content area of `Page`s. Each page is refreshed when shown.
class MainWindow : public QWidget {
public:
    explicit MainWindow(BankClient& client, QWidget* parent = nullptr);

    /// Invoked when the user clicks "Log out".
    std::function<void()> onLogout;

    /// @brief Switches to page @p index (also used by the screenshot smoke test).
    void selectPage(int index) { showPage(index); }
    [[nodiscard]] int pageCount() const { return _pageCount; }

private:
    void addPage(const QString& title, Page* page);
    void showPage(int index);

    BankClient& _client;
    QVBoxLayout* _navLayout{};
    QButtonGroup* _navGroup{};
    QStackedWidget* _stack{};
    QLabel* _title{};
    int _pageCount{0};
};

}  // namespace bankgui
