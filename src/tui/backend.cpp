// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <morph/tui/backend.hpp>
#include <utility>

#include "tui/container_widgets.hpp"
#include "tui/context.hpp"
#include "tui/field_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui/widget.hpp"

namespace morph::tui {

struct Backend::Impl {
    Impl(::core::tui::Screen& screen, Clock now) : context{screen} {
        if (now) {
            context.now = std::move(now);
        }
    }
    detail::Context context;
};

namespace {

template <class WidgetType, class... Args>
std::unique_ptr<WidgetType> make(detail::Context& context, ui::ContainerWidget* parent, Args&&... args) {
    auto widget = std::make_unique<WidgetType>(context, std::forward<Args>(args)...);
    detail::attach(context, parent, *widget);
    return widget;
}

}  // namespace

Backend::Backend(::core::tui::Screen& screen, Clock now) : _impl{std::make_unique<Impl>(screen, std::move(now))} {}

Backend::~Backend() = default;

std::unique_ptr<ui::TextWidget> Backend::createText(ui::ContainerWidget* parent) {
    return make<detail::TextImpl>(_impl->context, parent);
}

std::unique_ptr<ui::ButtonWidget> Backend::createButton(ui::ContainerWidget* parent) {
    return make<detail::ButtonImpl>(_impl->context, parent);
}

std::unique_ptr<ui::TextInputWidget> Backend::createTextInput(ui::ContainerWidget* parent, ui::TextInputMode mode) {
    return make<detail::TextInputImpl>(_impl->context, parent, mode);
}

std::unique_ptr<ui::CheckboxWidget> Backend::createCheckbox(ui::ContainerWidget* parent) {
    return make<detail::CheckboxImpl>(_impl->context, parent);
}

std::unique_ptr<ui::SelectWidget> Backend::createSelect(ui::ContainerWidget* parent, ui::SelectStyle style) {
    if (style == ui::SelectStyle::Radio) {
        return make<detail::RadioSelectImpl>(_impl->context, parent);
    }
    return make<detail::DropdownSelectImpl>(_impl->context, parent);
}

std::unique_ptr<ui::MenuWidget> Backend::createMenu(ui::ContainerWidget* parent) {
    return make<detail::MenuImpl>(_impl->context, parent);
}

std::unique_ptr<ui::StackWidget> Backend::createStack(ui::ContainerWidget* parent, ui::Axis axis) {
    return make<detail::StackImpl>(_impl->context, parent, axis);
}

std::unique_ptr<ui::GridWidget> Backend::createGrid(ui::ContainerWidget* parent) {
    return make<detail::GridImpl>(_impl->context, parent);
}

std::unique_ptr<ui::SpacerWidget> Backend::createSpacer(ui::ContainerWidget* parent) {
    return make<detail::SpacerImpl>(_impl->context, parent);
}

std::unique_ptr<ui::PanelWidget> Backend::createPanel(ui::ContainerWidget* parent) {
    return make<detail::PanelImpl>(_impl->context, parent);
}

std::unique_ptr<ui::ScrollWidget> Backend::createScroll(ui::ContainerWidget* parent, ui::Axis axis) {
    return make<detail::ScrollImpl>(_impl->context, parent, axis);
}

std::unique_ptr<ui::SlotWidget> Backend::createSlot(ui::ContainerWidget* parent) {
    return make<detail::SlotImpl>(_impl->context, parent);
}

std::unique_ptr<ui::TabsWidget> Backend::createTabs(ui::ContainerWidget* parent) {
    return make<detail::TabsImpl>(_impl->context, parent);
}

std::unique_ptr<ui::DialogWidget> Backend::createDialog(ui::ContainerWidget* parent) {
    return make<detail::DialogImpl>(_impl->context, parent);
}

std::unique_ptr<ui::BusyWidget> Backend::createBusy(ui::ContainerWidget* parent) {
    return make<detail::BusyImpl>(_impl->context, parent);
}

std::unique_ptr<ui::TableWidget> Backend::createTable(ui::ContainerWidget* parent) {
    return make<detail::TableImpl>(_impl->context, parent);
}

std::unique_ptr<ui::DateTimeInputWidget> Backend::createDateTimeInput(ui::ContainerWidget* parent, ui::DateMode mode,
                                                                      int offsetMinutes) {
    return make<detail::DateTimeInputImpl>(_impl->context, parent, mode, offsetMinutes);
}

std::unique_ptr<ui::SliderWidget> Backend::createSlider(ui::ContainerWidget* parent) {
    return make<detail::SliderImpl>(_impl->context, parent);
}

std::unique_ptr<ui::FilePickerWidget> Backend::createFilePicker(ui::ContainerWidget* parent, ui::FilePickerMode mode) {
    return make<detail::FilePickerImpl>(_impl->context, parent, mode);
}

void Backend::fit() { detail::fit(_impl->context); }

void Backend::focusNext() { detail::moveFocus(_impl->context, detail::Direction::Forward); }

void Backend::focusPrev() { detail::moveFocus(_impl->context, detail::Direction::Backward); }

// Also when the focus is on an open dialog's frame: a dialog that held nothing focusable when it opened (a Switch
// inside it showing no case, say) could focus nothing but its frame, and focusable content has arrived since.
void Backend::focusFirst() {
    auto const& context = _impl->context;
    auto const* const focused = context.screen->focusedComponent();
    auto* const dialog = detail::activeDialog(context);
    bool const onFrame = dialog != nullptr && focused == &dialog->host();
    if (focused == nullptr || onFrame) {
        focusNext();
    }
}

bool Backend::animating() const noexcept { return _impl->context.activeBusy > 0; }

void Backend::advanceAnimation() noexcept { ++_impl->context.animationFrame; }

}  // namespace morph::tui
