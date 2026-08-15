# `morph::core::FileIoOps` — design

`morph::core::FileIoOps` (`include/morph/core/file_io_ops.hpp`) is an
injectable strategy for the raw file-I/O primitives `morph::journal::
FileActionLog` and `morph::offline::FileOfflineQueue` both call:
`fwrite`, `fflush`, `fsync`/`_commit`, `fopen`, an ifstream-open probe, and
`std::filesystem::resize_file`. Every member is a `std::function` defaulting
to the real syscall/stdlib call it stands in for.

## Contents

- [Why it exists](#why-it-exists)
- [Shape](#shape)
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

A plain aggregate of six `std::function` members, each mirroring one
underlying call:

| Member | Mirrors | Signature |
|---|---|---|
| `fwrite` | `std::fwrite` | `size_t(const void*, size_t, FILE*)` |
| `fflush` | `std::fflush` | `int(FILE*)` |
| `fsync` | POSIX `fsync` / Windows `_commit` | `int(FILE*)` |
| `fopen` | `std::fopen` | `FILE*(const std::string&, const char*)` |
| `canOpenForRead` | `std::ifstream{path}`'s own open check | `bool(const std::filesystem::path&)` |
| `resizeFile` | `std::filesystem::resize_file` | `void(const std::filesystem::path&, uintmax_t, std::error_code&)` |

`canOpenForRead` exists because a fault-injection test has no way to make a
*real* `std::ifstream` construction fail without actually breaking the
filesystem — so `repairTornTail()`'s "can I read this path right now" check
is factored out as its own predicate, consulted before the real
`std::ifstream` is constructed, rather than trying to intercept the stream
construction itself.

## Usage

Both classes take an optional second constructor parameter:

```cpp
explicit FileActionLog(std::filesystem::path path, morph::core::FileIoOps ioOps = {});
explicit FileOfflineQueue(std::filesystem::path path, morph::core::FileIoOps ioOps = {});
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
