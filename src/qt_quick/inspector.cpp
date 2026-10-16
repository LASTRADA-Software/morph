// SPDX-License-Identifier: Apache-2.0
//
// The development inspector's window (spec 6 §21). Built only when MORPH_UI_DEV_TOOLS is on: the build adds this file
// to morph_qt_quick only then, so a release client contains none of it.

#include <QByteArray>
#include <QEvent>
#include <QKeyEvent>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/qt_quick/inspector.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/ui/inspector.hpp>
#include <morph/ui/interpreter.hpp>
#include <string>
#include <utility>
#include <vector>

#include "qt_quick/inspector_model.hpp"
#include "qt_quick/keys.hpp"

namespace morph::qt_quick {

namespace {

// The window: the counters, the queries with a refetch button each, the nodes and the command log.
constexpr char const* kWindowQml = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    width: 760
    height: 680
    title: "morph inspector"
    ScrollView {
        anchors.fill: parent
        anchors.margins: 8
        ColumnLayout {
            width: parent.width
            spacing: 6
            Label { text: "Reactive runtime"; font.bold: true }
            Label { text: inspector.counters; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: "Queries and mutations"; font.bold: true }
            Repeater {
                model: inspector.queries
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        text: modelData.line
                        textFormat: Text.PlainText
                        font.family: "monospace"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Refetch"
                        visible: modelData.query
                        onClicked: inspector.refetch(modelData.name)
                    }
                }
            }
            Label { text: "Mounted nodes"; font.bold: true }
            Label { text: inspector.nodes; textFormat: Text.PlainText; font.family: "monospace" }
            Label { text: "Command log"; font.bold: true }
            Label { text: inspector.log; textFormat: Text.PlainText; font.family: "monospace" }
        }
    }
}
)";

// How often a shown window reads the screen again.
constexpr std::chrono::milliseconds kRefreshEvery{500};

std::string countersText(reactive::RuntimeStats const& stats) {
    std::string out = "flushes " + std::to_string(stats.flushes) + ", effects " + std::to_string(stats.effectsRun) +
                      " (last flush " + std::to_string(stats.lastFlushEffects) + ", most in one flush " +
                      std::to_string(stats.maxFlushEffects) + "), batches " + std::to_string(stats.batches) +
                      ", misuses " + std::to_string(stats.misuses);
    for (std::string const& site : stats.recentMisuses) {
        out += "\n  " + site;
    }
    return out;
}

std::string queryLine(ui::InspectedQuery const& query) {
    std::string out = query.name + " (" + query.scope + (query.mutation ? " mutation" : " query") + ") " +
                      query.model + "/" + query.action + "  " + query.state;
    if (query.age) {
        out += "  age " + std::to_string(query.age->count()) + " ms";
    }
    if (!query.key.empty()) {
        out += "  key " + query.key;
    }
    if (!query.lastError.empty()) {
        out += "  error: " + query.lastError;
    }
    return out;
}

std::string nodeLine(ui::InspectedNode const& node) {
    std::string out = node.path + "  " + node.kind;
    if (!node.id.empty()) {
        out += " #" + node.id;
    }
    if (!node.testId.empty()) {
        out += " [" + node.testId + "]";
    }
    for (auto const& [key, value] : node.properties) {
        out += "  " + key + "=" + ui::detail::canonicalJson(value);
    }
    return out;
}

std::string logLine(ui::CommandLogEntry const& entry) {
    std::string out = "batch " + std::to_string(entry.batch) + "  " + entry.event + ":";
    for (ui::LoggedCommand const& command : entry.commands) {
        out += " " + command.kind;
        if (!command.target.empty()) {
            out += " " + command.target;
        }
        out += ";";
    }
    return out;
}

/// Toggles the inspector on Ctrl+Shift+I, before any item of the window sees the key.
class Shortcut final : public QObject {
public:
    Shortcut(QObject* parent, std::function<void()> toggle) : QObject{parent}, _toggle{std::move(toggle)} {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            auto const* const key = dynamic_cast<QKeyEvent const*>(event);
            if (key != nullptr && detail::chordMatches("Ctrl+Shift+I", key->key(), key->modifiers())) {
                _toggle();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> _toggle;
};

}  // namespace

struct InspectorWindow::Impl {
    explicit Impl(InspectFocused source) : focused{std::move(source)} {}

    InspectFocused focused;
    // The model outlives the engine, whose context names it, and the engine outlives the window it made.
    std::unique_ptr<detail::InspectorModel> model;
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;
    QTimer timer;
    QPointer<Shortcut> shortcut;
    std::string text;
};

InspectorWindow::InspectorWindow(InspectFocused focused) : _impl{std::make_unique<Impl>(std::move(focused))} {
    _impl->model =
        std::make_unique<detail::InspectorModel>([this](QString const& query) { refetch(detail::toUtf8(query)); });
    _impl->engine.rootContext()->setContextProperty(QStringLiteral("inspector"), _impl->model.get());
    _impl->timer.setInterval(kRefreshEvery);
    QObject::connect(&_impl->timer, &QTimer::timeout, &_impl->timer, [this] { refresh(); });
    refresh();
}

InspectorWindow::~InspectorWindow() {
    _impl->timer.stop();
    delete _impl->shortcut.data();  // NOLINT(cppcoreguidelines-owning-memory): the filter's owner is the main window.
    _impl->window.reset();
}

void InspectorWindow::attachTo(QQuickWindow& window) {
    auto* const filter = new Shortcut{&window, [this] { toggle(); }};  // NOLINT(cppcoreguidelines-owning-memory)
    window.installEventFilter(filter);
    _impl->shortcut = filter;
}

void InspectorWindow::toggle() {
    if (isVisible()) {
        hide();
        return;
    }
    if (!_impl->window) {
        QQmlComponent component{&_impl->engine};
        component.setData(QByteArray{kWindowQml}, {});
        QObject* const made = component.create();
        _impl->window.reset(qobject_cast<QQuickWindow*>(made));
        if (!_impl->window) {
            delete made;  // NOLINT(cppcoreguidelines-owning-memory): create() hands ownership to the caller.
            return;
        }
    }
    refresh();
    _impl->window->show();
    _impl->timer.start();
}

void InspectorWindow::hide() {
    _impl->timer.stop();
    if (_impl->window) {
        _impl->window->hide();
    }
}

bool InspectorWindow::isVisible() const { return _impl->window && _impl->window->isVisible(); }

void InspectorWindow::refresh() {
    std::string counters = "no screen is shown";
    QVariantList queries;
    std::string nodes;
    std::string log;
    std::string queriesText;
    if (_impl->focused) {
        _impl->focused([&](ui::Inspector& inspector) {
            counters = countersText(inspector.counters());
            for (ui::InspectedQuery const& query : inspector.queries()) {
                std::string const line = queryLine(query);
                queriesText += line + "\n";
                QVariantMap row;
                row.insert(QStringLiteral("name"), detail::toQString(query.name));
                row.insert(QStringLiteral("line"), detail::toQString(line));
                row.insert(QStringLiteral("query"), !query.mutation);
                queries.push_back(row);
            }
            for (ui::InspectedNode const& node : inspector.nodes()) {
                nodes += nodeLine(node) + "\n";
            }
            for (ui::CommandLogEntry const& entry : inspector.commandLog()) {
                log += logLine(entry) + "\n";
            }
        });
    }
    _impl->text = "Reactive runtime\n" + counters + "\nQueries and mutations\n" + queriesText + "Mounted nodes\n" +
                  nodes + "Command log\n" + log;
    _impl->model->set(detail::toQString(counters), std::move(queries), detail::toQString(nodes),
                      detail::toQString(log));
}

std::string InspectorWindow::text() const { return _impl->text; }

void InspectorWindow::refetch(std::string const& query) {
    if (_impl->focused) {
        _impl->focused([&query](ui::Inspector& inspector) { static_cast<void>(inspector.refetch(query)); });
    }
    refresh();
}

}  // namespace morph::qt_quick
