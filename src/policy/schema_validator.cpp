// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/policy.hpp"

#include <nlohmann/json.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <string>
#include <vector>

namespace asclepius {

namespace {

using nlohmann::json;

// Subset JSON-Schema validator. Supports: type, required, properties, enum,
// minimum, maximum, minLength, maxLength, items.
//
// We do not pull in a full schema library; the substrate's job is to
// cheaply guard against gross output-shape failures, not to do exhaustive
// validation. Customers can compose a fuller validator on top (the chain
// is the right place for ajv-class strictness).
class SchemaValidator final : public IPolicy {
public:
    SchemaValidator(std::string schema_text, json schema)
        : schema_text_(std::move(schema_text)), schema_(std::move(schema)) {}

    std::string_view name() const noexcept override { return "schema_validator"; }

    PolicyVerdict evaluate_output(const InferenceContext&, std::string_view payload) override {
        json doc;
        try {
            doc = json::parse(payload);
        } catch (const json::parse_error& e) {
            PolicyVerdict v;
            v.decision   = PolicyDecision::block;
            v.rationale  = std::string{"invalid JSON: "} + e.what();
            v.violations = {"json_parse"};
            return v;
        }
        std::vector<std::string> errors;
        validate(schema_, doc, "", errors);
        if (errors.empty()) {
            return PolicyVerdict{};
        }
        PolicyVerdict v;
        v.decision   = PolicyDecision::block;
        v.rationale  = fmt::format("schema violations: {}", fmt::join(errors, "; "));
        v.violations = std::move(errors);
        return v;
    }

private:
    std::string schema_text_;
    json        schema_;

    static void validate(const json& schema,
                         const json& doc,
                         const std::string& path,
                         std::vector<std::string>& errs) {
        if (schema.contains("type")) {
            const auto& t = schema["type"];
            if (t.is_string()) {
                if (!matches_type(t.get<std::string>(), doc)) {
                    errs.push_back(fmt::format("{}: expected type {}", path.empty() ? "$" : path,
                                               t.get<std::string>()));
                    return;
                }
            }
        }

        if (schema.contains("enum") && schema["enum"].is_array()) {
            bool ok = false;
            for (const auto& e : schema["enum"]) {
                if (e == doc) { ok = true; break; }
            }
            if (!ok) {
                errs.push_back(fmt::format("{}: not one of enum", path.empty() ? "$" : path));
            }
        }

        if (doc.is_object()) {
            if (schema.contains("required") && schema["required"].is_array()) {
                for (const auto& r : schema["required"]) {
                    if (!doc.contains(r.get<std::string>())) {
                        errs.push_back(fmt::format("{}/{}: required", path, r.get<std::string>()));
                    }
                }
            }
            if (schema.contains("properties") && schema["properties"].is_object()) {
                for (auto it = schema["properties"].begin();
                          it != schema["properties"].end(); ++it) {
                    if (doc.contains(it.key())) {
                        validate(it.value(), doc[it.key()],
                                 path + "/" + it.key(), errs);
                    }
                }
            }
        }

        if (doc.is_array() && schema.contains("items") && schema["items"].is_object()) {
            for (std::size_t i = 0; i < doc.size(); ++i) {
                validate(schema["items"], doc[i],
                         fmt::format("{}/{}", path, i), errs);
            }
        }

        if (doc.is_string()) {
            const auto sl = doc.get<std::string>().size();
            if (schema.contains("minLength") && sl < schema["minLength"].get<std::size_t>()) {
                errs.push_back(fmt::format("{}: too short", path.empty() ? "$" : path));
            }
            if (schema.contains("maxLength") && sl > schema["maxLength"].get<std::size_t>()) {
                errs.push_back(fmt::format("{}: too long", path.empty() ? "$" : path));
            }
        }

        if (doc.is_number()) {
            const double n = doc.get<double>();
            if (schema.contains("minimum") && n < schema["minimum"].get<double>()) {
                errs.push_back(fmt::format("{}: < minimum", path.empty() ? "$" : path));
            }
            if (schema.contains("maximum") && n > schema["maximum"].get<double>()) {
                errs.push_back(fmt::format("{}: > maximum", path.empty() ? "$" : path));
            }
        }
    }

    static bool matches_type(const std::string& t, const json& v) {
        if (t == "object")  return v.is_object();
        if (t == "array")   return v.is_array();
        if (t == "string")  return v.is_string();
        if (t == "number")  return v.is_number();
        if (t == "integer") return v.is_number_integer();
        if (t == "boolean") return v.is_boolean();
        if (t == "null")    return v.is_null();
        return true;
    }
};

}  // namespace

std::shared_ptr<IPolicy> make_schema_validator(std::string schema_json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(schema_json);
    } catch (const nlohmann::json::parse_error&) {
        // We accept the policy with a degenerate schema; validation will
        // simply allow everything. Better to report this as a configuration
        // error in the future when we add a policy linter.
        parsed = nlohmann::json::object();
    }
    return std::make_shared<SchemaValidator>(std::move(schema_json), std::move(parsed));
}

}  // namespace asclepius
