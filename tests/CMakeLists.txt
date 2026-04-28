include(doctest)

add_executable(asclepius_tests
    test_main.cpp
    test_core.cpp
    test_hashing.cpp
    test_identity.cpp
    test_audit.cpp
    test_policy.cpp
    test_telemetry.cpp
    test_consent.cpp
    test_consent_artefact.cpp
    test_family_graph.cpp
    test_emergency_override.cpp
    test_runtime.cpp
    test_evaluation.cpp
    test_fhir.cpp
)

target_link_libraries(asclepius_tests
    PRIVATE
        asclepius::asclepius
        doctest::doctest
        SQLite::SQLite3
)

asclepius_target_warnings(asclepius_tests)
asclepius_target_sanitizers(asclepius_tests)

doctest_discover_tests(asclepius_tests)

# Benchmark adapters — independent test executables, one per benchmark.
add_subdirectory(benchmarks/asclepius_med)
