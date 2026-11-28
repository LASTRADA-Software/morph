# MORPH_BUILD_UNIT_TESTS=OFF: nothing in this file is built (see its option in the root CMakeLists.txt).
if(NOT MORPH_BUILD_UNIT_TESTS)
    return()
endif()

# ── The SYSTEM-include demotion helper's own self-test ──────────────────────
# cmake/morph_demote_interface_includes.cmake moves unixODBC's `-I` out of the
# fetched Lightweight target's INTERFACE_COMPILE_OPTIONS. On Linux that flag is
# empty, so nothing observable here would change if the helper's body were
# deleted -- the self-test hands it a synthetic fixture instead and
# FATAL_ERRORs unless the transform is exactly right. Configure-time, no build,
# and it runs wherever tests are configured rather than only on the platform
# that has the symptom.
include(${CMAKE_SOURCE_DIR}/cmake/morph_demote_interface_includes.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/demote_interface_includes_selftest.cmake)

include(${CMAKE_SOURCE_DIR}/cmake/pinned_facts.cmake)
morph_generate_pinned_facts_header(
    ${CMAKE_SOURCE_DIR}/docs/spec/pinned_facts.toml
    ${CMAKE_BINARY_DIR}/generated/pinned_facts_generated.hpp
)

# ── The wide-table fixture ───────────────────────────────────────────────────
# The column-store source every table measurement runs against (docs/spec/table/table.md, "Performance"). Linked
# whole, so the registrations it carries are never dropped by a linker that finds nothing referencing them.
add_library(morph_wide_table_model STATIC table/wide_table_model.cpp)
target_link_libraries(morph_wide_table_model PUBLIC morph::morph)
target_include_directories(morph_wide_table_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
apply_warnings(morph_wide_table_model)

# The table tests' sources, one list for the native executable and the WebAssembly one below.
set(MORPH_TABLE_TEST_SOURCES
    table/test_table_source.cpp
    table/test_table_sort.cpp
    table/test_table_parse.cpp
    table/test_table_filter.cpp
    table/test_table_distinct.cpp
    table/test_table_view_change.cpp
    table/test_table_engine.cpp
    table/test_table_changes.cpp
    table/test_table_query.cpp
    table/test_table_selection.cpp
    table/test_table_columns.cpp
    table/test_table_pending.cpp
    table/test_table_reentrancy.cpp
    table/test_table_server.cpp
    table/test_table_description.cpp
    table/test_table_unloaded_cells.cpp
    table/test_table_level_cache.cpp
    table/test_table_view_cache.cpp
    table/test_table_cov_c_cells.cpp
    table/test_table_cov_c_query.cpp
    table/test_table_cov_c_filter.cpp
    table/test_table_cov_c_views.cpp
    table/test_table_cov_c_state.cpp
    table/test_table_cov_c_edits.cpp
    table/test_table_cov_c_cell_edits.cpp
    table/test_table_cov_c_layout.cpp
    table/test_table_cov_c_parse.cpp
    table/test_table_cov_c_units.cpp
    table/test_table_cov_c_selection.cpp
    table/test_table_cov_c_levels.cpp
    table/test_table_cov_c_answer.cpp
    table/test_table_cov_c_view_cache.cpp
    table/test_table_cov_c_fit.cpp
    table/test_table_cov_c_pages.cpp
    table/test_table_cov_f_window.cpp
    table/test_table_cov_f_levels.cpp
    table/test_table_cov_f_engine.cpp
    table/test_table_cov_f_edits.cpp
    table/test_table_cov_f_selection.cpp
    table/test_table_cov_f_description.cpp
    table/test_table_state.cpp
    table/test_table_measure.cpp
    table/test_wide_table.cpp
    table/bench_table.cpp
)

# ── WebAssembly: the table tests alone, run under Node ──────────────────────
# The table module builds without Qt, so its tests run in WebAssembly too and the count gates of
# the single-threaded path are checked there. Nothing else in this directory is built for that target, and Node runs
# the executable by hand, so no test is registered with ctest.
if(EMSCRIPTEN)
    add_executable(morph_table_tests ${MORPH_TABLE_TEST_SOURCES})
    target_link_libraries(morph_table_tests PRIVATE morph::morph morph_test_main
                          "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph_wide_table_model>")
    target_include_directories(morph_table_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Emscripten's default heap is a fixed 16 MiB; the 100,000 x 300 column store the baselines sort needs it to grow.
    target_link_options(morph_table_tests PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB)
    apply_warnings(morph_table_tests)
    return()
endif()

add_executable(morph_tests
    oom_injector.cpp
    test_oom_injector.cpp
    test_executor.cpp
    test_executor_scope.cpp
    test_io_loop.cpp
    test_example.cpp
    test_push_records.cpp
    test_push_effects.cpp
    test_cov_c_effects.cpp
    test_cov_c_forms_values.cpp
    test_cov_h_forms_rules.cpp
    test_cov_h_forms_values.cpp
    test_cov_h_rational.cpp
    test_cov_h_views_columns.cpp
    test_cov_c_forms_session.cpp
    test_cov_f_forms_session.cpp
    test_cov_c_observability.cpp
    test_cov_c_ui_document_mutations.cpp
    test_cov_c_ui_document_grids.cpp
    test_cov_c_ui_document_tables.cpp
    test_cov_c_ui_interpreter_inputs.cpp
    test_push_watch_table.cpp
    test_push_catalog.cpp
    test_push_channel.cpp
    test_push_watch.cpp
    test_push_cache.cpp
    test_ui_interpreter_cache.cpp
    ui_table_model_support.cpp
    test_ui_table_model_window_rows.cpp
    test_ui_table_model_table_catalog.cpp
    test_ui_table_model_model_table.cpp
    test_ui_table_model_value_rows.cpp
    test_ui_table_model_action_table.cpp
    test_ui_table_model_filter_menu.cpp
    test_ui_table_model_copy_collection.cpp
    test_ui_table_model_view_selection.cpp
    test_ui_table_model_row_changes.cpp
    test_ui_table_model_model_cells.cpp
    test_ui_table_model_cell_walk.cpp
    test_ui_table_model_cell_reads.cpp
    test_ui_table_model_cell_edits.cpp
    test_ui_table_model_cell_errors.cpp
    test_ui_grid.cpp
    test_ui_cov_grid_form_rows.cpp
    test_ui_cov_grid_keys.cpp
    test_ui_cov_expression.cpp
    test_ui_cov_h_expression.cpp
    test_ui_cov_h_theme.cpp
    test_ui_cov_h_page_layout.cpp
    test_ui_cov_h_navigator.cpp
    test_ui_cov_h_table_dialogs.cpp
    test_ui_cov_h_table_state.cpp
    test_ui_cov_h_interp_download.cpp
    test_ui_cov_h_interp_lowering.cpp
    test_ui_cov_interp_lowering.cpp
    test_ui_cov_interp_elements.cpp
    test_ui_cov_grid_unknown.cpp
    test_ui_cov_grid_paste.cpp
    test_ui_cov_grid_rows.cpp
    test_ui_cov_interp_rows.cpp
    test_ui_cov_grid_model.cpp
    test_ui_cov_grid_fixed.cpp
    test_ui_cov_interp_collections.cpp
    test_ui_cov_interp_forms.cpp
    test_ui_cov_mount_extents.cpp
    test_ui_cov_interp_fixed_grid.cpp
    test_ui_cov_d_form_grid.cpp
    test_ui_cov_d_fixed_grid.cpp
    test_ui_cov_d_custom_screen_checks.cpp
    test_ui_cov_d_custom_screen_words.cpp
    test_ui_cov_d_shell_parts.cpp
    test_ui_cov_d_chat_tools.cpp
    test_ui_cov_d_workspace.cpp
    test_ui_cov_d_rows_bound.cpp
    test_ui_cov_g_action_table_view.cpp
    test_ui_cov_g_document_loader.cpp
    test_ui_cov_g_table_points.cpp
    test_ui_cov_g_menu_walk.cpp
    test_ui_cov_g_rows_decorators.cpp
    test_ui_cov_g_model_grid.cpp
    test_ui_cov_g_element_values.cpp
    test_ui_cov_g_model_table_binds.cpp
    test_ui_cov_g_host_commands.cpp
    test_ui_cov_g_typed_table.cpp
    test_ui_cov_g_filter_menu.cpp
    test_ui_cov_g_window_fetch.cpp
    test_ui_cov_g_model_grid_values.cpp
    test_ui_cov_g_grid_controller.cpp
    test_ui_cov_g_form_grid_rows.cpp
    test_ui_cov_g_fixed_grid_source.cpp
    test_ui_cov_d_rows_state.cpp
    test_ui_cov_d_chart.cpp
    test_ui_cov_d_grid_groups.cpp
    test_ui_cov_d_host.cpp
    test_ui_cov_d_shell_frame.cpp
    test_ui_cov_d_grid_bound.cpp
    forms/test_forms_derive.cpp
    test_ui_table_state.cpp
    test_push_agent.cpp
    test_mcp_transport.cpp
    test_chat_settle.cpp
    test_chat_origin.cpp
    test_chat_service_seams.cpp
    test_chat_protocol.cpp
    test_chat_screen_tools.cpp
    test_chat_service.cpp
    test_chat_transcription.cpp
    test_chat_dictation_audio.cpp
    test_chat_propose_screen.cpp
    test_chat_config.cpp
    test_chat_conversations.cpp
    test_chat_context.cpp
    test_chat_client.cpp
    test_chat_panel.cpp
    test_push_documents.cpp
    test_push_refresh_keys.cpp
    push_records_late.cpp
    test_executor_extra.cpp
    test_strand.cpp
    test_completion.cpp
    test_completion_extra.cpp
    test_completion_multi_handler.cpp
    test_completion_promise.cpp
    test_completion_value_contract.cpp
    test_completion_owner.cpp
    test_storage_owner.cpp
    test_coroutine_client.cpp
    test_coroutine_model.cpp
    test_async_delay.cpp
    test_model.cpp
    test_post_commit_tail.cpp
    test_logger.cpp
    test_observability.cpp
    test_profiler.cpp
    test_backend_extra.cpp
    test_backend_registration_surface.cpp
    test_registry_extra.cpp
    test_registry_duplicates.cpp
    test_registration_phase.cpp
    test_registry_schema_forgery.cpp
    test_registration_qualified_types.cpp
    test_registration_same_line.cpp
    test_principal.cpp
    test_bridge_local.cpp
    test_bridge_remote.cpp
    test_bridge_pending_calls.cpp
    test_bridge_execute_json.cpp
    test_bridge_raw.cpp
    raw_dispatch_models.cpp
    test_bridge_pending_calls.cpp
    test_remote_extra.cpp
    test_remote_server_strand.cpp
    test_remote_connection_scope.cpp
    test_remote_cancel.cpp
    test_remote_step_interleaving.cpp
    test_remote_execute_ordering.cpp
    test_action_validation.cpp
    test_action_guard.cpp
    test_action_wire_rejection.cpp
    test_security_fixes.cpp
    test_bridge_lifetime.cpp
    test_bridge_bind_paths.cpp
    test_bridge_tracking.cpp
    test_bridge_owner.cpp
    test_core_branch_gaps.cpp
    test_client_execute_deadline.cpp
    test_timeout_scheduler.cpp
    test_dispatch_di.cpp
    test_handler_binding.cpp
    test_switch_backend.cpp
    test_async_registration.cpp
    test_network_monitor.cpp
    test_forms_boolean_anyof_wire.cpp
    test_forms_dom_access.cpp
    test_forms_exact_bounds.cpp
    test_offline_queue.cpp
    test_file_offline_queue.cpp
    test_replay_ledger.cpp
    test_sync_worker.cpp
    test_reconnect_coordinator.cpp
    test_offline_integration.cpp
    test_conflict_resolution.cpp
    test_bridge_fixes.cpp
    test_completion_branches.cpp
    test_issues.cpp
    test_concurrency_invariants.cpp
    test_subscription.cpp
    test_coverage_gaps.cpp
    test_coverage_extra.cpp
    test_coverage_push95.cpp
    test_server_limits.cpp
    test_shared_instances.cpp
    test_strong_id_keys.cpp
    test_wire_encode_fault.cpp
    test_wire_hardening.cpp
    test_wire_omitted_fields.cpp
    test_remote_reply_envelopes.cpp
    test_reply_router.cpp
    test_wire_schemas.cpp
    test_protocol_version.cpp
    test_limit_policy.cpp
    test_action_log.cpp
    test_action_log_phase2.cpp
    test_journal_format_versioning.cpp
    test_journal_payload_evolution.cpp
    test_outbox.cpp
    test_rational_checked.cpp
    test_rational_wire_policy.cpp
    test_rational.cpp
    test_ledger_rational_fuzz.cpp
    test_quantity.cpp
    test_tagged.cpp
    test_quantity_forms.cpp
    test_quantity_decode_validation.cpp
    test_nested_forms.cpp
    test_flows_apps.cpp
    test_sections.cpp
    test_views.cpp
    test_computed_fields.cpp
    test_forms_rules.cpp
    test_forms_rule_agreement.cpp
    test_forms_rule_corpus.cpp
    test_forms_layout.cpp
    test_forms_meta_members.cpp
    test_forms_field_bounds.cpp
    test_forms_display_unit.cpp
    test_forms_meta_options.cpp
    test_forms_blank_as.cpp
    test_forms_since.cpp
    test_forms_instance_constraints.cpp
    test_widget_hints.cpp
    test_datetime.cpp
    test_forms_i18n.cpp
    test_render_i18n.cpp
    test_render_locale_format.cpp
    test_render_parse_decimal.cpp
    test_forms_conformance_corpus.cpp
    test_forms_engine_text.cpp
    test_forms_engine_rules.cpp
    test_forms_engine_model.cpp
    test_forms_engine_values.cpp
    test_forms_engine_session.cpp
    test_forms_engine_nested.cpp
    test_forms_engine_choice.cpp
    test_forms_engine_handler_submitter.cpp
    test_forms_engine_typed.cpp
    test_forms_engine_corpora.cpp
    test_forms_engine_edges.cpp
    test_forms_engine_view.cpp
    test_forms_engine_overlay.cpp
    test_forms_engine_collection.cpp
    test_forms_engine_wizard.cpp
    test_forms_engine_collection_view.cpp
    test_forms_engine_wizard_view.cpp
    test_session_auth.cpp
    test_policy_hardening.cpp
    test_register_authorization.cpp
    test_register_session.cpp
    test_control_byte_escaping.cpp
    test_opaque_model_ids.cpp
    test_graceful_shutdown.cpp
    test_pinned_facts.cpp
    test_version.cpp
    test_callback_scope.cpp
    test_reactive_signal.cpp
    test_reactive_computed.cpp
    test_reactive_misuse.cpp
    test_reactive_scope.cpp
    test_reactive_store.cpp
    test_reactive_scheduler.cpp
    test_reactive_query.cpp
    test_reactive_mutation.cpp
    test_reactive_control_bridge.cpp
    test_ui_view.cpp
    test_ui_recording_backend.cpp
    test_ui_mount.cpp
    test_ui_controlled.cpp
    test_ui_structure.cpp
    test_ui_foreach.cpp
    test_ui_table.cpp
    test_ui_cell_format.cpp
    test_ui_cov_table_cell_format.cpp
    test_ui_cov_table_config.cpp
    test_ui_cov_table_filter_menu.cpp
    test_ui_cov_table_state.cpp
    test_ui_cov_table_engine_rows.cpp
    test_ui_cov_table_window_rows.cpp
    test_ui_cov_table_views_menu.cpp
    test_ui_cov_table_filter_dialog.cpp
    test_ui_cov_table_model_columns.cpp
    test_ui_cov_table_rows_pure.cpp
    test_ui_cov_table_window_state.cpp
    test_ui_cov_table_runs.cpp
    test_ui_cov_table_mount.cpp
    test_ui_cov_table_state_model.cpp
    test_ui_cov_h_table_state_model.cpp
    test_ui_cov_i_views.cpp
    test_ui_cov_i_cell_menu.cpp
    test_ui_cov_i_rows_table.cpp
    test_ui_cov_i_collection.cpp
    test_ui_cov_i_rows_columns.cpp
    test_ui_cov_i_stored_state.cpp
    test_ui_cov_i_groups.cpp
    test_ui_cov_i_table_gestures.cpp
    test_ui_cov_i_views_menu.cpp
    test_ui_cov_i_sort_dialog.cpp
    test_ui_table_config.cpp
    bench_interpreter.cpp
    bench_mount.cpp
    test_ui_frontend.cpp
    test_ui_conformance.cpp
    test_ui_cov_e_conformance_faults.cpp
    test_ui_cov_e_recording_backend.cpp
    test_ui_cov_e_interpreter.cpp
    test_ui_cov_f_interpreter.cpp
    test_ui_spotlight_recording.cpp
    test_ui_common.cpp
    test_ui_custom.cpp
    test_ui_shell_frame.cpp
    test_ui_shell_tours.cpp
    test_ui_shell_parts.cpp
    test_ui_explorer.cpp
    test_ui_workspace.cpp
    test_ui_workspace_start.cpp
    test_ui_field.cpp
    test_ui_kinds.cpp
    test_ui_step_detail.cpp
    test_ui_chart_bars.cpp
    test_ui_table_row_span.cpp
    test_ui_value.cpp
    test_ui_include_order.cpp
    test_ui_messages.cpp
    test_ui_expression.cpp
    test_ui_date_pattern.cpp
    test_ui_document.cpp
    test_ui_interpreter_control.cpp
    test_ui_interpreter_relations.cpp
    test_ui_interpreter_calculation.cpp
    test_ui_interpreter_models.cpp
    test_ui_interpreter_permissions.cpp
    test_ui_interpreter_forms.cpp
    test_ui_bound_decimals.cpp
    test_ui_interpreter_commands.cpp
    test_ui_document_page.cpp
    test_ui_interpreter_table.cpp
    test_ui_interpreter_chart.cpp
    test_ui_chart_scale.cpp
    test_ui_stretch_columns.cpp
    test_ui_document_rules.cpp
    test_ui_interpreter_gallery.cpp
    test_ui_interpreter_edges.cpp
    test_ui_interpreter_reports.cpp
    test_ui_expression_edges.cpp
    test_ui_theme_values.cpp
    test_ui_author.cpp
    test_ui_author_forms.cpp
    test_ui_width_class.cpp
    test_cancellation_policy.cpp
    test_file_io_ops.cpp
    test_extension_bag_spike.cpp
    test_remote_envelope_service.cpp
    test_ui_bundle.cpp
    test_ui_service.cpp
    test_ui_queueing_dispatcher.cpp
    test_ui_service_client.cpp
    test_ui_app_root.cpp
    test_ui_backend_switch.cpp
    test_ui_theme.cpp
    test_ui_evaluate.cpp
    test_ui_file_store.cpp
    test_ui_file.cpp
    test_ui_route.cpp
    test_ui_host.cpp
    test_ui_dev_reload.cpp
    test_ui_inspector.cpp
    test_ui_interpreter_detail.cpp
    test_async_ask_defaults.cpp
    test_ui_route_history.cpp
    test_ui_preferences.cpp
    test_ui_preference_sync.cpp
    test_ui_custom_screen_rules.cpp
    test_ui_custom_screen_checks.cpp
    test_ui_custom_screens.cpp
    test_ui_json_source.cpp
    test_ui_json_patch.cpp
    test_ui_host_drafts.cpp
    test_ui_workspace_in_place.cpp
    test_ui_navigator_custom.cpp
    test_ui_custom_screen_sync.cpp
    test_ui_custom_screen_chrome.cpp
    test_ui_custom_screen_controller.cpp
    test_ui_custom_screen_versions.cpp
    test_ui_host_vocabulary.cpp
    test_ui_tours_document.cpp
    test_ui_tours_registry.cpp
    test_ui_tours_controller.cpp
    test_ui_tour_card.cpp
    test_ui_tour_registry.cpp
    test_ui_tours_runtime.cpp
    test_ui_tour_sync.cpp
    test_ui_tours_edges.cpp
    test_ui_spotlight_edges.cpp
    test_ui_catalogue.cpp
    test_ui_texts.cpp
    test_ui_locale_table.cpp
    test_action_refusal.cpp
)

target_link_libraries(morph_tests
    PRIVATE
        morph::morph
        morph_test_main
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph_wide_table_model>"
)

apply_warnings(morph_tests)

# Several individual test files (schema/rule template instantiation over many
# action/form types, e.g. test_quantity_forms.cpp) have independently pushed
# their .obj's COFF section count past the 32-bit SN_LOFF format's limit
# under MSVC Debug (no /Og folding, full /Zi debug info), aborting with C1128
# ("number of sections exceeded object file format limit") -- three separate
# unrelated branches hit this on that one file alone, each adding only a
# little more template-instantiation weight. Rather than chase this file by
# file as it recurs, /bigobj is applied to the whole target: harmless on
# Release and on other compilers, and it costs nothing on the files that
# don't need it.
target_compile_options(morph_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

# Forwards CMakeLists.txt's project(VERSION ...) into tests/test_version.cpp
# so it can static_assert the checked-in include/morph/version.hpp constants
# never drift from the build system's declared version.
target_compile_definitions(morph_tests PRIVATE
    MORPH_CMAKE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MORPH_CMAKE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MORPH_CMAKE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# The shared forms corpora. Each is one file with several readers -- this
# suite (the compiled evaluators and the forms engine) and the QML suite --
# all pointed at it from CMake so none can quietly grow its own copy.
target_compile_definitions(morph_tests PRIVATE
    MORPH_FORMS_RULE_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/rule_corpus.json"
    MORPH_FORMS_INSTANCE_BOUNDS="${CMAKE_CURRENT_SOURCE_DIR}/data/instance_bounds.json"
    MORPH_FORMS_BODY_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/data/body_corpus.json")

# The UI document corpora: JSON files, so another implementation of the
# document and expr/1 can run the same cases.
target_compile_definitions(morph_tests PRIVATE
    MORPH_UI_MESSAGES_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/ui/corpus/messages.json"
    MORPH_UI_EXPRESSIONS_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/ui/corpus/expressions.json"
    MORPH_UI_DOCUMENTS_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/ui/corpus/documents.json")

# The shipped surfaces of the Qt Quick theme, which the contrast check reads.
target_compile_definitions(morph_tests PRIVATE
    MORPH_PAPER_THEME_QML="${CMAKE_SOURCE_DIR}/src/qt_quick/qml/Theme.qml")

# The authoring goldens: builder output checked against JSON files.
target_compile_definitions(morph_tests PRIVATE MORPH_UI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui/golden")

target_include_directories(morph_tests PRIVATE ${CMAKE_BINARY_DIR}/generated)

# MORPH_REQUIRE_VETTED_HMAC is a *deployment* switch aimed at application call
# sites; it rides in on morph::morph's INTERFACE compile definitions. morph's
# own suite deliberately exercises the reference hmacSha256 through its default
# argument (test_session_auth.cpp, test_security_fixes.cpp,
# test_policy_hardening.cpp, test_register_authorization.cpp), so the inherited
# definition is undefined again for this target only. Without this, configuring
# the documented `-DMORPH_REQUIRE_VETTED_HMAC=ON` would fail to build unless
# tests were also switched off — which would leave the option itself untested.
# The guard's own correctness is proven independently by the three try_compile
# probes below, which set/omit the macro per probe regardless of the option.
#
# COMPILE_OPTIONS rather than COMPILE_DEFINITIONS: CMake's compile rule expands
# <DEFINES> before <FLAGS>, so a `-U` in the flags reliably wins over the `-D`
# inherited from the interface target.
if(MORPH_REQUIRE_VETTED_HMAC)
    target_compile_options(morph_tests PRIVATE
        "$<IF:$<CXX_COMPILER_ID:MSVC>,/UMORPH_REQUIRE_VETTED_HMAC,-UMORPH_REQUIRE_VETTED_HMAC>")
endif()

# /w14062 (enabled project-wide) only fires when an enum switch has no
# `default`; test_pinned_facts.cpp's exhaustive enum-cardinality switches keep
# a `default:` (so GCC/Clang's -Wswitch-default does not fire), which would
# otherwise exempt them from MSVC's check. /w14061 fires even with a default
# present. Scoped to this one file rather than project-wide -- see the plan's
# Global Constraints for why.
set_source_files_properties(test_pinned_facts.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/w14061>"
)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_tests)
endif()

include(Catch)
# Every test is sub-second in practice; cap each at 120s so a hung/deadlocked
# test fails fast instead of stalling the whole CI job (observed: a single test
# hanging blocked a Windows runner for over half an hour).
#
# The one case that needed more, the strand race test tagged `[slow]`, went
# with morph's own strand when morph moved to core-cpp's `KeyedStrands`, which
# carries its own race tests.
catch_discover_tests(morph_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

# ── Two-binary journal-path skew test ───────────────────────────────────────
# The executable form of the journal's data-at-rest contract: an
# old build records a journal, a new build replays it, and a shape that changed
# in between must not decode to a plausible default in silence.
#
# tests/test_journal_payload_evolution.cpp already simulates an older build
# inside one process, via a struct it never registers. That proves the gate
# fires; it cannot prove anything about two separately compiled programs, since
# there is only ever one compilation. compile_checks/journal_skew_probe.cpp is
# compiled twice, into two executables that share no translation unit, and the
# "new" one asserts against a journal file the "old" one actually wrote.
#
# NOTE (deviation from the issue's suggestion): two real targets plus ordered
# ctest cases, not the try_compile()/try_run() pattern the other
# MORPH_CLIENT_ONLY gates use. Those gates need try_compile because they assert
# a *link failure*, and because they must define a macro the main build does
# not. Neither applies here: two ordinary executables differing only in a
# -D is exactly what add_executable()/target_compile_definitions() are for, and
# building them at build time lets ninja parallelise them instead of adding two
# serial multi-minute compiles to every configure.
#
# NOTE (scope): neither binary is built with MORPH_CLIENT_ONLY. That macro
# suppresses the model-owning registrars, so a client-only binary cannot
# execute a model and therefore cannot journal anything. It is the right gate
# for the *wire*-path skew test, which needs a per-action fingerprint exchanged
# at `hello` before there is anything to assert against, and no such exchange
# exists. This covers the journal path, which ships today.
set(MORPH_JOURNAL_SKEW_DIR "${CMAKE_CURRENT_BINARY_DIR}/journal_skew")
file(MAKE_DIRECTORY "${MORPH_JOURNAL_SKEW_DIR}")

foreach(_skew_role old new)
    string(TOUPPER "${_skew_role}" _skew_role_upper)
    add_executable(morph_journal_skew_${_skew_role} compile_checks/journal_skew_probe.cpp)
    target_compile_definitions(morph_journal_skew_${_skew_role}
        PRIVATE MORPH_SKEW_ROLE_${_skew_role_upper})
    target_link_libraries(morph_journal_skew_${_skew_role} PRIVATE morph::morph)
    morph_suppress_test_dialogs(morph_journal_skew_${_skew_role})
    apply_warnings(morph_journal_skew_${_skew_role})
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(morph_journal_skew_${_skew_role} ${AF_SANITIZER})
    endif()
endforeach()
unset(_skew_role)
unset(_skew_role_upper)

# FIXTURES_SETUP/FIXTURES_REQUIRED rather than a bare ordering: ctest then
# refuses to run the replay case at all if the writer failed, instead of
# reporting a second, confusing failure about a journal file that was never
# produced. Both are also serialised against each other by the fixture, which
# matters under `ctest -j`: they share one journal file.
add_test(NAME journal_skew_old_build_writes
    COMMAND morph_journal_skew_old "${MORPH_JOURNAL_SKEW_DIR}")
set_tests_properties(journal_skew_old_build_writes PROPERTIES
    FIXTURES_SETUP morph_journal_skew_fixture
    TIMEOUT 120)

add_test(NAME journal_skew_new_build_replays
    COMMAND morph_journal_skew_new "${MORPH_JOURNAL_SKEW_DIR}")
set_tests_properties(journal_skew_new_build_replays PROPERTIES
    FIXTURES_REQUIRED morph_journal_skew_fixture
    TIMEOUT 120)

# ── forms: schema generation must not be route-count sensitive ──────────────
# compile_checks/forms_dag_probe.cpp is compiled
# twice — once as a type graph with one route to each node, once as a DAG with
# 6,561 routes through the same number of types at the same depth — and
# compile_checks/forms_dag_budget.cmake asserts the second costs no more than
# MAX_RATIO_PERCENT of the first. Both files' headers say why it is a ratio and
# what was measured; the short version is that this fixture took 26.8 CPU-s at
# master @ a9cb5649 and 3.0 with the fix, against a 2.7-second control that the
# fix leaves alone.
#
# A ctest case rather than a configure-time try_compile: it costs ~12 seconds of
# compiling, which does not belong in every configure, and unlike the
# MORPH_CLIENT_ONLY gates it asserts nothing about whether a program builds —
# only about how long it takes, which is a thing to measure when someone asks
# for test results, not when they ask for a build system.
#
# Gated on a GCC/Clang driver: the script passes `-I`, `-std=` and
# `-fsyntax-only` literally, which clang-cl and MSVC spell differently. The
# pathology is compiler-independent (it is an instantiation count), so one
# front end proving it is enough; nothing here is skipped silently, because a
# skipped case reports as "Not Run" rather than as a pass.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # Genex-free include list: this runs under `cmake -P`, where nothing expands
    # a $<BUILD_INTERFACE:...>. morph's own include directory is named directly
    # rather than unwrapped from the target, so a change to how that target is
    # declared cannot quietly leave this pointing at an installed copy.
    set(MORPH_FORMS_DAG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
    get_target_property(_morph_forms_dag_glaze_includes glaze::glaze INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_dir IN LISTS _morph_forms_dag_glaze_includes)
        string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _dir "${_dir}")
        if(NOT _dir MATCHES "\\$<")
            list(APPEND MORPH_FORMS_DAG_INCLUDE_DIRS "${_dir}")
        endif()
    endforeach()
    unset(_dir)
    unset(_morph_forms_dag_glaze_includes)
    # Pipe-joined: a `;` inside one add_test() argument is an argument separator.
    string(REPLACE ";" "|" MORPH_FORMS_DAG_INCLUDE_DIRS "${MORPH_FORMS_DAG_INCLUDE_DIRS}")

    add_test(NAME forms_schema_generation_is_not_route_count_sensitive
        COMMAND ${CMAKE_COMMAND}
            "-DMORPH_DAG_CXX=${CMAKE_CXX_COMPILER}"
            "-DMORPH_DAG_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/forms_dag_probe.cpp"
            "-DMORPH_DAG_INCLUDES=${MORPH_FORMS_DAG_INCLUDE_DIRS}"
            "-DMORPH_DAG_FLAGS=-std=c++23|-fsyntax-only"
            "-DMORPH_DAG_MAX_RATIO_PERCENT=300"
            "-DMORPH_DAG_RUNS=2"
            "-DMORPH_DAG_MIN_CONTROL_MS=250"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/forms_dag_budget.cmake")
    # Generous enough that a regression reports the ratio it measured rather
    # than dying as a timeout: the unfixed fixture took ~27 s per run here, and
    # a slow shared runner can be several times that.
    set_tests_properties(forms_schema_generation_is_not_route_count_sensitive PROPERTIES
        TIMEOUT 900
        RUN_SERIAL TRUE)
endif()

# ── MORPH_REQUIRE_VETTED_HMAC guard check ────────────────────────────────────
# Configure-time proof that the default-argument-removal mechanism in
# session_auth.hpp (TokenIssuer/TokenVerifier/SigningAuthorizer) actually
# gates on MORPH_REQUIRE_VETTED_HMAC, independent of whatever value the
# top-level MORPH_REQUIRE_VETTED_HMAC option currently has — each try_compile
# defines (or omits) the macro just for that one probe, via COMPILE_DEFINITIONS.
# See docs/spec/security.md, "Recommended production wiring".
#
# unset(... CACHE) first: try_compile()'s result variable is stored as a cache
# entry, so without clearing it a stale PASS/FAIL from an earlier configure
# would survive unchanged even if session_auth.hpp regresses.
#
# NOTE (deviation from the plan): try_compile()'s LINK_LIBRARIES only accepts
# system libraries and *Imported* targets from the calling project (see the
# try_compile() docs) — `morph` is a plain (non-imported) library
# defined in this same project, so `LINK_LIBRARIES morph::morph` fails with
# "the target was not found" (confirmed experimentally). Since the probe only
# includes headers from morph and glaze, we instead read their
# INTERFACE_INCLUDE_DIRECTORIES via get_target_property() — which works for
# any target, imported or not, because it queries the calling project's own
# configure state rather than the try_compile scratch project — and forward
# them as an old-style `-DINCLUDE_DIRECTORIES=...` CMAKE_FLAGS entry (the
# generated scratch project turns that into `include_directories(...)`, per
# the try_compile() docs). Semicolons inside the combined list must be
# escaped so the outer CMAKE_FLAGS list isn't split apart.
get_target_property(MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS morph INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(MORPH_VETTED_HMAC_GUARD_GLAZE_INCLUDE_DIRS glaze::glaze INTERFACE_INCLUDE_DIRECTORIES)
list(APPEND MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS ${MORPH_VETTED_HMAC_GUARD_GLAZE_INCLUDE_DIRS})
# core-cpp's headers. An in-tree core-cpp's are spelled out rather than read
# from core::base: its include directories are $<BUILD_INTERFACE:...>
# generator expressions, which a try_compile() here would receive unevaluated.
# `src/` holds the headers and the binary directory's `include/` the generated
# <core/Config.hpp>. An installed core-cpp's imported target carries plain
# paths, which can be read directly.
if(DEFINED core-cpp_SOURCE_DIR)
    list(APPEND MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS "${core-cpp_SOURCE_DIR}/src" "${core-cpp_BINARY_DIR}/include")
else()
    get_target_property(_morph_core_cpp_include_dirs core::base INTERFACE_INCLUDE_DIRECTORIES)
    list(APPEND MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS ${_morph_core_cpp_include_dirs})
    unset(_morph_core_cpp_include_dirs)
endif()
string(REPLACE ";" "\\;" MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS "${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}")

unset(MORPH_VETTED_HMAC_GUARD_BLOCKS_DEFAULT CACHE)
try_compile(MORPH_VETTED_HMAC_GUARD_BLOCKS_DEFAULT
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/vetted_hmac_guard_uses_default.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_REQUIRE_VETTED_HMAC"
)
if(MORPH_VETTED_HMAC_GUARD_BLOCKS_DEFAULT)
    message(FATAL_ERROR
        "MORPH_REQUIRE_VETTED_HMAC guard check failed: "
        "compile_checks/vetted_hmac_guard_uses_default.cpp compiled even with "
        "MORPH_REQUIRE_VETTED_HMAC defined (expected a compile failure — the "
        "default-argument-removal #ifdef in session_auth.hpp is broken).")
endif()

unset(MORPH_VETTED_HMAC_GUARD_ALLOWS_EXPLICIT CACHE)
try_compile(MORPH_VETTED_HMAC_GUARD_ALLOWS_EXPLICIT
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/vetted_hmac_guard_explicit_mac.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_REQUIRE_VETTED_HMAC"
)
if(NOT MORPH_VETTED_HMAC_GUARD_ALLOWS_EXPLICIT)
    message(FATAL_ERROR
        "MORPH_REQUIRE_VETTED_HMAC guard check failed: "
        "compile_checks/vetted_hmac_guard_explicit_mac.cpp failed to compile "
        "with MORPH_REQUIRE_VETTED_HMAC defined, even though it passes an "
        "explicit MacFunction (the guard must not block explicit injection).")
endif()

unset(MORPH_VETTED_HMAC_GUARD_DEFAULT_WORKS_UNGATED CACHE)
try_compile(MORPH_VETTED_HMAC_GUARD_DEFAULT_WORKS_UNGATED
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/vetted_hmac_guard_uses_default.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
)
if(NOT MORPH_VETTED_HMAC_GUARD_DEFAULT_WORKS_UNGATED)
    message(FATAL_ERROR
        "MORPH_REQUIRE_VETTED_HMAC guard check failed: "
        "compile_checks/vetted_hmac_guard_uses_default.cpp failed to compile "
        "without MORPH_REQUIRE_VETTED_HMAC defined (regression: the default "
        "hmacSha256 argument must keep working when the option is off).")
endif()

# ── Action guard checks ──────────────────────────────────────────────────────
# Proof that an action whose guard type is only declared where an execution site is instantiated, or whose guard has
# no callable form, does not compile, and that the same action with a defined guard does -- see
# docs/spec/core/action_guards.md. The probes are compiled, not linked.
set(_morph_guard_try_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

unset(MORPH_ACTION_GUARD_INCOMPLETE_COMPILES CACHE)
try_compile(MORPH_ACTION_GUARD_INCOMPLETE_COMPILES
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/action_guard_incomplete.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
)
if(MORPH_ACTION_GUARD_INCOMPLETE_COMPILES)
    message(FATAL_ERROR
        "action guard check failed: compile_checks/action_guard_incomplete.cpp compiled although its guard type is "
        "never defined (expected a compile failure -- a site compiled without the guard's definition would leave the "
        "action unguarded).")
endif()

unset(MORPH_ACTION_GUARD_NO_FORM_COMPILES CACHE)
try_compile(MORPH_ACTION_GUARD_NO_FORM_COMPILES
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/action_guard_no_form.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
)
if(MORPH_ACTION_GUARD_NO_FORM_COMPILES)
    message(FATAL_ERROR
        "action guard check failed: compile_checks/action_guard_no_form.cpp compiled although its guard has no "
        "callable check for the action (expected a compile failure -- the action would run unchecked).")
endif()

unset(MORPH_ACTION_GUARD_DEFINED_COMPILES CACHE)
try_compile(MORPH_ACTION_GUARD_DEFINED_COMPILES
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/action_guard_defined.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    OUTPUT_VARIABLE MORPH_ACTION_GUARD_DEFINED_OUTPUT
)
if(NOT MORPH_ACTION_GUARD_DEFINED_COMPILES)
    message(FATAL_ERROR
        "action guard check failed: compile_checks/action_guard_defined.cpp failed to compile although its guard is "
        "defined with a callable check (the probes above would pass for any broken include). Compiler output:\n"
        "${MORPH_ACTION_GUARD_DEFINED_OUTPUT}")
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_morph_guard_try_target_type}")
unset(_morph_guard_try_target_type)

# ── MORPH_ENABLE_OBSERVABILITY=OFF checks ────────────────────────────────────
# Proof that a build with observability compiled out refuses to install a
# sink, and says why. The probes name the macro themselves (what the option
# defines on the morph target), so this holds whichever way the option is set.
#   - disabled_set_sink with the macro, for each of the three ways to install
#     a sink: must NOT compile, and the compiler output must carry the
#     refusal's message (any other error would also "fail to compile", which
#     proves nothing about the refusal).
#   - the same file and probe without the macro: must compile (control: the
#     file is valid and its includes resolve).
#   - disabled_no_sink with and without the macro: must compile (control: the
#     header still offers everything but the setters when compiled out).
set(_morph_obs_try_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

foreach(_morph_obs_probe IN ITEMS PROBE_METRIC_SINK PROBE_TRACE_SINK PROBE_SCOPED_OVERRIDE)
    unset(MORPH_OBSERVABILITY_OFF_ACCEPTS_SINK CACHE)
    try_compile(MORPH_OBSERVABILITY_OFF_ACCEPTS_SINK
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/observability_disabled_set_sink.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
        CXX_STANDARD 23
        COMPILE_DEFINITIONS "-DMORPH_OBSERVABILITY_DISABLED" "-D${_morph_obs_probe}"
        OUTPUT_VARIABLE MORPH_OBSERVABILITY_OFF_SINK_OUTPUT
    )
    if(MORPH_OBSERVABILITY_OFF_ACCEPTS_SINK)
        message(FATAL_ERROR
            "observability check failed: compile_checks/observability_disabled_set_sink.cpp compiled with "
            "MORPH_OBSERVABILITY_DISABLED and ${_morph_obs_probe} defined (expected the sink to be refused).")
    endif()
    string(FIND "${MORPH_OBSERVABILITY_OFF_SINK_OUTPUT}"
        "observability is compiled out (MORPH_ENABLE_OBSERVABILITY=OFF)" _morph_obs_message_at)
    if(_morph_obs_message_at EQUAL -1)
        message(FATAL_ERROR
            "observability check failed: compile_checks/observability_disabled_set_sink.cpp failed to compile with "
            "MORPH_OBSERVABILITY_DISABLED and ${_morph_obs_probe} defined, but not with the refusal's message. "
            "Compiler output:\n${MORPH_OBSERVABILITY_OFF_SINK_OUTPUT}")
    endif()
    unset(_morph_obs_message_at)

    unset(MORPH_OBSERVABILITY_ON_ACCEPTS_SINK CACHE)
    try_compile(MORPH_OBSERVABILITY_ON_ACCEPTS_SINK
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/observability_disabled_set_sink.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
        CXX_STANDARD 23
        COMPILE_DEFINITIONS "-D${_morph_obs_probe}"
        OUTPUT_VARIABLE MORPH_OBSERVABILITY_ON_SINK_OUTPUT
    )
    if(NOT MORPH_OBSERVABILITY_ON_ACCEPTS_SINK)
        message(FATAL_ERROR
            "observability check failed: compile_checks/observability_disabled_set_sink.cpp failed to compile with "
            "${_morph_obs_probe} defined and MORPH_OBSERVABILITY_DISABLED not (the refusal check above would pass "
            "for any broken include). Compiler output:\n${MORPH_OBSERVABILITY_ON_SINK_OUTPUT}")
    endif()
endforeach()
unset(_morph_obs_probe)

# The second value names an unrelated macro, so both runs have the same shape.
foreach(_morph_obs_define IN ITEMS "-DMORPH_OBSERVABILITY_DISABLED" "-DMORPH_OBSERVABILITY_UNDEFINED_CONTROL")
    unset(MORPH_OBSERVABILITY_NO_SINK_COMPILES CACHE)
    try_compile(MORPH_OBSERVABILITY_NO_SINK_COMPILES
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/observability_disabled_no_sink.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
        CXX_STANDARD 23
        COMPILE_DEFINITIONS "${_morph_obs_define}"
        OUTPUT_VARIABLE MORPH_OBSERVABILITY_NO_SINK_OUTPUT
    )
    if(NOT MORPH_OBSERVABILITY_NO_SINK_COMPILES)
        message(FATAL_ERROR
            "observability check failed: compile_checks/observability_disabled_no_sink.cpp failed to compile with "
            "${_morph_obs_define}. Compiler output:\n${MORPH_OBSERVABILITY_NO_SINK_OUTPUT}")
    endif()
endforeach()
unset(_morph_obs_define)

set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_morph_obs_try_target_type}")
unset(_morph_obs_try_target_type)

# ── MORPH_CLIENT_ONLY guard check ────────────────────────────────────────────
# Proof that MORPH_CLIENT_ONLY actually suppresses the two model-owning
# registrars (registerModelOnce, registerActionOnce) -- see
# docs/spec/core/registry.md, "MORPH_CLIENT_ONLY".
#
# compile_checks/client_only_no_model_link.cpp declares (never defines)
# ClientOnlyModel's constructor and execute(). Two probes, in opposite
# directions, prove the guard does something real rather than the probe being
# vacuously satisfied either way:
#   - WITH MORPH_CLIENT_ONLY defined: must LINK (the model-owning registrars
#     are macroed away, so nothing in the program ever references either
#     undefined symbol).
#   - WITHOUT it (today's default): must FAIL TO LINK, and must fail on
#     ClientOnlyModel's symbols (the registrars' lambda bodies do reference
#     them, and the linker cannot resolve either).
#
# The probes that must link, and the two that must also run, are ordinary
# build-time targets rather than try_compile()/try_run(): morph's headers use
# core-cpp's compiled modules (core::net's event loop, through
# TimeoutScheduler), and a try_compile() scratch project can link only
# imported targets -- core-cpp's are built by this project, after configure.
# A probe that fails to link now fails the build rather than the configure;
# the run probes are ctest cases. The probes link morph's dependencies
# directly rather than morph::morph, whose interface carries MORPH_CLIENT_ONLY
# when that option is on, so each probe decides the macro for itself.
add_library(morph_guard_probe_deps INTERFACE)
target_include_directories(morph_guard_probe_deps INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(morph_guard_probe_deps INTERFACE
    glaze::glaze core::base core::async core::net core::platform
    $<TARGET_NAME_IF_EXISTS:Threads::Threads>)

# The process-wide state the headers reach through a function is defined in
# src/ui/process_state.cpp. A probe that does not link morph::morph links its
# own static copy, built without any consumer macro.
add_library(morph_guard_probe_state STATIC ${PROJECT_SOURCE_DIR}/src/ui/process_state.cpp)
target_include_directories(morph_guard_probe_state PUBLIC
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
target_compile_definitions(morph_guard_probe_state PUBLIC MORPH_STATIC_DEFINE)
target_compile_features(morph_guard_probe_state PUBLIC cxx_std_23)
target_link_libraries(morph_guard_probe_state PUBLIC
    glaze::glaze core::base core::async core::net core::platform
    $<TARGET_NAME_IF_EXISTS:Threads::Threads>)
target_link_libraries(morph_guard_probe_deps INTERFACE morph_guard_probe_state)

add_executable(morph_client_only_guard_links compile_checks/client_only_no_model_link.cpp)
target_compile_definitions(morph_client_only_guard_links PRIVATE MORPH_CLIENT_ONLY)
target_link_libraries(morph_client_only_guard_links PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_client_only_guard_links)
add_test(NAME morph_client_only_guard_links COMMAND morph_client_only_guard_links)
set_tests_properties(morph_client_only_guard_links PROPERTIES TIMEOUT 60)

# Two model types registered under one id, in two translation units: static initialisation must stop with the
# duplicate-id message. Judged by that message, not by the exit status, so a run that died of something else, or
# reached main, does not pass; a script judges it, since ctest fails a run killed by a signal whatever it printed.
add_executable(morph_duplicate_model_id compile_checks/duplicate_model_id_first.cpp
                                        compile_checks/duplicate_model_id_second.cpp)
target_link_libraries(morph_duplicate_model_id PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_duplicate_model_id)
add_test(NAME morph_duplicate_model_id
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:morph_duplicate_model_id>
                 "-DPASS=the model id `Dup_StaticModel` is registered for two types" "-DFAIL=REACHED MAIN"
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/expect_message.cmake)
set_tests_properties(morph_duplicate_model_id PROPERTIES TIMEOUT 60)

# An action's effects registered in one translation unit and not seen by another that registers the same action:
# BRIDGE_REGISTER_EFFECTS defines no specialisation, so the program links, and both halves read the same effects.
add_executable(morph_effects_unseen_links compile_checks/effects_unseen_a.cpp compile_checks/effects_unseen_b.cpp)
target_link_libraries(morph_effects_unseen_links PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_effects_unseen_links)
add_test(NAME morph_effects_unseen_links COMMAND morph_effects_unseen_links)
set_tests_properties(morph_effects_unseen_links PROPERTIES TIMEOUT 60)

# The negative probe stays a configure-time try_compile(): the link failure is
# the assertion. Its scratch project cannot link core-cpp either, so a failure
# alone would prove nothing; the check is that the linker named
# ClientOnlyModel -- the symbols the default registrars reference.
unset(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE CACHE)
try_compile(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/client_only_no_model_link.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    OUTPUT_VARIABLE MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT
)
# GNU ld: "undefined reference to `ClientOnlyModel::...", lld: "undefined
# symbol: ClientOnlyModel::...", MSVC: "unresolved external symbol ...
# ClientOnlyModel...", ld64: "ClientOnlyModel::..., referenced from".
set(_morph_client_only_unresolved
    "(undefined reference to|undefined symbol:|unresolved external symbol)[^\n]*ClientOnlyModel|ClientOnlyModel[^\n]*referenced from")
if(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE)
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_no_model_link.cpp linked successfully "
        "WITHOUT MORPH_CLIENT_ONLY defined (expected a link failure -- the "
        "probe's undefined constructor/execute() should be referenced by "
        "the default (non-client-only) registrars; if it links anyway, this "
        "probe is not actually proving anything about the guard).\n"
        "--- compiler/linker output ---\n"
        "${MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT}")
elseif(NOT MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT MATCHES "${_morph_client_only_unresolved}")
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_no_model_link.cpp failed to link WITHOUT "
        "MORPH_CLIENT_ONLY defined, as it must -- but the linker did not name "
        "ClientOnlyModel, so it failed for some other reason and proves "
        "nothing about the guard.\n"
        "--- compiler/linker output ---\n"
        "${MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT}")
endif()
unset(_morph_client_only_unresolved)

# The link probe above only proves the *registration-suppression* half of the
# guard (static-init-time link resolution). The other half -- that
# Bridge::executeVia's localOp actually throws std::logic_error at *runtime*
# under MORPH_CLIENT_ONLY, instead of silently calling Model::execute -- needs
# the probe to run: compile_checks/client_only_runtime_throw.cpp exits 0 when
# the expected std::logic_error was caught.
add_executable(morph_client_only_runtime_throw compile_checks/client_only_runtime_throw.cpp)
target_compile_definitions(morph_client_only_runtime_throw PRIVATE MORPH_CLIENT_ONLY)
target_link_libraries(morph_client_only_runtime_throw PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_client_only_runtime_throw)
add_test(NAME morph_client_only_runtime_throw COMMAND morph_client_only_runtime_throw)
set_tests_properties(morph_client_only_runtime_throw PROPERTIES TIMEOUT 60)
# ── BRIDGE_DECLARE_MODEL/BRIDGE_DECLARE_ACTION link-time canary ─────────────
# Configure-time proof that a model/action declared via BRIDGE_DECLARE_MODEL/
# BRIDGE_DECLARE_ACTION but never registered via BRIDGE_REGISTER_MODEL_SOURCE/
# BRIDGE_REGISTER_ACTION_SOURCE anywhere in the link fails to LINK -- see
# docs/spec/core/registry.md, "Moving a registrar out of the header".
#
# Two probes, in opposite directions, prove the canary does something real:
#   - declare_only_no_source_link.cpp ALONE: must FAIL TO LINK. It declares a
#     real, fully-defined model/action and never calls
#     BRIDGE_REGISTER_MODEL_SOURCE/BRIDGE_REGISTER_ACTION_SOURCE for either,
#     so nothing in it reaches Bridge's registration machinery -- it stays a
#     configure-time try_compile() against MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS,
#     same as the MORPH_CLIENT_ONLY guard's own negative probe above.
#   - declare_only_no_source_link.cpp PLUS declare_only_source_provided.cpp
#     (which does call both SOURCE macros for the same model/action): must
#     LINK. This direction calls BRIDGE_REGISTER_ACTION_SOURCE, whose
#     registrar needs registerActionExecutorOnce's definition in bridge.hpp
#     and, through it, TimeoutScheduler's core-cpp compiled net module -- a
#     try_compile() scratch project cannot link that (same reasoning as the
#     MORPH_CLIENT_ONLY guard's positive probe above), so this direction is a
#     real linked executable via morph_guard_probe_deps instead.
unset(MORPH_DECLARE_SOURCE_CANARY_MISSING_FAILS CACHE)
try_compile(MORPH_DECLARE_SOURCE_CANARY_MISSING_FAILS
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/declare_only_no_source_link.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    OUTPUT_VARIABLE MORPH_DECLARE_SOURCE_CANARY_MISSING_OUTPUT
)
if(MORPH_DECLARE_SOURCE_CANARY_MISSING_FAILS)
    message(FATAL_ERROR
        "BRIDGE_DECLARE_MODEL/BRIDGE_DECLARE_ACTION canary check failed: "
        "compile_checks/declare_only_no_source_link.cpp LINKED successfully "
        "even though it never calls BRIDGE_REGISTER_MODEL_SOURCE/"
        "BRIDGE_REGISTER_ACTION_SOURCE. The whole point of BRIDGE_DECLARE_* "
        "is that a forgotten .cpp-side registration call fails to link "
        "instead of silently compiling into a runtime \"unknown model "
        "type\"/\"unknown action\" -- see docs/spec/core/registry.md, "
        "\"Moving a registrar out of the header\".\n"
        "--- compiler/linker output ---\n"
        "${MORPH_DECLARE_SOURCE_CANARY_MISSING_OUTPUT}")
endif()

add_executable(morph_declare_source_canary
    compile_checks/declare_only_no_source_link.cpp
    compile_checks/declare_only_source_provided.cpp)
target_link_libraries(morph_declare_source_canary PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_declare_source_canary)
add_test(NAME morph_declare_source_canary COMMAND morph_declare_source_canary)
set_tests_properties(morph_declare_source_canary PROPERTIES TIMEOUT 60)

# BRIDGE_REGISTER_ACTION_FOR_CLIENT lets a MORPH_CLIENT_ONLY client
# register an action's ActionTraits (JSON codecs + an explicitly-named Result
# type) without ever needing the real model's complete class body -- see
# docs/spec/core/registry.md, "BRIDGE_REGISTER_ACTION_FOR_CLIENT".
# compile_checks/client_only_facade_no_model_header.cpp forward-declares (never
# defines) ClientOnlyFacadeModel and proves the resulting ActionTraits
# specialisation still round-trips JSON correctly: it builds, links and exits 0.
add_executable(morph_client_only_facade compile_checks/client_only_facade_no_model_header.cpp)
target_compile_definitions(morph_client_only_facade PRIVATE MORPH_CLIENT_ONLY)
target_link_libraries(morph_client_only_facade PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_client_only_facade)
add_test(NAME morph_client_only_facade COMMAND morph_client_only_facade)
set_tests_properties(morph_client_only_facade PROPERTIES TIMEOUT 60)

# Instrumented like every other executable here: on a sanitizer leg core-cpp's
# modules are instrumented too (root CMakeLists.txt), and an uninstrumented
# program linking them fails with the sanitizer runtime undefined.
if(DEFINED AF_SANITIZER)
    foreach(_morph_guard_probe IN ITEMS morph_client_only_guard_links morph_client_only_runtime_throw
                                        morph_client_only_facade morph_declare_source_canary
                                        morph_effects_unseen_links morph_duplicate_model_id)
        apply_sanitizers(${_morph_guard_probe} ${AF_SANITIZER})
    endforeach()
    unset(_morph_guard_probe)
endif()

# ── Windows dialog canary ────────────────────────────────────────────────────
# Proves that a morph test executable dies on a failed assert() or an abort()
# instead of waiting on a dialog nobody will click, and that an invalid CRT
# parameter is handled rather than raising one. Its main() asks for nothing:
# linking core::testing_dialogs, as every test executable here does, is what
# installs the suppression. Modelled on core-cpp's WindowsDialogCanary, and
# judged the same way: by the marker each mode prints before it fails
# (PASS_REGULAR_EXPRESSION, which also makes ctest ignore the exit status), not
# by WILL_FAIL, which would score a run that never reached its assertion as a
# pass. The TIMEOUT is what catches a dialog: a run still waiting when it
# expires is waiting for a click.
if(WIN32 AND TARGET core-cpp-testing_dialogs)
    add_executable(morph_windows_dialog_canary windows_dialog_canary.cpp)
    morph_suppress_test_dialogs(morph_windows_dialog_canary)
    foreach(_mode IN ITEMS assert abort invalid-parameter)
        add_test(NAME morph_windows_dialog_canary.${_mode} COMMAND morph_windows_dialog_canary ${_mode})
        # `assert` and `abort` must kill the process; `invalid-parameter` must
        # survive, because the suppression installs a no-op handler, so the one
        # marker is the failure for two modes and the pass for the third.
        set(_pass "failing by ${_mode}")
        set(_fail "CONTINUED AFTER FAILURE")
        if(_mode STREQUAL "invalid-parameter")
            set(_pass "CONTINUED AFTER FAILURE")
            set(_fail "")
        endif()
        # A Debug CRT writes abort()'s message to stderr, and the suppression
        # keeps it there rather than dropping it: the abort run must show it. A
        # Release UCRT writes none, and a multi-config generator does not say
        # which it builds, so those keep the "failing by" marker.
        if(_mode STREQUAL "abort" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_pass "abort\\(\\) has been called")
        endif()
        set_tests_properties(morph_windows_dialog_canary.${_mode} PROPERTIES
            PASS_REGULAR_EXPRESSION "${_pass}"
            FAIL_REGULAR_EXPRESSION "${_fail}"
            TIMEOUT 60
            SKIP_RETURN_CODE 77
            LABELS "canary")
    endforeach()
    unset(_mode)
    unset(_pass)
    unset(_fail)
endif()

# ── morph::table ─────────────────────────────────────────────────────────────
# Its own executable so the threaded engine tests can run alone under a
# sanitizer preset. The `[.benchmark]` cases are hidden: catch_discover_tests
# does not list them, and they run only when asked for by tag.
add_executable(morph_table_tests ${MORPH_TABLE_TEST_SOURCES})
target_link_libraries(morph_table_tests PRIVATE morph::morph morph_test_main
                      "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph_wide_table_model>")
target_include_directories(morph_table_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(morph_table_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
apply_warnings(morph_table_tests)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_table_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_table_tests)
endif()
catch_discover_tests(morph_table_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
