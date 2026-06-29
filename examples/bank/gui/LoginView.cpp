// SPDX-License-Identifier: Apache-2.0

#include "LoginView.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include "Ui.hpp"
#include "bank/dto/auth_dto.hpp"

namespace bankgui {

LoginView::LoginView(BankClient& client, QWidget* parent)
    : QWidget(parent), _client{client}, _auth{client.bridge(), client.gui()} {
    setObjectName(QStringLiteral("Root"));

    auto* card = ui::card();
    card->setFixedWidth(380);
    auto* form = new QVBoxLayout(card);
    form->setContentsMargins(32, 32, 32, 32);
    form->setSpacing(12);

    auto* brand = ui::label(QStringLiteral("Morph Bank"), QStringLiteral("H1"));
    auto* subtitle = ui::label(QStringLiteral("Sign in to your account"), QStringLiteral("Muted"));

    _username = new QLineEdit;
    _username->setPlaceholderText(QStringLiteral("Username"));
    _password = new QLineEdit;
    _password->setPlaceholderText(QStringLiteral("Password"));
    _password->setEchoMode(QLineEdit::Password);
    _displayName = new QLineEdit;
    _displayName->setPlaceholderText(QStringLiteral("Display name (for new accounts)"));

    _error = ui::label(QString(), QStringLiteral("Danger"));
    _error->setWordWrap(true);
    _error->hide();

    auto* signIn = ui::button(QStringLiteral("Sign in"), QStringLiteral("primary"));
    auto* create = ui::button(QStringLiteral("Create account"), QStringLiteral("ghost"));

    form->addWidget(brand);
    form->addWidget(subtitle);
    form->addSpacing(8);
    form->addWidget(_username);
    form->addWidget(_password);
    form->addWidget(_displayName);
    form->addWidget(_error);
    form->addSpacing(4);
    form->addWidget(signIn);
    form->addWidget(create, 0, Qt::AlignHCenter);

    QObject::connect(signIn, &QPushButton::clicked, this, [this] { attemptLogin(); });
    QObject::connect(create, &QPushButton::clicked, this, [this] { attemptRegister(); });
    QObject::connect(_password, &QLineEdit::returnPressed, this, [this] { attemptLogin(); });

    // Centre the card on the paper background.
    auto* outer = new QVBoxLayout(this);
    outer->addStretch();
    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(card);
    row->addStretch();
    outer->addLayout(row);
    outer->addStretch();
}

void LoginView::setError(const QString& message) {
    _error->setText(message);
    _error->setVisible(!message.isEmpty());
}

void LoginView::attemptLogin() {
    setError({});
    const QString user = _username->text().trimmed();
    _auth.execute(bank::dto::LoginRequest{.username = user.toStdString(),
                                          .password = _password->text().toStdString()})
        .then([this, user](bank::dto::AuthResult result) {
            if (result.ok && onAuthenticated) {
                onAuthenticated(QString::fromStdString(result.principal),
                                QString::fromStdString(result.displayName));
            } else if (!result.ok) {
                setError(QString::fromStdString(result.message));
            }
        })
        .onError([this](const std::exception_ptr& err) { setError(ui::errorText(err)); });
}

void LoginView::attemptRegister() {
    setError({});
    const QString user = _username->text().trimmed();
    _auth.execute(bank::dto::RegisterUser{.username = user.toStdString(),
                                          .password = _password->text().toStdString(),
                                          .displayName = _displayName->text().toStdString()})
        .then([this](bank::dto::AuthResult result) {
            if (result.ok && onAuthenticated) {
                onAuthenticated(QString::fromStdString(result.principal),
                                QString::fromStdString(result.displayName));
            } else if (!result.ok) {
                setError(QString::fromStdString(result.message));
            }
        })
        .onError([this](const std::exception_ptr& err) { setError(ui::errorText(err)); });
}

}  // namespace bankgui
