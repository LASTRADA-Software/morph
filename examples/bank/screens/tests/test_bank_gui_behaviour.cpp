// SPDX-License-Identifier: Apache-2.0
//
// bank's GUI behaviour, driven as a UI-automation suite drives it (spec 6 §12, "Port check"): the generated Move
// money screen mounted by the Qt Quick renderer in a shown offscreen window, in local mode, and every node found
// through QAccessible by its accessible name, its role or its test id. Nothing reads a QML item's properties or
// names a QML component: a press is the node's press action, typing goes to the node the focus action focused, and
// what the screen says is the name a driver reads.

#include <QAccessible>
#include <QAccessibleActionInterface>
#include <QAccessibleInterface>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <QTest>
#include <algorithm>
#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <expected>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/qt_quick/backend.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/screen_harness.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

#include "bank_screens.hpp"
#include "bank_test_support.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::client::LocalSource;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

LocalApp bankApp() {
    return LocalApp{.name = "bank", .bundle = &bank::screens::bundle, .setUp = [](LocalSetup const&) {
                        bank::testing::ensureDatabase();
                    }};
}

/// A principal no other test uses: the tests share one database.
std::string uniqueUser(std::string_view stem) {
    static std::atomic<int> counter{0};
    return std::string{stem} + "-" + std::to_string(++counter);
}

/// One node of the accessibility tree, as a driver sees it.
struct Node {
    QAccessibleInterface* iface = nullptr;
    QString name;
    QAccessible::Role role = QAccessible::NoRole;
    QString testId;
};

// NOLINTNEXTLINE(misc-no-recursion): an accessibility tree, walked down.
void walk(QAccessibleInterface* iface, std::vector<Node>& out) {
    if (iface == nullptr || !iface->isValid()) {
        return;
    }
    QObject const* const object = iface->object();
    out.push_back(Node{.iface = iface,
                       .name = iface->text(QAccessible::Name),
                       .role = iface->role(),
                       .testId = object != nullptr ? object->objectName() : QString{}});
    for (int index = 0; index < iface->childCount(); ++index) {
        walk(iface->child(index), out);
    }
}

/// Every node under @p root, itself included, depth first.
std::vector<Node> under(QAccessibleInterface* root) {
    std::vector<Node> out;
    walk(root, out);
    return out;
}

/// The nodes of @p nodes that @p pred matches.
template <std::predicate<Node const&> Pred>
std::vector<Node> matching(std::vector<Node> const& nodes, Pred pred) {
    std::vector<Node> found;
    std::ranges::copy_if(nodes, std::back_inserter(found), std::move(pred));
    return found;
}

/// The one node of @p nodes that @p pred matches, or nothing when none or several do.
template <std::predicate<Node const&> Pred>
std::optional<Node> single(std::vector<Node> const& nodes, Pred pred) {
    std::vector<Node> const found = matching(nodes, std::move(pred));
    return found.size() == 1 ? std::optional<Node>{found.front()} : std::nullopt;
}

/// Looks a node up by its test id.
auto testIdIs(QString testId) {
    return [testId = std::move(testId)](Node const& node) { return node.testId == testId; };
}

/// Looks a node up by its test id and its role.
auto testIdAndRoleAre(QString testId, QAccessible::Role role) {
    return [testId = std::move(testId), role](Node const& node) { return node.testId == testId && node.role == role; };
}

/// Looks a node up by its accessible name and its role, the primary route.
auto nameAndRoleAre(QString name, QAccessible::Role role) {
    return [name = std::move(name), role](Node const& node) { return node.name == name && node.role == role; };
}

/// Turns the accessibility machinery on, as an assistive technology connecting does: Qt Quick Controls fill in
/// their accessible properties only while it is on.
class ActiveAccessibility {
public:
    ActiveAccessibility() : _was{QAccessible::isActive()} { QAccessible::setActive(true); }
    ~ActiveAccessibility() { QAccessible::setActive(_was); }
    ActiveAccessibility(ActiveAccessibility const&) = delete;
    ActiveAccessibility& operator=(ActiveAccessibility const&) = delete;
    ActiveAccessibility(ActiveAccessibility&&) = delete;
    ActiveAccessibility& operator=(ActiveAccessibility&&) = delete;

private:
    bool _was;
};

/// One of bank's screens, opened through the generic client's source and shell, on the Qt Quick backend in a shown
/// window, and driven through QAccessible.
class AutomatedScreen {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the screen, then the principal it runs as.
    AutomatedScreen(std::string screenId, std::string const& user)
        : _source{bankApp(), LocalSetup{.db = {}, .user = user}} {
        _window.resize(1000, 1400);
        _window.show();
        if (!QTest::qWaitForWindowExposed(&_window)) {
            throw std::runtime_error{"AutomatedScreen: the window was not exposed"};
        }
        _window.requestActivate();
        static_cast<void>(QTest::qWaitForWindowActive(&_window));
        std::optional<std::expected<morph::ui::Bundle, morph::ui::ConnectError>> opened;
        _source.open(_context, [&opened](std::expected<morph::ui::Bundle, morph::ui::ConnectError> result) {
            opened = std::move(result);
        });
        if (!waitFor([&opened] { return opened.has_value(); }) || !opened->has_value()) {
            throw std::runtime_error{"AutomatedScreen: the application did not open"};
        }
        _shell = morph::client::mountShell(
            _context, _backend, std::move(*opened), _source,
            morph::client::ShellOptions{
                .startScreen = std::move(screenId),
                .onProblem = [this](std::string const& problem) { _problems.push_back(problem); },
                .onMounted = [this](std::string const& mounted) { _mounted.push_back(mounted); }});
        if (_mounted.empty()) {
            throw std::runtime_error{"AutomatedScreen: no screen mounted" +
                                     (_problems.empty() ? std::string{} : ": " + _problems.front())};
        }
        settle();
    }

    ~AutomatedScreen() {
        _shell.reset();
        _source.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
    AutomatedScreen(AutomatedScreen const&) = delete;
    AutomatedScreen& operator=(AutomatedScreen const&) = delete;
    AutomatedScreen(AutomatedScreen&&) = delete;
    AutomatedScreen& operator=(AutomatedScreen&&) = delete;

    [[nodiscard]] std::vector<std::string> const& problems() const noexcept { return _problems; }

    /// Every node of the window, from its own accessible interface.
    [[nodiscard]] std::vector<Node> tree() { return under(QAccessible::queryAccessibleInterface(&_window)); }

    /// The one node with test id @p testId, or nothing.
    [[nodiscard]] std::optional<Node> byTestId(QString const& testId) { return single(tree(), testIdIs(testId)); }

    /// The one node inside the node with test id @p scope that @p pred matches, or nothing.
    template <std::predicate<Node const&> Pred>
    [[nodiscard]] std::optional<Node> within(QString const& scope, Pred pred) {
        std::optional<Node> const root = byTestId(scope);
        if (!root) {
            return std::nullopt;
        }
        return single(under(root->iface), std::move(pred));
    }

    /// Runs the node's press action, as a driver presses a button.
    void press(Node const& node) {
        QAccessibleActionInterface* const actions = node.iface->actionInterface();
        REQUIRE(actions != nullptr);
        REQUIRE(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
        actions->doAction(QAccessibleActionInterface::pressAction());
        settle();
    }

    /// Focuses the node through its focus action, as a driver does before it types.
    void focus(Node const& node) {
        QAccessibleActionInterface* const actions = node.iface->actionInterface();
        REQUIRE(actions != nullptr);
        REQUIRE(actions->actionNames().contains(QAccessibleActionInterface::setFocusAction()));
        actions->doAction(QAccessibleActionInterface::setFocusAction());
        settle();
        REQUIRE(node.iface->state().focused);
    }

    /// Types @p text into whatever has focus.
    void type(QString const& text) {
        for (QChar const character : text) {
            QTest::keyClick(&_window, character.toLatin1());
        }
        settle();
    }

    /// Selects all the text of whatever has focus, so what is typed next replaces it.
    void selectAll() {
        QTest::keyClick(&_window, Qt::Key_A, Qt::ControlModifier);
        settle();
    }

    /// Presses @p key on whatever has focus.
    void key(Qt::Key key) {
        QTest::keyClick(&_window, key);
        settle();
    }

    /// Runs the owner's work and Qt's events until @p pred holds or the budget runs out.
    template <std::predicate Pred>
    [[nodiscard]] bool waitFor(Pred pred, std::chrono::milliseconds budget = morph::examples::testing::kWaitBudget) {
        auto const deadline = morph::examples::testing::detail::deadlineFrom(budget);
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            _context.owner().runFor(5ms);
            QCoreApplication::processEvents();
        }
        return true;
    }

    /// Runs whatever is ready, on the owner and in Qt.
    void settle() {
        for (int round = 0; round < 20; ++round) {
            _context.owner().runFor(1ms);
            QCoreApplication::processEvents();
        }
    }

    /// The tree as text, for a failure's message.
    [[nodiscard]] std::string dump() {
        std::string text;
        for (Node const& node : tree()) {
            text += std::to_string(static_cast<int>(node.role)) + " [" + node.name.toStdString() + "] " +
                    node.testId.toStdString() + "\n";
        }
        return text;
    }

private:
    ActiveAccessibility _active;
    FakeAppContext _context;
    QQuickWindow _window;
    QQmlEngine _engine;
    morph::qt_quick::Backend _backend{_engine, *_window.contentItem()};
    LocalSource _source;
    std::unique_ptr<morph::ui::MountedShell> _shell;
    std::vector<std::string> _problems;
    std::vector<std::string> _mounted;
};

/// Opens one account through the accounts screen's form.
void openAccount(ScreenHarness& harness, std::size_t count) {
    harness.editField("openForm", "kind", "0");
    harness.editField("openForm", "currency", "0");
    harness.editField("openForm", "overdraftMinor", "0");
    REQUIRE(harness.submitForm("openForm"));
    REQUIRE(harness.waitFor([&harness, count] {
        auto const select = harness.find("Select", "historyAccount");
        if (!select) {
            return false;
        }
        std::string const options = harness.backend().prop(*select, "options");
        return std::cmp_equal(std::ranges::count(options, ':'), count);
    }));
}

/// The account number a picker's option names: its label up to " (".
QString numberIn(QString const& label) { return label.section(u" ("_s, 0, 0); }

/// The option a drop-down shows: the value of its display child, because a combo box node has no value of its own.
QString shownBy(Node const& combo) {
    for (int index = 0; index < combo.iface->childCount(); ++index) {
        QAccessibleInterface const* const child = combo.iface->child(index);
        if (child != nullptr && child->role() == QAccessible::EditableText) {
            return child->text(QAccessible::Value);
        }
    }
    return {};
}

}  // namespace

// Mutation: a new option list does not mark the chosen key again (after the deposit's refetch the picker no
// longer names the account the user chose).
TEST_CASE("bank GUI: Move money's picker keeps naming the account the next deposit lands in",
          "[bank][gui][automation][move-money]") {
    std::string const user = uniqueUser("gui-move");
    bank::testing::ensurePrincipal(user);
    {
        ScreenHarness accounts{
            bankApp(), std::string{bank::screens::kAccounts}, RigMode::Local, {}, LocalSetup{.db = {}, .user = user}};
        REQUIRE(accounts.waitFor([&accounts] { return accounts.find("Column", "openForm").has_value(); }));
        openAccount(accounts, 1);
        openAccount(accounts, 2);
    }

    AutomatedScreen screen{std::string{bank::screens::kMove}, user};
    INFO(screen.dump());

    // The picker: the combo box named by its label, the same node as its test id; the option it shows is its value.
    auto const picker = [&screen] {
        return single(screen.tree(), nameAndRoleAre(u"Account"_s, QAccessible::ComboBox));
    };
    auto const chosen = [&picker] { return numberIn(shownBy(*picker())); };
    auto const balance = [&screen] {
        std::optional<Node> const node = screen.byTestId(u"balance"_s);
        return node ? node->name : QString{};
    };
    REQUIRE(picker().has_value());
    std::optional<Node> const byTestId = single(screen.tree(), testIdAndRoleAre(u"account"_s, QAccessible::ComboBox));
    REQUIRE(byTestId.has_value());
    CHECK(byTestId->iface == picker()->iface);

    // Down from no choice is the first account, once the accounts have arrived; Down again the second.
    screen.focus(*picker());
    REQUIRE(screen.waitFor([&screen, &balance] {
        screen.key(Qt::Key_Down);
        return balance() == u"Balance 0.00"_s;
    }));
    QString const first = chosen();
    screen.key(Qt::Key_Down);
    QString const second = chosen();
    REQUIRE(second != first);
    REQUIRE_FALSE(second.isEmpty());

    // A deposit through the deposit form: its amount field by name and role, its submit button by test id.
    auto const deposit = [&screen](QString const& cents) {
        std::optional<Node> const amount =
            screen.within(u"depositForm"_s, nameAndRoleAre(u"Amount (cents)"_s, QAccessible::EditableText));
        REQUIRE(amount.has_value());
        screen.focus(*amount);
        screen.selectAll();
        screen.type(cents);
        std::optional<Node> const submit = screen.within(u"depositForm"_s, testIdIs(u"form_submit"_s));
        REQUIRE(submit.has_value());
        REQUIRE(screen.waitFor([&submit] { return !submit->iface->state().disabled; }));
        screen.press(*submit);
    };

    deposit(u"5000"_s);
    bool const deposited = screen.waitFor([&balance] { return balance() == u"Balance 50.00"_s; });
    for (std::string const& problem : screen.problems()) {
        UNSCOPED_INFO(problem);
    }
    CHECK(deposited);
    // After the refetch the deposit caused, the picker still names the account the user chose.
    CHECK(chosen() == second);

    // And it is not cosmetic: the next deposit lands in the account the picker names.
    QString const shown = chosen();
    deposit(u"1000"_s);
    CHECK(screen.waitFor([&balance] { return balance() == u"Balance 60.00"_s; }));
    CHECK(chosen() == shown);

    // The other account received nothing.
    screen.focus(*picker());
    screen.key(Qt::Key_Up);
    CHECK(chosen() == first);
    CHECK(screen.waitFor([&balance] { return balance() == u"Balance 0.00"_s; }));
    CHECK(screen.problems().empty());
}

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication const app{argc, argv};
    int const result = Catch::Session().run(argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    return result;
}
