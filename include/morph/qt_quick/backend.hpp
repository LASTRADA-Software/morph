// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../attributes.hpp"
#include "../ui/backend.hpp"

class QQmlEngine;
class QQuickItem;

/// @file
/// @brief `morph::qt_quick::Backend`: the view tree's widgets as Qt Quick items.
///
/// Specified in `docs/spec/qt_quick/backend.md`.

namespace morph::qt_quick {

class Components;

namespace detail {
class Context;
}  // namespace detail

/// @brief What a `Backend` is made with beside its engine and host item.
struct BackendOptions {
    /// @brief The restyle map and the custom components; null renders every kind with its `MorphUi` component and
    ///        loads no custom component. Borrowed: it outlives the backend.
    Components* components = nullptr;
};

// The view tree's custom component and request, which `Backend::createCustom` makes and takes.
using ui::CustomRequest;
using ui::CustomWidget;

/// @brief `ui::IViewBackend` over Qt Quick: each widget is a Qt Quick item made from its kind's `MorphUi`
///        component, or from the component the restyle map names for that kind.
///
/// Every root a mount makes is appended to a column the backend adds to the host item, so several roots share it
/// top to bottom. Neither copyable nor movable. It belongs to the thread of its engine, and every widget it makes
/// is destroyed before it.
class Backend final : public ui::IViewBackend {
public:
    /// @brief A backend whose roots fill @p host.
    /// @param engine  The engine its components are compiled in. Borrowed.
    /// @param host    The item its roots are shown in. Borrowed.
    /// @param options The components.
    Backend(QQmlEngine& engine MORPH_LIFETIMEBOUND, QQuickItem& host MORPH_LIFETIMEBOUND, BackendOptions options = {});
    /// @brief Removes the roots' column from the host.
    ~Backend() override;
    Backend(Backend const&) = delete;
    Backend& operator=(Backend const&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    /// @brief The Qt Quick item a widget of this backend shows.
    /// @param widget The widget.
    /// @return Its item; null for a widget this backend did not make.
    [[nodiscard]] static QQuickItem* itemOf(ui::Widget const& widget) noexcept;

    /// @brief Loads a custom component by name.
    ///
    /// A host component loads into this backend's engine; a bundle component must match its digest and compiles in
    /// the components' contained engine. A component that cannot be loaded — not registered, a host component below
    /// `minVersion`, a digest that does not match, a compile error, a declared prop it lacks — is reported once
    /// through the components' failure callback, and the caller shows the fallback.
    /// @param parent  The container to append it to; null makes a root.
    /// @param request The component and the props the document declares.
    /// @return The widget, or null when the fallback is shown instead.
    [[nodiscard]] std::unique_ptr<CustomWidget> createCustom(ui::ContainerWidget* parent,
                                                             CustomRequest const& request) override;

    /// @brief The host item's width class: its width in `Theme.unitWidth`s, classed by `ui::widthClassOf`.
    /// @return The class.
    [[nodiscard]] std::string widthClass() const override;

    /// @copydoc ui::IViewBackend::setOnWidthClass
    void setOnWidthClass(std::function<void(std::string const&)> onWidthClass) override;

    /// @copydoc ui::IViewBackend::createText
    [[nodiscard]] std::unique_ptr<ui::TextWidget> createText(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createButton
    [[nodiscard]] std::unique_ptr<ui::ButtonWidget> createButton(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createTextInput
    [[nodiscard]] std::unique_ptr<ui::TextInputWidget> createTextInput(ui::ContainerWidget* parent,
                                                                       ui::TextInputMode mode) override;
    /// @copydoc ui::IViewBackend::createCheckbox
    [[nodiscard]] std::unique_ptr<ui::CheckboxWidget> createCheckbox(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createSelect
    [[nodiscard]] std::unique_ptr<ui::SelectWidget> createSelect(ui::ContainerWidget* parent,
                                                                 ui::SelectStyle style) override;
    /// @copydoc ui::IViewBackend::createMenu
    [[nodiscard]] std::unique_ptr<ui::MenuWidget> createMenu(ui::ContainerWidget* parent,
                                                             ui::MenuStyle style) override;
    /// @copydoc ui::IViewBackend::createStack
    [[nodiscard]] std::unique_ptr<ui::StackWidget> createStack(ui::ContainerWidget* parent, ui::Axis axis) override;
    /// @copydoc ui::IViewBackend::createGrid
    [[nodiscard]] std::unique_ptr<ui::GridWidget> createGrid(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createSpacer
    [[nodiscard]] std::unique_ptr<ui::SpacerWidget> createSpacer(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createPanel
    [[nodiscard]] std::unique_ptr<ui::PanelWidget> createPanel(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createScroll
    [[nodiscard]] std::unique_ptr<ui::ScrollWidget> createScroll(ui::ContainerWidget* parent, ui::Axis axis) override;
    /// @copydoc ui::IViewBackend::createSlot
    [[nodiscard]] std::unique_ptr<ui::SlotWidget> createSlot(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createTabs
    [[nodiscard]] std::unique_ptr<ui::TabsWidget> createTabs(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createDialog
    [[nodiscard]] std::unique_ptr<ui::DialogWidget> createDialog(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createToast
    [[nodiscard]] std::unique_ptr<ui::ToastWidget> createToast(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createBusy
    [[nodiscard]] std::unique_ptr<ui::BusyWidget> createBusy(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createTable
    [[nodiscard]] std::unique_ptr<ui::TableWidget> createTable(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createDateTimeInput
    [[nodiscard]] std::unique_ptr<ui::DateTimeInputWidget> createDateTimeInput(ui::ContainerWidget* parent,
                                                                               ui::DateMode mode,
                                                                               int offsetMinutes) override;
    /// @copydoc ui::IViewBackend::createSlider
    [[nodiscard]] std::unique_ptr<ui::SliderWidget> createSlider(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createFilePicker
    [[nodiscard]] std::unique_ptr<ui::FilePickerWidget> createFilePicker(ui::ContainerWidget* parent,
                                                                         ui::FilePickerMode mode) override;
    /// @copydoc ui::IViewBackend::createBanner
    [[nodiscard]] std::unique_ptr<ui::BannerWidget> createBanner(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createBadge
    [[nodiscard]] std::unique_ptr<ui::BadgeWidget> createBadge(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createProgress
    [[nodiscard]] std::unique_ptr<ui::ProgressWidget> createProgress(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createSteps
    [[nodiscard]] std::unique_ptr<ui::StepsWidget> createSteps(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createKeyValue
    [[nodiscard]] std::unique_ptr<ui::KeyValueWidget> createKeyValue(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createEmptyState
    [[nodiscard]] std::unique_ptr<ui::EmptyStateWidget> createEmptyState(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createChart
    [[nodiscard]] std::unique_ptr<ui::ChartWidget> createChart(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createDrawer
    [[nodiscard]] std::unique_ptr<ui::DrawerWidget> createDrawer(ui::ContainerWidget* parent, ui::Side side) override;
    /// @copydoc ui::IViewBackend::createSplitter
    [[nodiscard]] std::unique_ptr<ui::SplitterWidget> createSplitter(ui::ContainerWidget* parent,
                                                                     ui::Axis axis) override;
    /// @copydoc ui::IViewBackend::createCollapsible
    [[nodiscard]] std::unique_ptr<ui::CollapsibleWidget> createCollapsible(ui::ContainerWidget* parent) override;
    /// @copydoc ui::IViewBackend::createDropZone
    [[nodiscard]] std::unique_ptr<ui::DropZoneWidget> createDropZone(ui::ContainerWidget* parent) override;

private:
    std::unique_ptr<detail::Context> _context;
};

}  // namespace morph::qt_quick
