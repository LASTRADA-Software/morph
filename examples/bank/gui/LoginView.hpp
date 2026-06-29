// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <QWidget>

#include <functional>

#include "BankClient.hpp"
#include "bank/models/auth_model.hpp"

class QLineEdit;
class QLabel;

namespace bankgui {

/// @brief Sign-in / create-account screen. Emits the authenticated principal
///        via the `onAuthenticated` callback.
class LoginView : public QWidget {
public:
    explicit LoginView(BankClient& client, QWidget* parent = nullptr);

    /// Invoked with (principal, displayName) once sign-in/registration succeeds.
    std::function<void(QString, QString)> onAuthenticated;

private:
    void attemptLogin();
    void attemptRegister();
    void setError(const QString& message);

    BankClient& _client;
    morph::bridge::BridgeHandler<bank::AuthModel> _auth;
    QLineEdit* _username{};
    QLineEdit* _password{};
    QLineEdit* _displayName{};
    QLabel* _error{};
};

}  // namespace bankgui
