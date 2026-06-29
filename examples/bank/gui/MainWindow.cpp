// SPDX-License-Identifier: Apache-2.0

#include "MainWindow.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "Ui.hpp"
#include "views/AccountsView.hpp"
#include "views/CardsView.hpp"
#include "views/LoansView.hpp"
#include "views/MoveMoneyView.hpp"
#include "views/PayeesView.hpp"

namespace bankgui {

MainWindow::MainWindow(BankClient& client, QWidget* parent) : QWidget(parent), _client{client} {
    setObjectName(QStringLiteral("Root"));
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Sidebar ──────────────────────────────────────────────────────────────
    auto* sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(232);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(0);

    side->addWidget(ui::label(QStringLiteral("Morph Bank"), QStringLiteral("Brand")));
    side->addWidget(ui::label(QStringLiteral("personal banking"), QStringLiteral("BrandSub")));

    _navGroup = new QButtonGroup(this);
    _navGroup->setExclusive(true);
    _navLayout = new QVBoxLayout;
    _navLayout->setContentsMargins(0, 8, 0, 8);
    _navLayout->setSpacing(0);
    side->addLayout(_navLayout);
    side->addStretch();

    auto* user = ui::label(_client.displayName(), QStringLiteral("SidebarUser"));
    auto* userSub = ui::label(QStringLiteral("@") + _client.principal(), QStringLiteral("SidebarUserSub"));
    side->addWidget(user);
    side->addWidget(userSub);
    auto* logout = ui::button(QStringLiteral("Log out"), QStringLiteral("ghost"));
    QObject::connect(logout, &QPushButton::clicked, this, [this] {
        if (onLogout) {
            onLogout();
        }
    });
    side->addWidget(logout, 0, Qt::AlignLeft);
    side->addSpacing(12);

    // ── Content area: header + stacked pages ──────────────────────────────────
    auto* content = new QVBoxLayout;
    content->setContentsMargins(32, 28, 32, 28);
    content->setSpacing(20);
    _title = ui::label(QString(), QStringLiteral("H1"));
    content->addWidget(_title);
    _stack = new QStackedWidget;
    content->addWidget(_stack, 1);

    root->addWidget(sidebar);
    root->addLayout(content, 1);

    // ── Pages ─────────────────────────────────────────────────────────────────
    addPage(QStringLiteral("Accounts"), new AccountsView(_client));
    addPage(QStringLiteral("Move Money"), new MoveMoneyView(_client));
    addPage(QStringLiteral("Cards"), new CardsView(_client));
    addPage(QStringLiteral("Payees & Bills"), new PayeesView(_client));
    addPage(QStringLiteral("Loans"), new LoansView(_client));

    showPage(0);
}

void MainWindow::addPage(const QString& title, Page* page) {
    const int index = _pageCount++;
    _stack->addWidget(page);

    // Double any '&' so QPushButton does not treat it as a mnemonic accelerator.
    auto* navButton = new QPushButton(QString(title).replace(QLatin1Char('&'), QStringLiteral("&&")));
    navButton->setObjectName(QStringLiteral("NavButton"));
    navButton->setCheckable(true);
    navButton->setCursor(Qt::PointingHandCursor);
    navButton->setProperty("pageTitle", title);
    _navGroup->addButton(navButton, index);
    _navLayout->addWidget(navButton);
    if (index == 0) {
        navButton->setChecked(true);
    }
    QObject::connect(navButton, &QPushButton::clicked, this, [this, index] { showPage(index); });
}

void MainWindow::showPage(int index) {
    _stack->setCurrentIndex(index);
    if (auto* button = _navGroup->button(index)) {
        button->setChecked(true);
        _title->setText(button->property("pageTitle").toString());
    }
    // Every stacked widget is a Page (added only via addPage).
    if (auto* page = static_cast<Page*>(_stack->widget(index))) {
        page->refresh();
    }
}

}  // namespace bankgui
