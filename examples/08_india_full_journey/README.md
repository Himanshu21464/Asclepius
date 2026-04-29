# Example 08 — India full journey

End-to-end demonstration that ties together every primitive shipped in
rounds 90&ndash;99 of the India healthtech profile. A single synthetic
patient flows through a realistic care pathway:

| Step | Primitive(s) used                                      | What it demonstrates |
|------|--------------------------------------------------------|----------------------|
| 1    | `FamilyGraph`                                          | adult-child records authority over an elder parent |
| 2    | `ConsentArtefact`, `ConsentRegistry`, `events::consent_artefact_issued`, `append_consent_artefact_issued` | ABDM-shaped consent issued and anchored in the ledger |
| 3    | `CalibrationMonitor`                                   | triage model's rolling sensitivity / specificity |
| 4    | `EmergencyOverride`, `events::emergency_override_*`, `append_emergency_override_*` | DPDP § 7 break-glass with backfill |
| 5    | `TeleConsultEnvelope`, `events::tele_consult_closed`, `append_tele_consult` | two-party signed specialist consult |
| 6    | `BillAuditBundle`, `events::bill_audited`, `append_bill_audit` | itemised bill audit vs CGHS-2025 reference |
| 7    | `SampleIntegrityBundle`, `events::sample_collected`, `events::sample_resulted`, `append_sample_integrity` | cold-chain attested chain-of-custody |
| 8    | `CarePathAttestation`, `access::Constraint`, `append_care_path` | care-path decision attested |
| 9    | `Ledger::verify`                                       | end-to-end Merkle chain integrity |

Every step writes one or more entries to a Merkle-chained, Ed25519-signed
ledger. After step 9 the chain length is printed alongside the chain head
hash; the chain verifies in a single call.

Vendor-neutral, India-shaped, no network: every primitive runs locally
against in-memory registries plus a SQLite ledger. The whole demo runs
in well under a second.

## Run

```sh
./build/examples/08_india_full_journey/example_india_full_journey
```

## Substrate posture

The example is *one possible composition*. The kernel does not prescribe
this care pathway; an operator running an EU/US deployment never
instantiates these types. The India profile compiles in but is opt-in
per step.
