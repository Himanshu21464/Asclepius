cmake_minimum_required(VERSION 3.24)

project(asclepius
    VERSION 0.1.0
    DESCRIPTION "A trust substrate for clinical AI"
    HOMEPAGE_URL "https://asclepius.health"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ASCLEPIUS_BUILD_TESTS    "Build unit + integration tests"            ON)
option(ASCLEPIUS_BUILD_EXAMPLES "Build worked examples"                     ON)
option(ASCLEPIUS_BUILD_CLI      "Build the asclepius CLI"                   ON)
option(ASCLEPIUS_BUILD_PYTHON   "Build pybind11 Python bindings"            OFF)
option(ASCLEPIUS_BUILD_SVC      "Build the HTTP/gRPC sidecar"               OFF)
option(ASCLEPIUS_ENABLE_ASAN    "Enable AddressSanitizer in Debug builds"   ON)
option(ASCLEPIUS_ENABLE_UBSAN   "Enable UndefinedBehaviorSanitizer (Debug)" ON)
option(ASCLEPIUS_WARNINGS_AS_ERRORS "Promote compiler warnings to errors"   OFF)
option(ASCLEPIUS_WITH_POSTGRES  "Enable PostgreSQL backend (requires libpq)" ON)

include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

# ---- Core library --------------------------------------------------------

add_library(asclepius)
add_library(asclepius::asclepius ALIAS asclepius)

target_include_directories(asclepius
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(asclepius
    PUBLIC
        fmt::fmt
        nlohmann_json::nlohmann_json
    PRIVATE
        spdlog::spdlog
        sodium
        SQLite::SQLite3
)
if(ASCLEPIUS_WITH_POSTGRES)
    target_link_libraries(asclepius PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(asclepius PRIVATE ASCLEPIUS_WITH_POSTGRES=1)
    target_sources(asclepius PRIVATE src/audit/postgres_backend.cpp)
endif()

target_sources(asclepius
    PRIVATE
        src/core/error.cpp
        src/core/identity.cpp
        src/core/time.cpp
        src/core/hashing.cpp
        src/policy/phi_scrubber.cpp
        src/policy/schema_validator.cpp
        src/policy/policy_chain.cpp
        src/policy/clinical_action_filter.cpp
        src/audit/ledger.cpp
        src/audit/keystore.cpp
        src/audit/sqlite_backend.cpp
        src/audit/storage_factory.cpp
        src/audit/migrator.cpp
        src/audit/jsonl.cpp
        src/telemetry/drift.cpp
        src/telemetry/metrics.cpp
        src/consent/registry.cpp
        src/evaluation/harness.cpp
        src/evaluation/bundle.cpp
        src/runtime/runtime.cpp
        src/runtime/inference.cpp
        src/interop/fhir.cpp
)

asclepius_target_warnings(asclepius)
asclepius_target_sanitizers(asclepius)

set_target_properties(asclepius PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# ---- CLI -----------------------------------------------------------------

if(ASCLEPIUS_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(ASCLEPIUS_BUILD_SVC)
    add_subdirectory(apps/svc)
endif()

# ---- Examples ------------------------------------------------------------

if(ASCLEPIUS_BUILD_EXAMPLES)
    add_subdirectory(examples/01_wrap_scribe)
    add_subdirectory(examples/02_drift_demo)
    add_subdirectory(examples/03_evidence_bundle)
    add_subdirectory(examples/04_bench)
endif()

# ---- Tests ---------------------------------------------------------------

if(ASCLEPIUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Python bindings -----------------------------------------------------

if(ASCLEPIUS_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

# ---- Install -------------------------------------------------------------
#
# Headers and library are installable. Exported CMake targets are deferred
# until we either vendor fmt/spdlog/nlohmann_json or require them as system
# packages — the FetchContent targets cannot participate in install(EXPORT).
include(GNUInstallDirs)

install(TARGETS asclepius
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/asclepius DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---- Summary -------------------------------------------------------------

message(STATUS "")
message(STATUS "Asclepius ${PROJECT_VERSION}")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests           : ${ASCLEPIUS_BUILD_TESTS}")
message(STATUS "  examples        : ${ASCLEPIUS_BUILD_EXAMPLES}")
message(STATUS "  cli             : ${ASCLEPIUS_BUILD_CLI}")
message(STATUS "  python bindings : ${ASCLEPIUS_BUILD_PYTHON}")
message(STATUS "  sanitizers      : asan=${ASCLEPIUS_ENABLE_ASAN} ubsan=${ASCLEPIUS_ENABLE_UBSAN}")
message(STATUS "")
