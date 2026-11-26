// SPDX-License-Identifier: Apache-2.0

#pragma once
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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/coroutine.hpp"
#include "../core/model.hpp"
#include "../core/registry.hpp"
#include "../session/session.hpp"
#include "../session/session_auth.hpp"
#include "../table/state_store.hpp"
#include "preferences.hpp"

/// @file
/// @brief The server half of `PreferencesTableStateStore`: the `morph.tableState` model, which keeps a table's state
///        of any size in the application's `IPreferencesStore`, under the person the verified session names.
///
/// Specified in `docs/spec/table/table.md`, "Persisted state", "The framework's stores".

namespace morph::ui {

/// @brief The most document bytes one `save` part carries and one `load` head or `read` chunk answers, by default.
///        `PreferencesTableStateStore` sends parts of this size, so a server's `partBytes` is at least this.
inline constexpr std::size_t kTableStatePartBytes = std::size_t{1} << 20;

/// @brief The table-state model's bounds.
struct TableStateOptions {
    /// @brief The longest document kept in its head entry, in bytes; a larger one is written as segments, each a JSON
    ///        string of at most this many bytes. A head also holds the key in clear and the revision, so a head of a
    ///        document near this size is longer than it.
    std::size_t segmentBytes = std::size_t{12} * 1024;
    /// @brief The most document bytes one `save` part carries and one `load` head or `read` chunk answers: 16 to
    ///        `kTableStatePartBytes`. The client sends parts of `kTableStatePartBytes`, so a server refuses its saves
    ///        in parts when this is lower; a larger value is refused, since one head could then outgrow its envelope.
    std::size_t partBytes = kTableStatePartBytes;
    /// @brief The operator's quota: the most document bytes one person may have stored, across every application.
    ///        Unset, nothing limits a state.
    std::optional<std::size_t> maxBytesPerPerson;
    /// @brief How long an upload in parts may wait for its next part before it is dropped. A client sends each part
    ///        as soon as the previous one is answered, so a gap a hundred times a session's `tableStateTimeout`
    ///        (300 ms) is an upload abandoned.
    std::chrono::milliseconds uploadExpiry = std::chrono::milliseconds{300} * 100;
    /// @brief The clock `uploadExpiry` is measured on; empty uses `std::chrono::steady_clock`.
    std::function<std::chrono::milliseconds()> clock;
};

/// @brief The most uploads in parts one person may have open on a server. A save that would start one more answers
///        `Refused` with `kTableStateUploadsBusy`, and `PreferencesTableStateStore` sends it again a little later.
inline constexpr std::size_t kMaxTableStateUploads = 4;

/// @brief The `reason` of a save refused because its person has `kMaxTableStateUploads` uploads open.
inline constexpr std::string_view kTableStateUploadsBusy = "too many table-state uploads in progress";

/// @brief The most bytes of head text one `load` answers, counted as the text travels, escaped once in the reply and
///        again in its envelope: a reply then stays inside the 8 MiB envelope, a head of a full part included.
inline constexpr std::size_t kTableStateLoadReplyBytes = std::size_t{6} << 20;

/// @brief The longest key member a table-state request may carry, in bytes.
inline constexpr std::size_t kMaxTableStateNameBytes = 256;

/// @brief The most keys one `load` may name.
inline constexpr std::size_t kMaxTableStateKeys = 256;

/// @brief `load`: the heads of the caller's stored documents under @p keys. A request carries no person.
struct TableStateLoad {
    /// @brief The keys, each answered in order.
    std::vector<table::TableStateKey> keys;
};

/// @brief One key's answer to `load`.
struct TableStateHead {
    /// @brief Whether a document is stored under the key.
    bool found = false;
    /// @brief The stored document's revision.
    std::string revision;
    /// @brief The document's whole length, in bytes.
    std::uint64_t size = 0;
    /// @brief The document's first bytes, at most `TableStateOptions::partBytes`; the rest is read by `read`.
    std::string text;
    /// @brief Why this key's document could not be read, such as a key out of bounds or a missing segment; empty
    ///        when it could.
    std::string error;
    /// @brief Not answered in this reply, which is full: the caller asks for the key again.
    bool deferred = false;
};

/// @brief What `load` answers.
struct TableStateLoaded {
    /// @brief One head per key, in the request's order.
    std::vector<TableStateHead> entries;
};

/// @brief `read`: more of a stored document, from @p offset.
struct TableStateRead {
    /// @brief Which document.
    table::TableStateKey key;
    /// @brief The revision the caller is reading.
    std::string revision;
    /// @brief The first byte wanted.
    std::uint64_t offset = 0;
};

/// @brief What `read` answers.
struct TableStateChunk {
    /// @brief The revision stored now. When it is not the one asked for, @p text is empty and the caller starts
    ///        again; empty when nothing is stored.
    std::string revision;
    /// @brief The document's bytes from the asked offset, at most `TableStateOptions::partBytes`.
    std::string text;
};

/// @brief `save`: stores a document, or one part of it.
///
/// A document of at most `TableStateOptions::partBytes` is one request with `upload` empty. A larger one is sent as
/// parts under one `upload` id, each at the offset where the previous ended, and is stored when the part with `last`
/// arrives; `expected` and the quota are checked then, against the whole text.
struct TableStateSave {
    /// @brief Which document.
    table::TableStateKey key;
    /// @brief Empty to replace whatever is stored; otherwise the revision the stored document must have.
    std::string expected;
    /// @brief The upload's id, for a document sent in parts; empty for one sent whole.
    std::string upload;
    /// @brief Where @p text starts in the document.
    std::uint64_t offset = 0;
    /// @brief The document's bytes from @p offset.
    std::string text;
    /// @brief Whether this part ends the document.
    bool last = true;
};

/// @brief What `save` answers.
struct TableStateSaved {
    /// @brief The outcome; a part that is not `last` answers `Saved` with no revision.
    table::SaveResult result;
};

/// @brief `publish`: applies one operation to the views published for everyone.
struct TableStatePublish {
    /// @brief Which table's shared views; its `part` is `"shared"`.
    table::TableStateKey key;
    /// @brief The operation's JSON, as `table::writeViewOperation` writes it.
    std::string operation;
    /// @brief Empty to apply it to whatever is stored; `table::kNoDocument` to apply it only when nothing is;
    ///        otherwise the revision the stored document must have. Even with it empty the model writes over the
    ///        revision it read, so a publish that lands between that read and its write answers `Conflict`.
    std::string expected;
};

/// @brief `remove`: forgets a stored document.
struct TableStateRemove {
    /// @brief Which document.
    table::TableStateKey key;
};

/// @brief What `remove` answers.
struct TableStateRemoved {
    /// @brief Whether a document was stored.
    bool removed = false;
};

namespace detail {

/// @brief A stored document's head entry: the key in clear, the revision, and either the document or its segments.
struct StoredStateHead {
    /// @brief The key, for whoever reads the store.
    table::TableStateKey key;
    /// @brief The document's revision.
    std::string revision;
    /// @brief The document, for one of at most `segmentBytes`.
    std::optional<glz::raw_json> document;
    /// @brief The number of segments, for a larger one.
    std::optional<std::uint64_t> segments;
    /// @brief The document's length, for a larger one.
    std::optional<std::uint64_t> bytes;
};

/// @brief The intent a save writes before its segments: the revisions whose segments may be stray if it stops.
struct StoredStateIntent {
    /// @brief The revision being written.
    std::string revision;
    /// @brief Its segments; 0 for an unsegmented document.
    std::uint64_t segments = 0;
    /// @brief The revision being replaced; empty when none.
    std::string previous;
    /// @brief Its segments; 0 for an unsegmented document.
    std::uint64_t previousSegments = 0;
};

/// @brief A person's table-state usage entry.
struct StoredStateUsage {
    /// @brief The document bytes stored.
    std::uint64_t bytes = 0;
};

/// @brief Reads stored entries leniently.
inline constexpr glz::opts kStoredRead{.null_terminated = false, .error_on_unknown_keys = false};

/// @brief The first @p digits lower-case hex digits of SHA-256 of @p text.
/// @param text   The text.
/// @param digits How many digits, at most 64.
/// @return The digits.
[[nodiscard]] inline std::string sha256Hex(std::string_view text, std::size_t digits) {
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

/// @brief The preferences scope a key's entries are kept under: `morph.table.` and 32 hex digits of the digest of
///        its `model`, `table` and `qualifier`, so the scope fits a preference name's length whatever the ids are.
/// @param key The key.
/// @return The scope.
[[nodiscard]] inline std::string tableStateScope(table::TableStateKey const& key) {
    return std::string{kTableStateScopePrefix} + "." +
           sha256Hex(key.model + "\x1F" + key.table + "\x1F" + key.qualifier, 32);
}

/// @brief A document's revision: 16 hex digits of the digest of the revision it replaces and its text.
/// @param previous The replaced revision; empty when none.
/// @param document The document's text.
/// @return The revision.
[[nodiscard]] inline std::string tableStateRevision(std::string const& previous, std::string_view document) {
    std::string input = previous;
    input += document;
    return sha256Hex(input, 16);
}

/// @brief How many bytes a byte takes inside a JSON string as `quotedSegment` writes it.
/// @param byte The byte.
/// @return 1, 2 for a short escape, or 6 for `\u00XX`.
[[nodiscard]] constexpr std::size_t escapedBytes(unsigned char byte) noexcept {
    switch (byte) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            return 2;
        default:
            return byte < 0x20 ? 6 : 1;
    }
}

/// @brief @p text as a JSON string, quotes included.
/// @param text The text.
/// @return The JSON string.
[[nodiscard]] inline std::string quotedSegment(std::string_view text) {
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

/// @brief The length of the UTF-8 sequence @p lead starts; 1 for a byte that starts none.
/// @param lead The byte.
/// @return 1 to 4.
[[nodiscard]] constexpr std::size_t sequenceBytes(unsigned char lead) noexcept {
    if (lead >= 0xF0 && lead <= 0xF7) {
        return 4;
    }
    if (lead >= 0xE0) {
        return lead <= 0xEF ? 3 : 1;
    }
    return lead >= 0xC0 ? 2 : 1;
}

/// @brief The longest prefix of @p text of at most @p limit bytes that does not cut a UTF-8 sequence.
/// @param text  The text.
/// @param limit The most bytes; at least 4.
/// @return The prefix's length.
[[nodiscard]] inline std::size_t utf8Prefix(std::string_view text, std::size_t limit) noexcept {
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

/// @brief How many bytes @p text takes as it travels in a `load` reply: escaped once as a JSON string in the reply,
///        and the reply escaped again as the envelope's body.
/// @param text The text.
/// @return Its length on the wire, quotes excluded.
[[nodiscard]] inline std::uint64_t wireBytes(std::string_view text) noexcept {
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

/// @brief A lock per name for coroutines: one holder at a time, the others waiting their turn in order and resumed
///        where they were running.
class TableStateLocks {
public:
    /// @brief The held lock of one name, released when it is destroyed.
    class Lease {
    public:
        /// @brief A lease of @p name.
        /// @param locks The locks. Borrowed.
        /// @param name  The name held.
        Lease(TableStateLocks* locks, std::string name) : _locks{locks}, _name{std::move(name)} {}
        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        /// @brief Takes @p other's lock.
        /// @param other The lease to take from; it then holds nothing.
        Lease(Lease&& other) noexcept : _locks{std::exchange(other._locks, nullptr)}, _name{std::move(other._name)} {}
        Lease& operator=(Lease&&) = delete;
        /// @brief Releases the lock, handing it to the next waiter.
        ~Lease() {
            if (_locks != nullptr) {
                _locks->release(_name);
            }
        }

    private:
        TableStateLocks* _locks;
        std::string _name;
    };

    /// @brief The awaiter `lock` returns.
    class Acquire {
    public:
        /// @brief An acquisition of @p name.
        /// @param locks The locks. Borrowed.
        /// @param name  The name.
        Acquire(TableStateLocks& locks, std::string name) : _locks{&locks}, _name{std::move(name)} {}
        Acquire(Acquire const&) = delete;
        Acquire& operator=(Acquire const&) = delete;
        Acquire(Acquire&&) = delete;
        Acquire& operator=(Acquire&&) = delete;
        /// @brief Leaves the queue when the waiting coroutine is destroyed before its turn, and passes the lock on
        ///        when it was handed the lock but destroyed before it ran.
        ~Acquire() {
            bool handed = false;
            {
                std::scoped_lock const lock{_locks->_mutex};
                if (auto const held = _queued ? _locks->_held.find(_name) : _locks->_held.end();
                    held != _locks->_held.end()) {
                    std::erase(held->second, this);
                }
                handed = _handed;
            }
            if (handed) {
                _locks->release(_name);
            }
        }

        /// @brief Never ready before `await_suspend` has decided.
        /// @return `false`.
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static): the awaiter protocol calls it on the object.
        [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

        /// @brief Takes the lock when it is free, or queues for it.
        /// @param awaiting The coroutine taking it.
        /// @return `false` when taken at once; `true` when queued.
        bool await_suspend(std::coroutine_handle<> awaiting) {
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

        /// @brief The lease.
        /// @return It.
        [[nodiscard]] Lease await_resume() {
            std::scoped_lock const lock{_locks->_mutex};
            _handed = false;  // the lease owns the lock from here
            return Lease{_locks, _name};
        }

    private:
        friend class TableStateLocks;
        TableStateLocks* _locks;
        std::string _name;
        std::coroutine_handle<> _awaiting;
        ::core::async::ResumeTarget _context;
        bool _queued = false;
        /// Holds the lock but has not yet made the lease that releases it.
        bool _handed = false;
    };

    /// @brief Waits for the lock of @p name.
    /// @param name The name.
    /// @return The awaiter; `co_await` it for the lease.
    [[nodiscard]] Acquire lock(std::string name) { return Acquire{*this, std::move(name)}; }

private:
    void release(std::string const& name) {
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

    std::mutex _mutex;
    /// @brief Each held name and its waiters, oldest first.
    std::map<std::string, std::deque<Acquire*>, std::less<>> _held;
};

/// @brief A person has `kMaxTableStateUploads` uploads in parts open; their next one is refused until one ends.
struct UploadsBusy : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief A segment a head names is missing or does not read. Reads take no lock, so a save that replaced the head
///        meanwhile is the usual cause: a reader then reads the head again.
struct MissingSegment : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief What every instance of the table-state model on one server shares: the per-key locks that keep two saves of
///        one key from interleaving, and the uploads in parts.
struct TableStateShared {
    /// @brief A document arriving in parts.
    struct Upload {
        /// @brief Which document.
        table::TableStateKey key;
        /// @brief The text so far.
        std::string text;
        /// @brief When it started, on the options' clock.
        std::chrono::milliseconds started{0};
        /// @brief When its last part arrived.
        std::chrono::milliseconds touched{0};
    };

    /// @brief The locks, one name per key of one owner.
    TableStateLocks locks;
    /// @brief Guards `uploads`.
    std::mutex mutex;
    /// @brief Open uploads, by person and upload id.
    std::map<std::pair<std::string, std::string>, Upload> uploads;
};

/// @brief Cuts @p text into segments, each written as a JSON string of at most @p segmentBytes bytes and none cutting
///        a UTF-8 sequence.
/// @param text         The document's text.
/// @param segmentBytes The longest segment entry; at least 32.
/// @return Each segment's entry, a JSON string.
[[nodiscard]] inline std::vector<std::string> segmentText(std::string_view text, std::size_t segmentBytes) {
    std::size_t const budget = segmentBytes - 2;  // the quotes
    std::vector<std::string> out;
    std::size_t start = 0;
    std::size_t used = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): cursor < text.size() by the loop
        std::size_t const length =
            std::min(sequenceBytes(static_cast<unsigned char>(text[cursor])), text.size() - cursor);
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

}  // namespace detail

/// @brief The server's table-state model: keeps each person's table states in the application's preferences store,
///        a document of any size in entries of at most `segmentBytes`.
///
/// The person is the principal of the verified session, as for the preferences model: no request names one. With no
/// verified principal, `load` answers nothing found for every key and `read`, `save` and `remove` are refused.
///
/// The `shared` part is the exception: it is the views published for everyone, so its entries are kept with no
/// person, and every verified caller's `load` and `read` reach the same document. It is written only by `publish`,
/// an action of its own, so an authoriser admits or refuses publishing by action id; `save` of it answers `Refused`
/// and `remove` of it is refused. It counts against the operator's quota as an account of its own, the person "",
/// so the ceiling bounds it as it bounds each person's state.
///
/// A key's entries are the caller's person's, of the key's `application`, under the scope
/// `detail::tableStateScope(key)`. The point `<part>` is the head: the key in clear, the revision, and the document
/// itself when it is at most `segmentBytes`, else its segment count and length, the segments being the points
/// `<part>.<revision>.<index>`. The store can only get, set and remove by exact key, so it cannot list a scope's
/// segments; a save that writes segments, or replaces segmented ones, first writes an intent at `<part>.pending`
/// naming both revisions, and the next save of the key removes whatever segments the intent names and the head does
/// not. The write order is: clean up a previous intent, write the intent, the segments, the head, then remove the
/// replaced segments and the intent. A reader takes the head and then the segments it names, so it reads one whole
/// revision or the other.
class TableStateModel {
public:
    /// @brief A model over @p store.
    /// @param store   Where states are kept. Shared by every instance.
    /// @param options The bounds and the quota.
    /// @param shared  The locks and uploads every instance on the server shares; null gives this instance its own.
    /// @throws std::invalid_argument for a null store, a `segmentBytes` under 32, or a `partBytes` under 16 or over
    ///         `kTableStatePartBytes`: a head of more than that could outgrow a reply's envelope.
    TableStateModel(std::shared_ptr<IPreferencesStore> store, TableStateOptions options,
                    std::shared_ptr<detail::TableStateShared> shared = nullptr)
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

    /// @brief Reads the heads of the caller's documents.
    ///
    /// Each key is answered alone: one out of bounds, or whose segment is missing, has its `error` and the others
    /// load. A reply whose head text would pass `kTableStateLoadReplyBytes` answers its remaining keys `deferred`,
    /// after at least one head, so a screen of large states takes more than one call and no reply outgrows its
    /// envelope.
    /// @param request The keys.
    /// @return One head per key, in order.
    /// @throws std::invalid_argument for more than `kMaxTableStateKeys` keys.
    ::core::async::Task<TableStateLoaded> execute(TableStateLoad request) {
        if (request.keys.size() > kMaxTableStateKeys) {
            throw std::invalid_argument{"a table-state load names more than " + std::to_string(kMaxTableStateKeys) +
                                        " keys"};
        }
        TableStateLoaded reply;
        std::string const person = verifiedPerson();
        std::uint64_t spent = 0;
        bool full = false;
        for (auto const& key : request.keys) {
            TableStateHead entry;
            if (full) {
                entry.deferred = true;
                reply.entries.push_back(std::move(entry));
                continue;
            }
            if (auto problem = keyProblem(key)) {
                entry.error = std::move(*problem);
                reply.entries.push_back(std::move(entry));
                continue;
            }
            if (person.empty()) {
                reply.entries.push_back(std::move(entry));
                continue;
            }
            entry = co_await readFirst(ownerOf(person, key), key);
            std::uint64_t const cost = detail::wireBytes(entry.text);
            if (spent > 0 && spent + cost > kTableStateLoadReplyBytes) {
                full = true;
                reply.entries.push_back(TableStateHead{
                    .found = false, .revision = {}, .size = 0, .text = {}, .error = {}, .deferred = true});
                continue;
            }
            spent += cost;
            reply.entries.push_back(std::move(entry));
        }
        co_return reply;
    }

    /// @brief Reads more of the caller's document.
    /// @param request The key, the revision and the offset.
    /// @return The bytes from the offset, or the current revision with no bytes when it is not the one asked for.
    ::core::async::Task<TableStateChunk> execute(TableStateRead request) {
        checkKey(request.key);
        std::string const person = verifiedPerson();
        if (person.empty()) {
            co_return TableStateChunk{};
        }
        std::string const owner = ownerOf(person, request.key);
        Head const head = co_await readHead(owner, request.key);
        if (!head.found || head.revision != request.revision) {
            co_return TableStateChunk{.revision = head.revision, .text = {}};
        }
        if (request.offset > head.size()) {
            throw std::invalid_argument{"a table-state read starts past the document's end"};
        }
        std::string text;
        std::string missing;
        try {
            text = co_await readRange(owner, request.key, head, request.offset);
        } catch (detail::MissingSegment const& error) {
            missing = error.what();
        }
        if (!missing.empty()) {
            // A save replaced the document meanwhile: the reader starts again from the head it names now.
            Head const now = co_await readHead(owner, request.key);
            if (!now.found || now.revision != head.revision) {
                co_return TableStateChunk{.revision = now.revision, .text = {}};
            }
            throw std::runtime_error{missing};
        }
        co_return TableStateChunk{.revision = head.revision, .text = std::move(text)};
    }

    /// @brief Stores the caller's document, or one part of it.
    /// @param request The key, the `expected` rule, and the document or part.
    /// @return `Saved` with the new revision; `Conflict` with the current one when `expected` does not hold;
    ///         `Refused` over the quota, and for the `shared` part, which only `publish` writes.
    ::core::async::Task<TableStateSaved> execute(TableStateSave request) {
        checkKey(request.key);
        if (request.text.size() > _options.partBytes) {
            throw std::invalid_argument{"a table-state save part is longer than " +
                                        std::to_string(_options.partBytes) + " bytes"};
        }
        std::string const person = signedInPerson();
        if (request.key.part == kSharedPart) {
            co_return TableStateSaved{.result =
                                          table::SaveResult{.outcome = table::SaveOutcome::Refused,
                                                            .revision = {},
                                                            .reason = "the shared views are written by publish"}};
        }
        if (request.upload.empty()) {
            if (request.offset != 0 || !request.last) {
                throw std::invalid_argument{"a table-state save in parts names its upload"};
            }
            auto const lease = co_await _shared->locks.lock(lockName(person, request.key));
            co_return TableStateSaved{
                .result = co_await commit(person, request.key, std::move(request.expected), std::move(request.text))};
        }
        if (request.upload.size() > kMaxTableStateNameBytes) {
            throw std::invalid_argument{"a table-state upload id is at most " +
                                        std::to_string(kMaxTableStateNameBytes) + " bytes"};
        }
        std::optional<std::string> whole;
        bool busy = false;
        try {
            whole = takePart(person, request);
        } catch (detail::UploadsBusy const&) {
            busy = true;
        }
        if (busy) {
            co_return TableStateSaved{.result = table::SaveResult{.outcome = table::SaveOutcome::Refused,
                                                                  .revision = {},
                                                                  .reason = std::string{kTableStateUploadsBusy}}};
        }
        if (!whole) {
            co_return TableStateSaved{};
        }
        auto const lease = co_await _shared->locks.lock(lockName(person, request.key));
        co_return TableStateSaved{
            .result = co_await commit(person, request.key, std::move(request.expected), std::move(*whole))};
    }

    /// @brief Forgets the caller's document.
    /// @param request The key.
    /// @return Whether a document was stored.
    /// @throws std::invalid_argument for the `shared` part, which only `publish` writes.
    ::core::async::Task<TableStateRemoved> execute(TableStateRemove request) {
        checkKey(request.key);
        std::string const person = signedInPerson();
        if (request.key.part == kSharedPart) {
            throw std::invalid_argument{"the shared views are written by publish"};
        }
        auto const lease = co_await _shared->locks.lock(lockName(person, request.key));
        co_await cleanUp(person, request.key);
        Head const head = co_await readHead(person, request.key);
        if (!head.found) {
            co_return TableStateRemoved{};
        }
        bool const intent = head.segments > 0;
        if (intent) {
            co_await setOrThrow(
                pointKey(person, request.key, pendingPoint(request.key)),
                write(detail::StoredStateIntent{
                    .revision = {}, .segments = 0, .previous = head.revision, .previousSegments = head.segments}));
        }
        static_cast<void>(co_await _store->remove(pointKey(person, request.key, request.key.part)));
        co_await removeSegments(person, request.key, head.revision, head.segments);
        if (intent) {
            static_cast<void>(co_await _store->remove(pointKey(person, request.key, pendingPoint(request.key))));
        }
        if (_options.maxBytesPerPerson) {
            std::uint64_t const used = co_await usage(person);
            co_await setUsage(person, used - std::min(used, head.counted));
        }
        co_return TableStateRemoved{.removed = true};
    }

    /// @brief Applies one operation to the views published for everyone, kept with no person.
    /// @param request The `shared` key, the operation's JSON and the `expected` rule.
    /// @return `Saved` with the new revision; `Conflict` with the current one when `expected` does not hold;
    ///         `Refused` for a default, which the shared views do not have, an operation that does not apply, or a
    ///         stored document that does not read.
    /// @throws std::invalid_argument for a key of another part or an operation that does not read;
    ///         std::runtime_error for a caller with no verified person.
    ::core::async::Task<TableStateSaved> execute(TableStatePublish request) {
        checkKey(request.key);
        if (request.key.part != kSharedPart) {
            throw std::invalid_argument{"a table-state publish names the part `shared`"};
        }
        static_cast<void>(signedInPerson());
        auto const operation = table::readViewOperation(request.operation);
        if (!operation) {
            throw std::invalid_argument{operation.error().message};
        }
        auto const refuse = [](std::string revision, std::string reason) {
            return TableStateSaved{.result = table::SaveResult{.outcome = table::SaveOutcome::Refused,
                                                               .revision = std::move(revision),
                                                               .reason = std::move(reason)}};
        };
        std::string const owner;  // the shared views are no one's
        auto const lease = co_await _shared->locks.lock(lockName(owner, request.key));
        co_await cleanUp(owner, request.key);
        Head const head = co_await readHead(owner, request.key);
        if (!holds(request.expected, head)) {
            co_return TableStateSaved{.result = table::SaveResult{.outcome = table::SaveOutcome::Conflict,
                                                                  .revision = head.revision,
                                                                  .reason = {}}};
        }
        table::ViewsDocument document;
        if (head.found) {
            auto read = table::readViews(co_await readWhole(owner, request.key, head));
            if (!read) {
                co_return refuse(head.revision, read.error().message);
            }
            document = std::move(*read);
        }
        auto applied = table::applyViewOperation(std::move(document), *operation, table::ViewsPart::Shared);
        if (!applied) {
            co_return refuse(head.revision, table::viewErrorText(applied.error(), *operation));
        }
        co_return TableStateSaved{
            .result = co_await commit(owner, request.key, head.found ? head.revision : std::string{table::kNoDocument},
                                      table::writeViews(*applied, table::ViewsPart::Shared))};
    }

private:
    /// @brief The part kept for everyone, with no person.
    static constexpr std::string_view kSharedPart = "shared";
    /// @brief How often a load reads a head again because saves keep replacing it while it reads.
    static constexpr int kMaxRereads = 8;

    /// @brief A head as read.
    struct Head {
        bool found = false;
        std::string revision;
        /// The document, for an unsegmented one.
        std::string document;
        std::uint64_t segments = 0;
        std::uint64_t bytes = 0;
        /// The document's length as its save received it, which the quota counts.
        std::uint64_t counted = 0;

        /// The document's length.
        [[nodiscard]] std::uint64_t size() const { return segments > 0 ? bytes : document.size(); }
    };

    /// @brief Whose entries a key's are: @p person's, or no one's for the `shared` part.
    /// @param person The verified caller.
    /// @param key    The key.
    /// @return The person the store is called with.
    [[nodiscard]] static std::string ownerOf(std::string const& person, table::TableStateKey const& key) {
        return key.part == kSharedPart ? std::string{} : person;
    }

    /// @brief The verified principal of the call being run.
    /// @return It; empty when the server verified nobody.
    [[nodiscard]] static std::string verifiedPerson() {
        session::Context const* const session = session::current();
        return session == nullptr ? std::string{} : session->principal;
    }

    /// @brief The verified principal, for a call that writes.
    /// @return It.
    /// @throws std::runtime_error when the server verified nobody.
    [[nodiscard]] static std::string signedInPerson() {
        std::string person = verifiedPerson();
        if (person.empty()) {
            throw std::runtime_error{"table state is stored for a signed-in person"};
        }
        return person;
    }

    /// @brief What is wrong with a key: a member out of bounds (`model` is 0 to 256 bytes, the others 1 to 256), or
    ///        a `part` holding a `.`, which would name another part's intent or segment points.
    /// @param key The key.
    /// @return The problem; nothing for a key in bounds.
    [[nodiscard]] static std::optional<std::string> keyProblem(table::TableStateKey const& key) {
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

    /// @brief Refuses a key out of bounds.
    /// @param key The key.
    static void checkKey(table::TableStateKey const& key) {
        if (auto problem = keyProblem(key)) {
            throw std::invalid_argument{*problem};
        }
    }

    /// @brief The lock of one owner's key: two writes of it never interleave on this server.
    /// @param owner The person whose entries they are; empty for the shared views.
    /// @param key   The key.
    /// @return The lock's name.
    [[nodiscard]] static std::string lockName(std::string const& owner, table::TableStateKey const& key) {
        return owner + "\x1F" + key.application + "\x1F" + detail::tableStateScope(key) + "\x1F" + key.part;
    }

    /// @brief The options' clock now.
    /// @return It.
    [[nodiscard]] std::chrono::milliseconds now() const {
        if (_options.clock) {
            return _options.clock();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }

    /// @brief Adds one part to its upload, after dropping every upload that waited past `uploadExpiry`.
    /// @param person  The caller.
    /// @param request The part.
    /// @return The whole text at the last part; nothing before it.
    /// @throws std::invalid_argument for a part that does not continue its upload, or that takes it past the quota;
    ///         detail::UploadsBusy for a first part while the person has `kMaxTableStateUploads` open.
    [[nodiscard]] std::optional<std::string> takePart(std::string const& person, TableStateSave& request) {
        std::scoped_lock const lock{_shared->mutex};
        auto& uploads = _shared->uploads;
        auto const instant = now();
        std::erase_if(uploads,
                      [&](auto const& entry) { return instant - entry.second.touched > _options.uploadExpiry; });
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
        if (found == uploads.end() || !(found->second.key == request.key) ||
            found->second.text.size() != request.offset) {
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

    [[nodiscard]] static std::string pendingPoint(table::TableStateKey const& key) { return key.part + ".pending"; }

    [[nodiscard]] static std::string segmentPoint(table::TableStateKey const& key, std::string const& revision,
                                                  std::uint64_t index) {
        return key.part + "." + revision + "." + std::to_string(index);
    }

    [[nodiscard]] static PrefKey pointKey(std::string const& person, table::TableStateKey const& key,
                                          std::string point) {
        return PrefKey{.person = person,
                       .app = key.application,
                       .scope = detail::tableStateScope(key),
                       .point = std::move(point)};
    }

    [[nodiscard]] static PrefKey usageKey(std::string const& person) {
        return PrefKey{.person = person,
                       .app = std::string{kTableStateModel},
                       .scope = std::string{kTableStateScopePrefix},
                       .point = "usage"};
    }

    template <typename T>
    [[nodiscard]] static std::string write(T const& value) {
        std::string out;
        if (auto const error = glz::write<model::detail::EscapingWriteOpts{}>(value, out)) {
            throw std::runtime_error{"a table-state entry does not write: " + glz::format_error(error, out)};
        }
        return out;
    }

    ::core::async::Task<void> setOrThrow(PrefKey key, std::string value) {
        if (!co_await _store->set(key, std::move(value))) {
            throw std::runtime_error{"the preferences store refused a table-state entry"};
        }
    }

    /// @brief Reads a key's head; one that does not read, or whose clear key is another's, is not found.
    ::core::async::Task<Head> readHead(std::string person, table::TableStateKey key) {
        std::optional<std::string> const stored = co_await _store->get(pointKey(person, key, key.part));
        Head head;
        if (!stored) {
            co_return head;
        }
        detail::StoredStateHead entry;
        if (glz::read<detail::kStoredRead>(entry, *stored) || !(entry.key == key) || entry.revision.empty()) {
            co_return head;
        }
        if (entry.document) {
            head.document = std::move(entry.document->str);
            head.counted = entry.bytes.value_or(head.document.size());
        } else if (entry.segments && *entry.segments > 0 && entry.bytes) {
            head.segments = *entry.segments;
            head.bytes = *entry.bytes;
            head.counted = head.bytes;
        } else {
            co_return head;
        }
        head.found = true;
        head.revision = std::move(entry.revision);
        co_return head;
    }

    /// @brief A key's head and the document's first bytes. A segment missing because a save replaced the head
    ///        meanwhile is read again from the head stored now, so a load answers the old document or the new one and
    ///        never fails over a concurrent save; one missing under an unchanged head is the key's `error`.
    ::core::async::Task<TableStateHead> readFirst(std::string owner, table::TableStateKey key) {
        TableStateHead entry;
        for (int attempt = 0; attempt < kMaxRereads; ++attempt) {
            Head const head = co_await readHead(owner, key);
            entry = TableStateHead{};
            if (!head.found) {
                co_return entry;
            }
            entry.found = true;
            entry.revision = head.revision;
            entry.size = head.size();
            std::string missing;
            try {
                entry.text = co_await readRange(owner, key, head, 0);
                co_return entry;
            } catch (detail::MissingSegment const& error) {
                missing = error.what();
            }
            Head const now = co_await readHead(owner, key);
            if (now.found && now.revision == head.revision) {
                co_return TableStateHead{.found = false, .revision = {}, .size = 0, .text = {}, .error = missing};
            }
        }
        co_return TableStateHead{.found = false,
                                 .revision = {},
                                 .size = 0,
                                 .text = {},
                                 .error = "a table's stored state kept changing while it was read"};
    }

    /// @brief The document's bytes from @p offset, at most `partBytes`, cut at a UTF-8 boundary.
    /// @throws detail::MissingSegment when a segment the head names is missing or does not read.
    ::core::async::Task<std::string> readRange(std::string person, table::TableStateKey key, Head head,
                                               std::uint64_t offset) {
        std::string_view whole = head.document;
        std::string gathered;
        std::uint64_t skip = offset;
        if (head.segments > 0) {
            // Up to three more bytes than a part, so the cut can find a whole UTF-8 sequence.
            std::size_t const wanted = _options.partBytes + 3;
            for (std::uint64_t index = 0; index < head.segments && gathered.size() < wanted; ++index) {
                std::optional<std::string> const stored =
                    co_await _store->get(pointKey(person, key, segmentPoint(key, head.revision, index)));
                std::string segment;
                if (!stored || glz::read<detail::kStoredRead>(segment, *stored)) {
                    throw detail::MissingSegment{"a table's stored state is missing segment " + std::to_string(index)};
                }
                if (skip >= segment.size()) {
                    skip -= segment.size();
                    continue;
                }
                gathered.append(segment, static_cast<std::size_t>(skip));
                skip = 0;
            }
            whole = gathered;
            skip = 0;
        }
        std::string_view const rest =
            whole.substr(std::min<std::size_t>(static_cast<std::size_t>(skip), whole.size()));
        co_return std::string{rest.substr(0, detail::utf8Prefix(rest, _options.partBytes))};
    }

    /// @brief The whole document a head names.
    /// @throws std::runtime_error when a segment the head names is missing or does not read.
    ::core::async::Task<std::string> readWhole(std::string person, table::TableStateKey key, Head head) {
        std::string text;
        while (text.size() < head.size()) {
            std::string const part = co_await readRange(person, key, head, text.size());
            if (part.empty()) {
                throw std::runtime_error{"a table's stored state ended before its size"};
            }
            text += part;
        }
        co_return text;
    }

    /// @brief Whether @p expected holds over @p head: empty always; `kNoDocument` when nothing is stored; otherwise
    ///        when the stored document has that revision.
    [[nodiscard]] static bool holds(std::string const& expected, Head const& head) {
        if (expected.empty()) {
            return true;
        }
        if (expected == table::kNoDocument) {
            return !head.found;
        }
        return head.found && expected == head.revision;
    }

    /// @brief Removes a revision's segments; a removal the store does not make is left to a later save.
    ::core::async::Task<void> removeSegments(std::string person, table::TableStateKey key, std::string revision,
                                             std::uint64_t segments) {
        for (std::uint64_t index = 0; index < segments; ++index) {
            static_cast<void>(co_await _store->remove(pointKey(person, key, segmentPoint(key, revision, index))));
        }
    }

    /// @brief Removes the segments a stopped save left: those its intent names and the head does not.
    ::core::async::Task<void> cleanUp(std::string person, table::TableStateKey key) {
        std::optional<std::string> const stored = co_await _store->get(pointKey(person, key, pendingPoint(key)));
        if (!stored) {
            co_return;
        }
        detail::StoredStateIntent intent;
        if (!glz::read<detail::kStoredRead>(intent, *stored)) {
            Head const head = co_await readHead(person, key);
            std::string const kept = head.found && head.segments > 0 ? head.revision : std::string{};
            if (!intent.revision.empty() && intent.revision != kept) {
                co_await removeSegments(person, key, intent.revision, intent.segments);
            }
            if (!intent.previous.empty() && intent.previous != kept) {
                co_await removeSegments(person, key, intent.previous, intent.previousSegments);
            }
        }
        static_cast<void>(co_await _store->remove(pointKey(person, key, pendingPoint(key))));
    }

    /// @brief The person's usage; 0 when the entry is absent or does not read.
    ::core::async::Task<std::uint64_t> usage(std::string person) {
        std::optional<std::string> const stored = co_await _store->get(usageKey(person));
        detail::StoredStateUsage entry;
        if (!stored || glz::read<detail::kStoredRead>(entry, *stored)) {
            co_return 0;
        }
        co_return entry.bytes;
    }

    /// @brief Writes the person's usage. A refused write leaves the old total; the document is stored either way.
    ::core::async::Task<void> setUsage(std::string person, std::uint64_t bytes) {
        static_cast<void>(co_await _store->set(usageKey(person), write(detail::StoredStateUsage{.bytes = bytes})));
    }

    /// @brief Stores a whole document under the `expected` rule and the quota, in the order the class describes. The
    ///        shared views, kept with no person, count as one more account.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a revision token, then a document, at every call.
    ::core::async::Task<table::SaveResult> commit(std::string person, table::TableStateKey key, std::string expected,
                                                  std::string text) {
        if (auto const error = glz::validate_json(text)) {
            throw std::invalid_argument{"a table's state is JSON text: " + glz::format_error(error, text)};
        }
        co_await cleanUp(person, key);
        Head const head = co_await readHead(person, key);
        if (!holds(expected, head)) {
            co_return table::SaveResult{
                .outcome = table::SaveOutcome::Conflict, .revision = head.revision, .reason = {}};
        }
        // The shared views, the person "", are an account of their own under the same quota.
        bool const counted = _options.maxBytesPerPerson.has_value();
        std::uint64_t const replaced = head.found ? head.counted : 0;
        std::uint64_t const added = text.size();
        std::uint64_t used = 0;
        if (counted) {
            used = co_await usage(person);
            // used - replaced + added > quota, written so that nothing wraps.
            if (used + added > static_cast<std::uint64_t>(*_options.maxBytesPerPerson) + replaced) {
                co_return table::SaveResult{.outcome = table::SaveOutcome::Refused,
                                            .revision = head.revision,
                                            .reason = "over the table-state quota"};
            }
        }
        std::string const revision = detail::tableStateRevision(head.revision, text);
        bool const segmented = text.size() > _options.segmentBytes;
        std::vector<std::string> const segments =
            segmented ? detail::segmentText(text, _options.segmentBytes) : std::vector<std::string>{};
        bool const replacesSegments = head.found && head.segments > 0;
        bool const intent = segmented || replacesSegments;
        if (intent) {
            co_await setOrThrow(
                pointKey(person, key, pendingPoint(key)),
                write(detail::StoredStateIntent{.revision = revision,
                                                .segments = segments.size(),
                                                .previous = replacesSegments ? head.revision : std::string{},
                                                .previousSegments = replacesSegments ? head.segments : 0}));
        }
        for (std::size_t index = 0; index < segments.size(); ++index) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < segments.size() by the loop
            co_await setOrThrow(pointKey(person, key, segmentPoint(key, revision, index)), segments[index]);
        }
        detail::StoredStateHead entry{.key = key, .revision = revision, .document = {}, .segments = {}, .bytes = {}};
        if (segmented) {
            entry.segments = segments.size();
            entry.bytes = added;
        } else {
            entry.document = glz::raw_json{text};
            entry.bytes = added;  // the text's own length: reading the document back drops surrounding whitespace
        }
        co_await setOrThrow(pointKey(person, key, key.part), write(entry));
        if (replacesSegments) {
            co_await removeSegments(person, key, head.revision, head.segments);
        }
        if (intent) {
            static_cast<void>(co_await _store->remove(pointKey(person, key, pendingPoint(key))));
        }
        if (counted) {
            // A usage below the replaced size undercounts (a quota set over existing states, two instances racing, a
            // refused usage write): what remains of it is counted as nothing rather than wrapping.
            co_await setUsage(person, (used > replaced ? used - replaced : 0) + added);
        }
        co_return table::SaveResult{.outcome = table::SaveOutcome::Saved, .revision = revision, .reason = {}};
    }

    std::shared_ptr<IPreferencesStore> _store;
    TableStateOptions _options;
    std::shared_ptr<detail::TableStateShared> _shared;
};

}  // namespace morph::ui

/// @cond detail
// The traits the registration macros write, without their static-init registrars: registerTableState registers.
// NOLINTBEGIN(misc-const-correctness): inside the macros' codec bodies.
BRIDGE_DETAIL_MODEL_TRAITS_ONLY_BODY(morph::ui::TableStateModel, "morph.tableState")
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::TableStateModel, morph::ui::TableStateLoad,
                                      morph::ui::TableStateLoaded, "load", ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::TableStateModel, morph::ui::TableStateRead,
                                      morph::ui::TableStateChunk, "read", ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::TableStateModel, morph::ui::TableStateSave,
                                      morph::ui::TableStateSaved, "save", ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::TableStateModel, morph::ui::TableStateRemove,
                                      morph::ui::TableStateRemoved, "remove", ::morph::model::Loggable::No)
BRIDGE_DETAIL_ACTION_TRAITS_ONLY_BODY(morph::ui::TableStateModel, morph::ui::TableStatePublish,
                                      morph::ui::TableStateSaved, "publish", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
/// @endcond

namespace morph::ui {

/// @brief Registers the table-state model and its five actions, with a factory that gives every instance @p store.
///
/// Nothing registers it at static initialisation: a server that offers table state calls this once, before it serves,
/// with the dispatcher and registry its `RemoteServer` uses. A deployment's authoriser then admits
/// `morph.tableState` and its actions `load`, `read`, `save` and `remove` for every signed-in principal; the model
/// keeps each person to their own entries. `publish` writes the views shared with everyone, so the authoriser admits
/// it for those who may publish.
///
/// The actions declare no effects: what they change is no record a screen shows, and a mounted table reads its state
/// when it mounts, not as it changes elsewhere.
///
/// Every instance it makes shares one set of per-key locks, so two writes of one key on this server run one after
/// the other and the last wins whole; and one set of uploads in parts, at most `kMaxTableStateUploads` per person.
/// Servers share nothing, and a preferences store has no conditional write, so two saves of one key that reach two
/// servers in the same moment can interleave. Two saves of one person's different keys can both pass the quota: each
/// such race can leave one document uncounted, so the quota is overshot by one document per race.
/// @param store      Where states are kept: the application's preferences store.
/// @param dispatcher The server's action dispatcher.
/// @param registry   The server's model registry.
/// @param options    The bounds and the quota.
inline void registerTableState(std::shared_ptr<IPreferencesStore> store, model::detail::ActionDispatcher& dispatcher,
                               model::detail::ModelRegistryFactory& registry, TableStateOptions options = {}) {
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
