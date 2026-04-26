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
    test_runtime.cpp
    test_evaluation.cpp
    test_fhir.cpp
    test_postgres_backend.cpp
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
