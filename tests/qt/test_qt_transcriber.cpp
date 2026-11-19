// SPDX-License-Identifier: Apache-2.0
//
// The configurable speech-to-text provider, against a local server answering one canned response. No network, except
// the live case, which runs only when asked for by name with a key:
//
//     MORPH_LIVE_STT_KEY=... morph_qt_tests "[.live][dictation]"

#include <QCoreApplication>
#include <QObject>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <morph/qt/qt_transcriber.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fake_http_server.hpp"

namespace chat = morph::ui::chat;
using qt_test::FakeHttpServer;
using qt_test::pumpUntil;

namespace {

constexpr std::string_view kKey = "sk-stt-secret-key";

std::string jsonAnswer(int code, std::string const& reason, std::string const& body) {
    return "HTTP/1.1 " + std::to_string(code) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

chat::TranscriptionConfig configFor(std::string url) {
    return chat::TranscriptionConfig{.name = "openai-transcriptions",
                                     .baseUrl = std::move(url),
                                     .model = "whisper-1",
                                     .apiKey = std::string{kKey},
                                     .language = std::nullopt};
}

// The body of the multipart field @p name, or nothing when the request has none.
std::optional<std::string> partValue(std::string const& request, std::string const& name) {
    auto const at = request.find("name=\"" + name + "\"");
    if (at == std::string::npos) {
        return std::nullopt;
    }
    auto const start = request.find("\r\n\r\n", at);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    auto const end = request.find("\r\n--", start + 4);
    return request.substr(start + 4, end == std::string::npos ? std::string::npos : end - start - 4);
}

struct Rig {
    QObject owner;
    std::unique_ptr<FakeHttpServer> server;
    std::shared_ptr<chat::ITranscriber> transcriber;
    std::optional<std::expected<std::string, std::string>> done;

    explicit Rig(std::string response) : server{std::make_unique<FakeHttpServer>(std::move(response))} {
        auto made = morph::qt::makeQtTranscriber(configFor(server->url()), &owner);
        REQUIRE(made.has_value());
        transcriber = *made;
    }

    std::shared_ptr<chat::ITranscriberCall> call(std::optional<std::string> language = std::nullopt) {
        return transcriber->transcribe(
            chat::TranscriptionInput{
                .audio = std::string{"RIFF-clip-bytes"}, .mediaType = "audio/wav", .language = std::move(language)},
            chat::TranscriberSink{
                .done = [this](std::expected<std::string, std::string> outcome) { done = std::move(outcome); }});
    }

    void finish() {
        REQUIRE(pumpUntil([this] { return done.has_value(); }));
    }
};

}  // namespace

// Mutation: leave out a part, or send the key in another header.
TEST_CASE("qt_transcriber_posts_the_clip_model_format_and_key", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", R"({"text":"hello there"})")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE(rig.done->has_value());
    CHECK(**rig.done == "hello there");
    std::string const& sent = rig.server->received;
    CHECK(sent.starts_with("POST /v1/audio/transcriptions "));
    CHECK(sent.contains("Authorization: Bearer " + std::string{kKey}));
    CHECK(sent.contains(R"(name="file"; filename="audio.wav")"));
    CHECK(partValue(sent, "file") == "RIFF-clip-bytes");
    CHECK(partValue(sent, "model") == "whisper-1");
    CHECK(partValue(sent, "response_format") == "json");
    CHECK_FALSE(partValue(sent, "language").has_value());
}

// Mutation: always send a language part, or never.
TEST_CASE("qt_transcriber_sends_a_language_only_when_given", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", R"({"text":"hallo"})")};
    static_cast<void>(rig.call(std::string{"de"}));
    rig.finish();
    CHECK(partValue(rig.server->received, "language") == "de");
}

// Mutation: put the request or the configuration in the reason.
TEST_CASE("qt_transcriber_fails_with_the_providers_message_not_the_key", "[qt][dictation]") {
    Rig rig{
        jsonAnswer(401, "Unauthorized", R"({"error":{"message":"bad credentials","type":"invalid_request_error"}})")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error().contains("401"));
    CHECK(rig.done->error().contains("bad credentials"));
    CHECK_FALSE(rig.done->error().contains(kKey));
}

// Mutation: answer an unreadable body with an empty text.
TEST_CASE("qt_transcriber_fails_on_an_unreadable_answer", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", "not json")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error() == "the transcription provider sent something unreadable");
}

// Mutation: leave the reply running after a cancel.
TEST_CASE("qt_transcriber_cancel_aborts_and_sends_no_done", "[qt][dictation]") {
    Rig rig{"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"te"};
    rig.server->holdOpen = true;
    auto const call = rig.call();
    REQUIRE(pumpUntil([&rig] { return rig.server->answered(); }));
    call->cancel();
    CHECK(pumpUntil([&rig] { return rig.server->disconnected; }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK_FALSE(rig.done.has_value());
}

// Mutation: accept any name as the one transcriber there is.
TEST_CASE("qt_transcriber_refuses_a_bad_configuration", "[qt][dictation]") {
    QObject owner;
    auto config = configFor("http://127.0.0.1:1");
    config.name = "no-such-transcriber";
    CHECK_FALSE(morph::qt::makeQtTranscriber(config, &owner).has_value());
    config = configFor("http://127.0.0.1:1");
    config.apiKey.clear();
    CHECK_FALSE(morph::qt::makeQtTranscriber(config, &owner).has_value());
    config = configFor("http://127.0.0.1:1");
    config.model.clear();
    CHECK_FALSE(morph::qt::makeQtTranscriber(config, &owner).has_value());
    CHECK_FALSE(morph::qt::makeQtTranscriber(configFor("http://127.0.0.1:1"), nullptr).has_value());
    CHECK(morph::qt::makeQtTranscriber(configFor(""), &owner).has_value());
}

TEST_CASE("qt_transcriber_live_endpoint", "[.live][qt][dictation]") {
    // NOLINTBEGIN(concurrency-mt-unsafe): read before any thread starts.
    char const* const key = std::getenv("MORPH_LIVE_STT_KEY");
    char const* const url = std::getenv("MORPH_LIVE_STT_URL");
    char const* const model = std::getenv("MORPH_LIVE_STT_MODEL");
    // NOLINTEND(concurrency-mt-unsafe)
    if (key == nullptr) {
        SKIP("MORPH_LIVE_STT_KEY is not set");
    }
    QObject owner;
    auto made = morph::qt::makeQtTranscriber(chat::TranscriptionConfig{.name = "openai-transcriptions",
                                                                       .baseUrl = url != nullptr ? url : "",
                                                                       .model = model != nullptr ? model : "whisper-1",
                                                                       .apiKey = key,
                                                                       .language = "en"},
                                             &owner);
    REQUIRE(made.has_value());
    std::vector<std::int16_t> const second(static_cast<std::size_t>(chat::kSpeechSampleRate), 0);
    std::optional<std::expected<std::string, std::string>> done;
    auto const call = (*made)->transcribe(
        chat::TranscriptionInput{.audio = chat::wavFromPcm16(second), .mediaType = "audio/wav", .language = "en"},
        chat::TranscriberSink{
            .done = [&done](std::expected<std::string, std::string> outcome) { done = std::move(outcome); }});
    REQUIRE(pumpUntil([&done] { return done.has_value(); }, std::chrono::seconds{60}));
    INFO((done->has_value() ? std::string{} : done->error()));
    CHECK(done->has_value());
}
