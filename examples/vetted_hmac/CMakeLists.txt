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
        # Catch2 is fetched by the root CMakeLists.txt above every
        # add_subdirectory() in the tree (morph#674), so this is a
        # build-system invariant rather than an environment question. It used
        # to be a `find_package(Catch2 3 CONFIG QUIET)` with a
        # message(WARNING) fallback, which meant a machine without the distro
        # package configured fine and silently built no tests here at all.
        if(NOT TARGET Catch2::Catch2WithMain)
            message(FATAL_ERROR
                "Catch2::Catch2WithMain does not exist; the root CMakeLists.txt's "
                "Catch2 section must run before add_subdirectory(examples/vetted_hmac).")
        else()
            add_executable(morph_vetted_hmac_libsodium_tests test_libsodium_adapter.cpp)
            target_link_libraries(morph_vetted_hmac_libsodium_tests
                PRIVATE morph::morph PkgConfig::SODIUM morph_test_main)
            apply_bigobj(morph_vetted_hmac_libsodium_tests)

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
        # See the matching comment in the libsodium block above.
        if(NOT TARGET Catch2::Catch2WithMain)
            message(FATAL_ERROR
                "Catch2::Catch2WithMain does not exist; the root CMakeLists.txt's "
                "Catch2 section must run before add_subdirectory(examples/vetted_hmac).")
        else()
            add_executable(morph_vetted_hmac_openssl_tests test_openssl_adapter.cpp)
            target_link_libraries(morph_vetted_hmac_openssl_tests
                PRIVATE morph::morph OpenSSL::Crypto morph_test_main)
            apply_bigobj(morph_vetted_hmac_openssl_tests)

            include(Catch)
            catch_discover_tests(morph_vetted_hmac_openssl_tests DISCOVERY_MODE PRE_TEST)
        endif()
    endif()
endif()
