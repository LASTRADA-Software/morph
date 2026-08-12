# SPDX-License-Identifier: Apache-2.0
#
# Vetted-HMAC adapter examples: libsodium and OpenSSL implementations of the
# MacFunction seam (include/morph/session/session_auth.hpp), each a drop-in
# replacement for the reference morph::session::hmacSha256. See
# docs/spec/security.md ("MAC-primitive recommended wiring"). Both adapters
# are optional and off unless the corresponding library is available *and*
# its sub-option is ON — the default morph build stays free of crypto
# dependencies. Enabled overall via -DMORPH_BUILD_HMAC_EXAMPLES=ON.

if(NOT TARGET morph::morph)
    message(FATAL_ERROR
        "examples/vetted_hmac expects the morph::morph target. Configure from the "
        "repository root with -DMORPH_BUILD_HMAC_EXAMPLES=ON instead of configuring "
        "examples/vetted_hmac directly.")
endif()

option(MORPH_BUILD_HMAC_EXAMPLE_LIBSODIUM "Build the libsodium HMAC adapter example (requires libsodium)" ON)
option(MORPH_BUILD_HMAC_EXAMPLE_OPENSSL   "Build the OpenSSL HMAC adapter example (requires OpenSSL)"     ON)

# ── libsodium adapter ────────────────────────────────────────────────────────
if(MORPH_BUILD_HMAC_EXAMPLE_LIBSODIUM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

    add_executable(morph_vetted_hmac_libsodium_demo libsodium_adapter_demo.cpp)
    target_link_libraries(morph_vetted_hmac_libsodium_demo PRIVATE morph::morph PkgConfig::SODIUM)
    apply_bigobj(morph_vetted_hmac_libsodium_demo)
    # Deliberately NOT calling apply_warnings() here — libsodium's public
    # headers are not -Weverything/-Werror clean and would fail the strict
    # build (same rationale as examples/bank/CMakeLists.txt's ORM headers).

    if(MORPH_BUILD_TESTS)
        # This directory is add_subdirectory()'d from the MORPH_BUILD_EXAMPLES
        # block, which runs *before* the top-level MORPH_BUILD_TESTS block that
        # find_package()s/FetchContent's Catch2 (see root CMakeLists.txt), so
        # Catch2's `Catch` CMake module (needed by include(Catch) below) is not
        # yet on CMAKE_MODULE_PATH here even though the Catch2::Catch2WithMain
        # *target* itself resolves fine (CMake defers target_link_libraries()
        # resolution). Same fix as examples/bank/CMakeLists.txt's tests block:
        # look up Catch2 independently and extend CMAKE_MODULE_PATH from here.
        find_package(Catch2 3 CONFIG QUIET)
        if(NOT Catch2_FOUND)
            message(WARNING "Catch2 not found; libsodium HMAC adapter tests will not be built.")
        else()
            add_executable(morph_vetted_hmac_libsodium_tests test_libsodium_adapter.cpp)
            target_link_libraries(morph_vetted_hmac_libsodium_tests
                PRIVATE morph::morph PkgConfig::SODIUM Catch2::Catch2WithMain)
            apply_bigobj(morph_vetted_hmac_libsodium_tests)

            list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
            include(Catch)
            catch_discover_tests(morph_vetted_hmac_libsodium_tests DISCOVERY_MODE PRE_TEST)
        endif()
    endif()
endif()

# ── OpenSSL adapter ──────────────────────────────────────────────────────────
if(MORPH_BUILD_HMAC_EXAMPLE_OPENSSL)
    find_package(OpenSSL REQUIRED)

    add_executable(morph_vetted_hmac_openssl_demo openssl_adapter_demo.cpp)
    target_link_libraries(morph_vetted_hmac_openssl_demo PRIVATE morph::morph OpenSSL::Crypto)
    apply_bigobj(morph_vetted_hmac_openssl_demo)
    # Deliberately NOT calling apply_warnings() here — OpenSSL's public
    # headers are not -Weverything/-Werror clean and would fail the strict
    # build (same rationale as examples/bank/CMakeLists.txt's ORM headers).

    if(MORPH_BUILD_TESTS)
        # See the matching comment in the libsodium block above: this
        # directory is processed before the top-level MORPH_BUILD_TESTS block
        # resolves Catch2, so its `Catch` CMake module isn't on
        # CMAKE_MODULE_PATH yet here.
        find_package(Catch2 3 CONFIG QUIET)
        if(NOT Catch2_FOUND)
            message(WARNING "Catch2 not found; OpenSSL HMAC adapter tests will not be built.")
        else()
            add_executable(morph_vetted_hmac_openssl_tests test_openssl_adapter.cpp)
            target_link_libraries(morph_vetted_hmac_openssl_tests
                PRIVATE morph::morph OpenSSL::Crypto Catch2::Catch2WithMain)
            apply_bigobj(morph_vetted_hmac_openssl_tests)

            list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
            include(Catch)
            catch_discover_tests(morph_vetted_hmac_openssl_tests DISCOVERY_MODE PRE_TEST)
        endif()
    endif()
endif()
