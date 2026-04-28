// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors

#include <doctest/doctest.h>

#include "asclepius/policy.hpp"

using namespace asclepius;
using namespace asclepius::access;

TEST_CASE("access: empty constraint allows anything") {
    Constraint c;
    Context    ctx;
    auto r = evaluate(c, ctx);
    CHECK(r.decision == Decision::allow);
    CHECK(r.reason.empty());
}

TEST_CASE("access: staff_gender any matches all actuals") {
    Constraint c;
    c.staff_gender = Constraint::StaffGender::any;
    for (auto g : {Constraint::StaffGender::female, Constraint::StaffGender::male}) {
        Context ctx;
        ctx.staff_gender = g;
        CHECK(evaluate(c, ctx).decision == Decision::allow);
    }
}

TEST_CASE("access: staff_gender female requires female") {
    Constraint c;
    c.staff_gender = Constraint::StaffGender::female;
    Context ctx;
    ctx.staff_gender = Constraint::StaffGender::female;
    CHECK(evaluate(c, ctx).decision == Decision::allow);

    ctx.staff_gender = Constraint::StaffGender::male;
    auto r = evaluate(c, ctx);
    CHECK(r.decision == Decision::deny);
    CHECK(r.reason.find("staff_gender") != std::string::npos);
}

TEST_CASE("access: staff_gender denies when context is unspecified") {
    Constraint c;
    c.staff_gender = Constraint::StaffGender::female;
    Context ctx;  // staff_gender unspecified
    CHECK(evaluate(c, ctx).decision == Decision::deny);
}

TEST_CASE("access: device_mode on_device_only requires on_device") {
    Constraint c;
    c.device_mode = Constraint::DeviceMode::on_device_only;
    Context ctx;
    ctx.device_mode = Constraint::DeviceMode::on_device_only;
    CHECK(evaluate(c, ctx).decision == Decision::allow);

    ctx.device_mode = Constraint::DeviceMode::off_device_allowed;
    CHECK(evaluate(c, ctx).decision == Decision::deny);
}

TEST_CASE("access: device_mode off_device_allowed accepts either") {
    Constraint c;
    c.device_mode = Constraint::DeviceMode::off_device_allowed;
    Context ctx;
    ctx.device_mode = Constraint::DeviceMode::on_device_only;
    CHECK(evaluate(c, ctx).decision == Decision::allow);
    ctx.device_mode = Constraint::DeviceMode::off_device_allowed;
    CHECK(evaluate(c, ctx).decision == Decision::allow);
}

TEST_CASE("access: language allowlist") {
    Constraint c;
    c.allowed_languages = {"hi", "en", "ta"};
    Context ctx;
    ctx.language = "hi";
    CHECK(evaluate(c, ctx).decision == Decision::allow);
    ctx.language = "ta";
    CHECK(evaluate(c, ctx).decision == Decision::allow);
    ctx.language = "fr";
    auto r = evaluate(c, ctx);
    CHECK(r.decision == Decision::deny);
    CHECK(r.reason.find("language") != std::string::npos);
}

TEST_CASE("access: empty language allowlist allows anything") {
    Constraint c;
    Context ctx;
    ctx.language = "any-language-fine";
    CHECK(evaluate(c, ctx).decision == Decision::allow);
}

TEST_CASE("access: required_role_code") {
    Constraint c;
    c.required_role_code = "nurse";
    Context ctx;
    ctx.role_code = "nurse";
    CHECK(evaluate(c, ctx).decision == Decision::allow);
    ctx.role_code = "physician";
    auto r = evaluate(c, ctx);
    CHECK(r.decision == Decision::deny);
    CHECK(r.reason.find("role_code") != std::string::npos);
}

TEST_CASE("access: combined constraint — Sakhi-style female-only on-device-only") {
    Constraint c;
    c.staff_gender    = Constraint::StaffGender::female;
    c.device_mode     = Constraint::DeviceMode::on_device_only;
    c.allowed_languages = {"hi", "ta", "te"};

    Context ok;
    ok.staff_gender = Constraint::StaffGender::female;
    ok.device_mode  = Constraint::DeviceMode::on_device_only;
    ok.language     = "ta";
    CHECK(evaluate(c, ok).decision == Decision::allow);

    Context wrong_gender = ok;
    wrong_gender.staff_gender = Constraint::StaffGender::male;
    CHECK(evaluate(c, wrong_gender).decision == Decision::deny);

    Context wrong_device = ok;
    wrong_device.device_mode = Constraint::DeviceMode::off_device_allowed;
    CHECK(evaluate(c, wrong_device).decision == Decision::deny);

    Context wrong_lang = ok;
    wrong_lang.language = "fr";
    CHECK(evaluate(c, wrong_lang).decision == Decision::deny);
}

TEST_CASE("access: to_string for enums") {
    CHECK(std::string{to_string(Constraint::StaffGender::any)}    == "any");
    CHECK(std::string{to_string(Constraint::StaffGender::female)} == "female");
    CHECK(std::string{to_string(Constraint::StaffGender::male)}   == "male");

    CHECK(std::string{to_string(Constraint::DeviceMode::any)}                == "any");
    CHECK(std::string{to_string(Constraint::DeviceMode::on_device_only)}     == "on_device_only");
    CHECK(std::string{to_string(Constraint::DeviceMode::off_device_allowed)} == "off_device_allowed");

    CHECK(std::string{to_string(Decision::allow)} == "allow");
    CHECK(std::string{to_string(Decision::deny)}  == "deny");
}
