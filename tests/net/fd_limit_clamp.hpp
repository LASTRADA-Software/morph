// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <string>
#include <system_error>
#include <vector>

namespace morph::testing {

// What a scan of this process's fd table found, up to some ceiling: the
// highest fd open, and how many fds in that range are open at all. Same
// technique as tests/net/test_socket_server.cpp's own `highestOpenFd()`,
// with the count added -- `highest + 1 - open` is the size of the gap that
// `FdLimitClamp` exists to fill, and it is *reported* rather than assumed
// away.
struct FdScan {
    int highest = -1;
    int open = 0;
};

inline FdScan scanOpenFds(int limit) {
    FdScan scan;
    for (int fd = 0; fd < limit; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            scan.highest = fd;
            ++scan.open;
        }
    }
    return scan;
}

// RAII guard: forces the fd table to genuinely zero headroom and restores the
// original state on destruction.
//
// Lowering RLIMIT_NOFILE to `highestOpenFd()+1` assumes fd allocation so
// far has been gap-free, which nothing guarantees: a sanitizer runtime opens
// and closes descriptors of its own at startup, and an earlier TEST_CASE can
// open and close a *lower*-
// numbered fd (e.g. a transient connect() attempt) while a *higher*-numbered
// one stays permanently open (this process's getaddrinfo() call opens a
// long-lived resolver connection on first use, on this platform), leaving a
// gap below the computed "highest". The very next fd-allocating syscall then
// silently reuses that gap -- allowed by RLIMIT_NOFILE, since the gap's fd
// number is still under the limit -- and the intended EMFILE never fires.
// Confirmed empirically: the naive `highest+1` version of this guard passed
// this file's [tcp] tag roughly half the time and failed the other half.
//
// This version is self-verifying instead of computed: it lowers the limit,
// then actually opens `/dev/null` repeatedly until `open()` itself fails,
// filling any such gap for real rather than assuming there isn't one. Only
// once real exhaustion has been *observed* does the syscall under test run.
//
// This test fails under concurrent machine load if the clamp does not hold, so
// the clamp *says* what it measured rather than leaving the next reader
// guessing. `exhausted()` and `summary()` are that: every call site asserts
// `exhausted()` before the syscall under test -- so a clamp that did not bite
// fails on its own terms instead of being mistaken for a bug in `accept()` --
// and `INFO(summary())` puts the whole measurement (ambient fd table, limit
// applied, fds it took to fill, and the errno the fill loop stopped on) into
// the failure output of whichever assertion follows.
//
// The `exhausted()` check is deliberately *not* a REQUIRE inside this
// constructor: a Catch2 assertion failure there throws out of a half-built
// object, whose destructor never runs, leaving the process clamped and the
// filler fds leaked for every test after it.
class FdLimitClamp {
public:
    FdLimitClamp() {
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &_original) == 0);
        _ambient = scanOpenFds(_original.rlim_cur < static_cast<rlim_t>(65536) ? static_cast<int>(_original.rlim_cur)
                                                                               : 65536);
        REQUIRE(_ambient.highest >= 0);
        // A little headroom above `highest` so the fill loop below has a
        // small, bounded number of fds to open rather than racing to a huge
        // platform-default ceiling.
        rlimit constrained = _original;
        constrained.rlim_cur = static_cast<rlim_t>(_ambient.highest + 17);
        _clampedTo = constrained.rlim_cur;
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &constrained) == 0);

        for (;;) {
            int const fd = ::open("/dev/null", O_RDONLY);
            if (fd < 0) {
                _fillErrno = errno;
                break;
            }
            _dummyFds.push_back(fd);
        }
        // Genuinely exhausted now (confirmed by the loop above observing
        // open() itself fail), regardless of any gap in what was already open.
    }
    FdLimitClamp(const FdLimitClamp&) = delete;
    FdLimitClamp& operator=(const FdLimitClamp&) = delete;
    FdLimitClamp(FdLimitClamp&&) = delete;
    FdLimitClamp& operator=(FdLimitClamp&&) = delete;
    ~FdLimitClamp() {
        for (int const fd : _dummyFds) {
            ::close(fd);
        }
        ::setrlimit(RLIMIT_NOFILE, &_original);
    }

    /// `true` only if the fill loop stopped because the fd table was full.
    /// Any other stopping errno means the syscall under test is about to run
    /// against an fd table that still has headroom, so whatever it does next
    /// measures nothing.
    [[nodiscard]] bool exhausted() const { return _fillErrno == EMFILE; }

    /// Everything the clamp measured, on one line, for `INFO()` at a call
    /// site: the ambient fd table it found, the limit it applied, how many
    /// fds it had to open to fill the table, and why the fill loop stopped.
    [[nodiscard]] std::string summary() const {
        return "FdLimitClamp: ambient highest fd=" + std::to_string(_ambient.highest) +
               " open fds=" + std::to_string(_ambient.open) +
               " gap=" + std::to_string(_ambient.highest + 1 - _ambient.open) + "; RLIMIT_NOFILE soft " +
               std::to_string(_original.rlim_cur) + " -> " + std::to_string(_clampedTo) +
               "; filler fds opened=" + std::to_string(_dummyFds.size()) +
               "; fill loop stopped on errno=" + std::to_string(_fillErrno) + " (" +
               std::system_category().message(_fillErrno) + ")";
    }

private:
    rlimit _original{};
    FdScan _ambient{};
    rlim_t _clampedTo = 0;
    int _fillErrno = 0;
    std::vector<int> _dummyFds;
};

}  // namespace morph::testing
