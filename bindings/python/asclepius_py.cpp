// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// pybind11 bindings for Asclepius. Exposes a Pythonic subset of the C++
// API; high-throughput callers should still prefer the C++ entry points.

#include <pybind11/chrono.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <nlohmann/json.hpp>

#include "asclepius/asclepius.hpp"

#include <chrono>

namespace py = pybind11;
using namespace asclepius;

namespace {

// Convert any Result<T> into a Python value or raise.
template <class T>
T or_raise(Result<T> r) {
    if (!r) {
        throw py::value_error(std::string{r.error().what()});
    }
    return std::move(r).value();
}

void or_raise_void(Result<void> r) {
    if (!r) {
        throw py::value_error(std::string{r.error().what()});
    }
}

}  // namespace

PYBIND11_MODULE(_asclepius, m) {
    m.doc() = "Asclepius — clinical-AI trust substrate";
    m.attr("__version__") = ASCLEPIUS_VERSION_STRING;

    // ---- Errors ----------------------------------------------------------
    py::enum_<ErrorCode>(m, "ErrorCode")
        .value("ok",                ErrorCode::ok)
        .value("invalid_argument",  ErrorCode::invalid_argument)
        .value("not_found",         ErrorCode::not_found)
        .value("conflict",          ErrorCode::conflict)
        .value("permission_denied", ErrorCode::permission_denied)
        .value("consent_missing",   ErrorCode::consent_missing)
        .value("consent_expired",   ErrorCode::consent_expired)
        .value("policy_violation",  ErrorCode::policy_violation)
        .value("schema_violation",  ErrorCode::schema_violation)
        .value("integrity_failure", ErrorCode::integrity_failure)
        .value("backend_failure",   ErrorCode::backend_failure)
        .value("internal",          ErrorCode::internal);

    // ---- Identity --------------------------------------------------------
    py::class_<ActorId>(m, "Actor")
        .def_static("clinician", &ActorId::clinician)
        .def_static("service",   &ActorId::service)
        .def_static("system",    &ActorId::system)
        .def("__str__", [](const ActorId& a) { return std::string{a.str()}; });

    py::class_<PatientId>(m, "Patient")
        .def_static("pseudonymous", &PatientId::pseudonymous)
        .def_static("fhir",         &PatientId::fhir)
        .def("__str__", [](const PatientId& p) { return std::string{p.str()}; });

    py::class_<EncounterId>(m, "Encounter")
        .def_static("new",  &EncounterId::make)
        .def_static("fhir", &EncounterId::fhir)
        .def("__str__", [](const EncounterId& e) { return std::string{e.str()}; });

    py::class_<ModelId>(m, "Model")
        .def(py::init<std::string, std::string>(), py::arg("name"), py::arg("version"))
        .def("__str__", [](const ModelId& m) { return std::string{m.str()}; });

    py::class_<TenantId>(m, "Tenant")
        .def(py::init<std::string>(), py::arg("id") = "")
        .def("__str__", [](const TenantId& t) { return std::string{t.str()}; });

    // ---- Purpose ---------------------------------------------------------
    py::enum_<Purpose>(m, "Purpose")
        .value("AMBIENT_DOCUMENTATION", Purpose::ambient_documentation)
        .value("DIAGNOSTIC_SUGGESTION", Purpose::diagnostic_suggestion)
        .value("TRIAGE",                Purpose::triage)
        .value("MEDICATION_REVIEW",     Purpose::medication_review)
        .value("RISK_STRATIFICATION",   Purpose::risk_stratification)
        .value("QUALITY_IMPROVEMENT",   Purpose::quality_improvement)
        .value("RESEARCH",              Purpose::research)
        .value("OPERATIONS",            Purpose::operations);

    // ---- ConsentToken ----------------------------------------------------
    py::class_<ConsentToken>(m, "ConsentToken")
        .def_readonly("token_id",   &ConsentToken::token_id)
        .def_readonly("purposes",   &ConsentToken::purposes)
        .def_readonly("revoked",    &ConsentToken::revoked);

    // ---- Policies (functions that produce IPolicy) -----------------------
    auto policies_mod = m.def_submodule("policies", "built-in policies");
    policies_mod.def("phi_scrubber", &make_phi_scrubber);
    policies_mod.def("schema_validator", [](py::object schema) {
        return make_schema_validator(py::str(schema));
    });
    policies_mod.def("clinical_action_filter", &make_clinical_action_filter);
    policies_mod.def("length_limit", &make_length_limit,
                      py::arg("input_max"), py::arg("output_max"));

    py::class_<IPolicy, std::shared_ptr<IPolicy>>(m, "Policy")
        .def("name", [](const IPolicy& p) { return std::string{p.name()}; });

    // ---- PolicyChain (limited surface) -----------------------------------
    py::class_<PolicyChain>(m, "PolicyChain")
        .def("push", &PolicyChain::push, py::arg("policy"))
        .def("size", &PolicyChain::size)
        .def("names", &PolicyChain::names);

    // ---- DriftMonitor / MetricRegistry ----------------------------------
    py::class_<DriftMonitor>(m, "DriftMonitor")
        .def("register_feature", [](DriftMonitor& d, std::string name,
                                    std::vector<double> baseline,
                                    double lo, double hi, std::size_t bins) {
            return or_raise_void(d.register_feature(std::move(name), std::move(baseline), lo, hi, bins));
        }, py::arg("name"), py::arg("baseline"),
           py::arg("lo") = 0.0, py::arg("hi") = 1.0, py::arg("bins") = 20)
        .def("observe", [](DriftMonitor& d, std::string feature, double v) {
            return or_raise_void(d.observe(feature, v));
        })
        .def("rotate", &DriftMonitor::rotate)
        .def("report", &DriftMonitor::report);

    py::class_<DriftReport>(m, "DriftReport")
        .def_readonly("feature",      &DriftReport::feature)
        .def_readonly("psi",          &DriftReport::psi)
        .def_readonly("ks_statistic", &DriftReport::ks_statistic)
        .def_readonly("emd",          &DriftReport::emd)
        .def_readonly("severity",     &DriftReport::severity)
        .def_readonly("reference_n",  &DriftReport::reference_n)
        .def_readonly("current_n",    &DriftReport::current_n);

    py::enum_<DriftSeverity>(m, "DriftSeverity")
        .value("none",   DriftSeverity::none)
        .value("minor",  DriftSeverity::minor)
        .value("moder",  DriftSeverity::moder)
        .value("severe", DriftSeverity::severe);

    py::class_<MetricRegistry>(m, "MetricRegistry")
        .def("inc",     &MetricRegistry::inc, py::arg("name"), py::arg("delta") = 1)
        .def("count",   &MetricRegistry::count)
        .def("snapshot_json", &MetricRegistry::snapshot_json);

    // ---- ConsentRegistry -------------------------------------------------
    py::class_<ConsentRegistry>(m, "ConsentRegistry")
        .def("grant", [](ConsentRegistry& c, PatientId p,
                          std::vector<Purpose> ps, double ttl_seconds) {
            return or_raise(c.grant(std::move(p), std::move(ps),
                                     std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::duration<double>{ttl_seconds})));
        }, py::arg("patient"), py::arg("purposes"), py::arg("ttl_seconds"))
        .def("revoke", [](ConsentRegistry& c, std::string id) {
            return or_raise_void(c.revoke(id));
        })
        .def("permits", [](const ConsentRegistry& c, const PatientId& p, Purpose pp) {
            return or_raise(c.permits(p, pp));
        });

    // ---- Inference -------------------------------------------------------
    py::class_<Inference>(m, "Inference")
        .def("run", [](Inference& i, std::string input, py::function model_call) {
            ModelCallback cb = [model_call](std::string in) -> Result<std::string> {
                py::gil_scoped_acquire g;
                try {
                    auto out = model_call(in).cast<std::string>();
                    return out;
                } catch (const py::error_already_set& e) {
                    return Error::internal(std::string{"python model raised: "} + e.what());
                }
            };
            return or_raise(i.run(std::move(input), cb));
        }, py::arg("input"), py::arg("model_call"))
        .def("commit", [](Inference& i) { return or_raise_void(i.commit()); })
        .def("capture_override", [](Inference& i, std::string rationale, py::object corrected) {
            auto json_text = py::module_::import("json").attr("dumps")(corrected).cast<std::string>();
            auto j = nlohmann::json::parse(json_text);
            return or_raise_void(i.capture_override(std::move(rationale), j));
        }, py::arg("rationale"), py::arg("corrected"))
        .def("observe_drift", [](Inference& i, std::string feature, double v) {
            return or_raise_void(i.observe_drift(feature, v));
        })
        .def("add_metadata", [](Inference& i, std::string key, py::object value) {
            auto json_text = py::module_::import("json").attr("dumps")(value).cast<std::string>();
            auto j = nlohmann::json::parse(json_text);
            return or_raise_void(i.add_metadata(key, j));
        }, py::arg("key"), py::arg("value"));

    // ---- Runtime ---------------------------------------------------------
    py::class_<Runtime>(m, "Runtime")
        .def(py::init([](std::filesystem::path db) {
            return or_raise(Runtime::open(std::move(db)));
        }), py::arg("db_path"))
        .def_property_readonly("policies",
            [](Runtime& r) -> PolicyChain&     { return r.policies(); },
            py::return_value_policy::reference_internal)
        .def_property_readonly("drift",
            [](Runtime& r) -> DriftMonitor&    { return r.drift(); },
            py::return_value_policy::reference_internal)
        .def_property_readonly("consent",
            [](Runtime& r) -> ConsentRegistry& { return r.consent(); },
            py::return_value_policy::reference_internal)
        .def_property_readonly("metrics",
            [](Runtime& r) -> MetricRegistry&  { return r.metrics(); },
            py::return_value_policy::reference_internal)
        .def("begin_inference",
             [](Runtime& rt, ModelId model, ActorId actor, PatientId patient,
                EncounterId enc, Purpose purpose, TenantId tenant,
                std::optional<std::string> consent_token_id) {
                 return or_raise(rt.begin_inference({
                     .model = std::move(model),
                     .actor = std::move(actor),
                     .patient = std::move(patient),
                     .encounter = std::move(enc),
                     .purpose = purpose,
                     .tenant = std::move(tenant),
                     .consent_token_id = std::move(consent_token_id),
                 }));
             },
             py::arg("model"), py::arg("actor"), py::arg("patient"),
             py::arg("encounter"), py::arg("purpose"),
             py::arg("tenant") = TenantId{},
             py::arg("consent_token_id") = std::nullopt);
}
