// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/table_state_model.hpp>.

#include <algorithm>
#include <chrono>
#include <core/async/ExecutorContext.hpp>
#include <core/async/ParkedWork.hpp>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/coroutine.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/preferences.hpp>
#include <morph/ui/table_state_model.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui::detail {

std::string sha256Hex(std::string_view text, std::size_t digits) {
    std::string const digest = session::detail::sha256(text);
    std::string out;
    out.reserve(digits);
    for (char const byte : digest) {
        if (out.size() >= digits) {
            break;
        }
        out += std::format("{:02x}", static_cast<unsigned char>(byte));
    }
    out.resize(std::min(out.size(), digits));
    return out;
}

std::string tableStateScope(table::TableStateKey const& key) {
    return std::string{kTableStateScopePrefix} + "." +
           sha256Hex(key.model + "\x1F" + key.table + "\x1F" + key.qualifier, 32);
}

std::string tableStateRevision(std::string const& previous, std::string_view document) {
    std::string input = previous;
    input += document;
    return sha256Hex(input, 16);
}

std::string quotedSegment(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (char const each : text) {
        auto const byte = static_cast<unsigned char>(each);
        switch (byte) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    out += std::format("\\u{:04x}", byte);
                } else {
                    out += each;
                }
        }
    }
    out += '"';
    return out;
}

std::size_t utf8Prefix(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t end = limit;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): end <= limit < text.size(), and the loop only lowers it
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U && limit - end < 3) {
        --end;
    }
    return end == 0 ? limit : end;
}

std::uint64_t wireBytes(std::string_view text) noexcept {
    std::uint64_t out = 0;
    for (char const each : text) {
        auto const byte = static_cast<unsigned char>(each);
        switch (escapedBytes(byte)) {
            case 1:
                out += 1;
                break;
            case 2:  // a short escape: its backslash escaped again, and the escaped quote or backslash too
                out += 2 + ((byte == '"' || byte == '\\') ? 2 : 1);
                break;
            default:  // a six-byte unicode escape: its backslash escaped again
                out += 7;
        }
    }
    return out;
}

TableStateLocks::Lease::~Lease() {
    if (_locks != nullptr) {
        _locks->release(_name);
    }
}

TableStateLocks::Acquire::~Acquire() {
    bool handed = false;
    {
        std::scoped_lock const lock{_locks->_mutex};
        if (auto const held = _queued ? _locks->_held.find(_name) : _locks->_held.end(); held != _locks->_held.end()) {
            std::erase(held->second, this);
        }
        handed = _handed;
    }
    if (handed) {
        _locks->release(_name);
    }
}

bool TableStateLocks::Acquire::await_suspend(std::coroutine_handle<> awaiting) {
    std::scoped_lock const lock{_locks->_mutex};
    // `find` then `emplace`, not `try_emplace`: the latter's already-present path, inlined here, makes GCC at
    // -O3 report a `-Wstrict-overflow` inside the deque's destructor.
    auto const held = _locks->_held.find(_name);
    if (held == _locks->_held.end()) {
        _locks->_held.emplace(_name, std::deque<Acquire*>{});
        _handed = true;
        return false;
    }
    _awaiting = awaiting;
    _context = ::core::async::ResumeTarget::current();
    _queued = true;
    held->second.push_back(this);
    return true;
}

TableStateLocks::Lease TableStateLocks::Acquire::await_resume() {
    std::scoped_lock const lock{_locks->_mutex};
    _handed = false;  // the lease owns the lock from here
    return Lease{_locks, _name};
}

void TableStateLocks::release(std::string const& name) {
    std::coroutine_handle<> next;
    ::core::async::ResumeTarget context;
    {
        std::scoped_lock const lock{_mutex};
        auto const held = _held.find(name);
        if (held == _held.end()) {
            return;
        }
        if (held->second.empty()) {
            _held.erase(held);
            return;
        }
        Acquire* const waiter = held->second.front();
        held->second.pop_front();
        waiter->_queued = false;
        waiter->_handed = true;
        next = waiter->_awaiting;
        context = waiter->_context;
    }
    if (context) {
        context.submit(::core::async::ParkedWork{.resume = next, .abandon = {}});
    } else {
        next.resume();
    }
}

std::vector<std::string> segmentText(std::string_view text, std::size_t segmentBytes) {
    std::size_t const budget = segmentBytes - 2;  // the quotes
    std::vector<std::string> out;
    std::size_t start = 0;
    std::size_t used = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): cursor < text.size() by the loop
        auto const lead = static_cast<unsigned char>(text[cursor]);
        std::size_t const length = std::min(sequenceBytes(lead), text.size() - cursor);
        std::size_t cost = 0;
        for (std::size_t each = cursor; each < cursor + length; ++each) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): each < cursor + length <= text.size()
            cost += escapedBytes(static_cast<unsigned char>(text[each]));
        }
        if (used + cost > budget && cursor > start) {
            out.push_back(quotedSegment(text.substr(start, cursor - start)));
            start = cursor;
            used = 0;
        }
        used += cost;
        cursor += length;
    }
    if (start < text.size()) {
        out.push_back(quotedSegment(text.substr(start)));
    }
    return out;
}

}  // namespace morph::ui::detail

namespace morph::ui {

TableStateModel::TableStateModel(std::shared_ptr<IPreferencesStore> store, TableStateOptions options,
                                 std::shared_ptr<detail::TableStateShared> shared)
    : _store{std::move(store)},
      _options{std::move(options)},
      _shared{shared != nullptr ? std::move(shared) : std::make_shared<detail::TableStateShared>()} {
    if (_store == nullptr) {
        throw std::invalid_argument{"TableStateModel: a null store"};
    }
    if (_options.segmentBytes < 32 || _options.partBytes < 16 || _options.partBytes > kTableStatePartBytes) {
        throw std::invalid_argument{"TableStateModel: segmentBytes is at least 32, and partBytes 16 to " +
                                    std::to_string(kTableStatePartBytes)};
    }
}

std::string TableStateModel::verifiedPerson() {
    session::Context const* const session = session::current();
    return session == nullptr ? std::string{} : session->principal;
}

std::string TableStateModel::signedInPerson() {
    std::string person = verifiedPerson();
    if (person.empty()) {
        throw std::runtime_error{"table state is stored for a signed-in person"};
    }
    return person;
}

std::optional<std::string> TableStateModel::keyProblem(table::TableStateKey const& key) {
    auto const check = [](std::string const& member, std::string_view name,
                          std::size_t least) -> std::optional<std::string> {
        if (member.size() < least || member.size() > kMaxTableStateNameBytes) {
            return "a table-state key's `" + std::string{name} + "` is " + std::to_string(least) + " to " +
                   std::to_string(kMaxTableStateNameBytes) + " bytes";
        }
        return std::nullopt;
    };
    for (auto problem :
         {check(key.application, "application", 1), check(key.model, "model", 0), check(key.table, "table", 1),
          check(key.qualifier, "qualifier", 1), check(key.part, "part", 1)}) {
        if (problem) {
            return problem;
        }
    }
    if (key.part.contains('.')) {
        return std::string{"a table-state key's `part` holds no `.`"};
    }
    return std::nullopt;
}

void TableStateModel::checkKey(table::TableStateKey const& key) {
    if (auto problem = keyProblem(key)) {
        throw std::invalid_argument{*problem};
    }
}

std::chrono::milliseconds TableStateModel::now() const {
    if (_options.clock) {
        return _options.clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

std::optional<std::string> TableStateModel::takePart(std::string const& person, TableStateSave& request) {
    std::scoped_lock const lock{_shared->mutex};
    auto& uploads = _shared->uploads;
    auto const instant = now();
    std::erase_if(uploads, [&](auto const& entry) { return instant - entry.second.touched > _options.uploadExpiry; });
    std::pair<std::string, std::string> const uploadKey{person, request.upload};
    if (request.offset == 0) {
        uploads.erase(uploadKey);
        auto const open =
            std::ranges::count_if(uploads, [&](auto const& entry) { return entry.first.first == person; });
        if (std::cmp_greater_equal(open, kMaxTableStateUploads)) {
            throw detail::UploadsBusy{std::string{kTableStateUploadsBusy}};
        }
        uploads.emplace(uploadKey, detail::TableStateShared::Upload{
                                       .key = request.key, .text = {}, .started = instant, .touched = instant});
    }
    auto const found = uploads.find(uploadKey);
    if (found == uploads.end() || !(found->second.key == request.key) || found->second.text.size() != request.offset) {
        throw std::invalid_argument{"a table-state save part does not continue its upload"};
    }
    if (_options.maxBytesPerPerson && request.offset + request.text.size() > *_options.maxBytesPerPerson) {
        uploads.erase(found);
        throw std::invalid_argument{"a table-state upload is over the table-state quota"};
    }
    found->second.text += request.text;
    found->second.touched = instant;
    if (!request.last) {
        return std::nullopt;
    }
    std::string text = std::move(found->second.text);
    uploads.erase(found);
    return text;
}

PrefKey TableStateModel::pointKey(std::string const& person, table::TableStateKey const& key, std::string point) {
    return PrefKey{
        .person = person, .app = key.application, .scope = detail::tableStateScope(key), .point = std::move(point)};
}

PrefKey TableStateModel::usageKey(std::string const& person) {
    return PrefKey{.person = person,
                   .app = std::string{kTableStateModel},
                   .scope = std::string{kTableStateScopePrefix},
                   .point = "usage"};
}

bool TableStateModel::holds(std::string const& expected, Head const& head) {
    if (expected.empty()) {
        return true;
    }
    if (expected == table::kNoDocument) {
        return !head.found;
    }
    return head.found && expected == head.revision;
}

void registerTableState(std::shared_ptr<IPreferencesStore> store, model::detail::ActionDispatcher& dispatcher,
                        model::detail::ModelRegistryFactory& registry, TableStateOptions options) {
    if (store == nullptr) {
        throw std::invalid_argument{"registerTableState: a null store"};
    }
    registry.registerModel<TableStateModel>(kTableStateModel, [store = std::move(store), options = std::move(options),
                                                               shared = std::make_shared<detail::TableStateShared>()] {
        return std::make_unique<model::detail::ModelHolder<TableStateModel>>(store, options, shared);
    });
    dispatcher.registerAction<TableStateModel, TableStateLoad>(kTableStateModel, "load");
    dispatcher.registerAction<TableStateModel, TableStateRead>(kTableStateModel, "read");
    dispatcher.registerAction<TableStateModel, TableStateSave>(kTableStateModel, "save");
    dispatcher.registerAction<TableStateModel, TableStateRemove>(kTableStateModel, "remove");
    dispatcher.registerAction<TableStateModel, TableStatePublish>(kTableStateModel, "publish");
}

}  // namespace morph::ui
