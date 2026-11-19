// SPDX-License-Identifier: Apache-2.0
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QByteArray>
#include <QCoreApplication>
#include <QIODevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QtGlobal>
// Permissions came with Qt 6.5; on an older Qt the platform's own prompt, where there is one, appears on first use.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/qt/qt_dictation_recorder.hpp>
#include <morph/ui/chat/dictation.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace morph::qt {

namespace {

constexpr std::chrono::milliseconds kTick{250};

constexpr char const* kNoDevice = "no microphone is connected";

// A source is destroyed from its own device's signal when the clip fills, so it goes once control returns.
struct LaterDelete {
    void operator()(QAudioSource* source) const {
        if (source != nullptr) {
            source->stop();
            source->deleteLater();
        }
    }
};

}  // namespace

// One recording, from `start` to its clip or its refusal.
struct QtDictationRecorder::Take {
    explicit Take(ui::chat::DictationLimits given) : limits{given} {}

    ui::chat::DictationLimits limits;
    // Scopes every connection and the permission answer: destroying it disconnects them.
    QObject context;
    std::unique_ptr<QAudioSource, LaterDelete> source;
    QIODevice* input = nullptr;
    QAudioFormat format;
    QByteArray leftover;
    std::optional<ui::chat::SpeechResampler> resampler;
    std::optional<ui::chat::DictationClip> clip;
    QTimer ticker;
    std::chrono::seconds shown{0};
    bool begun = false;
};

QtDictationRecorder::QtDictationRecorder() = default;

QtDictationRecorder::~QtDictationRecorder() = default;

std::optional<std::string> QtDictationRecorder::unavailable() const {
    if (QMediaDevices::defaultAudioInput().isNull()) {
        return std::string{kNoDevice};
    }
    return std::nullopt;
}

void QtDictationRecorder::start(ui::chat::DictationLimits limits) {
    if (_take != nullptr) {
        return;
    }
    _take = std::make_unique<Take>(limits);
    ++_generation;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QMicrophonePermission const microphone;
    switch (qApp->checkPermission(microphone)) {
        case Qt::PermissionStatus::Granted:
            break;
        case Qt::PermissionStatus::Denied:
            refuse("the microphone permission was denied");
            return;
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(microphone, &_take->context,
                                    [this, generation = _generation](QPermission const& answer) {
                                        if (_take == nullptr || generation != _generation) {
                                            return;
                                        }
                                        if (answer.status() == Qt::PermissionStatus::Granted) {
                                            begin();
                                        } else {
                                            refuse("the microphone permission was denied");
                                        }
                                    });
            return;
    }
#endif
    begin();
}

void QtDictationRecorder::begin() {
    QAudioDevice const device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        refuse(kNoDevice);
        return;
    }
    Take& take = *_take;
    take.format = device.preferredFormat();
    take.resampler.emplace(take.format.channelCount(), take.format.sampleRate());
    take.clip.emplace(take.limits, std::chrono::steady_clock::now());
    take.source.reset(new QAudioSource(device, take.format));  // NOLINT(cppcoreguidelines-owning-memory)
    take.input = take.source->start();
    if (take.input == nullptr || take.source->error() != QAudio::NoError) {
        refuse("the microphone could not be opened");
        return;
    }
    QObject::connect(take.input, &QIODevice::readyRead, &take.context, [this] { read(); });
    QObject::connect(&take.ticker, &QTimer::timeout, &take.context, [this] { tick(); });
    take.ticker.start(kTick);
    take.begun = true;
    if (onStarted) {
        onStarted({});
    }
}

void QtDictationRecorder::refuse(std::string reason) {
    _take.reset();
    if (onStarted) {
        onStarted(std::unexpected(std::move(reason)));
    }
}

void QtDictationRecorder::read() {
    if (_take == nullptr || _take->input == nullptr) {
        return;
    }
    Take& take = *_take;
    if (!take.resampler.has_value() || !take.clip.has_value()) {
        return;
    }
    take.leftover += take.input->readAll();
    int const frameBytes = take.format.bytesPerFrame();
    int const sampleBytes = take.format.bytesPerSample();
    if (frameBytes <= 0 || sampleBytes <= 0) {
        return;
    }
    qsizetype const whole = take.leftover.size() - (take.leftover.size() % frameBytes);
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(whole / sampleBytes));
    for (qsizetype at = 0; at < whole; at += sampleBytes) {
        samples.push_back(take.format.normalizedSampleValue(QByteArrayView{take.leftover}.sliced(at).constData()));
    }
    take.leftover.remove(0, whole);
    std::vector<std::int16_t> const speech = take.resampler->push(samples);
    if (take.clip->append(speech, std::chrono::steady_clock::now()) == ui::chat::ClipState::Full) {
        finish();
    }
}

void QtDictationRecorder::tick() {
    if (_take == nullptr || !_take->clip) {
        return;
    }
    Take& take = *_take;
    auto const now = std::chrono::steady_clock::now();
    if (take.clip->append({}, now) == ui::chat::ClipState::Full) {
        finish();
        return;
    }
    if (auto const elapsed = take.clip->elapsed(now); elapsed != take.shown) {
        take.shown = elapsed;
        if (onProgress) {
            onProgress(elapsed);
        }
    }
}

void QtDictationRecorder::stop() {
    if (_take == nullptr) {
        return;
    }
    if (!_take->begun) {
        _take.reset();
        ++_generation;
        if (onClip) {
            onClip(std::unexpected(std::string{"the recording was stopped"}));
        }
        return;
    }
    finish();
}

void QtDictationRecorder::finish() {
    // `finish` runs inside the take's own `readyRead` or `timeout`, so the take, whose context receives them, is
    // destroyed once control is back in the event loop.
    std::shared_ptr<Take> const take{std::move(_take)};
    take->ticker.stop();
    QObject::disconnect(take->input, nullptr, &take->context, nullptr);
    QObject::disconnect(&take->ticker, nullptr, &take->context, nullptr);
    take->source = nullptr;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [take] {}, Qt::QueuedConnection);
    std::string wav = take->clip.has_value() ? take->clip->wav() : std::string{};
    if (onClip) {
        onClip(std::move(wav));
    }
}

}  // namespace morph::qt
