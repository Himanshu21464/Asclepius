// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Backend dispatcher. Picks SQLite vs PostgreSQL based on URI scheme.

#include "storage.hpp"

#include <cstring>

namespace asclepius::detail {

// Defined in sqlite_backend.cpp / postgres_backend.cpp.
Result<std::unique_ptr<LedgerStorage>> make_sqlite_storage(const std::string& path);
Result<std::unique_ptr<LedgerStorage>> make_postgres_storage(const std::string& uri);

namespace {

bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, std::strlen(p), p) == 0;
}

}  // namespace

Result<std::unique_ptr<LedgerStorage>> make_storage(const std::string& uri) {
    if (starts_with(uri, "postgres://") || starts_with(uri, "postgresql://")) {
        return make_postgres_storage(uri);
    }
    // Anything else is a SQLite filesystem path.
    return make_sqlite_storage(uri);
}

}  // namespace asclepius::detail
