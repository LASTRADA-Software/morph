// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../attributes.hpp"
#include "../backend.hpp"
#include "client.hpp"
#include "protocol.hpp"

/// @file
/// @brief The chat's panel, built from backend widgets so every shell frontend has it. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief A conversation's lines above an input, a send button and, while a turn runs, a cancel button.
///
/// Reads where the person is at each send, so a message goes to the screen they are looking at when they send it. A
/// shell calls `refresh` from the client's change callback, and when the active tab changes. Neither copyable nor
/// movable; owned by the owner thread of its backend.
class ChatPanel {
public:
    /// @brief The panel, appended to @p parent.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param parent  The container it is appended to. Borrowed.
    /// @param client  The client it shows and sends through. Borrowed.
    /// @param context Where the person is now: the open screen's context, with no text. Nothing for a screen with no
    ///                document behind it, such as a host screen, which disables the input.
    ChatPanel(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget& parent MORPH_LIFETIMEBOUND,
              ChatClient& client MORPH_LIFETIMEBOUND, std::function<std::optional<TurnRequest>()> context)
        : _backend{backend},
          _client{client},
          _context{std::move(context)},
          _root{backend.createStack(&parent, Axis::Vertical)},
          _scroll{backend.createScroll(_root.get(), Axis::Vertical)},
          _lines{backend.createStack(_scroll.get(), Axis::Vertical)},
          _inputRow{backend.createStack(_root.get(), Axis::Horizontal)},
          _input{backend.createTextInput(_inputRow.get(), TextInputMode::SingleLine)},
          _send{backend.createButton(_inputRow.get())},
          _cancel{backend.createButton(_inputRow.get())} {
        _root->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _root->setGap(2);
        _root->setTestId("chat_panel");
        _scroll->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _scroll->setTestId("chat_entries");
        _lines->setLayout({.width = Sizing::stretch(), .height = {}});
        _lines->setGap(2);
        _inputRow->setLayout({.width = Sizing::stretch(), .height = {}});
        _inputRow->setGap(1);
        _inputRow->setAlign(Align::Center);
        _input->setLayout({.width = Sizing::stretch(), .height = {}});
        _input->setPlaceholder("Ask about this screen");
        _input->setTestId("chat_input");
        _input->setOnChange([this](std::string text) { _draft = std::move(text); });
        _input->setOnSubmit([this](std::string text) { submit(std::move(text)); });
        _send->setLabel("Send");
        _send->setTestId("chat_send");
        _send->setOnClick([this] { submit(_draft); });
        _cancel->setLabel("Stop");
        _cancel->setEmphasis(ButtonEmphasis::Ghost);
        _cancel->setTestId("chat_cancel");
        _cancel->setOnClick([this] { _client.cancel(); });
        refresh();
    }

    ChatPanel(ChatPanel const&) = delete;
    ChatPanel& operator=(ChatPanel const&) = delete;
    ChatPanel(ChatPanel&&) = delete;
    ChatPanel& operator=(ChatPanel&&) = delete;
    ~ChatPanel() = default;

    /// @brief Redraws from the client, and from where the person is now.
    void refresh() {
        auto const& entries = _client.entries();
        _shown.resize(std::min(_shown.size(), entries.size()));
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index == _shown.size()) {
                _shown.push_back(_backend.createText(_lines.get()));
                _shown.back()->setTestId("chat_entry_" + std::to_string(index));
            }
            show(*_shown.at(index), entries.at(index));
        }
        bool const onScreen = _context && _context().has_value();
        bool const running = _client.running();
        _input->setEnabled(onScreen);
        _send->setEnabled(onScreen && !running);
        _cancel->setVisible(running);
    }

private:
    static void show(TextWidget& line, ChatClient::Entry const& entry) {
        if (entry.kind == "user") {
            line.setRole(TextRole::Normal);
            line.setText("You: " + entry.text);
        } else if (entry.kind == "tool") {
            line.setRole(TextRole::Muted);
            line.setText("Ran " + entry.text);
        } else if (entry.kind == "error") {
            line.setRole(TextRole::Error);
            line.setText(entry.text);
        } else {
            line.setRole(TextRole::Normal);
            line.setText(entry.text);
        }
    }

    void submit(std::string text) {
        bool const blank = std::ranges::all_of(text, [](unsigned char const letter) { return std::isspace(letter); });
        if (blank || _client.running() || !_context) {
            return;
        }
        std::optional<TurnRequest> request = _context();
        if (!request) {
            return;
        }
        request->text = std::move(text);
        _draft.clear();
        _input->setText("");
        _client.send(*request);
    }

    IViewBackend& _backend;
    ChatClient& _client;
    std::function<std::optional<TurnRequest>()> _context;
    std::unique_ptr<StackWidget> _root;
    std::unique_ptr<ScrollWidget> _scroll;
    std::unique_ptr<StackWidget> _lines;
    std::unique_ptr<StackWidget> _inputRow;
    std::unique_ptr<TextInputWidget> _input;
    std::unique_ptr<ButtonWidget> _send;
    std::unique_ptr<ButtonWidget> _cancel;
    std::vector<std::unique_ptr<TextWidget>> _shown;
    std::string _draft;
};

}  // namespace morph::ui::chat
