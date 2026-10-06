// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/VtParser.hpp>
#include <cstddef>
#include <memory>
#include <morph/ui/backend.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/context.hpp"
#include "tui/widget.hpp"

namespace morph::tui::testing {

/// The rows of the screen's last frame, trailing blanks removed and trailing empty rows dropped.
inline std::vector<std::string> rowsOf(::core::tui::Screen const& screen) {
    auto const text = ::core::tui::test::canvasToString(screen.renderedBuffer());
    std::vector<std::string> rows;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto const end = std::min(text.find('\n', start), text.size());
        auto row = text.substr(start, end - start);
        while (!row.empty() && row.back() == ' ') {
            row.pop_back();
        }
        rows.push_back(std::move(row));
        start = end + 1;
    }
    while (!rows.empty() && rows.back().empty()) {
        rows.pop_back();
    }
    return rows;
}

/// A mock terminal whose size a test can change, and which counts the frames drawn to it.
class ResizableOutput final : public ::core::tui::MockTerminalOutput {
public:
    explicit ResizableOutput(::core::tui::Size size) : MockTerminalOutput{size.width, size.height}, _size{size} {}
    [[nodiscard]] auto columns() const noexcept -> int override { return _size.width; }
    [[nodiscard]] auto rows() const noexcept -> int override { return _size.height; }
    void resize(::core::tui::Size size) noexcept { _size = size; }
    /// One synchronised update per `Screen::draw()` in fullscreen mode, so this counts frames.
    [[nodiscard]] auto syncGuard() -> ::core::tui::SyncGuard override {
        ++_frames;
        return MockTerminalOutput::syncGuard();
    }
    [[nodiscard]] int frames() const noexcept { return _frames; }

private:
    ::core::tui::Size _size;
    int _frames = 0;
};

/// A headless screen and the widget context of one backend over it.
class Harness {
public:
    explicit Harness(int columns = 40, int rows = 12)
        : Harness{std::make_unique<::core::tui::MockTerminalOutput>(columns, rows)} {}
    explicit Harness(std::unique_ptr<::core::tui::TerminalOutput> output)
        : _terminal{std::move(output)}, _screen{_terminal}, _context{_screen} {}

    /// Creates a widget and attaches it under @p parent (null: the screen's root), as a backend factory does.
    template <class Widget, class... Args>
    std::unique_ptr<Widget> make(ui::ContainerWidget* parent, Args&&... args) {
        auto widget = std::make_unique<Widget>(_context, std::forward<Args>(args)...);
        detail::attach(_context, parent, *widget);
        return widget;
    }

    /// Fits the roots to the screen, draws a frame and returns its rows.
    std::vector<std::string> draw() {
        detail::fit(_context);
        _screen.draw();
        return rowsOf(_screen);
    }

    ::core::tui::EventResult send(::core::tui::InputEvent const& event) { return _screen.dispatchEvent(event); }

    ::core::tui::EventResult key(::core::tui::KeyCode code,
                                 ::core::tui::Modifier modifiers = ::core::tui::Modifier::None) {
        return send(::core::tui::test::specialKey(code, modifiers));
    }

    /// Sends one key event per character of @p text; returns the last result.
    ::core::tui::EventResult type(std::string_view text) {
        auto result = ::core::tui::EventResult::Ignored;
        for (char const character : text) {
            result = send(::core::tui::test::charKey(character));
        }
        return result;
    }

    /// A left-button press and release on a 0-based viewport cell; returns the release's result.
    ::core::tui::EventResult click(::core::tui::Point cell) {
        using Type = ::core::tui::MouseEvent::Type;
        static_cast<void>(
            send(::core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = cell.x + 1, .y = cell.y + 1}));
        return send(::core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = cell.x + 1, .y = cell.y + 1});
    }

    /// Decodes @p bytes as terminal input (SGR mouse reports included) and dispatches each event.
    void sgr(std::string_view bytes) {
        ::core::tui::VtParser parser;
        for (auto const& event : parser.feed(bytes)) {
            static_cast<void>(_screen.dispatchEvent(event));
        }
    }

    void focus(ui::Widget const& widget) { _screen.setFocus(&detail::WidgetBase::of(widget).view()); }

    [[nodiscard]] bool focused(ui::Widget const& widget) const {
        return _screen.focusedComponent() == &detail::WidgetBase::of(widget).view();
    }

    [[nodiscard]] ::core::tui::Screen& screen() noexcept { return _screen; }
    [[nodiscard]] detail::Context& context() noexcept { return _context; }

private:
    ::core::tui::Terminal _terminal;
    ::core::tui::Screen _screen;
    detail::Context _context;
};

}  // namespace morph::tui::testing
