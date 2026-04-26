# Asclepius wire schemas

Protobuf definitions for the Asclepius on-wire formats. The C++ runtime
itself does not depend on protobuf — the canonical on-disk form is JSON
(see [`docs/SPEC.md`](../docs/SPEC.md)). These schemas exist for:

1. **The `asclepius-svc` sidecar.** A gRPC transport over the same runtime
   so non-C++ hosts can sit on the same substrate. See
   [`asclepius/v1/sidecar.proto`](asclepius/v1/sidecar.proto).
2. **Cross-language ledger consumers.** Tools that ingest evidence bundles
   or replicate the ledger off-system can decode entries with these
   messages instead of writing a JSON parser. The `body_json` field
   carries the canonical JSON unchanged so that proto-decoded entries
   reverify against the on-disk Ed25519 signature byte-for-byte.

## Layout

```
asclepius/v1/
  common.proto      Hash, Time, Signature, Id, enums (Purpose, Status, Severity)
  ledger.proto      LedgerEntry envelope + per-event body messages
  evidence.proto    EvidenceManifest, ModelMetrics, DriftReport
  sidecar.proto     gRPC service definition for asclepius-svc
```

## Versioning

`asclepius.v1` is **stable for the v1.x runtime**. Within v1:

* New messages and new fields are additive.
* Removing or renaming fields is a v2 break and requires a new package
  (`asclepius.v2`).
* The `body_json` string and the signature scheme are part of the
  invariants that hold across the v1.x line — a v1.0 bundle verifies on
  a v1.5 verifier and vice versa.

## Build (planned)

The `proto/` directory is not currently wired into the CMake build —
protobuf is a transitive dependency that the in-tree library does not
need. When `ASCLEPIUS_BUILD_SVC=ON` lands, `cmake/Dependencies.cmake`
will fetch `protobuf` and `grpc` and generate the bindings into
`build/proto/`. Until then these `.proto` files are the authoritative
contract.

Manual generation:

```sh
protoc -I=proto \
       --cpp_out=gen \
       --grpc_out=gen \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       proto/asclepius/v1/*.proto
```

## What is NOT here

* The on-disk SQLite ledger schema is in `docs/SPEC.md`. It is not a
  protobuf concern.
* The evidence-bundle tar layout is in `docs/SPEC.md`. The proto messages
  describe the structured contents of files inside the tar, not the tar
  itself.
* Policy DSL. Today policies are C++ classes; a YAML/JSON DSL is on the
  roadmap and will get its own schema (likely JSON-Schema rather than
  protobuf).
