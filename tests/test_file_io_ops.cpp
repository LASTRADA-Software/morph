// SPDX-License-Identifier: Apache-2.0
//
// Direct coverage of the free functions in
// `morph/core/file_io_ops.hpp`. They are reached indirectly through
// `FileActionLog`/`FileOfflineQueue`/`SqliteOfflineQueue` elsewhere, but only
// along the paths those classes happen to take -- which left the error
// classifications and the degenerate arguments untested, and they are precisely
// the parts whose whole job is to behave correctly when something has already
// gone wrong.

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <morph/core/file_io_ops.hpp>
#include <string>
#include <system_error>

namespace {

std::filesystem::path tempIoPath(const std::string& tag) {
    static int counter = 0;
    return std::filesystem::temp_directory_path() /
           ("morph_file_io_ops_" + tag + "_" + std::to_string(++counter) + ".bin");
}

// `std::fopen` and `std::tmpfile` hand back an owning `std::FILE*`. Parking one
// in a bare local leaves the close to a statement the test has to remember to
// reach -- and on Windows a forgotten close does not fail at the close but at
// the `std::filesystem::remove()` afterwards, which cannot unlink a file
// another handle still holds open. An owner that closes at scope exit states
// the lifetime instead of restating the close, and is what
// cppcoreguidelines-owning-memory asks for. Each test therefore scopes its
// handle and unlinks outside that scope.
struct FileCloser {
    void operator()(std::FILE* file) const {
        // NOLINTNEXTLINE(cert-err33-c, cppcoreguidelines-owning-memory) — a close failure has no one left to report to
        (void)std::fclose(file);
    }
};

using OpenFile = std::unique_ptr<std::FILE, FileCloser>;

OpenFile openForAppend(const std::filesystem::path& path) { return OpenFile{std::fopen(path.string().c_str(), "a")}; }

}  // namespace

// ── classifyDirectorySync ───────────────────────────────────────────────────
//
// The split this function draws is load-bearing: `unsupported` lets three
// classes keep opening on layouts where a directory fsync is simply not
// available, while `failed` still refuses. Getting a code onto the wrong side
// either bricks an ordinary deployment or silently downgrades a real durability
// failure to a warning.

TEST_CASE("morph::core::classifyDirectorySync: zero is durable", "[file_io_ops]") {
    CHECK(morph::core::classifyDirectorySync(0) == morph::core::DirectorySync::durable);
}

TEST_CASE("morph::core::classifyDirectorySync: a permission or unimplemented errno is unsupported", "[file_io_ops]") {
    // EACCES/EPERM: a directory fsync needs a *read* handle on the directory,
    // strictly stronger than writing a file inside it -- a mode-0300 spool
    // directory, or a write-without-read SELinux/AppArmor policy, produces
    // exactly this while fopen(path, "a") keeps working.
    CHECK(morph::core::classifyDirectorySync(EACCES) == morph::core::DirectorySync::unsupported);
    CHECK(morph::core::classifyDirectorySync(EPERM) == morph::core::DirectorySync::unsupported);

    // EINVAL/ENOSYS/ENOTDIR: fsync on a directory fd is unimplemented on
    // sshfs, gvfs, Docker Desktop's gRPC-FUSE, WSL drvfs/9p under /mnt/c, and
    // some overlay and network mounts.
    CHECK(morph::core::classifyDirectorySync(EINVAL) == morph::core::DirectorySync::unsupported);
    CHECK(morph::core::classifyDirectorySync(ENOSYS) == morph::core::DirectorySync::unsupported);
    CHECK(morph::core::classifyDirectorySync(ENOTDIR) == morph::core::DirectorySync::unsupported);
#ifdef ENOTSUP
    CHECK(morph::core::classifyDirectorySync(ENOTSUP) == morph::core::DirectorySync::unsupported);
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    CHECK(morph::core::classifyDirectorySync(EOPNOTSUPP) == morph::core::DirectorySync::unsupported);
#endif
}

TEST_CASE("morph::core::classifyDirectorySync: a real I/O errno is a failure", "[file_io_ops]") {
    // The other side of the split. If these ever became `unsupported`, a
    // genuine durability failure would be logged and shrugged off.
    CHECK(morph::core::classifyDirectorySync(EIO) == morph::core::DirectorySync::failed);
    CHECK(morph::core::classifyDirectorySync(ENOSPC) == morph::core::DirectorySync::failed);
    CHECK(morph::core::classifyDirectorySync(EROFS) == morph::core::DirectorySync::failed);

    // A custom FileIoOps may return any nonzero value; anything unrecognised
    // must count as a real failure rather than be waved through. -1 is what the
    // suite's own fault-injection stubs return.
    CHECK(morph::core::classifyDirectorySync(-1) == morph::core::DirectorySync::failed);
    CHECK(morph::core::classifyDirectorySync(999999) == morph::core::DirectorySync::failed);
}

// ── retryOnEintr ────────────────────────────────────────────────────────────

TEST_CASE("morph::core::retryOnEintr: a call that succeeds first time is issued once", "[file_io_ops]") {
    int calls = 0;
    const int result = morph::core::retryOnEintr([&calls] {
        ++calls;
        return 7;
    });

    CHECK(result == 7);
    CHECK(calls == 1);
}

TEST_CASE("morph::core::retryOnEintr: an EINTR failure is reissued until it is not", "[file_io_ops]") {
    // The case the real syscalls cannot be made to take from a test: a signal
    // arrives mid-open/mid-fsync, the call returns -1/EINTR having done
    // nothing, and it must be issued again rather than reported as a failure.
    int calls = 0;
    const int result = morph::core::retryOnEintr([&calls] {
        ++calls;
        if (calls < 3) {
            errno = EINTR;
            return -1;
        }
        return 0;
    });

    CHECK(result == 0);
    CHECK(calls == 3);
}

TEST_CASE("morph::core::retryOnEintr: a non-EINTR failure is returned, not retried", "[file_io_ops]") {
    // Retrying an EACCES forever would hang the caller rather than let it
    // report the error.
    int calls = 0;
    const int result = morph::core::retryOnEintr([&calls] {
        ++calls;
        errno = EACCES;
        return -1;
    });

    CHECK(result == -1);
    CHECK(calls == 1);
}

// ── rollBackShortWrite ──────────────────────────────────────────────────────

TEST_CASE("morph::core::rollBackShortWrite: a negative offset truncates nothing", "[file_io_ops]") {
    // `wideFtell` returning negative means the position could not be queried,
    // so there is no offset to roll back *to*. Truncating on a guess would
    // discard complete, fsynced records; the file must be left exactly as it
    // is for the next open's repair to deal with.
    auto const path = tempIoPath("negative_offset");
    {
        std::ofstream out{path, std::ios::binary};
        out << "{\"one\":1}\n{\"two\":2}\n";
    }
    auto const sizeBefore = std::filesystem::file_size(path);

    morph::core::FileIoOps ioOps;
    {
        auto const file = openForAppend(path);
        REQUIRE(file != nullptr);

        // `torn`: with no offset to roll back to, the file is left exactly as
        // it is, so the caller must treat the tail as possibly partial.
        CHECK(morph::core::rollBackShortWrite(ioOps, file.get(), path, -1) == morph::core::RollBack::torn);
    }

    CHECK(std::filesystem::file_size(path) == sizeBefore);
    std::filesystem::remove(path);
}

TEST_CASE("morph::core::rollBackShortWrite: a failing flush truncates nothing", "[file_io_ops]") {
    // On a full disk the flush is the *likely* failure, since that is what made
    // the write short. The on-disk contents are then unknowable and the
    // buffered bytes cannot portably be discarded, so truncating to a stream
    // offset that may exceed the real size would pad the file with NULs rather
    // than trim it -- the bricking the rollback exists to prevent.
    auto const path = tempIoPath("failing_flush");
    {
        std::ofstream out{path, std::ios::binary};
        out << "{\"one\":1}\n";
    }
    auto const sizeBefore = std::filesystem::file_size(path);

    morph::core::FileIoOps ioOps;
    ioOps.fflush = [](std::FILE*) { return -1; };
    {
        auto const file = openForAppend(path);
        REQUIRE(file != nullptr);

        // `torn`: the flush that had to succeed before anything could be
        // truncated did not, so nothing was truncated.
        CHECK(morph::core::rollBackShortWrite(ioOps, file.get(), path, 0) == morph::core::RollBack::torn);
    }

    CHECK(std::filesystem::file_size(path) == sizeBefore);
    std::filesystem::remove(path);
}

TEST_CASE("morph::core::rollBackShortWrite: the truncation can only ever shrink", "[file_io_ops]") {
    // The defect this clamp closes, reproduced directly: an offset past the end
    // of the file. std::filesystem::resize_file GROWS in that case, padding
    // with NUL bytes -- so an unclamped rollback appends garbage to the very
    // log it is trying to repair.
    auto const path = tempIoPath("clamped");
    {
        std::ofstream out{path, std::ios::binary};
        out << "0123456789";
    }
    REQUIRE(std::filesystem::file_size(path) == 10);

    morph::core::FileIoOps ioOps;
    {
        auto const file = openForAppend(path);
        REQUIRE(file != nullptr);

        // `clean`: clamped to the real size, which is a successful resize --
        // there is no partial tail to warn the caller about.
        CHECK(morph::core::rollBackShortWrite(ioOps, file.get(), path, 9999) == morph::core::RollBack::clean);
    }

    CHECK(std::filesystem::file_size(path) == 10);
    std::filesystem::remove(path);
}

TEST_CASE("morph::core::rollBackShortWrite: a real short write is trimmed back to its offset", "[file_io_ops]") {
    // The ordinary path, asserted on the bytes rather than only on the size:
    // what survives must be exactly the records written before the failure.
    auto const path = tempIoPath("trimmed");
    {
        std::ofstream out{path, std::ios::binary};
        out << "{\"one\":1}\n";
    }

    morph::core::FileIoOps ioOps;
    {
        auto const file = openForAppend(path);
        REQUIRE(file != nullptr);
        morph::core::positionAtEnd(file.get());
        long long const offsetBeforeWrite = morph::core::wideFtell(file.get());
        REQUIRE(offsetBeforeWrite == 10);

        // A partial record, exactly as a short write would leave it.
        REQUIRE(std::fwrite("{\"tw", 1, 4, file.get()) == 4);

        CHECK(morph::core::rollBackShortWrite(ioOps, file.get(), path, offsetBeforeWrite) ==
              morph::core::RollBack::clean);
    }

    // The reader is scoped too: it holds the same file open, and Windows
    // refuses the unlink below while it does.
    std::string contents;
    {
        std::ifstream in{path, std::ios::binary};
        contents.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    CHECK(contents == "{\"one\":1}\n");

    std::filesystem::remove(path);
}

// ── positionAtEnd ───────────────────────────────────────────────────────────

TEST_CASE("morph::core::positionAtEnd: a null handle is ignored", "[file_io_ops]") {
    // Called before the caller's own open-failure check, so it must tolerate
    // the failed-open case rather than dereference it.
    REQUIRE_NOTHROW(morph::core::positionAtEnd(nullptr));
}

TEST_CASE("morph::core::positionAtEnd: an append stream reports the file's size", "[file_io_ops]") {
    // The property the Microsoft CRT and musl do not give for free: until the
    // first I/O they report position 0 on an append stream, which would make
    // rollBackShortWrite truncate a whole file to nothing on its first short
    // write. glibc already seeks to end, so this asserts the invariant rather
    // than the platform.
    auto const path = tempIoPath("append_position");
    {
        std::ofstream out{path, std::ios::binary};
        out << "0123456789ab";
    }

    {
        auto const file = openForAppend(path);
        REQUIRE(file != nullptr);
        morph::core::positionAtEnd(file.get());

        CHECK(morph::core::wideFtell(file.get()) == 12);
    }

    std::filesystem::remove(path);
}

TEST_CASE("morph::core::rollBackShortWrite: an unreadable size leaves the offset unclamped", "[file_io_ops]") {
    // `file_size` failing means the clamp cannot be computed. The rollback
    // still attempts the resize with the offset it was given rather than
    // silently doing nothing -- the resize is itself error-code-based, so a
    // path that no longer exists is a no-op rather than a throw. What is
    // asserted is that this path is quiet: it runs on an already-failing write,
    // and an exception escaping here would replace the caller's real error.
    auto const path = tempIoPath("missing_for_size");
    std::filesystem::remove(path);
    REQUIRE_FALSE(std::filesystem::exists(path));

    morph::core::FileIoOps ioOps;
    OpenFile const file{std::tmpfile()};
    REQUIRE(file != nullptr);

    morph::core::RollBack outcome{};
    REQUIRE_NOTHROW(outcome = morph::core::rollBackShortWrite(ioOps, file.get(), path, 4));

    // `torn`: the resize on a path that does not exist fails, and a caller that
    // cannot be told the tail is clean must assume it is not.
    CHECK(outcome == morph::core::RollBack::torn);
    CHECK_FALSE(std::filesystem::exists(path));
}

// ── repairTornTail's read-error path ────────────────────────────────────────

#ifndef _WIN32
TEST_CASE("morph::core::repairTornTail: a read error leaves the file untouched", "[file_io_ops]") {
    // The distinction this guard draws: reaching the end of the scan because
    // the stream *failed* is not the same as reaching it because the file
    // ended. Everything past the last complete record is then merely unread,
    // not established to be torn, and truncating would discard complete,
    // fsynced records.
    //
    // A directory stands in for the I/O error: opening one succeeds, and the
    // first read sets badbit. POSIX-only -- Windows refuses the open itself,
    // which takes the earlier canOpenForRead branch instead.
    auto const dir = std::filesystem::temp_directory_path() / "morph_repair_torn_tail_dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "child");

    morph::core::FileIoOps ioOps;
    bool resized = false;
    ioOps.resizeFile = [&resized](const std::filesystem::path&, std::uintmax_t, std::error_code&) { resized = true; };

    REQUIRE_NOTHROW(morph::core::repairTornTail(ioOps, dir, "TestComponent"));

    INFO("a stream that failed mid-scan must not trigger a truncation");
    CHECK_FALSE(resized);

    std::filesystem::remove_all(dir);
}
#endif
