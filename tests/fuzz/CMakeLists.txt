# libFuzzer harnesses over morph::wire::decode and RemoteServer::dispatchMessage.
# Requires Clang (libFuzzer ships with its runtime via -fsanitize=fuzzer); not
# built unless MORPH_BUILD_FUZZERS=ON, so the default build never sees these
# targets. See docs/spec/testing_strategy.md.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "MORPH_BUILD_FUZZERS=ON requires Clang (libFuzzer via -fsanitize=fuzzer); "
        "current compiler is ${CMAKE_CXX_COMPILER_ID}. Reconfigure with Clang "
        "(cmake/llvm-toolchain.cmake) or unset MORPH_BUILD_FUZZERS.")
endif()

add_executable(fuzz_wire_decode fuzz_wire_decode.cpp)
target_link_libraries(fuzz_wire_decode PRIVATE morph::morph)
apply_warnings(fuzz_wire_decode)
apply_fuzzer(fuzz_wire_decode)
# LLVMFuzzerTestOneInput has no declaring header -- libFuzzer's runtime calls it
# by symbol name -- so there is no previous prototype for Clang to match against.
target_compile_options(fuzz_wire_decode PRIVATE -Wno-missing-prototypes)
