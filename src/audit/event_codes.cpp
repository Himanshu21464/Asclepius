// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
//
// Round 92 — well-known event-type codes. The kernel's event_type field is
// free-form by design; this module just enumerates the canonical names that
// the India healthtech profile expects so dashboards and conformance tests
// can agree on what counts as a "kernel-known" event vs a caller-defined
// custom event.

#include "asclepius/audit.hpp"

#include <algorithm>
#include <array>

namespace asclepius {

namespace {

// Sorted list, kept in lockstep with the constants in events:: above.
// Adding a new well-known event is a governance event — update the
// `events::` constants, this list, and decisions.html in the same change.
constexpr std::array<std::string_view, 12> kKnownEvents = {
    events::bill_audited,
    events::consent_artefact_issued,
    events::consent_artefact_revoked,
    events::emergency_override_activated,
    events::emergency_override_backfilled,
    events::human_attestation,
    events::prescription_parsed,
    events::sample_collected,
    events::sample_resulted,
    events::substitution_event,
    events::tele_consult_closed,
    events::triage_decision,
};

}  // namespace

bool is_well_known_event(std::string_view event_type) noexcept {
    // Linear scan is fine — N=12, branch-predictor-friendly, no allocations.
    for (auto e : kKnownEvents) {
        if (e == event_type) return true;
    }
    return false;
}

std::vector<std::string_view> well_known_events() {
    std::vector<std::string_view> out{kKnownEvents.begin(), kKnownEvents.end()};
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace asclepius
