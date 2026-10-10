// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace morph::testing {

/// A Select that, like a toolkit combo box being repopulated, reports the option it now marks when its options are
/// replaced. The contract allows this: only `setSelected` promises never to call `onSelect`.
class EchoingSelect final : public ::morph::ui::SelectWidget {
public:
    explicit EchoingSelect(std::unique_ptr<::morph::ui::SelectWidget> inner) : _inner{std::move(inner)} {}
    void setVisible(bool visible) override { _inner->setVisible(visible); }
    void setEnabled(bool enabled) override { _inner->setEnabled(enabled); }
    void setLayout(::morph::ui::LayoutHints const& hints) override { _inner->setLayout(hints); }
    void setDragKey(std::optional<::morph::ui::Key> const& key) override { _inner->setDragKey(key); }
    void setDropHandler(std::function<bool(::morph::ui::Key const&)> accepts,
                        std::function<void(::morph::ui::Key)> onDrop) override {
        _inner->setDropHandler(std::move(accepts), std::move(onDrop));
    }
    void setAccessibleName(std::string_view name) override { _inner->setAccessibleName(name); }
    void setAccessibleRole(std::string_view role) override { _inner->setAccessibleRole(role); }
    void setTestId(std::string_view testId) override { _inner->setTestId(testId); }
    void setTooltip(std::string_view text) override { _inner->setTooltip(text); }
    void setSurface(std::string_view surface) override { _inner->setSurface(surface); }
    void setKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) override {
        _inner->setKeys(chords, std::move(onChord));
    }
    void focus() override { _inner->focus(); }
    void setOptions(std::vector<::morph::ui::SelectOption> const& options) override {
        _inner->setOptions(options);
        if (auto const echo = _onSelect; echo && !options.empty()) {
            echo(options.front().key);
        }
    }
    void setSelected(std::optional<::morph::ui::Key> const& key) override { _inner->setSelected(key); }
    void setOnSelect(std::function<void(::morph::ui::Key)> onSelect) override {
        _onSelect = onSelect;
        _inner->setOnSelect(std::move(onSelect));
    }

private:
    std::unique_ptr<::morph::ui::SelectWidget> _inner;
    std::function<void(::morph::ui::Key)> _onSelect;
};

/// A `RecordingBackend` whose Selects echo (see `EchoingSelect`), and whose factories first run `onCreate`, as a
/// backend that reads state while it builds a widget would.
class EchoingBackend final : public ::morph::ui::IViewBackend {
public:
    [[nodiscard]] ::morph::ui::testing::RecordingBackend& recording() noexcept { return _recording; }
    void setOnCreate(std::function<void()> onCreate) { _onCreate = std::move(onCreate); }

    std::unique_ptr<::morph::ui::TextWidget> createText(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createText(parent);
    }
    std::unique_ptr<::morph::ui::ButtonWidget> createButton(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createButton(parent);
    }
    std::unique_ptr<::morph::ui::TextInputWidget> createTextInput(::morph::ui::ContainerWidget* parent,
                                                                  ::morph::ui::TextInputMode mode) override {
        created();
        return _recording.createTextInput(parent, mode);
    }
    std::unique_ptr<::morph::ui::CheckboxWidget> createCheckbox(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createCheckbox(parent);
    }
    std::unique_ptr<::morph::ui::SelectWidget> createSelect(::morph::ui::ContainerWidget* parent,
                                                            ::morph::ui::SelectStyle style) override {
        created();
        return std::make_unique<EchoingSelect>(_recording.createSelect(parent, style));
    }
    std::unique_ptr<::morph::ui::MenuWidget> createMenu(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createMenu(parent);
    }
    std::unique_ptr<::morph::ui::StackWidget> createStack(::morph::ui::ContainerWidget* parent,
                                                          ::morph::ui::Axis axis) override {
        created();
        return _recording.createStack(parent, axis);
    }
    std::unique_ptr<::morph::ui::GridWidget> createGrid(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createGrid(parent);
    }
    std::unique_ptr<::morph::ui::SpacerWidget> createSpacer(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createSpacer(parent);
    }
    std::unique_ptr<::morph::ui::PanelWidget> createPanel(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createPanel(parent);
    }
    std::unique_ptr<::morph::ui::ScrollWidget> createScroll(::morph::ui::ContainerWidget* parent,
                                                            ::morph::ui::Axis axis) override {
        created();
        return _recording.createScroll(parent, axis);
    }
    std::unique_ptr<::morph::ui::SlotWidget> createSlot(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createSlot(parent);
    }
    std::unique_ptr<::morph::ui::TabsWidget> createTabs(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createTabs(parent);
    }
    std::unique_ptr<::morph::ui::DialogWidget> createDialog(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createDialog(parent);
    }
    std::unique_ptr<::morph::ui::BusyWidget> createBusy(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createBusy(parent);
    }
    std::unique_ptr<::morph::ui::TableWidget> createTable(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createTable(parent);
    }
    std::unique_ptr<::morph::ui::DateTimeInputWidget> createDateTimeInput(::morph::ui::ContainerWidget* parent,
                                                                          ::morph::ui::DateMode mode,
                                                                          int offsetMinutes) override {
        created();
        return _recording.createDateTimeInput(parent, mode, offsetMinutes);
    }
    std::unique_ptr<::morph::ui::SliderWidget> createSlider(::morph::ui::ContainerWidget* parent) override {
        created();
        return _recording.createSlider(parent);
    }
    std::unique_ptr<::morph::ui::FilePickerWidget> createFilePicker(::morph::ui::ContainerWidget* parent,
                                                                    ::morph::ui::FilePickerMode mode) override {
        created();
        return _recording.createFilePicker(parent, mode);
    }

private:
    void created() const {
        if (_onCreate) {
            _onCreate();
        }
    }

    ::morph::ui::testing::RecordingBackend _recording;
    std::function<void()> _onCreate;
};

}  // namespace morph::testing
