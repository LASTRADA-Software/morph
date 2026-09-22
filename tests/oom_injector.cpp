// SPDX-License-Identifier: Apache-2.0

#include "oom_injector.hpp"

#include <cstdlib>
#include <new>
#include <stdexcept>

// ASan/TSan already define their own operator new/operator new[]/operator
// delete/operator delete[] inside their runtime (libclang_rt.{asan,tsan}_cxx.a)
// to track allocations for their own instrumentation. So this file's own
// overloads are *compiled out* under either sanitizer, by the guard a few
// lines below, rather than linked beside that runtime's.
//
// Which means -- and an earlier version of this comment said the opposite, in
// a form confident enough to act on (morph#718) -- there is no link failure
// and no "multiple definition of `operator new(unsigned long)'". Measured with
// clang 22.1.8, the major CI pins: `clang++ -std=c++23 -fsanitize=address
// tests/oom_injector.cpp` links, and `nm -C --defined-only` finds eight
// operator new/delete symbols in a plain object and zero in the ASan one.
//
// What happens instead is a *runtime* failure. OomInjector's constructor
// throws under the guard (see the #ifdef in it), so every test that constructs
// one fails when it runs, with
//
//     OomInjector: unusable under ASan/TSan (operator new/delete overrides are
//     compiled out -- see oom_injector.cpp)
//
// ctest-level test exclusion is therefore exactly the remedy for it, and is
// the one in use: .github/workflows/ci.yml excludes `OomInjector|morph#108` by
// name on the clang-asan and clang-tsan legs. With that filter bypassed,
// morph#719's lane measured six tests failing on each of the two legs. Deleting
// the exclusion on the strength of the old comment turns both legs red.
//
// The link failure the old comment described was presumably real before the
// guard below existed -- it is why the guard exists -- and the comment was not
// updated when the guard landed.
//
// Detected via nested #ifdef/#if blocks (not one combined boolean
// expression): MSVC's preprocessor does not define __has_feature at all, and
// some preprocessors do not short-circuit `defined(__has_feature) &&
// __has_feature(...)` on a single line the way C++ code would -- they can
// still try to macro-expand `__has_feature` as a bare identifier and choke
// on the unmatched parenthesis that follows (`__has_feature(address_
// sanitizer)`) once `defined(__has_feature)` alone is false. Nesting avoids
// ever writing `__has_feature` on a line MSVC actually preprocesses.
//
// GCC and Clang both define __SANITIZE_ADDRESS__/__SANITIZE_THREAD__
// whenever the corresponding -fsanitize=address/thread flag is active, which
// covers this repo's own clang-asan/clang-tsan presets without needing
// __has_feature at all; the __has_feature branch below only matters for a
// Clang invocation that enables a sanitizer through some other means.
//
// No CI leg escapes the first branch, which is what makes the paragraph above
// true of CI and not merely of this machine: every sanitizer build in this
// repository is clang (linux-sanitizers' clang-asan/clang-tsan/clang-ubsan
// matrix, ladder-sanitizers, kanban-tsan and the bank leg), the Windows legs
// (cl-*, clangcl-*) enable no sanitizer at all, and clang 22 -- the major
// ci.yml's CLANG_VERSION pins -- defines both macros. Verified by compiling a
// probe carrying this exact #if/#elif chain with clang++ 22.1.8: it reports
// `guard FIRES via defined(__SANITIZE_ADDRESS__)/(__SANITIZE_THREAD__)` under
// -fsanitize=address and under -fsanitize=thread, and does not fire under
// -fsanitize=undefined or with no sanitizer, which is correct -- the overrides
// work normally on the ubsan leg, and that leg is not excluded.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MORPH_TESTKIT_UNDER_ASAN_OR_TSAN
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MORPH_TESTKIT_UNDER_ASAN_OR_TSAN
#elif __has_feature(thread_sanitizer)
#define MORPH_TESTKIT_UNDER_ASAN_OR_TSAN
#endif
#endif

namespace {

// injectorArmed off means no injector active on this thread. While armed,
// the next operator new call with size >= minSizeToFail throws and disarms
// (one-shot). Plain built-in types only -- these variables' own
// reads/writes must never themselves allocate, or arming the injector would
// recurse into itself the moment operator new next runs. thread_local, not
// a class member: operator new below is a free function with no `this` to
// hang state off, and every thread needs its own independent state (see the
// header's own @par Thread safety).
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables) -- this
// state *is* the seam; there is no non-global way to reach into a bare
// operator new call from outside.
thread_local std::size_t minSizeToFail = 0;
thread_local bool injectorArmed = false;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

namespace morph::testkit {

OomInjector::OomInjector(std::size_t minSize) {
#ifdef MORPH_TESTKIT_UNDER_ASAN_OR_TSAN
    // The operator new/delete overrides below are compiled out under this
    // build (see the guard around them). Constructing an OomInjector here
    // would silently do nothing, so this throws instead of letting a test
    // misread that silence as "the injected failure never happened to
    // trigger". In practice this never fires: every test using OomInjector
    // is excluded from the clang-asan/clang-tsan CI legs by tag (see
    // .github/workflows/ci.yml) -- this is a correctness backstop, not the
    // primary mechanism.
    (void)minSize;
    throw std::logic_error(
        "OomInjector: unusable under ASan/TSan (operator new/delete overrides are compiled out -- "
        "see oom_injector.cpp)");
#else
    if (injectorArmed) {
        throw std::logic_error("OomInjector: another instance is already active on this thread");
    }
    injectorArmed = true;
    minSizeToFail = minSize;
#endif
}

OomInjector::~OomInjector() {
    injectorArmed = false;
    minSizeToFail = 0;
}

}  // namespace morph::testkit

#ifndef MORPH_TESTKIT_UNDER_ASAN_OR_TSAN

namespace {

// Every operator new overload below funnels through this so the trigger
// logic lives in one place. Recursion guard: reading/writing the
// thread_local state above touches only built-ins, never the heap, so this
// cannot re-enter itself.
void* allocateOrInject(std::size_t size) {
    if (injectorArmed && size >= minSizeToFail) {
        injectorArmed = false;  // one-shot: disarm before throwing, so the
                                // catch block itself (and anything else on
                                // this thread afterward) allocates normally.
        throw std::bad_alloc{};
    }
    // This *is* the process-wide operator new/delete pair; std::malloc/free is
    // what it has to be built from. The directive stays on one physical line;
    // see the note at include/morph/detail/fixed_string.hpp:48.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
        return ptr;
    }
    throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) { return allocateOrInject(size); }

void* operator new[](std::size_t size) { return allocateOrInject(size); }

void* operator new(std::size_t size, const std::nothrow_t& tag) noexcept {
    (void)tag;
    try {
        return allocateOrInject(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    (void)tag;
    try {
        return allocateOrInject(size);
    } catch (...) {
        return nullptr;
    }
}

// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name,
// cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) -- these
// replace the library's own global operator delete overloads; the "sized"
// forms' declarations in <new> don't name their second parameter, and
// std::free is what a hand-written operator delete has to call to release
// what allocateOrInject's std::malloc above returned.
void operator delete(void* ptr) noexcept { std::free(ptr); }

void operator delete[](void* ptr) noexcept { std::free(ptr); }

void operator delete(void* ptr, std::size_t size) noexcept {
    (void)size;
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
    (void)size;
    std::free(ptr);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name,
// cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

#endif  // !MORPH_TESTKIT_UNDER_ASAN_OR_TSAN
