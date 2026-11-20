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
// Permissions came with Qt 6.5; on an older Qt, or one built without them, the platform's own prompt, where there is
// one, appears when the device opens.
#if defined(QT_FEATURE_permissions) && QT_FEATURE_permissions == 1
#include <QPermissions>
#endif
#include <algorithm>
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

namespace morph::qt {

namespace {

constexpr std::chrono::milliseconds kTick{250};

constexpr char const* kNoDevice = "no microphone is connected";
constexpr char const* kDenied = "the microphone permission was denied";

// The size in bytes of one sample.
[[nodiscard]] int sampleBytes(DictationSample sample) {
    switch (sample) {
        case DictationSample::UInt8:
            return 1;
        case DictationSample::Int16:
            return 2;
        case DictationSample::Int32:
        case DictationSample::Float:
        default:
            // `default:` only because the project builds with -Wswitch-default; -Wswitch-enum still requires a case
            // for every enumerator.
            return 4;
    }
}

// One sample as a float in [-1, 1]; @p bytes holds `sampleBytes(sample)` bytes.
[[nodiscard]] float decode(DictationSample sample, QByteArrayView bytes) {
    switch (sample) {
        case DictationSample::UInt8: {
            std::uint8_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof value);
            return (static_cast<float>(value) - 128.0F) / 128.0F;
        }
        case DictationSample::Int16: {
            std::int16_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof value);
            return static_cast<float>(value) / 32768.0F;
        }
        case DictationSample::Int32: {
            std::int32_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof value);
            return static_cast<float>(static_cast<double>(value) / 2147483648.0);
        }
        case DictationSample::Float:
        default: {
            // `default:` only because the project builds with -Wswitch-default; -Wswitch-enum still requires a case
            // for every enumerator.
            float value = 0.0F;
            std::memcpy(&value, bytes.constData(), sizeof value);
            return value;
        }
    }
}

[[nodiscard]] std::expected<DictationInput, std::string> openSystemInput() {
    QAudioDevice const device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        return std::unexpected(std::string{kNoDevice});
    }
    QAudioFormat const format = device.preferredFormat();
    DictationSample sample = DictationSample::Int16;
    switch (format.sampleFormat()) {
        case QAudioFormat::UInt8:
            sample = DictationSample::UInt8;
            break;
        case QAudioFormat::Int16:
            sample = DictationSample::Int16;
            break;
        case QAudioFormat::Int32:
            sample = DictationSample::Int32;
            break;
        case QAudioFormat::Float:
            sample = DictationSample::Float;
            break;
        case QAudioFormat::Unknown:
        case QAudioFormat::NSampleFormats:
        default:
            return std::unexpected(std::string{"the microphone's sample format is not supported"});
    }
    // A source is stopped and destroyed once control is back in the event loop: it may be released from its own
    // device's `readyRead`.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): shared with the deleter below, which hands it to the event loop.
    std::shared_ptr<QAudioSource> const source{new QAudioSource(device, format), [](QAudioSource* released) {
                                                   released->stop();
                                                   released->deleteLater();
                                               }};
    QIODevice* const input = source->start();
    if (input == nullptr || source->error() != QAudio::NoError) {
        return std::unexpected(std::string{"the microphone could not be opened"});
    }
    return DictationInput{
        .device = input,
        .channels = format.channelCount(),
        .sampleRate = format.sampleRate(),
        .sample = sample,
        .keepAlive = source,
    };
}

void askSystemPermission(std::function<void(bool)> answer) {
#if defined(QT_FEATURE_permissions) && QT_FEATURE_permissions == 1
    QMicrophonePermission const microphone;
    switch (qApp->checkPermission(microphone)) {
        case Qt::PermissionStatus::Granted:
            answer(true);
            return;
        case Qt::PermissionStatus::Denied:
            answer(false);
            return;
        case Qt::PermissionStatus::Undetermined:
        default:
            // `default:` only because the project builds with -Wswitch-default; -Wswitch-enum still requires a case
            // for every enumerator.
            qApp->requestPermission(microphone, qApp, [answer = std::move(answer)](QPermission const& result) {
                answer(result.status() == Qt::PermissionStatus::Granted);
            });
            return;
    }
#else
    answer(true);
#endif
}

}  // namespace

// One recording, from `start` to its clip or its refusal.
struct QtDictationRecorder::Take {
    explicit Take(ui::chat::DictationLimits given) : limits{given} {}

    ui::chat::DictationLimits limits;
    // Scopes the connections: destroying it disconnects them.
    QObject context;
    DictationInput input;
    QByteArray leftover;
    std::optional<ui::chat::SpeechResampler> resampler;
    std::optional<ui::chat::DictationClip> clip;
    QTimer ticker;
    std::chrono::seconds shown{0};
    bool begun = false;
};

QtDictationRecorder::QtDictationRecorder(QtDictationOptions options)
    : _options{std::move(options)}, _usesSystemInput{!_options.openInput} {
    if (!_options.openInput) {
        _options.openInput = openSystemInput;
    }
    if (!_options.askPermission) {
        _options.askPermission = askSystemPermission;
    }
}

QtDictationRecorder::~QtDictationRecorder() = default;

std::optional<std::string> QtDictationRecorder::unavailable() const {
    if (_usesSystemInput && QMediaDevices::defaultAudioInput().isNull()) {
        return std::string{kNoDevice};
    }
    return std::nullopt;
}

void QtDictationRecorder::start(ui::chat::DictationLimits limits) {
    if (_take != nullptr) {
        if (onStarted) {
            onStarted(std::unexpected(std::string{"a recording is already running"}));
        }
        return;
    }
    _take = std::make_unique<Take>(limits);
    ++_generation;
    _options.askPermission([this, generation = _generation, alive = std::weak_ptr<bool>{_alive}](bool granted) {
        if (alive.expired() || _take == nullptr || generation != _generation || _take->begun) {
            return;
        }
        if (granted) {
            begin();
        } else {
            refuse(kDenied);
        }
    });
}

void QtDictationRecorder::begin() {
    std::expected<DictationInput, std::string> opened = _options.openInput();
    if (!opened.has_value()) {
        refuse(std::move(opened.error()));
        return;
    }
    Take& take = *_take;
    take.input = std::move(*opened);
    if (take.input.device == nullptr || sampleBytes(take.input.sample) <= 0) {
        refuse("the microphone could not be opened");
        return;
    }
    take.resampler.emplace(take.input.channels, take.input.sampleRate);
    take.clip.emplace(take.limits, std::chrono::steady_clock::now());
    QObject::connect(take.input.device, &QIODevice::readyRead, &take.context, [this] { read(); });
    QObject::connect(&take.ticker, &QTimer::timeout, &take.context, [this] { tick(); });
    take.ticker.start(kTick);
    take.begun = true;
    if (onStarted) {
        onStarted({});
    }
}

void QtDictationRecorder::refuse(std::string reason) {
    // The refusal can come from the device's own opening; its release is handed to the event loop as `finish` does.
    std::shared_ptr<Take> const take{std::move(_take)};
    QTimer::singleShot(std::chrono::milliseconds{0}, [take] { take->input.keepAlive.reset(); });
    if (onStarted) {
        onStarted(std::unexpected(std::move(reason)));
    }
}

void QtDictationRecorder::read() {
    if (_take == nullptr) {
        return;
    }
    Take& take = *_take;
    if (!take.resampler.has_value() || !take.clip.has_value() || take.input.device == nullptr) {
        return;
    }
    take.leftover += take.input.device->readAll();
    auto const width = static_cast<qsizetype>(sampleBytes(take.input.sample));
    auto const frameBytes = width * std::max(take.input.channels, 1);
    qsizetype const whole = take.leftover.size() - (take.leftover.size() % frameBytes);
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(whole / width));
    QByteArrayView const bytes{take.leftover};
    for (qsizetype at = 0; at < whole; at += width) {
        samples.push_back(decode(take.input.sample, bytes.sliced(at, width)));
    }
    take.leftover.remove(0, whole);
    std::vector<std::int16_t> const speech = take.resampler->push(samples);
    if (take.clip->append(speech, std::chrono::steady_clock::now()) == ui::chat::ClipState::Full) {
        finish();
    }
}

void QtDictationRecorder::tick() {
    if (_take == nullptr || !_take->clip.has_value()) {
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
        _take = nullptr;
        ++_generation;
        if (onClip) {
            onClip(std::unexpected(std::string{"the recording was stopped"}));
        }
        return;
    }
    finish();
}

void QtDictationRecorder::finish() {
    // `finish` runs inside the device's `readyRead` or the ticker's `timeout`, so neither the device nor the take
    // whose context receives them may go yet: the take is kept by a zero-delay timer on this thread, which releases
    // the device (stopping it) and destroys the take once control is back in the event loop.
    std::shared_ptr<Take> const take{std::move(_take)};
    take->ticker.stop();
    QObject::disconnect(take->input.device, nullptr, &take->context, nullptr);
    QObject::disconnect(&take->ticker, nullptr, &take->context, nullptr);
    std::string wav = take->clip.has_value() ? take->clip->wav() : std::string{};
    QTimer::singleShot(std::chrono::milliseconds{0}, [take] { take->input.keepAlive.reset(); });
    if (onClip) {
        onClip(std::move(wav));
    }
}

}  // namespace morph::qt
