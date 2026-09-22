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
    # Catch2 is fetched by the root CMakeLists.txt above every
    # add_subdirectory() in the tree (morph#674), so this is a build-system
    # invariant rather than an environment question. It used to be a
    # `find_package(Catch2 3 CONFIG QUIET)` with a message(WARNING) fallback,
    # which meant a machine without the distro package configured fine and
    # silently built no tests here at all.
    if(NOT TARGET Catch2::Catch2WithMain)
        message(FATAL_ERROR
            "Catch2::Catch2WithMain does not exist; the root CMakeLists.txt's "
            "Catch2 section must run before add_subdirectory(examples/concepts).")
    else()
        add_executable(morph_concepts_tests
            getting_started.cpp
            journal_and_outbox.cpp
            offline_queue_and_sync.cpp
            server_side_validation.cpp
            register_authorization_and_ids.cpp
            transport_limits.cpp
            protocol_and_connections.cpp
            observability_and_shutdown.cpp
        )
        target_link_libraries(morph_concepts_tests PRIVATE morph::morph morph_test_main)
        apply_warnings(morph_concepts_tests)
        apply_bigobj(morph_concepts_tests)
        # 8 TUs driving include/morph's journal, offline-queue, validation,
        # transport-limit, versioning, connection-scope, observability and
        # shutdown paths -- ctest-registered, so every sanitizer leg ran them
        # and learned nothing from them (morph#542).
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(morph_concepts_tests ${AF_SANITIZER})
        endif()

        include(Catch)
        catch_discover_tests(morph_concepts_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()
