add_executable(asclepius_bench_asclepius_med
    adapter.cpp
    test_adapter.cpp
)

target_compile_definitions(asclepius_bench_asclepius_med
    PRIVATE
        ASCLEPIUS_BENCH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(asclepius_bench_asclepius_med
    PRIVATE
        asclepius::asclepius
        doctest::doctest
        SQLite::SQLite3
)

asclepius_target_warnings(asclepius_bench_asclepius_med)
asclepius_target_sanitizers(asclepius_bench_asclepius_med)

doctest_discover_tests(asclepius_bench_asclepius_med)
