# `morph::core::FileIoOps` — design

`morph::core::FileIoOps` (`include/morph/core/file_io_ops.hpp`) is an
injectable strategy for the raw file-I/O primitives `morph::journal::
FileActionLog` and `morph::offline::FileOfflineQueue` both call:
`fwrite`, `fflush`, `fsync`/`_commit`, `fopen`, an ifstream-open probe,
`std::filesystem::resize_file`, and a directory `fsync` (`syncPath`). Every
member is a `std::function` defaulting to the real syscall/stdlib call it
stands in for.

The header also hosts four **free functions** that are not part of the struct
and are not injectable — shared file-handling logic that had been duplicated
across the two classes, or that exists to paper over a platform difference:
`wideFtell`, `positionAtEnd`, `rollBackShortWrite`, `repairTornTail`, plus the
`classifyDirectorySync` helper and its `DirectorySync` enum. See
[Free functions](#free-functions).

## Contents

- [Why it exists](#why-it-exists)
- [Shape](#shape)
- [Free functions](#free-functions)
- [Usage](#usage)
- [Thread safety](#thread-safety)
- [Cross-references](#cross-references)

## Why it exists

`FileActionLog`/`FileOfflineQueue` both have several branch arms that only
run when a real OS-level file-I/O call fails partway through an
otherwise-successful operation — disk full, a file descriptor closed
underneath, a permission change racing an exact window between two library
calls. None of those are reachable from a portable unit test without a way
to fail one specific call on demand (see `LASTRADA-Software/morph#97`,
which requested exactly this for `FileActionLog`; `FileOfflineQueue` has the
identical gap).

`FileIoOps` is that seam. A test constructs one, overrides the one member it
wants to fail (optionally gated behind a `std::shared_ptr<bool>` or a call
counter so it only fails on a specific call, not every call for the rest of
the object's lifetime), and passes it to the class under test's constructor.
Every other member stays at its real default, so the rest of the class's I/O
behaves normally around the one injected failure.

## Shape

A plain aggregate of seven `std::function` members, each mirroring one
underlying call:

| Member | Mirrors | Signature |
|---|---|---|
| `fwrite` | `std::fwrite` | `size_t(const void*, size_t, FILE*)` |
| `fflush` | `std::fflush` | `int(FILE*)` |
| `fsync` | POSIX `fsync` / Windows `_commit` | `int(FILE*)` |
| `fopen` | `std::fopen` | `FILE*(const std::string&, const char*)` |
| `canOpenForRead` | `std::ifstream{path}`'s own open check | `bool(const std::filesystem::path&)` |
| `resizeFile` | `std::filesystem::resize_file` | `void(const std::filesystem::path&, uintmax_t, std::error_code&)` |
| `syncPath` | `open(dir, O_RDONLY\|O_DIRECTORY)` + `fsync` (POSIX); no-op on Windows | `int(const std::filesystem::path&)` |

`syncPath` (morph#532) commits a directory's own metadata — a new or
renamed entry within it — to durable storage; `fsync` on a *file* makes
only that file's data durable, not the directory entry that names it. Both
`FileActionLog` and `FileOfflineQueue` call it after every directory
mutation (file creation at construction, `rotate()`'s seal rename, and
`compact()`'s rewrite-in-place rename), surfacing a failure rather than
swallowing it — see `docs/spec/journal/journal.md` and
`docs/spec/offline/offline.md` for the call sites. `rollBackShortWrite()`
(morph#530) is the other seam-driven addition in this header: on a short
`fwrite`, it truncates the file back to its pre-write length using the
same injectable `resizeFile`/`fflush` this struct provides, so a partial
write never sits where the next append would otherwise merge with it.

`canOpenForRead` exists because a fault-injection test has no way to make a
*real* `std::ifstream` construction fail without actually breaking the
filesystem — so `repairTornTail()`'s "can I read this path right now" check
is factored out as its own predicate, consulted before the real
`std::ifstream` is constructed, rather than trying to intercept the stream
construction itself.

## Free functions

| Function | What it is for |
|---|---|
| `long long wideFtell(std::FILE*)` | `std::ftell` widened past ~2 GiB (`_ftelli64`/`ftello`). |
| `void positionAtEnd(std::FILE*)` | Seeks a just-opened append-mode stream to end-of-file. C11 leaves an append stream's *initial* position implementation-defined: glibc seeks to end, while the Microsoft CRT and musl report position 0 until the first I/O. Every write still lands at the end; only what `ftell` reports differs — which matters because `rollBackShortWrite` takes a pre-write `ftell` as the offset to roll back to. Unnormalised, the first short write after any open would roll a Windows file back to **zero bytes**. Called after each successful append-mode `fopen`. |
| `void rollBackShortWrite(FileIoOps&, std::FILE*, const path&, long long)` | Undoes a partial record so the next write cannot merge with it. See [Rolling back a short write](#rolling-back-a-short-write). |
| `void repairTornTail(FileIoOps&, const path&, std::string_view)` | Trims bytes after the final newline at open time. Called by `FileActionLog` only. It trims **by newline, not by parse**, so a *complete* final record whose only missing byte is the terminating newline is discarded along with a genuinely torn one; a caller that accepts externally-appended records needs to know that before adopting it. `FileOfflineQueue` deliberately does not call it — see `docs/spec/offline/offline.md`. |
| `DirectorySync classifyDirectorySync(int)` | Splits a nonzero `syncPath` result into `unsupported` (warn and continue) and `failed` (throw). See `docs/spec/journal/journal.md`, "Directory durability". |

### Rolling back a short write

`rollBackShortWrite` **flushes first and checks the result**, and truncates
nothing if that flush fails.

`offsetBeforeWrite` is a *stream* position, and callers buffer —
`FileActionLog::append()` is documented as buffered-until-`flush()`, so `ftell`
routinely runs ahead of the file's real on-disk size. `std::filesystem::
resize_file` to an offset **beyond** the current size does not shrink the file;
it **grows** it, padding with NUL bytes, and a later flush then appends the
buffered record after that padding. The result is a NUL-bearing *interior* line
that the caller's reader rejects for the life of the file — the exact bricking
morph#530 exists to prevent, manufactured by the rollback meant to prevent it.
(Measured: `ftell` 30 against an on-disk size of 10, `resize_file(30)` yielding
a 30-byte file, and a final 50-byte file of data + 20 NULs + the flushed
record.)

On a full disk the flush is the *likely* failure, since that is what made the
write short. When it fails nothing is truncated: the on-disk contents are
unknowable, the buffered bytes cannot portably be discarded, and a later flush
would re-append them after whatever had been removed. The file is left for
`repairTornTail`/`compact()` to trim at the next open. The truncation is also
clamped to the file's actual size, so it can only ever shrink, and `clearerr`
runs first — a real short write latches the stream's error indicator and `fseek`
does not clear it, so without that one transient `ENOSPC` could leave the log
throwing for the life of the process on a CRT that refuses writes to an
error-flagged stream.

## Usage

The three classes take it as an optional constructor parameter — **second** for
the two file-backed ones, **third** for `SqliteOfflineQueue`, which uses it for
`syncPath` alone and reaches SQLite through the C API directly:

```cpp
explicit FileActionLog(std::filesystem::path path, morph::core::FileIoOps ioOps = {});
explicit FileOfflineQueue(std::filesystem::path path, morph::core::FileIoOps ioOps = {},
                          std::optional<std::size_t> maxDepth = std::nullopt);
explicit SqliteOfflineQueue(std::filesystem::path path, std::optional<std::size_t> maxDepth = std::nullopt,
                            morph::core::FileIoOps ioOps = {},
                            SqliteOfflineQueue::Synchronous synchronous = Synchronous::normal,
                            std::chrono::milliseconds busyTimeout = std::chrono::milliseconds{5000});
```

A normal caller never passes one — the default-constructed `FileIoOps` is
byte-for-byte what both classes called directly before this seam existed, so
this is not a behavior change for any existing caller. Example, forcing a
short write:

```cpp
morph::core::FileIoOps ioOps;
ioOps.fwrite = [](const void* buffer, std::size_t size, std::FILE* file) {
    return size - 1;  // always one byte short
};
FileActionLog log{path, ioOps};
REQUIRE_THROWS_AS(log.append(entry), std::runtime_error);
```

To fail only a *specific* call (e.g. "the reopen after rotate()'s rename, not
the constructor's own open"), capture a shared counter or flag and check it
inside the lambda — see `tests/test_action_log_phase2.cpp`'s and
`tests/test_file_offline_queue.cpp`'s own fault-injection test cases for the
established idiom.

## Thread safety

`FileIoOps` itself is a plain value type with no shared state — copying or
moving one has ordinary value semantics. Whether the *callbacks* themselves
are safe to call from multiple threads concurrently is up to whatever a test
installs; the real default callbacks are exactly the real syscalls, which
already have their own well-defined thread-safety.

## Cross-references

- [`docs/spec/journal/journal.md`](../journal/journal.md) — `FileActionLog`'s
  own design, including the branches this seam closes.
- [`docs/spec/offline/offline.md`](../offline/offline.md) — `FileOfflineQueue`'s
  own design, including the identical class of branch this seam closes.
- `LASTRADA-Software/morph#97` — the issue that requested this seam.
