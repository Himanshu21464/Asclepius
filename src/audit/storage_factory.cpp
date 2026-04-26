// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Backend dispatcher. Picks SQLite vs PostgreSQL based on URI scheme.

#include "storage.hpp"

#include <cstring>

namespace asclepius::detail {

// Defined in sqlite_backend.cpp.
Result<std::unique_ptr<LedgerStorage>> make_sqlite_storage(const std::string& path);

#ifdef ASCLEPIUS_WITH_POSTGRES
// Defined in postgres_backend.cpp when the PG backend is enabled at build.
Result<std::unique_ptr<LedgerStorage>> make_postgres_storage(const std::string& uri);
#endif

namespace {

bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, std::strlen(p), p) == 0;
}

}  // namespace

Result<std::unique_ptr<LedgerStorage>> make_storage(const std::string& uri) {
    if (starts_with(uri, "postgres://") || starts_with(uri, "postgresql://")) {
#ifdef ASCLEPIUS_WITH_POSTGRES
        return make_postgres_storage(uri);
#else
        return Error::invalid(
            "postgres:// URI but this build was compiled with "
            "-DASCLEPIUS_WITH_POSTGRES=OFF; rebuild with the flag ON to "
            "enable the PostgreSQL backend");
#endif
    }
    // Anything else is a SQLite filesystem path.
    return make_sqlite_storage(uri);
}

}  // namespace asclepius::detail
