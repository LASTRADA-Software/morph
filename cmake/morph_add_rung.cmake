# SPDX-License-Identifier: Apache-2.0
#
# morph_add_rung(NAME <rung>): scaffolds the standard target set for one
# ladder rung, per examples/TESTING.md "Build system and CI". Convention
# over configuration: every target below is created only if its source
# directory (relative to the caller's CMAKE_CURRENT_SOURCE_DIR, i.e.
# examples/<rung>/) actually has files — a rung with no screens/ yet simply
# gets no ladder_<rung>_screens target, silently, so this one function
# serves every rung unchanged as each rung grows into more of the target
# set.
#
# Directory -> target convention:
#   src/models/*.cpp, src/db/*.cpp, src/app/*.cpp  -> ladder_<rung>_lib       STATIC (morph + Lightweight)
#   screens/*.cpp                                  -> ladder_<rung>_screens   STATIC (morph + the rung's models; toolkit-free)
#   src/server/*.cpp                                -> ladder_<rung>_server   EXE    (standalone server; skipped under Emscripten)
#   tests/*.cpp                                     -> ladder_<rung>_tests    EXE    (Catch2; skipped under Emscripten)
#
# Every ctest case discovered from ladder_<rung>_tests is registered under the
# name "<rung>.<TEST_CASE name>" and gets the single label "ladder-<rung>",
# both applied by catch_discover_tests itself (TEST_PREFIX and PROPERTIES on
# the call below). CI selects with `ctest -L ladder`
# (.github/workflows/ci.yml, job ladder-tests), which still matches: ctest's
# -L takes a regex, not an exact label. End-to-end journeys additionally get
# "journey" from a small generated post-pass. See the catch_discover_tests
# call below for why the label cannot be a two-value LABELS, and why the rung
# label is not applied by the post-pass.
#
# The name prefix is what makes each case's ctest entry unique. A ctest name
# is global to the build tree -- not scoped to the target or the directory it
# was discovered from -- and rungs share TEST_CASE names freely, mostly
# through the App/auth/forms scaffolding they all exercise. Two entries under
# one name are both reached by every by-name operation, and
# `set_tests_properties(<name> PROPERTIES LABELS ...)` is one of them: CTest
# *appends* labels, so the first-registered entry accumulated later rungs'
# labels too and `ctest -L ladder-<rung>` over-selects -- running another
# rung's binary alongside its own, with nothing to say so (measured once as
# one rung selecting 180 cases while owning 168). A failure line, `--output-junit` and
# CDash also identify a test by name alone, so a duplicate could not say which
# binary failed. scripts/check_ctest_name_collisions.sh is the gate that keeps
# this property true; ci.yml's ladder-tests job runs it on the all-rungs
# build, which is the only place a cross-rung collision can exist.
#
# RESOURCE_LOCK is the literal string "morph_ladder_test_db" for every rung's
# tests, its screen tests and its render smoke included (a DbFixture opens
# morph_ladder_test.db in the shared working directory), matching examples/common's own ladder_common_tests — deliberately
# the *same* name across every rung/binary, not a per-rung one: ctest's
# RESOURCE_LOCK serializes any two ctest cases sharing a lock name even
# across different test *binaries*, which is exactly what's needed if two
# rungs' test binaries ever point at the same on-disk database file (e.g. a
# shared ODBC_CONNECTION_STRING override in some future CI leg) — harmless
# extra serialization if they don't.
#
# CONFIGURE_DEPENDS: every file(GLOB_RECURSE ...) below passes it so a newly
# added source file re-triggers CMake's configure step on the next build
# without an explicit reconfigure. This is a Ninja/Makefiles-generator
# feature (silently a no-op elsewhere, per CMake's own docs); every preset in
# this repo's CMakePresets.json inherits from base-linux or base-vcpkg, both
# of which pin "generator": "Ninja", so this is safe repo-wide today. If a
# non-Ninja/Makefiles preset is ever added, new ladder source files added
# under that preset would need an explicit reconfigure (`cmake --preset ...`)
# before they show up in the build — CONFIGURE_DEPENDS would silently not
# catch them.
function(morph_add_rung)
    set(options "")
    set(oneValueArgs NAME)
    set(multiValueArgs "")
    cmake_parse_arguments(RUNG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT RUNG_NAME)
        message(FATAL_ERROR "morph_add_rung() requires NAME <rung>")
    endif()
    # A rung builds nothing in a browser: its models need the SQLite ORM, and its screens reach a browser through the
    # generic client, which examples/client builds.
    if(EMSCRIPTEN)
        return()
    endif()
    if(NOT TARGET morph_ladder_testkit)
        message(FATAL_ERROR "morph_add_rung(NAME ${RUNG_NAME}) called before examples/common was added "
                            "(morph_ladder_testkit does not exist yet) — add_subdirectory(common) first.")
    endif()

    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_rung "${RUNG_NAME}")

    # examples/common/CMakeLists.txt already calls find_package(Qt6 ...
    # COMPONENTS Core WebSockets) and qt_standard_project_setup(), but that
    # call's IMPORTED targets (Qt6::Core etc.) and qt_standard_project_setup's
    # directory-scoped defaults are visible only in common/'s own directory
    # scope and its subdirectories — CMake does not propagate find_package()
    # imported targets sideways to sibling directories. examples/<rung>/ is a
    # *sibling* of common/ (both are add_subdirectory()'d from
    # examples/CMakeLists.txt), not a descendant of it, so without this,
    # ladder_<rung>_lib's `target_link_libraries(... Qt6::Core)` below fails
    # with "target was not found" the first time this function is actually
    # exercised (verified empirically). Calling both again here is cheap and, per Qt's own docs,
    # idempotent/harmless if some ancestor scope already ran them — this is
    # the one place in the whole rung that needs it, since every target below
    # is created in *this* function's (i.e. the calling rung directory's) scope.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
    qt_standard_project_setup(REQUIRES 6.5)

    # ── ladder_<rung>_lib: models + db + app bootstrap (native only) ────
    # Lightweight::Lightweight (ODBC) does not exist under Emscripten:
    # examples/common/CMakeLists.txt returns early, before its
    # CPMAddPackage(Lightweight) call, whenever EMSCRIPTEN is
    # set.
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _lib_sources CONFIGURE_DEPENDS
            "${_dir}/src/models/*.cpp" "${_dir}/src/db/*.cpp" "${_dir}/src/app/*.cpp")
        # The rung's public headers are listed as target sources purely so
        # AUTOMOC sees them. AUTOMOC looks for a Q_OBJECT header next to the
        # .cpp of the same basename, and a rung's layout deliberately splits
        # those apart (include/<rung>/app/app.hpp vs src/app/app.cpp), so a
        # QObject declared in include/ gets no moc output at all otherwise —
        # which a static library happily builds and only fails at the first
        # link that actually needs the vtable (a rung's `app::App`, the moment
        # its test binary links it). Header entries are not
        # compiled; they only join the AUTOMOC scan.
        file(GLOB_RECURSE _lib_headers CONFIGURE_DEPENDS "${_dir}/include/*.hpp")
        if(_lib_sources)
            add_library(ladder_${_rung}_lib STATIC ${_lib_sources} ${_lib_headers})
            add_library(morph::ladder_${_rung}_lib ALIAS ladder_${_rung}_lib)
            # examples/common (PROJECT_SOURCE_DIR, not a "../common" relative
            # path — see examples/CMakeLists.txt's own comment on why: robust to
            # morph being embedded via add_subdirectory() in a parent project)
            # is on the include path for clock.hpp, the ladder-wide injectable
            # "now()" every rung's time-dependent model logic reads instead of
            # DateTime::now() directly (examples/common/clock.hpp's own doc
            # comment). Discovered as a real gap, not present in the original
            # sketch: ladder_<rung>_lib links no examples/common target, so its
            # own include path needs common added directly.
            target_include_directories(ladder_${_rung}_lib PUBLIC "${_dir}/include" "${PROJECT_SOURCE_DIR}/examples/common")
            target_link_libraries(ladder_${_rung}_lib PUBLIC morph::morph Lightweight::Lightweight Qt6::Core)
            target_compile_features(ladder_${_rung}_lib PUBLIC cxx_std_23)
            set_target_properties(ladder_${_rung}_lib PROPERTIES AUTOMOC ON)
            apply_bigobj(ladder_${_rung}_lib)
            # Lightweight's headers are not -Werror clean (bank's own caveat,
            # examples/bank/CMakeLists.txt) — no apply_warnings() here.
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_lib)
            endif()
            # AF_SANITIZER (asan/tsan/ubsan): applied the same way apply_coverage()
            # is above. Without this, a sanitizer build of the ladder compiles
            # this target (a rung's models) with no instrumentation at all,
            # silently defeating the whole point of building under that preset.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_lib ${AF_SANITIZER})
            endif()
        endif()
    endif()

    # ── ladder_<rung>_screens: the rung's UI documents (spec 4 §6) ──────
    # Toolkit-free by its link set: morph and the rung's own models, nothing
    # else, so the server, the generic client's local mode and the screen tests
    # all link the same screens. The screens name the models' action types, so
    # they are built only where the models are (not in a client-only WebAssembly
    # build).
    file(GLOB _screen_sources CONFIGURE_DEPENDS "${_dir}/screens/*.cpp")
    if(_screen_sources AND TARGET ladder_${_rung}_lib)
        add_library(ladder_${_rung}_screens STATIC ${_screen_sources})
        add_library(morph::ladder_${_rung}_screens ALIAS ladder_${_rung}_screens)
        target_include_directories(ladder_${_rung}_screens PUBLIC "${_dir}/screens")
        target_link_libraries(ladder_${_rung}_screens PUBLIC morph::morph morph::ladder_${_rung}_lib)
        target_compile_features(ladder_${_rung}_screens PUBLIC cxx_std_23)
        apply_warnings(ladder_${_rung}_screens)
        apply_bigobj(ladder_${_rung}_screens)
        if(AF_COVERAGE)
            apply_coverage(ladder_${_rung}_screens)
        endif()
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(ladder_${_rung}_screens ${AF_SANITIZER})
        endif()
        # The screen tests need examples/common's harness, which the top level
        # adds after the ladder: decide once every directory is configured.
        # A deferred call's arguments are evaluated when it runs, when this
        # function's variables are gone, so EVAL bakes their values in now.
        if(NOT EMSCRIPTEN)
            cmake_language(EVAL CODE "
                cmake_language(DEFER DIRECTORY [[${PROJECT_SOURCE_DIR}]]
                    CALL _morph_rung_screen_tests [[${_rung}]] [[${_dir}]])")
        endif()
    endif()

    # ── ladder_<rung>_server: standalone server binary (native only) ────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _server_sources CONFIGURE_DEPENDS "${_dir}/src/server/*.cpp")
        if(_server_sources AND TARGET ladder_${_rung}_lib)
            add_executable(ladder_${_rung}_server ${_server_sources})
            target_link_libraries(ladder_${_rung}_server PRIVATE
                morph::ladder_${_rung}_lib morph::qt morph_qt_impl Qt6::Core)
            # The server serves the rung's screens beside its models (spec 4 §1), so a generic client opens them.
            if(TARGET ladder_${_rung}_screens)
                target_link_libraries(ladder_${_rung}_server PRIVATE morph::ladder_${_rung}_screens)
            endif()
            target_compile_features(ladder_${_rung}_server PRIVATE cxx_std_23)
            apply_bigobj(ladder_${_rung}_server)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_server)
            endif()
            # See ladder_${_rung}_lib's identical AF_SANITIZER block above.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_server ${AF_SANITIZER})
            endif()
        endif()
    endif()

    # ── ladder_<rung>_tests: Catch2 model + presenter tests ──────────────
    if(NOT EMSCRIPTEN)
        file(GLOB_RECURSE _test_sources CONFIGURE_DEPENDS "${_dir}/tests/*.cpp")
        if(_test_sources)
            # examples/common/testkit/testkit_main.cpp is compiled into every
            # rung's test binary rather than linked from morph_ladder_testkit:
            # that library links Catch2::Catch2 (the no-main variant), so a
            # rung whose tests/ holds only TEST_CASE translation units has no
            # `main` at all and fails to link. The main is Qt-owning (a
            # QCoreApplication that outlives every QObject Catch2 constructs —
            # see that file's own comment), which every rung needs anyway the
            # moment it touches BackendRig's Socket mode. It stays a compiled
            # source rather than a library member so ladder_common_tests, which
            # already compiles the same file directly, keeps exactly one
            # definition of `main`.
            add_executable(ladder_${_rung}_tests
                ${_test_sources}
                "${PROJECT_SOURCE_DIR}/examples/common/testkit/testkit_main.cpp")
            target_link_libraries(ladder_${_rung}_tests PRIVATE morph::ladder_testkit)
            # ctest runs a rung's test binary from its own build directory, so
            # repo-relative test data (e.g. tests/fuzz/findings/*, replayed as
            # hostile input by a model suite) cannot be found
            # by a relative path. Compile the source root in instead — the same
            # thing tests/fuzz/CMakeLists.txt does by passing absolute corpus
            # paths on the command line, expressed here as a macro because a
            # Catch2 binary takes no such arguments.
            target_compile_definitions(ladder_${_rung}_tests
                PRIVATE MORPH_LADDER_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
            if(TARGET ladder_${_rung}_lib)
                # WHOLE_ARCHIVE, not a plain link: a rung's schema TU
                # (src/db/schema.cpp) contributes nothing but static-init
                # side effects — LIGHTWEIGHT_SQL_MIGRATION registers the
                # rung's tables with the process-wide MigrationManager from a
                # namespace-scope initializer. No test references a symbol in
                # that TU, so an ordinary static-library link never pulls the
                # object in and DbFixture::ApplyPendingMigrations() finds no
                # migrations at all ("no such table: pastes"). Pulling the
                # whole archive is the standard fix and keeps the schema
                # exactly where IMPLEMENTATION.md rule 4 puts it, instead of
                # making every rung's test suite name a dummy symbol to force
                # the link.
                target_link_libraries(ladder_${_rung}_tests PRIVATE
                    "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_${_rung}_lib>")
                # The SYSTEM-include demotion, for this reason:
                # Lightweight's target_include_directories() call is plain
                # PUBLIC, not SYSTEM, so apply_warnings() below (-Werror
                # included) would otherwise apply in full to every Lightweight
                # header a test TU reaches (directly, by testing the model
                # layer, or transitively through template instantiation).
                get_target_property(_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
                if(_lightweight_includes)
                    target_include_directories(ladder_${_rung}_tests SYSTEM PRIVATE ${_lightweight_includes})
                endif()
                unset(_lightweight_includes)
            endif()
            # The durable SQLite-backed IOfflineQueue, when the top-level
            # MORPH_BUILD_OFFLINE_SQLITE option built it (off by default -- see
            # the option's own comment in the root CMakeLists.txt: SQLite3 is a
            # system dev package the standard build tree does not require).
            #
            # A rung that exercises the offline queue must therefore compile
            # and pass *both* ways: against morph::offline::InMemoryOfflineQueue
            # always, and additionally against SqliteOfflineQueue when this
            # macro is defined. Gating on the macro rather than on the header's
            # presence is deliberate -- the header exists in every checkout, but
            # including it without SQLite3 on the link line fails at link time,
            # not at compile time, which is the harder failure to read.
            if(TARGET morph::offline_sqlite)
                target_link_libraries(ladder_${_rung}_tests PRIVATE morph::offline_sqlite)
                target_compile_definitions(ladder_${_rung}_tests PRIVATE MORPH_LADDER_HAVE_OFFLINE_SQLITE=1)
            endif()
            target_compile_features(ladder_${_rung}_tests PRIVATE cxx_std_23)
            set_target_properties(ladder_${_rung}_tests PROPERTIES AUTOMOC ON)
            apply_warnings(ladder_${_rung}_tests)
            apply_bigobj(ladder_${_rung}_tests)
            if(AF_COVERAGE)
                apply_coverage(ladder_${_rung}_tests)
            endif()
            # AF_SANITIZER (asan/tsan/ubsan): applied the same way apply_coverage()
            # is above. Without this, a sanitizer build compiles the rung's suite
            # with no instrumentation at all, and a sanitizer leg over it would
            # build and pass while never exercising the sanitizer.
            if(DEFINED AF_SANITIZER)
                apply_sanitizers(ladder_${_rung}_tests ${AF_SANITIZER})
            endif()

            include(Catch)
            get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
            cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)
            catch_discover_tests(ladder_${_rung}_tests
                DISCOVERY_MODE POST_BUILD
                # "<rung>." on every discovered ctest name, so no two rungs'
                # entries can share one (see the header comment for what a
                # shared name does to `ctest -L ladder-<rung>`).
                # One word, no space and no `;`: TEST_PREFIX rides the same
                # `-D VAR=...` channel as PROPERTIES below, whose flattening
                # the comment there documents. Catch2 prefixes the *ctest*
                # name only -- the filter argument it passes back to the
                # binary stays the unprefixed test name
                # (CatchAddTests.cmake's `escaped_name`), so this changes
                # nothing about which case each entry runs, and it does not
                # address the `~`-prefix hazard.
                TEST_PREFIX "${_rung}."
                DL_PATHS "${_qt_bin_dir}"
                # One label, not two, and the *rung* one. catch_discover_tests
                # forwards PROPERTIES as a flat CMake list through a
                # `-D VAR=a;b;c` command line, where a list separator and a
                # literal semicolon are indistinguishable and no escaping
                # survives, so a multi-value `LABELS "ladder;ladder-${_rung}"`
                # does not produce a two-label test — it shifts every following
                # name/value pair by one (examples/common/CMakeLists.txt
                # documents the same trap at length).
                #
                # Nothing is lost by dropping the generic `ladder` label:
                # ctest's `-L` is a *regex*, not an exact match, so `-L ladder`
                # matches `ladder-${_rung}` by prefix and CI's
                # `ctest -L ladder -LE stress` selects exactly what it did.
                #
                # Applying the rung label *here* rather than in a post-pass is
                # what makes per-rung selection correct for every test name.
                # A post-pass cannot do it: Catch2's discovery script appends
                # the plain test name to `<target>_TESTS`
                # (`list(APPEND tests "${prefix}${plain_name}${suffix}")`), so
                # a TEST_CASE whose name contains a `;` is flattened into two
                # fragments that name no test, and set_tests_properties then
                # silently applies to nothing: the case keeps `ladder` and
                # never gains `ladder-<rung>`, so `ctest -L ladder-<rung>`
                # under-selects without saying so.
                PROPERTIES LABELS ladder-${_rung} TIMEOUT 120 RESOURCE_LOCK morph_ladder_test_db
            )
            # `journey` still needs a post-pass: it applies to some cases and
            # not others, which catch_discover_tests' uniform PROPERTIES cannot
            # express. It therefore inherits the flattening limitation
            # described above — a journey case whose name contained a `;` would
            # keep its rung label but never gain `journey`. Rather than let
            # that go quiet, it is rejected at configure time by the guard
            # below.
            #
            # set_tests_properties *appends* to LABELS rather than replacing
            # them -- the same append that let two same-named ctest entries
            # accumulate each other's rung labels. The journey
            # branch restates the rung label alongside `journey` anyway: the
            # resulting repeat of `ladder-<rung>` is inert (`-L` asks whether
            # a label is present, not how often), and restating it keeps the
            # branch correct without depending on which of the two behaviours
            # CTest has.
            foreach(_journey_src IN LISTS _test_sources)
                file(STRINGS "${_journey_src}" _bad_journey_names
                     REGEX "TEST_CASE\\(\"Journey: [^\"]*;")
                if(_bad_journey_names)
                    message(FATAL_ERROR
                        "morph_add_rung(${_rung}): a \"Journey: \" TEST_CASE name contains a "
                        "semicolon. CMake cannot round-trip that through Catch2's discovered "
                        "test list, so the case would silently never gain its `journey` label. "
                        "Rename it.\n  File: ${_journey_src}\n  Name: ${_bad_journey_names}")
                endif()
            endforeach()
            file(GENERATE
                OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ladder_${_rung}_tests_journey_label.cmake"
                CONTENT "foreach(_ladder_test IN LISTS ladder_${_rung}_tests_TESTS)
    # End-to-end user journeys carry an extra `journey` label so they can be
    # selected (ctest -L journey) or excluded the way `stress` is. Keyed off
    # the test name's own \"Journey: \" prefix rather than a separate source
    # list, so adding a journey needs no CMake edit.
    if(_ladder_test MATCHES \"Journey: \")
        set_tests_properties(\"\${_ladder_test}\" PROPERTIES LABELS \"ladder-${_rung};journey\")
    endif()
endforeach()
"
            )
            set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
                "${CMAKE_CURRENT_BINARY_DIR}/ladder_${_rung}_tests_journey_label.cmake")
        endif()
    endif()
endfunction()

# ── ladder_<rung>_screen_tests: screens/tests/*.cpp on the screen harness ────
# Deferred to the end of the top-level directory by the screens block above:
# morph::client, whose ScreenHarness the tests drive, is added after
# the ladder, and exists only when MORPH_BUILD_EXAMPLES is on. Without it the
# rung's screens still build; only these tests are skipped.
function(_morph_rung_screen_tests _rung _dir)
    if(NOT MORPH_BUILD_TESTS OR NOT TARGET morph::client OR NOT TARGET ladder_${_rung}_screens)
        return()
    endif()
    _morph_rung_render_smoke("${_rung}" "${_dir}")
    file(GLOB _sources CONFIGURE_DEPENDS "${_dir}/screens/tests/*.cpp")
    if(NOT _sources)
        return()
    endif()
    add_executable(ladder_${_rung}_screen_tests ${_sources}
        "${PROJECT_SOURCE_DIR}/examples/common/testkit/testkit_main.cpp")
    target_link_libraries(ladder_${_rung}_screen_tests PRIVATE
        morph::ladder_${_rung}_screens morph::client morph::ladder_testkit)
    target_include_directories(ladder_${_rung}_screen_tests PRIVATE "${_dir}/tests")
    if(TARGET ladder_${_rung}_lib)
        # As for ladder_<rung>_tests: the schema TU registers its migrations
        # from a static initializer that nothing else references.
        target_link_libraries(ladder_${_rung}_screen_tests PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_${_rung}_lib>")
        # Lightweight's headers do not meet this project's warnings: system headers here, as for the rung's tests.
        get_target_property(_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
        if(_lightweight_includes)
            target_include_directories(ladder_${_rung}_screen_tests SYSTEM PRIVATE ${_lightweight_includes})
        endif()
    endif()
    target_compile_features(ladder_${_rung}_screen_tests PRIVATE cxx_std_23)
    set_target_properties(ladder_${_rung}_screen_tests PROPERTIES AUTOMOC ON)
    apply_warnings(ladder_${_rung}_screen_tests)
    apply_bigobj(ladder_${_rung}_screen_tests)
    if(AF_COVERAGE)
        apply_coverage(ladder_${_rung}_screen_tests)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(ladder_${_rung}_screen_tests ${AF_SANITIZER})
    endif()
    include(Catch)
    catch_discover_tests(ladder_${_rung}_screen_tests
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "${_rung}.screens."
        PROPERTIES TIMEOUT 120 LABELS "ladder-${_rung}" ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            RESOURCE_LOCK morph_ladder_test_db)
endfunction()

# ── ladder_<rung>_render_smoke: screens/render_smoke/*.cpp on Qt Quick ─────────
# Every screen of the rung mounted by the Qt Quick renderer, offscreen (spec 4
# §7). Its own executable with a plain Catch2 main: the Qt Quick frontend makes
# the QGuiApplication it needs only when no application exists yet, and the
# rung tests' main makes a QCoreApplication.
function(_morph_rung_render_smoke _rung _dir)
    if(NOT TARGET morph_qt_quick)
        return()
    endif()
    file(GLOB _sources CONFIGURE_DEPENDS "${_dir}/screens/render_smoke/*.cpp")
    if(NOT _sources)
        return()
    endif()
    add_executable(ladder_${_rung}_render_smoke ${_sources})
    target_link_libraries(ladder_${_rung}_render_smoke PRIVATE
        morph::ladder_${_rung}_screens morph::client morph::qt_quick morph::ladder_testkit morph_test_main)
    target_include_directories(ladder_${_rung}_render_smoke PRIVATE "${_dir}/tests")
    if(TARGET ladder_${_rung}_lib)
        target_link_libraries(ladder_${_rung}_render_smoke PRIVATE
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_${_rung}_lib>")
        get_target_property(_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
        if(_lightweight_includes)
            target_include_directories(ladder_${_rung}_render_smoke SYSTEM PRIVATE ${_lightweight_includes})
        endif()
    endif()
    target_compile_features(ladder_${_rung}_render_smoke PRIVATE cxx_std_23)
    apply_warnings(ladder_${_rung}_render_smoke)
    apply_bigobj(ladder_${_rung}_render_smoke)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(ladder_${_rung}_render_smoke ${AF_SANITIZER})
    endif()
    include(Catch)
    get_target_property(_qt_core Qt6::Core IMPORTED_LOCATION)
    if(NOT _qt_core)
        get_target_property(_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
    endif()
    set(_dl_paths "")
    if(_qt_core)
        cmake_path(GET _qt_core PARENT_PATH _dl_paths)
    endif()
    catch_discover_tests(ladder_${_rung}_render_smoke
        DISCOVERY_MODE PRE_TEST
        DL_PATHS "${_dl_paths}"
        TEST_PREFIX "${_rung}.render."
        PROPERTIES TIMEOUT 120 LABELS "ladder-${_rung}" ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            RESOURCE_LOCK morph_ladder_test_db)
endfunction()
