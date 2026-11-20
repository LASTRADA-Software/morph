// SPDX-License-Identifier: Apache-2.0
//
// The Qt recorder. Most cases give it an input of their own, a device that is fed known samples, so what it does with
// a stream is tested on every machine; one smoke test uses the system's input when there is one. What a clip is made
// of is tested without Qt in test_chat_dictation_audio.cpp.

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QIODevice>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <morph/qt/qt_dictation_recorder.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fake_http_server.hpp"

using namespace std::chrono_literals;
namespace chat = morph::ui::chat;

namespace {

// A sequential device that holds what it is fed until it is read.
class FeedDevice : public QIODevice {
public:
    FeedDevice() { open(QIODevice::ReadOnly); }

    void feed(QByteArray const& bytes) {
        _pending += bytes;
        Q_EMIT readyRead();
    }

    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] qint64 bytesAvailable() const override { return _pending.size() + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char* out, qint64 maxSize) override {
        qint64 const count = std::min<qint64>(maxSize, _pending.size());
        std::memcpy(out, _pending.constData(), static_cast<std::size_t>(count));
        _pending.remove(0, count);
        return count;
    }
    qint64 writeData(char const* /*data*/, qint64 /*size*/) override { return -1; }

private:
    QByteArray _pending;
};

// @p frames frames of 48 kHz stereo float, both channels at @p level.
QByteArray stereoFloat(std::size_t frames, float level) {
    std::vector<float> samples(frames * 2, level);
    QByteArray bytes(static_cast<qsizetype>(samples.size() * sizeof(float)), Qt::Uninitialized);
    std::memcpy(bytes.data(), samples.data(), static_cast<std::size_t>(bytes.size()));
    return bytes;
}

std::int16_t sampleAt(std::string const& wav, std::size_t index) {
    auto const low = static_cast<unsigned char>(wav.at(chat::kWavHeaderBytes + (index * 2)));
    auto const high = static_cast<unsigned char>(wav.at(chat::kWavHeaderBytes + (index * 2) + 1));
    return static_cast<std::int16_t>(static_cast<unsigned>(low) | (static_cast<unsigned>(high) << 8U));
}

// A recorder on a device the test feeds, with what it said kept.
struct Rig {
    FeedDevice device;
    std::vector<std::expected<void, std::string>> started;
    std::vector<std::expected<std::string, std::string>> clips;
    int opened = 0;
    int released = 0;
    int releasedWhenClipArrived = -1;
    bool holdPermission = false;
    std::function<void(bool)> held;
    std::optional<std::string> openFails;
    std::unique_ptr<morph::qt::QtDictationRecorder> recorder;

    Rig() {
        morph::qt::QtDictationOptions options;
        options.openInput = [this] -> std::expected<morph::qt::DictationInput, std::string> {
            if (openFails) {
                return std::unexpected(*openFails);
            }
            ++opened;
            return morph::qt::DictationInput{
                .device = &device,
                .channels = 2,
                .sampleRate = 48000,
                .sample = morph::qt::DictationSample::Float,
                .keepAlive = std::shared_ptr<void>{nullptr, [this](void*) { ++released; }},
            };
        };
        options.askPermission = [this](std::function<void(bool)> answer) {
            if (holdPermission) {
                held = std::move(answer);
            } else {
                answer(true);
            }
        };
        recorder = std::make_unique<morph::qt::QtDictationRecorder>(std::move(options));
        recorder->onStarted = [this](std::expected<void, std::string> outcome) {
            started.push_back(std::move(outcome));
        };
        recorder->onClip = [this](std::expected<std::string, std::string> outcome) {
            releasedWhenClipArrived = released;
            clips.push_back(std::move(outcome));
        };
    }

    void settle() const {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        qt_test::pumpUntil([] { return false; }, 60ms);
    }
};

}  // namespace

// Mutation: make `begin` refuse, or never connect the device.
TEST_CASE("qt_dictation_recorder_converts_a_stream_and_stops_at_its_byte_bound", "[qt][dictation]") {
    Rig rig;
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = chat::kWavHeaderBytes + 3200});
    REQUIRE(rig.started.size() == 1);
    REQUIRE(rig.started.front().has_value());
    // Two chunks under the bound, the first cut in the middle of a frame.
    QByteArray const first = stereoFloat(2000, 0.5F);
    rig.device.feed(first.left(4003));
    rig.device.feed(first.mid(4003));
    rig.device.feed(stereoFloat(2000, 0.5F));
    CHECK(rig.clips.empty());
    rig.device.feed(stereoFloat(2000, 0.5F));
    REQUIRE(rig.clips.size() == 1);
    REQUIRE(rig.clips.front().has_value());
    std::string const& wav = *rig.clips.front();
    CHECK(wav.size() == chat::kWavHeaderBytes + 3200);
    CHECK(wav.starts_with("RIFF"));
    CHECK(sampleAt(wav, 0) >= 16383);
    CHECK(sampleAt(wav, 0) <= 16384);
    CHECK(sampleAt(wav, 1599) >= 16383);
    CHECK(sampleAt(wav, 1599) <= 16384);
    // The device is released after the callback, from the event loop, not inside the device's own signal.
    CHECK(rig.releasedWhenClipArrived == 0);
    REQUIRE(qt_test::pumpUntil([&rig] { return rig.released == 1; }));
    rig.device.feed(stereoFloat(2000, 0.5F));
    rig.settle();
    CHECK(rig.clips.size() == 1);
}

// Mutation: answer a stop with a second clip, or ignore it.
TEST_CASE("qt_dictation_recorder_stop_delivers_the_clip_once", "[qt][dictation]") {
    Rig rig;
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = 1U << 20U});
    rig.device.feed(stereoFloat(4800, 0.25F));
    rig.recorder->stop();
    rig.recorder->stop();
    rig.settle();
    REQUIRE(rig.clips.size() == 1);
    REQUIRE(rig.clips.front().has_value());
    CHECK(rig.clips.front()->size() == chat::kWavHeaderBytes + 3200);
    CHECK(rig.released == 1);
}

// Mutation: leave out the ticker, so a silent input never reaches its time bound.
TEST_CASE("qt_dictation_recorder_time_bound_stops_a_silent_input", "[qt][dictation]") {
    Rig rig;
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 1s, .maxBytes = 1U << 20U});
    REQUIRE(qt_test::pumpUntil([&rig] { return !rig.clips.empty(); }, 5s));
    rig.settle();
    REQUIRE(rig.clips.size() == 1);
    REQUIRE(rig.clips.front().has_value());
    CHECK(rig.clips.front()->size() == chat::kWavHeaderBytes);
}

// Mutation: answer a stop before the take began with nothing, or let the late permission answer start it.
TEST_CASE("qt_dictation_recorder_stop_before_the_take_began", "[qt][dictation]") {
    Rig rig;
    rig.holdPermission = true;
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = 1U << 20U});
    CHECK(rig.started.empty());
    rig.recorder->stop();
    REQUIRE(rig.clips.size() == 1);
    CHECK_FALSE(rig.clips.front().has_value());
    rig.held(true);
    rig.settle();
    CHECK(rig.opened == 0);
    CHECK(rig.started.empty());
    CHECK(rig.clips.size() == 1);
}

// Mutation: treat a denial as a grant.
TEST_CASE("qt_dictation_recorder_denied_permission_fails_the_start", "[qt][dictation]") {
    Rig rig;
    rig.holdPermission = true;
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = 1U << 20U});
    rig.held(false);
    REQUIRE(rig.started.size() == 1);
    REQUIRE_FALSE(rig.started.front().has_value());
    CHECK(rig.started.front().error().contains("permission"));
    CHECK(rig.opened == 0);
    CHECK(rig.clips.empty());
}

// Mutation: return silently from a second start.
TEST_CASE("qt_dictation_recorder_second_start_is_answered_and_changes_nothing", "[qt][dictation]") {
    Rig rig;
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = 1U << 20U});
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = 1U << 20U});
    REQUIRE(rig.started.size() == 2);
    CHECK(rig.started.front().has_value());
    REQUIRE_FALSE(rig.started.back().has_value());
    CHECK(rig.started.back().error() == "a recording is already running");
    CHECK(rig.opened == 1);
    rig.device.feed(stereoFloat(4800, 0.25F));
    rig.recorder->stop();
    rig.settle();
    CHECK(rig.clips.size() == 1);
}

// Mutation: answer a failed open with nothing, or with a clip.
TEST_CASE("qt_dictation_recorder_no_device_fails_the_start_with_its_reason", "[qt][dictation]") {
    Rig rig;
    rig.openFails = "no microphone is connected";
    rig.recorder->start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = 1U << 20U});
    rig.settle();
    REQUIRE(rig.started.size() == 1);
    REQUIRE_FALSE(rig.started.front().has_value());
    CHECK(rig.started.front().error() == "no microphone is connected");
    CHECK(rig.clips.empty());
    rig.recorder->stop();
    CHECK(rig.clips.empty());
}

// Mutation: release the take from the application object's thread.
TEST_CASE("qt_dictation_recorder_on_a_worker_thread_releases_its_input_there", "[qt][dictation]") {
    std::atomic<bool> finished{false};
    std::atomic<bool> gotClip{false};
    std::atomic<bool> releasedOnWorker{false};
    std::atomic<bool> released{false};
    QThread* worker = nullptr;
    worker = QThread::create([&] {
        FeedDevice device;
        morph::qt::QtDictationOptions options;
        options.openInput = [&] -> std::expected<morph::qt::DictationInput, std::string> {
            return morph::qt::DictationInput{
                .device = &device,
                .channels = 2,
                .sampleRate = 48000,
                .sample = morph::qt::DictationSample::Float,
                .keepAlive = std::shared_ptr<void>{nullptr,
                                                   [&](void*) {
                                                       releasedOnWorker = QThread::currentThread() == worker;
                                                       released = true;
                                                   }},
            };
        };
        options.askPermission = [](std::function<void(bool)> const& answer) { answer(true); };
        morph::qt::QtDictationRecorder recorder{std::move(options)};
        QEventLoop loop;
        recorder.onClip = [&](std::expected<std::string, std::string> outcome) {
            gotClip = outcome.has_value();
            QTimer::singleShot(100ms, &loop, [&loop] { loop.quit(); });
        };
        recorder.start(chat::DictationLimits{.maxSeconds = 60s, .maxBytes = chat::kWavHeaderBytes + 3200});
        QTimer::singleShot(0ms, &loop, [&device] { device.feed(stereoFloat(6000, 0.5F)); });
        loop.exec();
        finished = true;
    });
    worker->start();
    REQUIRE(qt_test::pumpUntil([&finished] { return finished.load(); }, 10s));
    worker->wait();
    delete worker;  // NOLINT(cppcoreguidelines-owning-memory): QThread::create hands over ownership.
    CHECK(gotClip.load());
    CHECK(released.load());
    CHECK(releasedOnWorker.load());
}

// Smoke test on whatever input this machine has; what the recorder does with a stream is tested below, without one.
// Hidden (`[.]`, run by name): it is the only test that loads a real audio backend, which a runner may lack or which
// may trip a leak checker, so ctest does not run it.
TEST_CASE("qt_dictation_recorder_smoke_on_the_system_input", "[.][qt][dictation]") {
    morph::qt::QtDictationRecorder recorder;
    std::optional<std::expected<void, std::string>> started;
    std::optional<std::expected<std::string, std::string>> clip;
    recorder.onStarted = [&started](std::expected<void, std::string> outcome) { started = std::move(outcome); };
    recorder.onClip = [&clip](std::expected<std::string, std::string> outcome) { clip = std::move(outcome); };
    std::optional<std::string> const why = recorder.unavailable();
    recorder.start(morph::ui::chat::DictationLimits{.maxSeconds = 1s, .maxBytes = 1U << 20U});
    REQUIRE(qt_test::pumpUntil([&] { return started.has_value() || clip.has_value(); }));
    if (why) {
        REQUIRE(started.has_value());
        REQUIRE_FALSE(started->has_value());
        CHECK(started->error() == *why);
        return;
    }
    if (started && !started->has_value()) {
        WARN("the input device would not open: " << started->error());
        return;
    }
    REQUIRE(qt_test::pumpUntil([&] { return clip.has_value(); }, 10s));
    REQUIRE(clip->has_value());
    CHECK((*clip)->starts_with("RIFF"));
    CHECK((*clip)->size() <= morph::ui::chat::kWavHeaderBytes + (std::size_t{2} * 16000 * 2));
}
