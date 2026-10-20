// SPDX-License-Identifier: Apache-2.0

#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariantList>
#include <QVariantMap>
#include <utility>

#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

namespace {

/// A relay handler that calls @p action.
std::function<void(QVariant const&)> actionHandler(ui::Action action) {
    return [action = std::move(action)](QVariant const&) {
        if (action) {
            action();
        }
    };
}

// NOLINTBEGIN(misc-no-recursion): a menu is a tree of entries.
QVariantList menuEntries(std::vector<ui::MenuEntry> const& entries) {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(entries.size()));
    for (ui::MenuEntry const& entry : entries) {
        // A null `checked` is an entry that is not checkable.
        QVariant checked = QVariant::fromValue(nullptr);
        if (entry.checked) {
            checked = *entry.checked;
        }
        list.append(QVariantMap{
            {QStringLiteral("label"), toQString(entry.label)},
            {QStringLiteral("icon"), toQString(entry.icon)},
            {QStringLiteral("keys"), toQString(entry.keys)},
            {QStringLiteral("checked"), checked},
            {QStringLiteral("enabled"), entry.enabled},
            {QStringLiteral("items"), menuEntries(entry.items)},
        });
    }
    return list;
}
// NOLINTEND(misc-no-recursion)

}  // namespace

// ── Button ────────────────────────────────────────────────────────────────

ButtonImpl::ButtonImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphButton"), parent} {}

void ButtonImpl::setLabel(std::string_view label) { write("label", toQString(label)); }

void ButtonImpl::setIcon(std::string_view icon) {
    context().checkIcon(icon);
    write("icon", toQString(icon));
}

void ButtonImpl::setEmphasis(ui::ButtonEmphasis emphasis) {
    // MorphButton's `emphasis`: 0 normal, 1 primary, 2 danger.
    int shown = 0;
    if (emphasis == ui::ButtonEmphasis::Primary) {
        shown = 1;
    } else if (emphasis == ui::ButtonEmphasis::Danger) {
        shown = 2;
    }
    write("emphasis", shown);
}

// MorphButton's `style`: 0 push, 1 item.
void ButtonImpl::setStyle(ui::ButtonStyle style) { write("style", style == ui::ButtonStyle::Item ? 1 : 0); }

void ButtonImpl::setSelected(bool selected) { write("selected", selected); }

void ButtonImpl::setOnClick(ui::Action onClick) {
    relay().on(QStringLiteral("click"), actionHandler(std::move(onClick)));
}

std::string ButtonImpl::shownText() const { return toUtf8(read("label").toString()); }

// ── Menu ──────────────────────────────────────────────────────────────────

bool choosable(std::vector<ui::MenuEntry> const& entries, std::vector<std::size_t> const& path) {
    std::vector<ui::MenuEntry> const* level = &entries;
    for (std::size_t depth = 0; depth < path.size(); ++depth) {
        std::size_t const index = path.at(depth);
        if (index >= level->size() || !level->at(index).enabled) {
            return false;
        }
        ui::MenuEntry const& entry = level->at(index);
        if (depth + 1 == path.size()) {
            return entry.items.empty();
        }
        level = &entry.items;
    }
    return false;
}

MenuImpl::MenuImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphMenu"), parent} {}

void MenuImpl::setItems(std::vector<ui::MenuEntry> const& entries) {
    if (entries == _entries) {
        return;
    }
    _entries = entries;
    checkIcons(entries);
    write("entries", menuEntries(entries));
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu's entries are checked as its parent's are.
void MenuImpl::checkIcons(std::vector<ui::MenuEntry> const& entries) const {
    for (ui::MenuEntry const& entry : entries) {
        context().checkIcon(entry.icon);
        checkIcons(entry.items);
    }
}

void MenuImpl::setOnActivate(std::function<void(std::vector<std::size_t>)> onActivate) {
    relay().on(QStringLiteral("activate"), [this, onActivate = std::move(onActivate)](QVariant const& payload) {
        QVariantList const indices = payload.toList();
        std::vector<std::size_t> path;
        path.reserve(static_cast<std::size_t>(indices.size()));
        for (QVariant const& index : indices) {
            int const value = index.toInt();
            if (value < 0) {
                return;
            }
            path.push_back(static_cast<std::size_t>(value));
        }
        if (choosable(_entries, path) && onActivate) {
            onActivate(std::move(path));
        }
    });
}

void MenuImpl::choose(std::vector<std::size_t> const& path) const {
    QVariantList list;
    for (std::size_t const index : path) {
        list.append(static_cast<int>(index));
    }
    call("choose", QVariant{list});
}

// ── Spacer, Busy ──────────────────────────────────────────────────────────

SpacerImpl::SpacerImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphSpacer"), parent} {}

BusyImpl::BusyImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphBusy"), parent} {}

void BusyImpl::setActive(bool active) { write("active", active); }

void BusyImpl::setLabel(std::string_view label) { write("label", toQString(label)); }

// ── Banner ────────────────────────────────────────────────────────────────

BannerImpl::BannerImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphBanner"), parent} {}

void BannerImpl::setTone(ui::Tone tone) { write("tone", static_cast<int>(tone)); }

void BannerImpl::setText(std::string_view text) { write("text", toQString(text)); }

void BannerImpl::setActionLabel(std::string_view label) { write("actionLabel", toQString(label)); }

void BannerImpl::setOnAction(ui::Action onAction) {
    relay().on(QStringLiteral("action"), actionHandler(std::move(onAction)));
}

void BannerImpl::setDismissible(bool dismissible) { write("dismissible", dismissible); }

void BannerImpl::setOnDismiss(ui::Action onDismiss) {
    relay().on(QStringLiteral("dismiss"), actionHandler(std::move(onDismiss)));
}

std::string BannerImpl::shownText() const { return toUtf8(read("text").toString()); }

// ── Badge, Progress, Steps, KeyValue ──────────────────────────────────────

BadgeImpl::BadgeImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphBadge"), parent} {}

void BadgeImpl::setTone(ui::Tone tone) { write("tone", static_cast<int>(tone)); }

void BadgeImpl::setText(std::string_view text) { write("text", toQString(text)); }

void BadgeImpl::setIcon(std::string_view icon) {
    context().checkIcon(icon);
    write("icon", toQString(icon));
}

std::string BadgeImpl::shownText() const { return toUtf8(read("text").toString()); }

ProgressImpl::ProgressImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphProgress"), parent} {}

void ProgressImpl::setValue(std::optional<double> value) { write("value", value ? *value : -1.0); }

void ProgressImpl::setLabel(std::string_view label) { write("label", toQString(label)); }

StepsImpl::StepsImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphSteps"), parent} {}

void StepsImpl::setSteps(std::vector<ui::Step> const& steps) {
    QVariantList list;
    for (ui::Step const& step : steps) {
        list.append(QVariantMap{{QStringLiteral("label"), toQString(step.label)},
                                {QStringLiteral("state"), static_cast<int>(step.state)}});
    }
    write("steps", list);
}

void StepsImpl::setCurrent(std::optional<std::size_t> index) {
    write("current", index ? static_cast<int>(*index) : -1);
}

KeyValueImpl::KeyValueImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphKeyValue"), parent} {}

void KeyValueImpl::setItems(std::vector<ui::KeyValueItem> const& items) {
    QVariantList list;
    for (ui::KeyValueItem const& item : items) {
        list.append(QVariantMap{{QStringLiteral("label"), toQString(item.label)},
                                {QStringLiteral("value"), toQString(item.value)}});
    }
    write("items", list);
}

// ── EmptyState ────────────────────────────────────────────────────────────

EmptyStateImpl::EmptyStateImpl(Context& context, ui::ContainerWidget* parent)
    : ItemWidget{context, QStringLiteral("MorphEmptyState"), parent} {}

void EmptyStateImpl::setTitle(std::string_view title) { write("title", toQString(title)); }

void EmptyStateImpl::setText(std::string_view text) { write("text", toQString(text)); }

void EmptyStateImpl::setIcon(std::string_view icon) {
    context().checkIcon(icon);
    write("icon", toQString(icon));
}

void EmptyStateImpl::setActionLabel(std::string_view label) { write("actionLabel", toQString(label)); }

void EmptyStateImpl::setOnAction(ui::Action onAction) {
    relay().on(QStringLiteral("action"), actionHandler(std::move(onAction)));
}

std::string EmptyStateImpl::shownText() const { return toUtf8(read("title").toString()); }

// ── Custom ────────────────────────────────────────────────────────────────

CustomImpl::CustomImpl(Context& context, Context::Made made, ui::ContainerWidget* parent)
    : ItemWidget{context, made, parent} {}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the signature of CustomWidget::setTextProp, overridden.
void CustomImpl::setTextProp(std::string_view name, std::string_view value) {
    std::string const property{name};
    write(property.c_str(), toQString(value));
}

void CustomImpl::setBoolProp(std::string_view name, bool value) {
    std::string const property{name};
    write(property.c_str(), value);
}

void CustomImpl::setNumberProp(std::string_view name, double value) {
    std::string const property{name};
    write(property.c_str(), value);
}

void CustomImpl::setOnEvent(std::function<void(std::string event, std::string payload)> onEvent) {
    relay().onAny([onEvent = std::move(onEvent)](QString const& event, QVariant const& payload) {
        if (!onEvent) {
            return;
        }
        // A JavaScript array or object arrives wrapped as a QJSValue.
        QVariant const value =
            payload.metaType() == QMetaType::fromType<QJSValue>() ? payload.value<QJSValue>().toVariant() : payload;
        std::string text;
        QJsonValue const json = QJsonValue::fromVariant(value);
        if (json.isObject()) {
            text = QJsonDocument{json.toObject()}.toJson(QJsonDocument::Compact).toStdString();
        } else if (json.isArray()) {
            text = QJsonDocument{json.toArray()}.toJson(QJsonDocument::Compact).toStdString();
        } else if (value.isValid() && !value.isNull()) {
            text = toUtf8(value.toString());
        }
        onEvent(toUtf8(event), std::move(text));
    });
}

}  // namespace morph::qt_quick::detail
