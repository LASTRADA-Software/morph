// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief The file side channel's store (`files/1`, spec 6 §18): uploads in chunks, and files a principal may read by
///        a capability reference.
///
/// Specified in `docs/spec/ui/service.md`, "Files".

namespace morph::ui {

/// @brief Why a store refused a call.
enum class FileError : std::uint8_t {
    NotFound,    ///< No such upload or file for this principal; a caller cannot tell the two apart.
    TooLarge,    ///< The file is larger than the store accepts.
    OverQuota,   ///< The principal's files would exceed what the store keeps for one principal.
    BadOffset,   ///< A chunk does not start where the bytes received so far end.
    Incomplete,  ///< An upload ended before every byte it announced arrived.
};

/// @brief The text of a `FileError`, as a `ui-file` refusal carries it.
/// @param error The error.
/// @return Its text.
[[nodiscard]] constexpr std::string_view fileErrorText(FileError error) noexcept {
    switch (error) {
        case FileError::NotFound:
            return "not found";
        case FileError::TooLarge:
            return "too large";
        case FileError::OverQuota:
            return "over quota";
        case FileError::BadOffset:
            return "bad offset";
        case FileError::Incomplete:
        default:
            return "incomplete";
    }
}

/// @brief What a file is, as its uploader described it.
struct FileInfo {
    /// @brief The file's name.
    std::string name;
    /// @brief Its media type.
    std::string type;
    /// @brief Its size in bytes.
    std::size_t size = 0;
};

/// @brief A slice of a stored file.
struct FileSlice {
    /// @brief The bytes of the slice.
    std::string bytes;
    /// @brief The file's description; `size` is the whole file's.
    FileInfo info;
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters): every call takes the principal, then the id it owns.
/// @brief Where uploaded and issued files live. Every call names the principal it is made for, and an upload id or a
///        file reference works only for the principal it was made for.
///
/// A reference is a capability: whoever presents it, as the principal it belongs to, may read the file. It is
/// therefore unguessable (`newFileRef`), and a store answers a reference of another principal exactly as one that
/// does not exist.
class IFileStore {
public:
    IFileStore() = default;
    IFileStore(IFileStore const&) = delete;
    IFileStore& operator=(IFileStore const&) = delete;
    IFileStore(IFileStore&&) = delete;
    IFileStore& operator=(IFileStore&&) = delete;
    virtual ~IFileStore() = default;

    /// @brief Starts an upload.
    /// @param principal The uploader.
    /// @param info      The file's name, type and size.
    /// @return The upload id, or why it was refused.
    [[nodiscard]] virtual std::expected<std::string, FileError> begin(std::string const& principal, FileInfo info) = 0;

    /// @brief Appends a chunk to an upload.
    /// @param principal The uploader.
    /// @param upload    The upload id.
    /// @param offset    Where the chunk starts: the bytes received so far.
    /// @param bytes     The chunk.
    /// @return The bytes received so far, or why the chunk was refused.
    [[nodiscard]] virtual std::expected<std::size_t, FileError> append(std::string const& principal,
                                                                       std::string const& upload, std::size_t offset,
                                                                       std::string_view bytes) = 0;

    /// @brief Ends an upload and files it.
    /// @param principal The uploader.
    /// @param upload    The upload id.
    /// @return The file's reference, or why it was refused.
    [[nodiscard]] virtual std::expected<std::string, FileError> finish(std::string const& principal,
                                                                       std::string const& upload) = 0;

    /// @brief Files bytes the server made for a principal, such as a report a reply returns.
    /// @param principal The principal the reference is for.
    /// @param info      The file's name and type; `size` is taken from @p bytes.
    /// @param bytes     The file.
    /// @return The file's reference, or why it was refused.
    [[nodiscard]] virtual std::expected<std::string, FileError> issue(std::string const& principal, FileInfo info,
                                                                      std::string bytes) = 0;

    /// @brief Reads a slice of a file.
    /// @param principal The reader.
    /// @param ref       The file's reference.
    /// @param offset    The first byte.
    /// @param length    At most this many bytes.
    /// @return The slice, or why it was refused.
    [[nodiscard]] virtual std::expected<FileSlice, FileError> read(std::string const& principal,
                                                                   std::string const& ref, std::size_t offset,
                                                                   std::size_t length) = 0;
};

/// @brief A new unguessable id: @p prefix, then 32 lower-case hex digits read straight from `std::random_device`.
///
/// `std::random_device` is read directly, never used as a seed: libstdc++, libc++ and MSVC back it with the
/// operating system's entropy source, which is what makes the id a capability.
/// @param prefix `file:` for a reference, `upload:` for an upload id.
/// @return The id.
[[nodiscard]] inline std::string newFileRef(std::string_view prefix = "file:") {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::random_device entropy;
    std::string text{prefix};
    for (int word = 0; word < 4; ++word) {
        std::uint32_t value = entropy();  // random_device yields 32 bits of entropy per call
        for (int nibble = 0; nibble < 8; ++nibble) {
            text += kHex.at(value & 0xFU);
            value >>= 4U;
        }
    }
    return text;
}

/// @brief How much a `MemoryFileStore` keeps.
struct MemoryFileStoreOptions {
    /// @brief The largest file it accepts.
    std::size_t maxFileBytes = std::size_t{64} * 1024 * 1024;
    /// @brief The most bytes it keeps for one principal, uploads in progress included.
    std::size_t maxBytesPerPrincipal = std::size_t{256} * 1024 * 1024;
    /// @brief How long an upload may go without a chunk before it is dropped.
    std::chrono::seconds uploadTtl{3600};
    /// @brief The clock expiry reads; the steady clock when empty.
    std::function<std::chrono::steady_clock::time_point()> clock;
};

/// @brief An `IFileStore` in memory: bounded per file and per principal; an upload that stalls expires.
///
/// Thread-safe: the UI service calls it on the server strand while an application issues references from its models'.
class MemoryFileStore final : public IFileStore {
public:
    /// @brief A store with @p options.
    /// @param options The bounds and the clock.
    explicit MemoryFileStore(MemoryFileStoreOptions options = {}) : _options{std::move(options)} {}

    /// @brief Starts an upload.
    /// @param principal The uploader.
    /// @param info      The file's name, type and size.
    /// @return The upload id, or `TooLarge` or `OverQuota`.
    [[nodiscard]] std::expected<std::string, FileError> begin(std::string const& principal, FileInfo info) override {
        std::scoped_lock const lock{_mutex};
        expire();
        if (info.size > _options.maxFileBytes) {
            return std::unexpected(FileError::TooLarge);
        }
        if (usage(principal) + info.size > _options.maxBytesPerPrincipal) {
            return std::unexpected(FileError::OverQuota);
        }
        std::string uploadId = newFileRef("upload:");
        _uploads.emplace(uploadId,
                         Upload{.principal = principal, .info = std::move(info), .bytes = {}, .touched = now()});
        return uploadId;
    }

    /// @brief Appends a chunk.
    /// @param principal The uploader.
    /// @param upload    The upload id.
    /// @param offset    The bytes received so far.
    /// @param bytes     The chunk.
    /// @return The bytes received so far, or `NotFound`, `BadOffset` or `TooLarge` (past the announced size).
    [[nodiscard]] std::expected<std::size_t, FileError> append(std::string const& principal, std::string const& upload,
                                                               std::size_t offset, std::string_view bytes) override {
        std::scoped_lock const lock{_mutex};
        expire();
        auto const found = _uploads.find(upload);
        if (found == _uploads.end() || found->second.principal != principal) {
            return std::unexpected(FileError::NotFound);
        }
        Upload& open = found->second;
        if (offset != open.bytes.size()) {
            return std::unexpected(FileError::BadOffset);
        }
        if (open.bytes.size() + bytes.size() > open.info.size) {
            return std::unexpected(FileError::TooLarge);
        }
        open.bytes.append(bytes);
        open.touched = now();
        return open.bytes.size();
    }

    /// @brief Ends an upload and files it.
    /// @param principal The uploader.
    /// @param upload    The upload id.
    /// @return The reference, or `NotFound` or `Incomplete`.
    [[nodiscard]] std::expected<std::string, FileError> finish(std::string const& principal,
                                                               std::string const& upload) override {
        std::scoped_lock const lock{_mutex};
        expire();
        auto const found = _uploads.find(upload);
        if (found == _uploads.end() || found->second.principal != principal) {
            return std::unexpected(FileError::NotFound);
        }
        if (found->second.bytes.size() != found->second.info.size) {
            return std::unexpected(FileError::Incomplete);
        }
        Upload done = std::move(found->second);
        _uploads.erase(found);
        std::string ref = newFileRef();
        _files.emplace(ref, Stored{.principal = std::move(done.principal),
                                   .info = std::move(done.info),
                                   .bytes = std::move(done.bytes)});
        return ref;
    }

    /// @brief Files bytes for a principal.
    /// @param principal The principal.
    /// @param info      The name and type.
    /// @param bytes     The file.
    /// @return The reference, or `TooLarge` or `OverQuota`.
    [[nodiscard]] std::expected<std::string, FileError> issue(std::string const& principal, FileInfo info,
                                                              std::string bytes) override {
        std::scoped_lock const lock{_mutex};
        expire();
        if (bytes.size() > _options.maxFileBytes) {
            return std::unexpected(FileError::TooLarge);
        }
        if (usage(principal) + bytes.size() > _options.maxBytesPerPrincipal) {
            return std::unexpected(FileError::OverQuota);
        }
        info.size = bytes.size();
        std::string ref = newFileRef();
        _files.emplace(ref, Stored{.principal = principal, .info = std::move(info), .bytes = std::move(bytes)});
        return ref;
    }

    /// @brief Reads a slice.
    /// @param principal The reader.
    /// @param ref       The reference.
    /// @param offset    The first byte.
    /// @param length    At most this many bytes.
    /// @return The slice (empty past the end), or `NotFound`.
    [[nodiscard]] std::expected<FileSlice, FileError> read(std::string const& principal, std::string const& ref,
                                                           std::size_t offset, std::size_t length) override {
        std::scoped_lock const lock{_mutex};
        auto const found = _files.find(ref);
        if (found == _files.end() || found->second.principal != principal) {
            return std::unexpected(FileError::NotFound);
        }
        Stored const& file = found->second;
        std::size_t const start = std::min(offset, file.bytes.size());
        return FileSlice{.bytes = file.bytes.substr(start, length), .info = file.info};
    }

private:
    struct Upload {
        std::string principal;
        FileInfo info;
        std::string bytes;
        std::chrono::steady_clock::time_point touched;
    };

    struct Stored {
        std::string principal;
        FileInfo info;
        std::string bytes;
    };

    [[nodiscard]] std::chrono::steady_clock::time_point now() const {
        return _options.clock ? _options.clock() : std::chrono::steady_clock::now();
    }

    /// Drops the uploads that went without a chunk for longer than the TTL. Under the lock.
    void expire() {
        auto const cutoff = now() - _options.uploadTtl;
        std::erase_if(_uploads, [cutoff](auto const& entry) { return entry.second.touched < cutoff; });
    }

    /// The bytes a principal holds: its files, and the announced sizes of its uploads. Under the lock.
    [[nodiscard]] std::size_t usage(std::string const& principal) const {
        std::size_t total = 0;
        for (auto const& entry : _files) {
            if (entry.second.principal == principal) {
                total += entry.second.bytes.size();
            }
        }
        for (auto const& entry : _uploads) {
            if (entry.second.principal == principal) {
                total += entry.second.info.size;
            }
        }
        return total;
    }

    MemoryFileStoreOptions _options;
    mutable std::mutex _mutex;
    std::map<std::string, Upload, std::less<>> _uploads;
    std::map<std::string, Stored, std::less<>> _files;
};

// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace morph::ui
