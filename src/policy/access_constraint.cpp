// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 93 — access::Constraint evaluation.
//
// Vendor-neutral: the kernel does not impose any specific care-path policy.
// It evaluates a Context against a Constraint configured by the operator.
// Each dimension that is "any" / unconfigured matches anything.

#include "asclepius/policy.hpp"

#include <algorithm>

namespace asclepius {
namespace access {

const char* to_string(Constraint::StaffGender g) noexcept {
    switch (g) {
        case Constraint::StaffGender::any:    return "any";
        case Constraint::StaffGender::female: return "female";
        case Constraint::StaffGender::male:   return "male";
    }
    return "unknown";
}

const char* to_string(Constraint::DeviceMode d) noexcept {
    switch (d) {
        case Constraint::DeviceMode::any:                return "any";
        case Constraint::DeviceMode::on_device_only:     return "on_device_only";
        case Constraint::DeviceMode::off_device_allowed: return "off_device_allowed";
    }
    return "unknown";
}

const char* to_string(Decision d) noexcept {
    switch (d) {
        case Decision::allow: return "allow";
        case Decision::deny:  return "deny";
    }
    return "unknown";
}

namespace {

bool gender_matches(Constraint::StaffGender required,
                    std::optional<Constraint::StaffGender> actual) {
    if (required == Constraint::StaffGender::any) return true;
    if (!actual.has_value()) return false;
    return *actual == required;
}

bool device_matches(Constraint::DeviceMode required,
                    std::optional<Constraint::DeviceMode> actual) {
    if (required == Constraint::DeviceMode::any) return true;
    if (!actual.has_value()) return false;
    // on_device_only requires actual to be on_device_only.
    // off_device_allowed accepts either on_device_only or off_device_allowed.
    if (required == Constraint::DeviceMode::on_device_only) {
        return *actual == Constraint::DeviceMode::on_device_only;
    }
    // required == off_device_allowed
    return true;
}

bool language_matches(const std::vector<std::string>& allowed,
                      const std::optional<std::string>& actual) {
    if (allowed.empty()) return true;
    if (!actual.has_value()) return false;
    return std::find(allowed.begin(), allowed.end(), *actual) != allowed.end();
}

bool role_matches(const std::optional<std::string>& required,
                  const std::optional<std::string>& actual) {
    if (!required.has_value() || required->empty()) return true;
    if (!actual.has_value()) return false;
    return *required == *actual;
}

}  // namespace

Result evaluate(const Constraint& c, const Context& ctx) noexcept {
    if (!gender_matches(c.staff_gender, ctx.staff_gender)) {
        return Result{Decision::deny, std::string{"staff_gender mismatch: required="} +
                                       to_string(c.staff_gender)};
    }
    if (!device_matches(c.device_mode, ctx.device_mode)) {
        return Result{Decision::deny, std::string{"device_mode mismatch: required="} +
                                       to_string(c.device_mode)};
    }
    if (!language_matches(c.allowed_languages, ctx.language)) {
        return Result{Decision::deny, std::string{"language not in allowlist"}};
    }
    if (!role_matches(c.required_role_code, ctx.role_code)) {
        return Result{Decision::deny, std::string{"role_code mismatch: required="} +
                                       *c.required_role_code};
    }
    return Result{Decision::allow, ""};
}

}  // namespace access
}  // namespace asclepius
