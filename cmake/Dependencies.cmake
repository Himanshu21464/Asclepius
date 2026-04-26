# Asclepius dependencies.
#
# Header-only / lightweight deps are fetched. System libs (libsodium,
# sqlite3) are required to be installed because they have ABI guarantees
# and broad packaging across distros.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# fmt -------------------------------------------------------------------
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.0.2
    GIT_SHALLOW    TRUE
    SYSTEM
)

# spdlog ----------------------------------------------------------------
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
    SYSTEM
)

# nlohmann_json --------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SYSTEM
)

# doctest ---------------------------------------------------------------
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
    SYSTEM
)

# CLI11 -----------------------------------------------------------------
FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE
    SYSTEM
)

FetchContent_MakeAvailable(fmt spdlog nlohmann_json doctest CLI11)

# Provide doctest's CMake helper for `doctest_discover_tests`.
list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")

# pybind11 (only when Python bindings are requested) -------------------
if(ASCLEPIUS_BUILD_PYTHON)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# System deps ----------------------------------------------------------

find_package(SQLite3 REQUIRED)

find_path(SODIUM_INCLUDE_DIR sodium.h)
find_library(SODIUM_LIBRARY  NAMES sodium)
if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
    message(FATAL_ERROR
        "libsodium not found. Install with:\n"
        "  Debian/Ubuntu : sudo apt install libsodium-dev\n"
        "  macOS         : brew install libsodium\n"
        "  Fedora/RHEL   : sudo dnf install libsodium-devel\n"
    )
endif()

if(NOT TARGET sodium)
    add_library(sodium UNKNOWN IMPORTED)
    set_target_properties(sodium PROPERTIES
        IMPORTED_LOCATION             "${SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}"
    )
endif()

# libpq (PostgreSQL client library) ------------------------------------
# Required at runtime for the optional postgres:// ledger backend; the
# SQLite backend is always available. We use libpq's C API rather than
# a higher-level wrapper to keep the dependency surface small and stable.

find_package(PostgreSQL REQUIRED)
if(NOT TARGET PostgreSQL::PostgreSQL)
    message(FATAL_ERROR
        "libpq not found. Install with:\n"
        "  Debian/Ubuntu : sudo apt install libpq-dev\n"
        "  macOS         : brew install libpq\n"
        "  Fedora/RHEL   : sudo dnf install libpq-devel\n"
    )
endif()
