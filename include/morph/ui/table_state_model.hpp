// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
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

/// @brief The table-state model's bounds.
struct TableStateOptions {
    /// @brief The longest entry the model writes, in bytes: a document of at most this many bytes is one entry, a
    ///        larger one is written as segments, each a JSON string of at most this many bytes.
    std::size_t segmentBytes = std::size_t{12} * 1024;
    /// @brief The most document bytes one `save` part carries and one `load` head or `read` chunk answers.
    std::size_t partBytes = std::size_t{1} << 20;
    /// @brief The operator's quota: the most document bytes one person may have stored, across every application.
    ///        Unset, nothing limits a state.
    std::optional<std::size_t> maxBytesPerPerson;
};

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
    /// @brief Why this key's document could not be read, such as a missing segment; empty when it could.
    std::string error;
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
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U && limit - end < 3) {
        --end;
    }
    return end == 0 ? limit : end;
}

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
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t const length = std::min(sequenceBytes(static_cast<unsigned char>(text[at])), text.size() - at);
        std::size_t cost = 0;
        for (std::size_t each = at; each < at + length; ++each) {
            cost += escapedBytes(static_cast<unsigned char>(text[each]));
        }
        if (used + cost > budget && at > start) {
            out.push_back(quotedSegment(text.substr(start, at - start)));
            start = at;
            used = 0;
        }
        used += cost;
        at += length;
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
    /// @throws std::invalid_argument for a null store, a `segmentBytes` under 32 or a `partBytes` under 16.
    TableStateModel(std::shared_ptr<IPreferencesStore> store, TableStateOptions options)
        : _store{std::move(store)}, _options{options} {
        if (_store == nullptr) {
            throw std::invalid_argument{"TableStateModel: a null store"};
        }
        if (_options.segmentBytes < 32 || _options.partBytes < 16) {
            throw std::invalid_argument{"TableStateModel: segmentBytes is at least 32 and partBytes at least 16"};
        }
    }

    /// @brief Reads the heads of the caller's documents.
    /// @param request The keys.
    /// @return One head per key, in order.
    ::core::async::Task<TableStateLoaded> execute(TableStateLoad request) {
        if (request.keys.size() > kMaxTableStateKeys) {
            throw std::invalid_argument{"a table-state load names more than " + std::to_string(kMaxTableStateKeys) +
                                        " keys"};
        }
        for (auto const& key : request.keys) {
            checkKey(key);
        }
        TableStateLoaded reply;
        std::string const person = verifiedPerson();
        for (auto& key : request.keys) {
            if (person.empty()) {
                reply.entries.emplace_back();
                continue;
            }
            Head const head = co_await readHead(person, key);
            TableStateHead entry{
                .found = head.found, .revision = head.revision, .size = head.size(), .text = {}, .error = {}};
            if (head.found) {
                try {
                    entry.text = co_await readRange(person, key, head, 0);
                } catch (std::exception const& error) {
                    entry =
                        TableStateHead{.found = false, .revision = {}, .size = 0, .text = {}, .error = error.what()};
                }
            }
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
        Head const head = co_await readHead(person, request.key);
        if (!head.found || head.revision != request.revision) {
            co_return TableStateChunk{.revision = head.revision, .text = {}};
        }
        if (request.offset > head.size()) {
            throw std::invalid_argument{"a table-state read starts past the document's end"};
        }
        std::string text = co_await readRange(person, request.key, head, request.offset);
        co_return TableStateChunk{.revision = head.revision, .text = std::move(text)};
    }

    /// @brief Stores the caller's document, or one part of it.
    /// @param request The key, the `expected` rule, and the document or part.
    /// @return `Saved` with the new revision; `Conflict` with the current one when `expected` does not hold;
    ///         `Refused` over the quota.
    ::core::async::Task<TableStateSaved> execute(TableStateSave request) {
        checkKey(request.key);
        if (request.text.size() > _options.partBytes) {
            throw std::invalid_argument{"a table-state save part is longer than " +
                                        std::to_string(_options.partBytes) + " bytes"};
        }
        std::string const person = signedInPerson();
        if (request.upload.empty()) {
            if (request.offset != 0 || !request.last) {
                throw std::invalid_argument{"a table-state save in parts names its upload"};
            }
            co_return TableStateSaved{
                .result = co_await commit(person, request.key, std::move(request.expected), std::move(request.text))};
        }
        if (request.upload.size() > kMaxTableStateNameBytes) {
            throw std::invalid_argument{"a table-state upload id is at most " +
                                        std::to_string(kMaxTableStateNameBytes) + " bytes"};
        }
        if (request.offset == 0) {
            _uploads.insert_or_assign(request.upload, Upload{.person = person, .key = request.key, .text = {}});
        }
        auto const found = _uploads.find(request.upload);
        if (found == _uploads.end() || found->second.person != person || !(found->second.key == request.key) ||
            found->second.text.size() != request.offset) {
            throw std::invalid_argument{"a table-state save part does not continue its upload"};
        }
        found->second.text += request.text;
        if (!request.last) {
            co_return TableStateSaved{};
        }
        std::string text = std::move(found->second.text);
        _uploads.erase(found);
        co_return TableStateSaved{
            .result = co_await commit(person, request.key, std::move(request.expected), std::move(text))};
    }

    /// @brief Forgets the caller's document.
    /// @param request The key.
    /// @return Whether a document was stored.
    ::core::async::Task<TableStateRemoved> execute(TableStateRemove request) {
        checkKey(request.key);
        std::string const person = signedInPerson();
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
            co_await setUsage(person, used - std::min(used, head.size()));
        }
        co_return TableStateRemoved{.removed = true};
    }

private:
    /// @brief A head as read.
    struct Head {
        bool found = false;
        std::string revision;
        /// The document, for an unsegmented one.
        std::string document;
        std::uint64_t segments = 0;
        std::uint64_t bytes = 0;

        /// The document's length.
        [[nodiscard]] std::uint64_t size() const { return segments > 0 ? bytes : document.size(); }
    };

    /// @brief A document arriving in parts.
    struct Upload {
        std::string person;
        table::TableStateKey key;
        std::string text;
    };

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

    /// @brief Refuses a key whose members are out of bounds: `model` is 0 to 256 bytes, the others 1 to 256.
    /// @param key The key.
    static void checkKey(table::TableStateKey const& key) {
        auto const check = [](std::string const& member, std::string_view name, std::size_t least) {
            if (member.size() < least || member.size() > kMaxTableStateNameBytes) {
                throw std::invalid_argument{"a table-state key's `" + std::string{name} + "` is " +
                                            std::to_string(least) + " to " + std::to_string(kMaxTableStateNameBytes) +
                                            " bytes"};
            }
        };
        check(key.application, "application", 1);
        check(key.model, "model", 0);
        check(key.table, "table", 1);
        check(key.qualifier, "qualifier", 1);
        check(key.part, "part", 1);
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
        } else if (entry.segments && *entry.segments > 0 && entry.bytes) {
            head.segments = *entry.segments;
            head.bytes = *entry.bytes;
        } else {
            co_return head;
        }
        head.found = true;
        head.revision = std::move(entry.revision);
        co_return head;
    }

    /// @brief The document's bytes from @p offset, at most `partBytes`, cut at a UTF-8 boundary.
    /// @throws std::runtime_error when a segment the head names is missing or does not read.
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
                    throw std::runtime_error{"a table's stored state is missing segment " + std::to_string(index)};
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

    /// @brief Stores a whole document under the `expected` rule and the quota, in the order the class describes.
    ::core::async::Task<table::SaveResult> commit(std::string person, table::TableStateKey key, std::string expected,
                                                  std::string text) {
        if (auto const error = glz::validate_json(text)) {
            throw std::invalid_argument{"a table's state is JSON text: " + glz::format_error(error, text)};
        }
        co_await cleanUp(person, key);
        Head const head = co_await readHead(person, key);
        if (!expected.empty() && expected != head.revision) {
            co_return table::SaveResult{
                .outcome = table::SaveOutcome::Conflict, .revision = head.revision, .reason = {}};
        }
        std::uint64_t const replaced = head.found ? head.size() : 0;
        std::uint64_t const added = text.size();
        std::uint64_t used = 0;
        if (_options.maxBytesPerPerson) {
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
            co_await setOrThrow(pointKey(person, key, segmentPoint(key, revision, index)), segments[index]);
        }
        detail::StoredStateHead entry{.key = key, .revision = revision, .document = {}, .segments = {}, .bytes = {}};
        if (segmented) {
            entry.segments = segments.size();
            entry.bytes = added;
        } else {
            entry.document = glz::raw_json{text};
        }
        co_await setOrThrow(pointKey(person, key, key.part), write(entry));
        if (replacesSegments) {
            co_await removeSegments(person, key, head.revision, head.segments);
        }
        if (intent) {
            static_cast<void>(co_await _store->remove(pointKey(person, key, pendingPoint(key))));
        }
        if (_options.maxBytesPerPerson) {
            co_await setUsage(person, used - replaced + added);
        }
        co_return table::SaveResult{.outcome = table::SaveOutcome::Saved, .revision = revision, .reason = {}};
    }

    std::shared_ptr<IPreferencesStore> _store;
    TableStateOptions _options;
    /// @brief Documents arriving in parts, by upload id.
    std::map<std::string, Upload, std::less<>> _uploads;
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
// NOLINTEND(misc-const-correctness)
/// @endcond

namespace morph::ui {

/// @brief Registers the table-state model and its four actions, with a factory that gives every instance @p store.
///
/// Nothing registers it at static initialisation: a server that offers table state calls this once, before it serves,
/// with the dispatcher and registry its `RemoteServer` uses. A deployment's authoriser then admits
/// `morph.tableState` and its actions `load`, `read`, `save` and `remove` for every signed-in principal; the model
/// keeps each person to their own entries.
///
/// Two instances share no lock, and a preferences store has no conditional write, so two saves of one person that
/// reach two instances in the same moment can both pass the quota: the quota can then be overshot by one document.
/// @param store      Where states are kept: the application's preferences store.
/// @param dispatcher The server's action dispatcher.
/// @param registry   The server's model registry.
/// @param options    The bounds and the quota.
inline void registerTableState(std::shared_ptr<IPreferencesStore> store, model::detail::ActionDispatcher& dispatcher,
                               model::detail::ModelRegistryFactory& registry, TableStateOptions options = {}) {
    if (store == nullptr) {
        throw std::invalid_argument{"registerTableState: a null store"};
    }
    registry.registerModel<TableStateModel>(kTableStateModel, [store = std::move(store), options] {
        return std::make_unique<model::detail::ModelHolder<TableStateModel>>(store, options);
    });
    dispatcher.registerAction<TableStateModel, TableStateLoad>(kTableStateModel, "load");
    dispatcher.registerAction<TableStateModel, TableStateRead>(kTableStateModel, "read");
    dispatcher.registerAction<TableStateModel, TableStateSave>(kTableStateModel, "save");
    dispatcher.registerAction<TableStateModel, TableStateRemove>(kTableStateModel, "remove");
}

}  // namespace morph::ui
