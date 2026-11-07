// SPDX-License-Identifier: Apache-2.0
//
// The chat panel on the real Qt Quick backend: where it sits in the frame, which a recording backend cannot show. Set
// MORPH_CHAT_GRAB to a file to keep a picture of it.

#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <functional>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/client.hpp>
#include <morph/ui/chat/panel.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/host_vocabulary.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/shell_frame.hpp>
#include <optional>
#include <string>
#include <utility>

#include "qt_quick_harness.hpp"
#include "ui_interpreter_support.hpp"

namespace {

using morph::qt_quick::testing::Scene;
namespace chat = morph::ui::chat;
namespace detail = morph::qt_quick::detail;

QQuickItem const& itemFor(char const* testId) {
    detail::ItemBase const* const widget = detail::findWidget(QString::fromLatin1(testId));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->item() != nullptr);
    return *widget->item();
}

qreal leftOf(QQuickItem const& item) { return item.mapToScene(QPointF{0, 0}).x(); }

qreal rightOf(QQuickItem const& item) { return item.mapToScene(QPointF{item.width(), 0}).x(); }

}  // namespace

// Mutation: leave the chat's column out of the frame's row (the panel then has no width of its own beside the page).
TEST_CASE("qt_quick chat: the panel sits beside the page, and its input and lines have room", "[qt_quick][chat]") {
    Scene scene;
    scene.window().resize(1200, 700);
    morph::ui::ShellFrameOptions options;
    options.applicationName = "Morph LIMS";
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.setTitle("Ignition tests");
    frame.setPrincipal("tech");
    frame.setChatShown(true);

    morph::ui::HostVocabulary host;
    std::unique_ptr<chat::ChatPanelBinding> binding;
    chat::ChatClient client{
        [](morph::wire::Envelope const& /*request*/, std::function<void(morph::ui::ExchangeReply)> const& done) {
            done(morph::wire::makeOk(0, R"({"turnId":1})"));
        },
        [] { return morph::session::Context{}; },
        [&binding] {
            if (binding != nullptr) {
                binding->refresh();
            }
        }};
    morph::reactive::testing::ManualScheduler scheduler;
    morph::ui::Session session{scene.runtime(), morph::ui::SessionOptions{.scheduler = &scheduler}};
    morph::ui::testing::ScriptedDispatcher dispatcher{scene.owner()};
    morph::ui::testing::RecordingHost recordingHost;
    morph::ui::Interpreter interpreter{session, dispatcher, morph::ui::InterpreterOptions{.host = &recordingHost}};
    host.connect(session);
    binding = std::make_unique<chat::ChatPanelBinding>(
        host, client, [] { return std::optional<chat::TurnRequest>{chat::TurnRequest{}}; });
    auto const document = chat::chatPanelDocument();
    REQUIRE(document.has_value());
    auto mounted =
        interpreter.mount(*document, {}, scene.backend(), morph::ui::MountOptions{.parent = &frame.chatSlot()});
    REQUIRE(mounted.has_value());
    chat::TurnRequest request;
    request.text = "container 100, before 299, after 223";
    client.send(request);
    scene.settle();

    QQuickItem const& page = itemFor("shell_page");
    QQuickItem const& column = itemFor("shell_chat");
    QQuickItem const& input = itemFor("chat_input");
    QQuickItem const& send = itemFor("chat_send");
    QQuickItem const& line = itemFor("chat_entry");

    if (char const* const file = std::getenv("MORPH_CHAT_GRAB")) {  // NOLINT(concurrency-mt-unsafe): one thread here.
        CHECK(scene.window().grabWindow().save(QString::fromUtf8(file)));
    }

    CHECK(column.isVisible());
    CHECK(rightOf(page) <= leftOf(column) + 1);
    CHECK(rightOf(column) <= scene.window().width() + 1);
    CHECK(column.width() > 100);
    CHECK(page.width() > 400);
    CHECK(column.height() >= scene.window().height() - 1);
    // Inside the column, with a usable input and the button beside it rather than over it.
    CHECK(leftOf(input) >= leftOf(column));
    CHECK(rightOf(send) <= rightOf(column) + 1);
    CHECK(input.width() > 60);
    CHECK(rightOf(input) <= leftOf(send) + 1);
    CHECK(line.width() > 60);
    CHECK(line.height() > 0);
}
