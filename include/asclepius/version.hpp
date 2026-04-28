// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_VERSION_HPP
#define ASCLEPIUS_VERSION_HPP

#define ASCLEPIUS_VERSION_MAJOR 0
#define ASCLEPIUS_VERSION_MINOR 6
#define ASCLEPIUS_VERSION_PATCH 0
#define ASCLEPIUS_VERSION_STRING "0.6.0"

namespace asclepius {

struct Version {
    int major;
    int minor;
    int patch;
    const char* string;
};

inline constexpr Version version() noexcept {
    return Version{
        ASCLEPIUS_VERSION_MAJOR,
        ASCLEPIUS_VERSION_MINOR,
        ASCLEPIUS_VERSION_PATCH,
        ASCLEPIUS_VERSION_STRING,
    };
}

}  // namespace asclepius

#endif  // ASCLEPIUS_VERSION_HPP
