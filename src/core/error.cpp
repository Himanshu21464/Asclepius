// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/core.hpp"

namespace asclepius {

const char* to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::ok:                 return "ok";
        case ErrorCode::invalid_argument:   return "invalid_argument";
        case ErrorCode::not_found:          return "not_found";
        case ErrorCode::conflict:           return "conflict";
        case ErrorCode::permission_denied:  return "permission_denied";
        case ErrorCode::consent_missing:    return "consent_missing";
        case ErrorCode::consent_expired:    return "consent_expired";
        case ErrorCode::policy_violation:   return "policy_violation";
        case ErrorCode::schema_violation:   return "schema_violation";
        case ErrorCode::integrity_failure:  return "integrity_failure";
        case ErrorCode::backend_failure:    return "backend_failure";
        case ErrorCode::drift_threshold:    return "drift_threshold";
        case ErrorCode::rate_limited:       return "rate_limited";
        case ErrorCode::cancelled:          return "cancelled";
        case ErrorCode::deadline_exceeded:  return "deadline_exceeded";
        case ErrorCode::unimplemented:      return "unimplemented";
        case ErrorCode::internal:           return "internal";
    }
    return "unknown";
}

}  // namespace asclepius
