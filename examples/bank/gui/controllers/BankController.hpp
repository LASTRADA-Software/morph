// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <exception>

namespace bankgui {

class BankClient;

/// @brief Base for the QML-facing controllers. Holds the shared `BankClient`
///        and a common `error` signal that the QML shell surfaces as a toast.
class BankController : public QObject {
    Q_OBJECT
public:
    explicit BankController(BankClient& client, QObject* parent = nullptr) : QObject(parent), _client{client} {}

signals:
    void error(const QString& message);

protected:
    /// Extracts a human-readable message from a captured exception.
    static QString errorText(const std::exception_ptr& err);

    BankClient& _client;  // NOLINT(*-non-private-member-variables-in-classes)
};

}  // namespace bankgui
