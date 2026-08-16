// SPDX-License-Identifier: Apache-2.0

#include "oom_injector.hpp"

#include <cstdlib>
#include <new>
#include <stdexcept>

// ASan/TSan already define their own operator new/operator new[]/operator
// delete/operator delete[] inside their runtime (libclang_rt.{asan,tsan}_cxx.a)
// to track allocations for their own instrumentation. Defining this file's
// own overloads under either sanitizer fails at *link* time with "multiple
// definition of `operator new(unsigned long)'" against that runtime archive
// -- ctest-level test exclusion (see .github/workflows/ci.yml) cannot help
// here, since the conflict happens before any test ever runs.
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
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) --
    // this *is* the process-wide operator new/delete pair; std::malloc/free
    // is what it has to be built from.
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
        return ptr;
    }
    throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) {
    return allocateOrInject(size);
}

void* operator new[](std::size_t size) {
    return allocateOrInject(size);
}

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
void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

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
