// SPDX-License-Identifier: Apache-2.0

#include <QQmlEngine>
#include <QQuickItem>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/components.hpp>
#include <utility>

#include "qt_quick/context.hpp"
#include "qt_quick/kinds.hpp"
#include "qt_quick/relay.hpp"

namespace morph::qt_quick {

Backend::Backend(QQmlEngine& engine, QQuickItem& host, BackendOptions options)
    : _context{std::make_unique<detail::Context>(engine, host, std::move(options))} {}

Backend::~Backend() = default;

QQuickItem* Backend::itemOf(ui::Widget const& widget) noexcept {
    detail::ItemBase const* const base = detail::baseOf(widget);
    return base != nullptr ? base->item() : nullptr;
}

std::unique_ptr<CustomWidget> Backend::createCustom(ui::ContainerWidget* parent, CustomRequest const& request) {
    Components* const components = _context->components();
    if (components == nullptr) {
        return nullptr;
    }
    auto loaded = components->load(_context->engine(), request.name, request.minVersion);
    if (!loaded) {
        return nullptr;
    }
    std::unique_ptr<QObject> object{(*loaded)->create()};
    auto* const item = qobject_cast<QQuickItem*>(object.get());
    if (item == nullptr) {
        components->report(
            ComponentFailure{.name = request.name, .reason = "component " + request.name + " does not make an item"});
        return nullptr;
    }
    for (std::string const& prop : request.props) {
        if (item->metaObject()->indexOfProperty(prop.c_str()) < 0) {
            components->report(ComponentFailure{
                .name = request.name, .reason = "component " + request.name + " does not declare prop " + prop});
            return nullptr;
        }
    }
    static_cast<void>(object.release());
    QQmlEngine::setObjectOwnership(item, QQmlEngine::CppOwnership);
    auto* const relay = new detail::Relay{item};
    if (item->metaObject()->indexOfProperty("host") >= 0) {
        item->setProperty("host", QVariant::fromValue<QObject*>(relay));
    }
    item->setParent(_context->owner());
    return std::make_unique<detail::CustomImpl>(*_context, detail::Context::Made{.item = item, .relay = relay},
                                                parent);
}

std::unique_ptr<ui::TextWidget> Backend::createText(ui::ContainerWidget* parent) {
    return std::make_unique<detail::TextImpl>(*_context, parent);
}

std::unique_ptr<ui::ButtonWidget> Backend::createButton(ui::ContainerWidget* parent) {
    return std::make_unique<detail::ButtonImpl>(*_context, parent);
}

std::unique_ptr<ui::TextInputWidget> Backend::createTextInput(ui::ContainerWidget* parent, ui::TextInputMode mode) {
    return std::make_unique<detail::TextInputImpl>(*_context, parent, mode);
}

std::unique_ptr<ui::CheckboxWidget> Backend::createCheckbox(ui::ContainerWidget* parent) {
    return std::make_unique<detail::CheckboxImpl>(*_context, parent);
}

std::unique_ptr<ui::SelectWidget> Backend::createSelect(ui::ContainerWidget* parent, ui::SelectStyle style) {
    return std::make_unique<detail::SelectImpl>(*_context, parent, style);
}

std::unique_ptr<ui::MenuWidget> Backend::createMenu(ui::ContainerWidget* parent) {
    return std::make_unique<detail::MenuImpl>(*_context, parent);
}

std::unique_ptr<ui::StackWidget> Backend::createStack(ui::ContainerWidget* parent, ui::Axis axis) {
    // A horizontal stack the mount makes inside a table is one of its rows.
    if (auto* const table = dynamic_cast<detail::TableImpl*>(parent);
        table != nullptr && axis == ui::Axis::Horizontal) {
        return std::make_unique<detail::TableRowImpl>(*_context, parent, *table);
    }
    return std::make_unique<detail::StackImpl>(*_context, parent, axis);
}

std::unique_ptr<ui::GridWidget> Backend::createGrid(ui::ContainerWidget* parent) {
    return std::make_unique<detail::GridImpl>(*_context, parent);
}

std::unique_ptr<ui::SpacerWidget> Backend::createSpacer(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SpacerImpl>(*_context, parent);
}

std::unique_ptr<ui::PanelWidget> Backend::createPanel(ui::ContainerWidget* parent) {
    return std::make_unique<detail::PanelImpl>(*_context, parent);
}

std::unique_ptr<ui::ScrollWidget> Backend::createScroll(ui::ContainerWidget* parent, ui::Axis axis) {
    return std::make_unique<detail::ScrollImpl>(*_context, parent, axis);
}

std::unique_ptr<ui::SlotWidget> Backend::createSlot(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SlotImpl>(*_context, parent);
}

std::unique_ptr<ui::TabsWidget> Backend::createTabs(ui::ContainerWidget* parent) {
    return std::make_unique<detail::TabsImpl>(*_context, parent);
}

std::unique_ptr<ui::DialogWidget> Backend::createDialog(ui::ContainerWidget* parent) {
    return std::make_unique<detail::DialogImpl>(*_context, QStringLiteral("MorphDialog"), parent);
}

std::unique_ptr<ui::BusyWidget> Backend::createBusy(ui::ContainerWidget* parent) {
    return std::make_unique<detail::BusyImpl>(*_context, parent);
}

std::unique_ptr<ui::TableWidget> Backend::createTable(ui::ContainerWidget* parent) {
    return std::make_unique<detail::TableImpl>(*_context, parent);
}

std::unique_ptr<ui::DateTimeInputWidget> Backend::createDateTimeInput(ui::ContainerWidget* parent, ui::DateMode mode,
                                                                      int offsetMinutes) {
    return std::make_unique<detail::DateTimeImpl>(*_context, parent, mode, offsetMinutes);
}

std::unique_ptr<ui::SliderWidget> Backend::createSlider(ui::ContainerWidget* parent) {
    return std::make_unique<detail::SliderImpl>(*_context, parent);
}

std::unique_ptr<ui::FilePickerWidget> Backend::createFilePicker(ui::ContainerWidget* parent, ui::FilePickerMode mode) {
    return std::make_unique<detail::FilePickerImpl>(*_context, parent, mode);
}

std::unique_ptr<ui::BannerWidget> Backend::createBanner(ui::ContainerWidget* parent) {
    return std::make_unique<detail::BannerImpl>(*_context, parent);
}

std::unique_ptr<ui::BadgeWidget> Backend::createBadge(ui::ContainerWidget* parent) {
    return std::make_unique<detail::BadgeImpl>(*_context, parent);
}

std::unique_ptr<ui::ProgressWidget> Backend::createProgress(ui::ContainerWidget* parent) {
    return std::make_unique<detail::ProgressImpl>(*_context, parent);
}

std::unique_ptr<ui::StepsWidget> Backend::createSteps(ui::ContainerWidget* parent) {
    return std::make_unique<detail::StepsImpl>(*_context, parent);
}

std::unique_ptr<ui::KeyValueWidget> Backend::createKeyValue(ui::ContainerWidget* parent) {
    return std::make_unique<detail::KeyValueImpl>(*_context, parent);
}

std::unique_ptr<ui::EmptyStateWidget> Backend::createEmptyState(ui::ContainerWidget* parent) {
    return std::make_unique<detail::EmptyStateImpl>(*_context, parent);
}

std::unique_ptr<ui::DrawerWidget> Backend::createDrawer(ui::ContainerWidget* parent, ui::Side side) {
    return std::make_unique<detail::DrawerImpl>(*_context, QStringLiteral("MorphDrawer"), parent,
                                                QVariantMap{{QStringLiteral("side"), side == ui::Side::End ? 1 : 0}});
}

std::unique_ptr<ui::SplitterWidget> Backend::createSplitter(ui::ContainerWidget* parent, ui::Axis axis) {
    return std::make_unique<detail::SplitterImpl>(*_context, parent, axis);
}

std::unique_ptr<ui::CollapsibleWidget> Backend::createCollapsible(ui::ContainerWidget* parent) {
    return std::make_unique<detail::CollapsibleImpl>(*_context, parent);
}

std::unique_ptr<ui::DropZoneWidget> Backend::createDropZone(ui::ContainerWidget* parent) {
    return std::make_unique<detail::DropZoneImpl>(*_context, parent);
}

}  // namespace morph::qt_quick
