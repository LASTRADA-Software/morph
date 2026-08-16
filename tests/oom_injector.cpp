// SPDX-License-Identifier: Apache-2.0

#include "oom_injector.hpp"

#include <cstdlib>
#include <new>
#include <stdexcept>

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
    if (injectorArmed) {
        throw std::logic_error("OomInjector: another instance is already active on this thread");
    }
    injectorArmed = true;
    minSizeToFail = minSize;
}

OomInjector::~OomInjector() {
    injectorArmed = false;
    minSizeToFail = 0;
}

}  // namespace morph::testkit

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
