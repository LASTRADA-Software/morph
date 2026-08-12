# SPDX-License-Identifier: Apache-2.0
#
# Concept examples: one small, self-contained Catch2 test file per
# production-hardening feature (journal/outbox, offline queue durability,
# server-side validation, register authorization, transport limits, protocol
# versioning, connection scoping, observability, graceful shutdown), showing
# minimal golden-path usage for newcomers. See README.md for the index and
# docs/spec/ for the full design reference of each feature. Unlike
# examples/vetted_hmac, this has no external dependency, so it is built
# unconditionally under MORPH_BUILD_EXAMPLES (matching examples/forms's
# treatment).

if(NOT TARGET morph::morph)
    message(FATAL_ERROR
        "examples/concepts expects the morph::morph target. Configure from the "
        "repository root with -DMORPH_BUILD_EXAMPLES=ON instead of configuring "
        "examples/concepts directly.")
endif()

if(MORPH_BUILD_TESTS)
    # This directory is add_subdirectory()'d from the MORPH_BUILD_EXAMPLES
    # block, which runs *before* the top-level MORPH_BUILD_TESTS block that
    # find_package()s/FetchContent's Catch2 (see root CMakeLists.txt), so
    # Catch2's `Catch` CMake module (needed by include(Catch) below) is not
    # yet on CMAKE_MODULE_PATH here even though the Catch2::Catch2WithMain
    # *target* itself resolves fine (CMake defers target_link_libraries()
    # resolution). Same fix as examples/vetted_hmac/CMakeLists.txt's tests
    # block: look up Catch2 independently and extend CMAKE_MODULE_PATH from here.
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        message(WARNING "Catch2 not found; examples/concepts tests will not be built.")
    else()
        add_executable(morph_concepts_tests
            journal_and_outbox.cpp
            offline_queue_and_sync.cpp
            server_side_validation.cpp
            register_authorization_and_ids.cpp
            transport_limits.cpp
            protocol_and_connections.cpp
            observability_and_shutdown.cpp
        )
        target_link_libraries(morph_concepts_tests PRIVATE morph::morph Catch2::Catch2WithMain)
        apply_warnings(morph_concepts_tests)
        apply_bigobj(morph_concepts_tests)

        list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
        include(Catch)
        catch_discover_tests(morph_concepts_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()
