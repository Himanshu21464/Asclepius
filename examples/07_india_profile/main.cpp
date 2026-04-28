// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Worked example #7 — India profile primitives in motion.
//
// Demonstrates the three vendor-neutral types added by ADR-013 alongside
// the new ABDM-aligned Purpose values added by ADR-014:
//
//   1. EmergencyOverride — break-glass for an unconscious ED patient,
//      backfilled within the 72h DPDP § 7 window with an evidence id.
//   2. FamilyGraph — a parent records authority over a minor, then
//      issues a ConsentToken on the minor's behalf which the registry
//      honours iff the family-graph edge exists.
//   3. ConsentArtefact — round-trip a granted token to ABDM-shaped
//      JSON and back, the wire shape HIPs / HIUs / the Gateway speak.
//
// No network, no ABDM mock — every primitive runs locally against the
// in-memory registries. The whole demo runs in under a millisecond.
//
// Run:
//   ./example_india_profile

#include <fmt/core.h>

#include "asclepius/asclepius.hpp"

#include <cassert>
#include <chrono>
#include <string>

using namespace asclepius;
using namespace std::chrono_literals;

int main() {
    fmt::print("─── India profile demo (ADR-013 + ADR-014) ───\n\n");

    // ── 1. EmergencyOverride — DPDP § 7 break-glass ─────────────────────
    fmt::print("§1  EmergencyOverride — break-glass for unconscious ED patient\n");

    EmergencyOverride eo{std::chrono::hours(72)};

    auto er_actor   = ActorId::clinician("dr.iyer.ed.aiims");
    auto er_patient = PatientId::pseudonymous("p_unknown_admit_3a91");

    auto act = eo.activate(er_actor, er_patient,
                           "ED admit — unconscious post-MVA, no NoK present, "
                           "imaging needed before stabilisation");
    if (!act) {
        fmt::print(stderr, "  activate failed: {}\n", act.error().what());
        return 1;
    }
    auto& tok = act.value();
    fmt::print("    activated      : token_id={} actor={} patient={}\n",
               tok.token_id.substr(0, 16), er_actor.str(), er_patient.str());
    fmt::print("    deadline (72h) : {}\n", tok.backfill_deadline.iso8601());
    fmt::print("    pending_count  : {}\n", eo.pending_count());

    // Document the override within window with whatever the operator's
    // compliance regime accepts as evidence — here a signed-note id.
    auto bf = eo.backfill(tok.token_id, "note:ed-2026-04-28-iyer-mvA-3a91");
    if (!bf) {
        fmt::print(stderr, "  backfill failed: {}\n", bf.error().what());
        return 1;
    }
    fmt::print("    backfilled     : evidence_id=note:ed-2026-04-28-iyer-mvA-3a91\n");
    fmt::print("    completed      : {} · pending_count: {}\n\n",
               eo.completed_count(), eo.pending_count());

    // ── 2. FamilyGraph — parent for minor ───────────────────────────────
    fmt::print("§2  FamilyGraph — parent records authority over a minor\n");

    FamilyGraph fg;
    auto parent = PatientId::pseudonymous("p_parent_b821");
    auto minor  = PatientId::pseudonymous("p_minor_c4f0");

    auto rr = fg.record_relation(parent, minor, FamilyRelation::parent_for_minor);
    if (!rr) {
        fmt::print(stderr, "  record_relation failed: {}\n", rr.error().what());
        return 1;
    }
    fmt::print("    edge recorded  : {} → {} ({})\n",
               parent.str(), minor.str(),
               to_string(FamilyRelation::parent_for_minor));
    fmt::print("    can_consent_for: {}\n", fg.can_consent_for(parent, minor));
    fmt::print("    edges total    : {}\n", fg.total_relations());
    fmt::print("    summary        : {}\n\n", fg.summary_string());

    // ── 3. ConsentArtefact — wire-shape round trip ──────────────────────
    fmt::print("§3  ConsentArtefact — ABDM wire-shape round trip\n");

    ConsentRegistry registry;
    auto granted = registry.grant(
        minor,
        {Purpose::diagnostic_suggestion, Purpose::ambient_documentation},
        std::chrono::hours(24));
    if (!granted) {
        fmt::print(stderr, "  grant failed: {}\n", granted.error().what());
        return 1;
    }

    // Project the token into an ABDM artefact, sign + transmit shape.
    auto artefact = artefact_from_token(
        granted.value(),
        /*requester_id=*/ "hiu:apollo.bangalore",
        /*fetcher_id=*/   "hip:abha.gateway",
        /*artefact_id=*/  "abdm:cons:7af2-9c11-…");

    auto wire = to_abdm_json(artefact);
    fmt::print("    artefact JSON  : {} bytes\n", wire.size());
    fmt::print("    requester      : {}  fetcher: {}\n",
               artefact.requester_id, artefact.fetcher_id);

    // Consume from the wire (the inverse direction — receive an artefact
    // from a partner, project back to a registry-shaped token).
    auto round = from_abdm_json(wire);
    if (!round) {
        fmt::print(stderr, "  from_abdm_json failed: {}\n", round.error().what());
        return 1;
    }
    auto recovered = token_from_artefact(round.value());
    fmt::print("    round-tripped  : token_id_len={} · purposes={} · status=granted\n",
               recovered.token_id.size(), recovered.purposes.size());

    // Sanity: the round-trip preserves the consent-shape and patient.
    assert(recovered.patient.str() == minor.str());
    assert(recovered.purposes.size() == 2);

    fmt::print("\nok\n");
    return 0;
}
