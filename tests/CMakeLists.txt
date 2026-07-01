add_executable(morph_tests
    test_executor.cpp
    test_example.cpp
    test_executor_extra.cpp
    test_strand.cpp
    test_strand_extra.cpp
    test_completion.cpp
    test_completion_extra.cpp
    test_model.cpp
    test_logger.cpp
    test_backend_extra.cpp
    test_registry_extra.cpp
    test_bridge_local.cpp
    test_bridge_remote.cpp
    test_remote_extra.cpp
    test_dispatch_di.cpp
    test_handler_binding.cpp
    test_switch_backend.cpp
    test_network_monitor.cpp
    test_offline_queue.cpp
    test_sync_worker.cpp
    test_reconnect_coordinator.cpp
    test_offline_integration.cpp
    test_conflict_resolution.cpp
    test_completion_branches.cpp
    test_issues.cpp
    test_concurrency_invariants.cpp
    test_subscription.cpp
    test_coverage_gaps.cpp
    test_coverage_extra.cpp
    test_coverage_push95.cpp
    test_server_limits.cpp
)

target_link_libraries(morph_tests
    PRIVATE
        morph::morph
        Catch2::Catch2WithMain
)

apply_warnings(morph_tests)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_tests)
endif()

include(Catch)
catch_discover_tests(morph_tests DISCOVERY_MODE PRE_TEST)
