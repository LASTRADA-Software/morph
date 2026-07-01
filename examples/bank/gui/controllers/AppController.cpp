// SPDX-License-Identifier: Apache-2.0

#include "AppController.hpp"

#include "../BankClient.hpp"
#include "bank/dto/auth_dto.hpp"

namespace bankgui {

AppController::AppController(BankClient& client, QObject* parent)
    : BankController(client, parent), _auth{client.bridge(), client.gui()} {}

void AppController::adopt(const QString& principal, const QString& displayName) {
    _client.login(principal, displayName);
    _principal = principal;
    _displayName = displayName;
    _authenticated = true;
    emit authChanged();
}

void AppController::login(const QString& username, const QString& password) {
    _auth.execute(bank::dto::LoginRequest{.username = username.toStdString(),
                                          .password = password.toStdString()})
        .then([this](bank::dto::AuthResult result) {
            if (result.ok) {
                adopt(QString::fromStdString(result.principal),
                      QString::fromStdString(result.displayName));
            } else {
                emit error(QString::fromStdString(result.message));
            }
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void AppController::registerUser(const QString& username, const QString& password,
                                 const QString& displayName) {
    _auth.execute(bank::dto::RegisterUser{.username = username.toStdString(),
                                          .password = password.toStdString(),
                                          .displayName = displayName.toStdString()})
        .then([this](bank::dto::AuthResult result) {
            if (result.ok) {
                adopt(QString::fromStdString(result.principal),
                      QString::fromStdString(result.displayName));
            } else {
                emit error(QString::fromStdString(result.message));
            }
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void AppController::logout() {
    _client.logout();
    _authenticated = false;
    _principal.clear();
    _displayName.clear();
    emit authChanged();
}

}  // namespace bankgui
