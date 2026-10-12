// SPDX-License-Identifier: Apache-2.0

#include <QByteArray>
#include <QCryptographicHash>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QUrl>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <morph/qt_quick/components.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "qt_quick/keys.hpp"

namespace morph::qt_quick {

namespace {

struct KindType {
    std::string_view kind;
    std::string_view type;
};

// The kinds a bundle may restyle, and the MorphUi type each one is. The layout kinds are not restyled: they place
// other widgets and show nothing of their own.
// NOLINTBEGIN(modernize-use-designated-initializers): a table of positional pairs, kind beside type, reads as one.
constexpr std::array<KindType, 22> kRestylable = {
    {{"text", "MorphText"},           {"button", "MorphButton"},
     {"textInput", "MorphTextInput"}, {"checkbox", "MorphCheckbox"},
     {"select", "MorphSelect"},       {"menu", "MorphMenu"},
     {"panel", "MorphPanel"},         {"tabs", "MorphTabs"},
     {"dialog", "MorphDialog"},       {"busy", "MorphBusy"},
     {"table", "MorphTable"},         {"dateTimeInput", "MorphDateTimeInput"},
     {"slider", "MorphSlider"},       {"filePicker", "MorphFilePicker"},
     {"banner", "MorphBanner"},       {"badge", "MorphBadge"},
     {"progress", "MorphProgress"},   {"steps", "MorphSteps"},
     {"keyValue", "MorphKeyValue"},   {"emptyState", "MorphEmptyState"},
     {"drawer", "MorphDrawer"},       {"collapsible", "MorphCollapsible"}}};
// NOLINTEND(modernize-use-designated-initializers)

std::string errorsOf(QQmlComponent const& component) {
    std::string text;
    for (QQmlError const& error : component.errors()) {
        if (!text.empty()) {
            text += "; ";
        }
        text += detail::toUtf8(error.toString());
    }
    return text;
}

/// The names of the properties and methods a QML object declares beyond `QQuickItem`'s.
std::set<QByteArray> declaredNames(QObject const& object) {
    std::set<QByteArray> names;
    QMetaObject const* const meta = object.metaObject();
    for (int index = QQuickItem::staticMetaObject.propertyCount(); index < meta->propertyCount(); ++index) {
        names.insert(QByteArray{meta->property(index).name()});
    }
    for (int index = QQuickItem::staticMetaObject.methodCount(); index < meta->methodCount(); ++index) {
        names.insert(meta->method(index).name());
    }
    return names;
}

}  // namespace

struct Components::State {
    struct Host {
        int version = 0;
        QUrl source;
    };
    struct Bundle {
        std::string qml;
        std::string digest;
    };

    std::function<void(ComponentFailure const&)> report;
    std::map<std::string, Host, std::less<>> hosts;
    std::map<std::string, Bundle, std::less<>> bundles;
    // MorphUi type → component name.
    std::map<std::string, std::string, std::less<>> restyles;
    std::map<std::string, std::unique_ptr<QQmlComponent>, std::less<>> compiled;
    std::vector<ComponentFailure> failures;
    std::set<std::pair<std::string, std::string>> reported;
    // Where bundle components compile: an engine whose root context holds nothing.
    std::unique_ptr<QQmlEngine> contained;

    QQmlEngine& containedEngine() {
        if (contained == nullptr) {
            contained = std::make_unique<QQmlEngine>();
        }
        return *contained;
    }

    /// Why @p name cannot be loaded at @p minVersion, or none: not registered, a host component below the version,
    /// a bundle component whose text does not match its digest.
    [[nodiscard]] std::optional<std::string> refusal(std::string const& name, int minVersion) const {
        if (auto const host = hosts.find(name); host != hosts.end()) {
            if (host->second.version < minVersion) {
                return "host component " + name + " is version " + std::to_string(host->second.version) +
                       ", below the " + std::to_string(minVersion) + " the document needs";
            }
            return std::nullopt;
        }
        auto const bundle = bundles.find(name);
        if (bundle == bundles.end()) {
            return "component " + name + " is not registered";
        }
        QByteArray const hash =
            QCryptographicHash::hash(QByteArray::fromStdString(bundle->second.qml), QCryptographicHash::Sha256)
                .toHex();
        std::string given = bundle->second.digest;
        std::ranges::transform(given, given.begin(),
                               [](unsigned char chr) { return static_cast<char>(std::tolower(chr)); });
        if (given != "sha256:" + hash.toStdString()) {
            return "component " + name + " does not match its digest";
        }
        return std::nullopt;
    }

    [[nodiscard]] std::unique_ptr<QQmlComponent> compile(QQmlEngine& engine, std::string const& name) {
        if (auto const host = hosts.find(name); host != hosts.end()) {
            return std::make_unique<QQmlComponent>(&engine, host->second.source, QQmlComponent::PreferSynchronous);
        }
        auto const bundle = bundles.find(name);
        if (bundle == bundles.end()) {
            return nullptr;
        }
        auto component = std::make_unique<QQmlComponent>(&containedEngine());
        // A `qrc:` base URL compiles synchronously, and one in a directory no resource has leaves a relative import
        // nothing to find.
        component->setData(
            QByteArray::fromStdString(bundle->second.qml),
            QUrl{QStringLiteral("qrc:/morph-bundle/") + detail::toQString(name) + QStringLiteral(".qml")});
        return component;
    }
};

Components::Components(std::function<void(ComponentFailure const&)> report) : _state{std::make_unique<State>()} {
    _state->report = std::move(report);
}

Components::~Components() {
    // The compiled components belong to the contained engine, and go first.
    _state->compiled.clear();
}

void Components::addHost(std::string name, int version, QUrl const& source) {
    std::erase_if(_state->compiled,
                  [&name](auto const& entry) { return entry.first == name || entry.first.starts_with(name + "@"); });
    _state->bundles.erase(name);
    _state->hosts.insert_or_assign(std::move(name), State::Host{.version = version, .source = source});
}

void Components::addBundle(std::string name, std::string qml, std::string digest) {
    std::erase_if(_state->compiled,
                  [&name](auto const& entry) { return entry.first == name || entry.first.starts_with(name + "@"); });
    _state->hosts.erase(name);
    _state->bundles.insert_or_assign(std::move(name),
                                     State::Bundle{.qml = std::move(qml), .digest = std::move(digest)});
}

void Components::report(ComponentFailure const& failure) {
    if (!_state->reported.emplace(failure.name, failure.reason).second) {
        return;
    }
    _state->failures.push_back(failure);
    if (_state->report) {
        _state->report(failure);
    }
}

std::vector<ComponentFailure> const& Components::failures() const noexcept { return _state->failures; }

std::expected<QQmlComponent*, ComponentFailure> Components::load(QQmlEngine& engine, std::string const& name,
                                                                 int minVersion) {
    auto fail = [&](std::string reason) -> std::expected<QQmlComponent*, ComponentFailure> {
        ComponentFailure failure{.name = name, .reason = std::move(reason)};
        report(failure);
        return std::unexpected{std::move(failure)};
    };
    if (std::optional<std::string> refused = _state->refusal(name, minVersion)) {
        return fail(std::move(*refused));
    }
    // A host component compiles in the engine it is asked for, a bundle component in the contained one.
    std::string const key =
        _state->hosts.contains(name) ? std::format("{}@{}", name, static_cast<void const*>(&engine)) : name;
    auto found = _state->compiled.find(key);
    if (found == _state->compiled.end()) {
        found = _state->compiled.emplace(key, _state->compile(engine, name)).first;
    }
    QQmlComponent const* const compiled = found->second.get();
    if (compiled == nullptr || !compiled->isReady()) {
        return fail("component " + name + " does not compile: " + (compiled != nullptr ? errorsOf(*compiled) : ""));
    }
    return found->second.get();
}

std::expected<void, ComponentFailure> Components::restyle(QQmlEngine& engine, std::string const& kind,
                                                          std::string const& component) {
    auto fail = [&](std::string reason) -> std::expected<void, ComponentFailure> {
        ComponentFailure failure{.name = kind, .reason = std::move(reason)};
        report(failure);
        return std::unexpected{std::move(failure)};
    };
    auto const index = static_cast<std::size_t>(
        std::ranges::distance(kRestylable.begin(), std::ranges::find(kRestylable, kind, &KindType::kind)));
    if (index >= kRestylable.size()) {
        return fail("`" + kind + "` is not a kind that can be restyled");
    }
    if (std::optional<std::string> const refused = _state->refusal(component, 0)) {
        return fail("restyle of `" + kind + "`: " + *refused);
    }
    std::unique_ptr<QQmlComponent> const loaded = _state->compile(engine, component);
    if (loaded == nullptr || !loaded->isReady()) {
        return fail("restyle of `" + kind + "`: component " + component +
                    " does not compile: " + (loaded != nullptr ? errorsOf(*loaded) : ""));
    }
    QQmlComponent base{&engine};
    base.loadFromModule(QStringLiteral("MorphUi"), detail::toQString(kRestylable.at(index).type),
                        QQmlComponent::PreferSynchronous);
    std::unique_ptr<QObject> const baseObject{base.create()};
    std::unique_ptr<QObject> const candidate{loaded->create()};
    if (baseObject == nullptr || candidate == nullptr) {
        return fail("restyle of `" + kind + "`: " + component + " does not make an object");
    }
    std::set<QByteArray> const wanted = declaredNames(*baseObject);
    std::set<QByteArray> const given = declaredNames(*candidate);
    std::string missing;
    for (QByteArray const& name : wanted) {
        if (!given.contains(name)) {
            missing += (missing.empty() ? "" : ", ") + name.toStdString();
        }
    }
    if (!missing.empty()) {
        return fail("restyle of `" + kind + "`: " + component + " does not declare " + missing);
    }
    _state->restyles.insert_or_assign(std::string{kRestylable.at(index).type}, component);
    return {};
}

std::unique_ptr<QQmlComponent> Components::restyled(QQmlEngine& engine, std::string const& type) {
    auto const found = _state->restyles.find(type);
    if (found == _state->restyles.end()) {
        return nullptr;
    }
    return _state->compile(engine, found->second);
}

}  // namespace morph::qt_quick
