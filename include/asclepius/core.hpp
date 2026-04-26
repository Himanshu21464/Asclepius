// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_CORE_HPP
#define ASCLEPIUS_CORE_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace asclepius {

// ---- Error ---------------------------------------------------------------

enum class ErrorCode : std::uint16_t {
    ok                 = 0,
    invalid_argument   = 1,
    not_found          = 2,
    conflict           = 3,
    permission_denied  = 4,
    consent_missing    = 5,
    consent_expired    = 6,
    policy_violation   = 7,
    schema_violation   = 8,
    integrity_failure  = 9,
    backend_failure    = 10,
    drift_threshold    = 11,
    rate_limited       = 12,
    cancelled          = 13,
    deadline_exceeded  = 14,
    unimplemented      = 15,
    internal           = 16,
};

const char* to_string(ErrorCode) noexcept;

class Error {
public:
    Error() noexcept = default;
    Error(ErrorCode code, std::string message) noexcept
        : code_(code), message_(std::move(message)) {}

    ErrorCode        code()    const noexcept { return code_; }
    std::string_view what()    const noexcept { return message_; }
    bool             is_ok()   const noexcept { return code_ == ErrorCode::ok; }

    static Error invalid(std::string m)   { return {ErrorCode::invalid_argument, std::move(m)}; }
    static Error not_found(std::string m) { return {ErrorCode::not_found,        std::move(m)}; }
    static Error denied(std::string m)    { return {ErrorCode::permission_denied,std::move(m)}; }
    static Error policy(std::string m)    { return {ErrorCode::policy_violation, std::move(m)}; }
    static Error schema(std::string m)    { return {ErrorCode::schema_violation, std::move(m)}; }
    static Error integrity(std::string m) { return {ErrorCode::integrity_failure,std::move(m)}; }
    static Error backend(std::string m)   { return {ErrorCode::backend_failure,  std::move(m)}; }
    static Error timeout(std::string m)   { return {ErrorCode::deadline_exceeded,std::move(m)}; }
    static Error cancelled(std::string m) { return {ErrorCode::cancelled,        std::move(m)}; }
    static Error internal(std::string m)  { return {ErrorCode::internal,         std::move(m)}; }

private:
    ErrorCode   code_   = ErrorCode::ok;
    std::string message_;
};

// ---- Result<T> -----------------------------------------------------------
//
// A minimal Result<T, Error>. Models a success-or-error return without
// exceptions in the hot path. Convertible to bool.

template <class T>
class [[nodiscard]] Result {
public:
    using value_type = T;

    Result(T value) : v_(std::move(value)) {}             // NOLINT(google-explicit-constructor)
    Result(Error err) : v_(std::move(err)) {}             // NOLINT(google-explicit-constructor)

    bool has_value() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T&       value() &      { ensure_value(); return std::get<T>(v_); }
    const T& value() const& { ensure_value(); return std::get<T>(v_); }
    T&&      value() &&     { ensure_value(); return std::move(std::get<T>(v_)); }

    const Error& error() const& { ensure_error(); return std::get<Error>(v_); }
    Error&&      error() &&     { ensure_error(); return std::move(std::get<Error>(v_)); }

    T*       operator->()       { return &value(); }
    const T* operator->() const { return &value(); }
    T&       operator*() &      { return value(); }
    const T& operator*() const& { return value(); }

    template <class U>
    T value_or(U&& alt) const& {
        return has_value() ? value() : static_cast<T>(std::forward<U>(alt));
    }

private:
    void ensure_value() const { /* tests + asserts are responsible */ }
    void ensure_error() const {}

    std::variant<T, Error> v_;
};

// Specialization for void: a "completion or error".
template <>
class [[nodiscard]] Result<void> {
public:
    Result() noexcept = default;
    Result(Error err) : err_(std::move(err)) {}           // NOLINT(google-explicit-constructor)

    bool has_value() const noexcept { return !err_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    const Error& error() const { return err_.value(); }

    static Result<void> ok() noexcept { return {}; }

private:
    std::optional<Error> err_;
};

// ---- Time ---------------------------------------------------------------
//
// A nanosecond-resolution wall-clock timestamp expressed as nanos since the
// Unix epoch. Stored as int64 to make the on-disk representation portable.

class Time {
public:
    using nanos_t = std::int64_t;

    Time() noexcept = default;
    explicit Time(nanos_t ns) noexcept : ns_(ns) {}

    static Time now() noexcept;
    static Time from_iso8601(std::string_view);

    nanos_t     nanos_since_epoch() const noexcept { return ns_; }
    std::string iso8601() const;

    auto operator<=>(const Time&) const = default;

    Time  operator+(std::chrono::nanoseconds d) const noexcept { return Time{ns_ + d.count()}; }
    Time  operator-(std::chrono::nanoseconds d) const noexcept { return Time{ns_ - d.count()}; }
    std::chrono::nanoseconds operator-(Time other) const noexcept {
        return std::chrono::nanoseconds{ns_ - other.ns_};
    }

private:
    nanos_t ns_ = 0;
};

// ---- Bytes / span helpers -----------------------------------------------

using Bytes      = std::span<const std::uint8_t>;
using BytesOwned = std::vector<std::uint8_t>;

}  // namespace asclepius

#endif  // ASCLEPIUS_CORE_HPP
