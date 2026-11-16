// Force-included into the shared morph library's translation units on
// non-Windows platforms (see CMakeLists.txt).
//
// The library is compiled with hidden visibility, which also hides the
// function-local thread_local slots that core-cpp's headers define inline. A
// hidden copy is private to the library, so the library and the executable
// would each keep their own: an executor scope entered by the executable's
// event loop would be invisible to the library's code. Declaring the slots
// here, under default visibility, makes them symbols the dynamic linker merges
// into one per process. Later includes of the same header are no-ops.
#pragma GCC visibility push(default)
#include <core/async/ExecutorContext.hpp>
#pragma GCC visibility pop
