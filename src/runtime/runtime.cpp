// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#include "asclepius/runtime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <random>

namespace asclepius {

// ---- InferenceContext implementation ------------------------------------

InferenceContext::InferenceContext(std::string  inference_id,
                                   ModelId      model,
                                   ActorId      actor,
                                   PatientId    patient,
                                   EncounterId  encounter,
                                   Purpose      purpose,
                                   TenantId     tenant,
                                   Time         started_at)
    : inference_id_(std::move(inference_id)),
      model_(std::move(model)),
      actor_(std::move(actor)),
      patient_(std::move(patient)),
      encounter_(std::move(encounter)),
      purpose_(purpose),
      tenant_(std::move(tenant)),
      started_at_(started_at) {}

std::string_view  InferenceContext::id()         const noexcept { return inference_id_; }
const ModelId&    InferenceContext::model()      const noexcept { return model_; }
const ActorId&    InferenceContext::actor()      const noexcept { return actor_; }
const PatientId&  InferenceContext::patient()    const noexcept { return patient_; }
const EncounterId& InferenceContext::encounter() const noexcept { return encounter_; }
Purpose           InferenceContext::purpose()    const noexcept { return purpose_; }
const TenantId&   InferenceContext::tenant()     const noexcept { return tenant_; }
Time              InferenceContext::started_at() const noexcept { return started_at_; }

// ---- Runtime::Impl ------------------------------------------------------

struct Runtime::Impl {
    Ledger              ledger;
    PolicyChain         policies;
    DriftMonitor        drift;
    ConsentRegistry     consent;
    EvaluationHarness   evaluation;
    MetricRegistry      metrics;

    Impl(Ledger l) : ledger(std::move(l)), evaluation(ledger, drift) {}

    // Wire the DriftMonitor's alert sink to append a drift.crossed
    // event to the ledger whenever a feature's severity rises past
    // the configured threshold. The audit chain becomes the canonical
    // record of drift breaches, not just an in-process metric.
    void install_drift_to_ledger_bridge() {
        drift.set_alert_sink([this](const DriftReport& r) {
            nlohmann::json body;
            body["feature"]      = r.feature;
            body["psi"]          = r.psi;
            body["ks_statistic"] = r.ks_statistic;
            body["emd"]          = r.emd;
            body["severity"]     = std::string{to_string(r.severity)};
            body["reference_n"]  = r.reference_n;
            body["current_n"]    = r.current_n;
            ledger.append("drift.crossed", "system:drift_monitor", body);
            metrics.inc("drift.crossings");
        }, DriftSeverity::moder);
    }
};

Runtime::Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;
Runtime::~Runtime() = default;

Result<Runtime> Runtime::open(std::filesystem::path db_path) {
    auto led = Ledger::open(db_path);
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->install_drift_to_ledger_bridge();
    return rt;
}

Result<Runtime> Runtime::open(std::filesystem::path db_path, KeyStore key) {
    auto led = Ledger::open(std::move(db_path), std::move(key));
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->install_drift_to_ledger_bridge();
    return rt;
}

Result<Runtime> Runtime::open_uri(const std::string& uri) {
    auto led = Ledger::open_uri(uri);
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->install_drift_to_ledger_bridge();
    return rt;
}

Result<Runtime> Runtime::open_uri(const std::string& uri, KeyStore key) {
    auto led = Ledger::open_uri(uri, std::move(key));
    if (!led) return led.error();
    Runtime rt;
    rt.impl_ = std::make_unique<Impl>(std::move(led).value());
    rt.impl_->install_drift_to_ledger_bridge();
    return rt;
}

PolicyChain&       Runtime::policies()        { return impl_->policies; }
Ledger&            Runtime::ledger()          { return impl_->ledger; }
DriftMonitor&      Runtime::drift()           { return impl_->drift; }
ConsentRegistry&   Runtime::consent()         { return impl_->consent; }
EvaluationHarness& Runtime::evaluation()      { return impl_->evaluation; }
MetricRegistry&    Runtime::metrics()         { return impl_->metrics; }
const PolicyChain& Runtime::policies() const  { return impl_->policies; }
const Ledger&      Runtime::ledger()   const  { return impl_->ledger; }

}  // namespace asclepius
