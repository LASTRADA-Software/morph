// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QObject>
#include <QString>
#include <QVariant>
#include <functional>
#include <map>

namespace morph::qt_quick::detail {

/// The one object a MorphUi component reports user actions through: QML calls `host.fire(event, payload)` and
/// `host.ask(question, payload)`, and the relay calls the C++ handler registered under that name.
///
/// A handler runs as a copy, so it may destroy its own widget, and with it this relay's handlers. Once `kill()` has
/// run — the widget's destructor calls it — nothing is called any more, even for input QML delivers later. A
/// handler that throws is logged and goes no further: an exception never unwinds through Qt.
class Relay final : public QObject {
    Q_OBJECT

public:
    /// What `fire` calls.
    using Handler = std::function<void(QVariant const&)>;
    /// What `ask` calls.
    using Question = std::function<bool(QVariant const&)>;

    explicit Relay(QObject* parent = nullptr);

    /// Registers the handler for @p event, replacing any earlier one; an empty one removes it.
    void on(QString const& event, Handler handler);

    /// Registers what `fire` calls for an event with no handler of its own, with the event's name.
    void onAny(std::function<void(QString const&, QVariant const&)> handler);

    /// Registers the answer to @p question, replacing any earlier one; an empty one answers false.
    void answer(QString const& question, Question handler);

    /// Releases every handler; from now on, `fire` calls nothing and `ask` answers false.
    void kill() noexcept;

    /// Whether `kill` has run.
    [[nodiscard]] bool dead() const noexcept { return _dead; }

    /// A user action: calls the handler registered for @p event with @p payload.
    Q_INVOKABLE void fire(QString const& event, QVariant const& payload = {});

    /// A question QML needs C++ to answer, such as whether a drop target takes a key.
    Q_INVOKABLE bool ask(QString const& question, QVariant const& payload = {});

private:
    std::map<QString, Handler> _handlers;
    std::map<QString, Question> _questions;
    std::function<void(QString const&, QVariant const&)> _any;
    bool _dead = false;
};

}  // namespace morph::qt_quick::detail
