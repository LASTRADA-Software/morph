// SPDX-License-Identifier: Apache-2.0

// The profiler macros in both configurations. Compiled under the project's
// full warning set: every local below is used by nothing but a macro, so a
// stub that dropped an argument instead of consuming it would fail this file
// with -Wunused-variable or -Wunused-but-set-variable.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/core/profiler.hpp>
#include <string>
#include <string_view>

TEST_CASE("profiler macros: every macro accepts a local as its only use", "[profiler]") {
    std::string const requestId = "request-1";
    double const depth = 3.0;
    std::size_t const inFlight = 2;
    std::string_view const note = "note";
    // With Tracy off a macro names its argument only in an unevaluated
    // context, so the analyzer sees this store as never read -- which is the
    // property under test, not a defect.
    char const* const threadName = "morph.test";  // NOLINT(clang-analyzer-deadcode.DeadStores)

    MORPH_THREAD_NAME(threadName);
    MORPH_ZONE("profiler test");
    MORPH_ZONE_TEXT(requestId);
    MORPH_PLOT("morph.test.depth", depth);
    MORPH_PLOT("morph.test.inFlight", inFlight);
    MORPH_MESSAGE(note);
    SUCCEED();
}

TEST_CASE("profiler macros: a zone's text may be empty", "[profiler]") {
    MORPH_ZONE("profiler test, empty text");
    MORPH_ZONE_TEXT(std::string_view{});
    SUCCEED();
}

TEST_CASE("profiler macros: arguments are evaluated only when Tracy is on", "[profiler]") {
    int evaluations = 0;
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): unread with Tracy off, by design (see above)
    auto const countedText = [&evaluations] {
        ++evaluations;
        return std::string_view{"request-1"};
    };
    {
        MORPH_ZONE("profiler test, counted");
        MORPH_ZONE_TEXT(countedText());
        MORPH_MESSAGE(countedText());
    }
#ifdef MORPH_TRACY_ENABLED
    CHECK(evaluations == 2);
#else
    // A disabled build pays for nothing at the call site, not even the
    // computation of a zone's text.
    CHECK(evaluations == 0);
#endif
}
