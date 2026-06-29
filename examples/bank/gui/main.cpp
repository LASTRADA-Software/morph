// SPDX-License-Identifier: Apache-2.0
//
// Entry point for the Qt 6 bank GUI. Wires a BankClient (local backend + Qt
// executor) and swaps between the login screen and the signed-in main window.

#include <QApplication>
#include <QStackedWidget>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "BankClient.hpp"
#include "LoginView.hpp"
#include "MainWindow.hpp"
#include "Smoke.hpp"
#include "Theme.hpp"

int main(int argc, char* argv[]) {
    QApplication app{argc, argv};
    app.setApplicationName(QStringLiteral("Morph Bank"));
    app.setStyleSheet(bankgui::theme::styleSheet());

    const auto dbPath = std::filesystem::temp_directory_path() / "morph_bank_gui.db";
    bankgui::BankClient client{"DRIVER=SQLite3;Database=" + dbPath.string()};

    auto* window = new QStackedWidget;
    window->setObjectName(QStringLiteral("Root"));
    window->setWindowTitle(QStringLiteral("Morph Bank"));
    window->resize(1160, 760);

    auto* login = new bankgui::LoginView{client};
    window->addWidget(login);

    login->onAuthenticated = [&client, window](const QString& principal, const QString& displayName) {
        client.login(principal, displayName);
        auto* main = new bankgui::MainWindow{client};
        const int index = window->addWidget(main);
        main->onLogout = [&client, window, main] {
            client.logout();
            window->setCurrentIndex(0);
            main->deleteLater();
        };
        window->setCurrentIndex(index);
    };

    window->setCurrentWidget(login);
    window->show();

    if (std::getenv("BANK_GUI_SMOKE") != nullptr) {
        bankgui::smoke::run(client, window, QString::fromUtf8(std::getenv("BANK_GUI_SMOKE")));
    }

    return app.exec();
}
