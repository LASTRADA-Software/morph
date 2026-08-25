// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>

#include "BankController.hpp"

// Hidden from moc: moc follows includes and its parser trips on the heavy
// morph/Lightweight headers. The compiler still sees them.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>

#include "bank/models/auth_model.hpp"
#endif

namespace bankgui {

class BankClient;

/// @brief Authentication + session state, exposed to QML as `app`.
class AppController : public BankController {
    Q_OBJECT
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY authChanged)
    Q_PROPERTY(QString principal READ principal NOTIFY authChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY authChanged)

public:
    explicit AppController(BankClient& client, QObject* parent = nullptr);

    [[nodiscard]] bool authenticated() const { return _authenticated; }
    [[nodiscard]] QString principal() const { return _principal; }
    [[nodiscard]] QString displayName() const { return _displayName; }

    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void registerUser(const QString& username, const QString& password, const QString& displayName);
    Q_INVOKABLE void logout();

signals:
    void authChanged();

private:
    void adopt(const QString& principal, const QString& displayName);

    morph::bridge::BridgeHandler<bank::AuthModel> _auth;
    bool _authenticated{false};
    QString _principal;
    QString _displayName;
};

}  // namespace bankgui
