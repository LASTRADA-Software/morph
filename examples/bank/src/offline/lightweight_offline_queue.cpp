// SPDX-License-Identifier: Apache-2.0

#include "bank/offline/lightweight_offline_queue.hpp"

#include <Lightweight/SqlMigration.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace bank::offline {

namespace {

/// @brief Hex digits used by `toHex`, lower case.
constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

/// @brief Encodes an arbitrary byte string as lower-case hex.
///
/// The queue's idempotency key is opaque bytes and may contain an embedded
/// `\0`; hex is the NUL-free representation the dedup `WHERE` needs (see
/// `OfflineQueueRecord::idempotencyKeyHex`).
///
/// @param bytes Raw key bytes; may be empty and may contain any byte value.
/// @return Twice as many characters, drawn from `[0-9a-f]`.
[[nodiscard]] std::string toHex(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        out.push_back(kHexDigits.at(byte >> 4U));
        out.push_back(kHexDigits.at(byte & 0x0FU));
    }
    return out;
}

/// @brief Decodes one hex digit.
/// @param digit A character that should be one of `[0-9a-f]`.
/// @return Its value in `[0, 15]`, or `-1` if it is not a hex digit.
[[nodiscard]] int hexNibble(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return (digit - 'a') + 10;
    }
    return -1;
}

/// @brief Inverse of `toHex`.
/// @param hex Even-length lower-case hex string as produced by `toHex`.
/// @return The decoded bytes; an empty string for empty or malformed input.
[[nodiscard]] std::string fromHex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return {};
    }
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
        const std::string_view pair = hex.substr(offset, 2);
        const int high = hexNibble(pair.front());
        const int low = hexNibble(pair.back());
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

/// @brief Wraps a payload string in the record's binary field type.
/// @param payload Opaque payload bytes.
/// @return The same bytes as a `SqlDynamicBinary`.
[[nodiscard]] Light::SqlDynamicBinary<kMaxPayloadBytes> toBinary(std::string_view payload) {
    Light::SqlDynamicBinary<kMaxPayloadBytes> binary;
    binary.resize(payload.size());
    const std::span<std::uint8_t> target{binary.data(), binary.size()};
    std::ranges::transform(payload, target.begin(),
                           [](char character) { return static_cast<std::uint8_t>(character); });
    return binary;
}

/// @brief Unwraps the record's binary field back into a payload string.
/// @param binary Stored payload bytes.
/// @return The same bytes as a `std::string`, NULs included.
[[nodiscard]] std::string fromBinary(const Light::SqlDynamicBinary<kMaxPayloadBytes>& binary) {
    const auto bytes = binary.Bytes();
    return std::string{bytes.begin(), bytes.end()};
}

/// @brief Materialises one stored row as the interface's item type.
/// @param record Row as loaded by the data mapper.
/// @return The corresponding `QueueItem`.
[[nodiscard]] morph::offline::QueueItem toItem(const OfflineQueueRecord& record) {
    return morph::offline::QueueItem{
        .id = record.id.Value(),
        .payload = fromBinary(record.payload.Value()),
        .idempotencyKey = fromHex(record.idempotencyKeyHex.Value()),
        .attempts = record.attempts.Value(),
    };
}

}  // namespace

LightweightOfflineQueue::LightweightOfflineQueue(std::optional<std::size_t> maxDepth) : _maxDepth{maxDepth} {}

LightweightOfflineQueue::LightweightOfflineQueue(Lightweight::SqlConnectionString connectionString,
                                                 std::optional<std::size_t> maxDepth)
    : _mapper{std::move(connectionString)}, _maxDepth{maxDepth} {}

std::uint64_t LightweightOfflineQueue::enqueue(std::string payload) { return enqueue(std::move(payload), {}); }

std::uint64_t LightweightOfflineQueue::enqueue(std::string payload, std::string idempotencyKey) {
    const std::scoped_lock lock{_mtx};
    // Dedup first, capacity second -- the same order FileOfflineQueue uses
    // (docs/spec/offline/offline.md, "maxDepth and a dedup hit"): a re-enqueue
    // of a key a pending item already carries stores nothing, so it must not be
    // rejected by a full queue.
    if (!idempotencyKey.empty()) {
        const std::string keyHex = toHex(idempotencyKey);
        auto existing = _mapper.Query<OfflineQueueRecord>()
                            .Where(Lightweight::FieldNameOf<&OfflineQueueRecord::idempotencyKeyHex>, "=", keyHex)
                            .OrderBy(Lightweight::FieldNameOf<&OfflineQueueRecord::id>)
                            .First();
        if (existing.has_value()) {
            // First-write-wins with silent payload loss, as the interface
            // documents for every implementation that dedups.
            return existing->id.Value();
        }
    }

    if (_maxDepth.has_value()) {
        const auto pending = _mapper.Query<OfflineQueueRecord>().Count();
        if (pending >= *_maxDepth) {
            throw morph::offline::OfflineQueueFullError{*_maxDepth, pending};
        }
    }

    OfflineQueueRecord record;
    record.payload = toBinary(payload);
    record.idempotencyKeyHex = toHex(idempotencyKey);
    record.attempts = 0;
    return _mapper.Create(record);
}

std::vector<morph::offline::QueueItem> LightweightOfflineQueue::drain() const {
    const std::scoped_lock lock{_mtx};
    const auto rows =
        _mapper.Query<OfflineQueueRecord>().OrderBy(Lightweight::FieldNameOf<&OfflineQueueRecord::id>).All();
    std::vector<morph::offline::QueueItem> items;
    items.reserve(rows.size());
    for (const auto& row : rows) {
        items.push_back(toItem(row));
    }
    return items;
}

void LightweightOfflineQueue::markDone(std::uint64_t itemId) {
    const std::scoped_lock lock{_mtx};
    _mapper.Query<OfflineQueueRecord>().Where(Lightweight::FieldNameOf<&OfflineQueueRecord::id>, "=", itemId).Delete();
}

std::size_t LightweightOfflineQueue::size() const {
    const std::scoped_lock lock{_mtx};
    return _mapper.Query<OfflineQueueRecord>().Count();
}

std::optional<std::size_t> LightweightOfflineQueue::maxDepth() const { return _maxDepth; }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- the signature is IOfflineQueue's, not ours
void LightweightOfflineQueue::setAttempts(std::uint64_t itemId, std::uint32_t attempts) {
    const std::scoped_lock lock{_mtx};
    auto record = _mapper.QuerySingle<OfflineQueueRecord>(itemId);
    if (!record.has_value()) {
        return;
    }
    record->attempts = attempts;
    _mapper.Update(*record);
}

}  // namespace bank::offline

// ─── Schema migration ────────────────────────────────────────────────────────
// Registered with the process-wide MigrationManager at static-init time, like
// every other schema in this example (examples/bank/src/db/schema.cpp). It sits
// in this translation unit rather than one of its own so the linker cannot drop
// it: bank_lib is a static library, and an object file whose only content is a
// static initializer is not pulled in by any reference. The queue's member
// functions above are referenced, so this object file is always linked, and the
// migration always registers.
//
// The table is built from the record type rather than from a hand-written
// column list, so the two cannot drift apart.
//
// The four suppressed checks are all findings about the shape
// LIGHTWEIGHT_SQL_MIGRATION expands to -- a file-scope `static` object of a
// file-scope struct, whose constructor registers with the MigrationManager --
// and none of them is actionable from here without abandoning the macro that
// every other schema in this repository uses (examples/bank/src/db/schema.cpp,
// every ladder rung's schema.cpp). They are suppressed at this one call site
// rather than in a .clang-tidy file, because a directory-scoped subtraction
// would also cover code that has nothing to do with the macro:
//   * bugprone-throwing-static-initialization / cert-err58-cpp -- registration
//     happens before main and cannot be wrapped in a try block. A throw here
//     would abort the process at start-up, which is the intended outcome for a
//     schema that cannot be registered.
//   * misc-use-internal-linkage / misc-use-anonymous-namespace -- the macro
//     names both the struct and the object; moving either into an anonymous
//     namespace means not using the macro.
//   * cppcoreguidelines-avoid-non-const-global-variables -- the object exists
//     for its constructor's side effect, so it cannot be const.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20260922000001, "Create morph_offline_queue table") {
    (void)plan.CreateTable<bank::offline::OfflineQueueRecord>();
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
