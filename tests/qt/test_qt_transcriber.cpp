// SPDX-License-Identifier: Apache-2.0
//
// The configurable speech-to-text provider, against a local server answering one canned response. No network, except
// the live case, which runs only when asked for by name with a key:
//
//     MORPH_LIVE_STT_KEY=... morph_qt_tests "[.live][dictation]"
//
// `MORPH_LIVE_STT_URL` and `MORPH_LIVE_STT_MODEL` choose the endpoint. `MORPH_LIVE_STT_WAV` names a speech clip to
// send instead of a second of silence, and `MORPH_LIVE_STT_EXPECT` a phrase its transcript must contain.

#include <QCoreApplication>
#include <QObject>
#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iterator>
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

// The value of request header @p name, found whatever case the client wrote its name in: Qt versions differ, and
// header names are case-insensitive.
static std::optional<std::string> headerValue(std::string_view request, std::string_view name) {
    std::string lowered{request};
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char const letter) { return static_cast<char>(std::tolower(letter)); });
    auto const start = lowered.find("\r\n" + std::string{name} + ": ");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    auto const value = start + 2 + name.size() + 2;
    return std::string{request.substr(value, request.find("\r\n", value) - value)};
}

// Mutation: leave out a part, or send the key in another header.
TEST_CASE("qt_transcriber_posts_the_clip_model_format_and_key", "[qt][dictation]") {
    Rig rig{jsonAnswer(200, "OK", R"({"text":"hello there"})")};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE(rig.done->has_value());
    CHECK(**rig.done == "hello there");
    std::string const& sent = rig.server->received;
    CHECK(sent.starts_with("POST /v1/audio/transcriptions "));
    CHECK(headerValue(sent, "authorization") == "Bearer " + std::string{kKey});
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

// Mutation: relay the provider's message as it came.
TEST_CASE("qt_transcriber_scrubs_the_key_from_a_providers_message", "[qt][dictation]") {
    std::string const body = R"({"error":{"message":"invalid key )" + std::string{kKey} + R"( for this account"}})";
    Rig rig{jsonAnswer(401, "Unauthorized", body)};
    static_cast<void>(rig.call());
    rig.finish();
    REQUIRE_FALSE(rig.done->has_value());
    CHECK(rig.done->error().contains("invalid key"));
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

namespace {

// ASCII lowercase, so a phrase is found whatever capitals the provider chose.
std::string lowered(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char const letter) { return static_cast<char>(std::tolower(letter)); });
    return out;
}

}  // namespace

TEST_CASE("qt_transcriber_live_endpoint", "[.live][qt][dictation]") {
    // NOLINTBEGIN(concurrency-mt-unsafe): read before any thread starts.
    char const* const key = std::getenv("MORPH_LIVE_STT_KEY");
    char const* const url = std::getenv("MORPH_LIVE_STT_URL");
    char const* const model = std::getenv("MORPH_LIVE_STT_MODEL");
    char const* const wavPath = std::getenv("MORPH_LIVE_STT_WAV");
    char const* const expect = std::getenv("MORPH_LIVE_STT_EXPECT");
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
    std::string audio;
    if (wavPath != nullptr) {
        std::ifstream file{wavPath, std::ios::binary};
        REQUIRE(file.good());
        audio.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
        REQUIRE_FALSE(audio.empty());
    } else {
        std::vector<std::int16_t> const second(static_cast<std::size_t>(chat::kSpeechSampleRate), 0);
        audio = chat::wavFromPcm16(second);
    }
    std::optional<std::expected<std::string, std::string>> done;
    auto const call = (*made)->transcribe(
        chat::TranscriptionInput{.audio = std::move(audio), .mediaType = "audio/wav", .language = "en"},
        chat::TranscriberSink{
            .done = [&done](std::expected<std::string, std::string> outcome) { done = std::move(outcome); }});
    REQUIRE(pumpUntil([&done] { return done.has_value(); }, std::chrono::seconds{60}));
    INFO((done->has_value() ? std::string{} : done->error()));
    REQUIRE(done->has_value());
    if (wavPath != nullptr) {
        // A speech clip must come back as words, and as the phrase it holds when one is given.
        CHECK_FALSE((*done)->empty());
        if (expect != nullptr) {
            CHECK(lowered(**done).contains(lowered(expect)));
        }
    }
}
