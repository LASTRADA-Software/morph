// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "action_log.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace morph::journal {

/// @brief Append-only, newline-delimited-JSON `IActionLog` backed by a local file.
///
/// Each entry is written as one `journal::toJson`-encoded line. `flush()` flushes
/// the C stdio buffer and then issues a real fsync (POSIX `fsync` / Windows
/// `_commit`), so a crash immediately after `flush()` returns cannot lose data —
/// this is the "local file" sink from the original request, and the natural
/// target for `SessionLog::checkpoint()` at a "Save" action.
///
/// @par Process-local `seq`
/// Like `InMemoryActionLog`, `seq` is assigned fresh per process instance — it
/// does not resume from the highest `seq` already on disk if the file is
/// reopened. Entries remain correctly ordered on disk regardless (append-only),
/// but `seq` alone is not a cross-restart unique key; use `entries()`' natural
/// file order for that.
///
/// @par Thread safety
/// All public methods are thread-safe (guarded by an internal mutex). Safe to
/// use from multiple threads within one process; not safe for multiple
/// processes to append to the same path concurrently.
class FileActionLog : public IActionLog {
public:
    /// @brief Opens (creating if necessary) @p path for appending.
    /// @throws std::runtime_error if the file cannot be opened.
    explicit FileActionLog(std::filesystem::path path) : _path{std::move(path)} {
        _file = std::fopen(_path.string().c_str(), "a");
        if (_file == nullptr) {
            throw std::runtime_error("FileActionLog: failed to open " + _path.string());
        }
    }

    /// @brief Closes the underlying file.
    ~FileActionLog() override {
        if (_file != nullptr) {
            std::fclose(_file);
        }
    }

    FileActionLog(const FileActionLog&) = delete;
    FileActionLog& operator=(const FileActionLog&) = delete;
    FileActionLog(FileActionLog&&) = delete;
    FileActionLog& operator=(FileActionLog&&) = delete;

    /// @brief Appends @p entry as one JSON line. Buffered until `flush()`. Thread-safe.
    void append(LogEntry entry) override {
        std::scoped_lock lock{_mtx};
        entry.seq = ++_nextSeq;
        auto line = toJson(entry);
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), _file);
    }

    /// @brief Flushes stdio's buffer, then fsyncs the file descriptor. Thread-safe.
    void flush() override {
        std::scoped_lock lock{_mtx};
        std::fflush(_file);
#if defined(_WIN32)
        _commit(_fileno(_file));
#else
        ::fsync(fileno(_file));
#endif
    }

    /// @brief Re-reads the file from disk and decodes every line. Thread-safe.
    ///
    /// Reads whatever is currently on disk, including anything written but not
    /// yet `flush()`ed if the platform's stdio buffering has already handed it
    /// to the OS — callers that need a guaranteed-durable view should `flush()`
    /// first.
    /// @param entityKey If non-empty, restricts the result to that entity's entries.
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock lock{_mtx};
        std::ifstream in{_path};
        std::vector<LogEntry> out;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            auto entry = fromJson(line);
            if (entityKey.empty() || entry.entityKey == entityKey) {
                out.push_back(std::move(entry));
            }
        }
        return out;
    }

private:
    std::filesystem::path _path;
    std::FILE* _file = nullptr;
    mutable std::mutex _mtx;
    uint64_t _nextSeq{0};
};

}  // namespace morph::journal
