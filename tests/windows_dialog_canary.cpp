// SPDX-License-Identifier: Apache-2.0
//
// Fails in one way that can raise a Windows dialog, chosen by its argument, and
// must be seen to exit rather than wait for a click. tests/CMakeLists.txt
// registers one run per way, judged by the marker each prints before it fails,
// with a timeout: a run still waiting when the timeout expires is waiting on a
// dialog nobody will click.
//
// It calls nothing to suppress anything. What it proves is that linking
// core::testing_dialogs, as every morph test executable does on Windows,
// installs the suppression in an executable whose main() never asked for it.
// Modelled on core-cpp's tests/WindowsDialogCanary.cpp.

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

/// "This way of failing is not compiled into this build": a Release build has
/// no assert() to trip. ctest reports it as skipped.
[[maybe_unused]] constexpr int NotExercised = 77;

/// "The failure was handled and execution continued", distinct from success.
constexpr int ContinuedAfterFailure = 3;

/// Ends the process on SIGABRT with a plain exit status. Without it, abort()
/// ends in a way ctest reports as an exception whatever the test's properties
/// say, and no registration could judge the run by its output.
extern "C" void onAbort(int /*signal*/) { std::_Exit(1); }

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fputs("usage: morph_windows_dialog_canary assert|abort|invalid-parameter\n", stderr);
        return 2;
    }
    auto const mode = std::string_view{argv[1]};
    static_cast<void>(std::signal(SIGABRT, onAbort));

    std::fprintf(stderr, "morph_windows_dialog_canary: failing by %s\n", argv[1]);
    std::fflush(stderr);

    if (mode == "assert") {
#ifdef NDEBUG
        return NotExercised;
#else
        [[maybe_unused]] auto const canaryHolds = false;
        assert(canaryHolds && "morph_windows_dialog_canary asserts on purpose");
        std::fputs("morph_windows_dialog_canary: CONTINUED AFTER FAILURE\n", stderr);
        return ContinuedAfterFailure;
#endif
    }
    if (mode == "abort") {
        std::abort();
    }
    if (mode == "invalid-parameter") {
#ifdef _WIN32
        // A null destination is an invalid parameter to the CRT: a dialog in a
        // Debug CRT, Watson in a Release one, unless a handler was installed.
        char* volatile destination = nullptr;
        if (strcpy_s(destination, 1, "x") != 0) {
            std::fputs("morph_windows_dialog_canary: CONTINUED AFTER FAILURE\n", stderr);
            return ContinuedAfterFailure;
        }
        return 0;
#else
        return NotExercised;
#endif
    }
    std::fprintf(stderr, "morph_windows_dialog_canary: unknown mode %s\n", argv[1]);
    return 2;
}
