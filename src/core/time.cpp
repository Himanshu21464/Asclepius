// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/core.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace asclepius {

Time Time::now() noexcept {
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    return Time{ns};
}

std::string Time::iso8601() const {
    using namespace std::chrono;
    const auto secs   = ns_ / 1'000'000'000;
    const auto subsec = ns_ % 1'000'000'000;

    std::time_t tt = static_cast<std::time_t>(secs);
    std::tm     tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif

    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  tm_utc.tm_hour,
                  tm_utc.tm_min,
                  tm_utc.tm_sec,
                  static_cast<long long>(subsec));
    return std::string{buf};
}

Time Time::from_iso8601(std::string_view s) {
    // Parse a strict subset: YYYY-MM-DDTHH:MM:SS[.fff...]Z
    // Anything else returns Time{}; callers should validate beforehand if
    // strictness matters.
    int  y, mo, d, h, mi, sec;
    char tail[64] = {0};

    auto rc = std::sscanf(std::string{s}.c_str(),
                          "%4d-%2d-%2dT%2d:%2d:%2d%63s",
                          &y, &mo, &d, &h, &mi, &sec, tail);
    if (rc < 6) {
        return Time{};
    }

    std::tm tm_utc{};
    tm_utc.tm_year = y - 1900;
    tm_utc.tm_mon  = mo - 1;
    tm_utc.tm_mday = d;
    tm_utc.tm_hour = h;
    tm_utc.tm_min  = mi;
    tm_utc.tm_sec  = sec;

#ifdef _WIN32
    auto tt = _mkgmtime(&tm_utc);
#else
    auto tt = timegm(&tm_utc);
#endif
    if (tt < 0) {
        return Time{};
    }

    Time::nanos_t ns = static_cast<Time::nanos_t>(tt) * 1'000'000'000;

    if (tail[0] == '.') {
        // fractional seconds; pad/truncate to 9 digits.
        char digits[10] = "000000000";
        std::size_t i = 0;
        for (std::size_t k = 1; tail[k] && tail[k] != 'Z' && i < 9; ++k, ++i) {
            digits[i] = tail[k];
        }
        ns += std::strtoll(digits, nullptr, 10);
    }
    return Time{ns};
}

}  // namespace asclepius
