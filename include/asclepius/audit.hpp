// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_AUDIT_HPP
#define ASCLEPIUS_AUDIT_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "asclepius/core.hpp"
#include "asclepius/hashing.hpp"

namespace asclepius {

// ---- KeyStore ------------------------------------------------------------
//
// An Ed25519 signing keypair. Keys are 32-byte public, 64-byte secret. The
// secret is held in locked memory where the platform supports it.

class KeyStore {
public:
    static constexpr std::size_t pk_bytes  = 32;
    static constexpr std::size_t sk_bytes  = 64;
    static constexpr std::size_t sig_bytes = 64;

    KeyStore(const KeyStore&)            = delete;
    KeyStore& operator=(const KeyStore&) = delete;
    KeyStore(KeyStore&&) noexcept;
    KeyStore& operator=(KeyStore&&) noexcept;
    ~KeyStore();

    // Generate a fresh keypair from a CSPRNG.
    static KeyStore generate();

    // Reconstruct from a 32-byte seed. The seed must be high-entropy.
    static Result<KeyStore> from_seed(std::span<const std::uint8_t, 32> seed);

    // Load from a PEM-style serialized form produced by serialize().
    static Result<KeyStore> deserialize(std::string_view encoded);
    std::string             serialize() const;

    // The key id is the hex of the public key truncated to 16 hex chars.
    std::string                            key_id() const;
    // 8-byte BLAKE2b fingerprint of the public key, hex-encoded
    // (16 hex chars). Cheaper to display than the full pubkey but
    // collision-resistant for human comparison. Same shape as key_id()
    // but derived from a hash, so safer to compare across deployments.
    std::string                            fingerprint() const;
    std::array<std::uint8_t, pk_bytes>     public_key() const;
    std::array<std::uint8_t, sig_bytes>    sign(Bytes message) const;

    static bool verify(Bytes                                message,
                       std::span<const std::uint8_t, sig_bytes> signature,
                       std::span<const std::uint8_t, pk_bytes>  pk);

private:
    KeyStore() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---- Ledger entry --------------------------------------------------------

struct LedgerEntryHeader {
    std::uint64_t seq{};
    Time          ts{};
    Hash          prev_hash{};
    Hash          payload_hash{};
    std::string   actor;
    std::string   event_type;
    std::string   tenant;
};

struct LedgerEntry {
    LedgerEntryHeader                                  header;
    std::string                                        body_json;  // canonical JSON
    std::array<std::uint8_t, KeyStore::sig_bytes>      signature{};
    std::string                                        key_id;

    Hash entry_hash() const;  // hash(header || body || sig || key_id)
};

// ---- Checkpoint (declared before Ledger so Ledger::checkpoint() can name it) -

struct LedgerCheckpoint {
    std::uint64_t                                 seq{};
    Hash                                          head_hash{};
    Time                                          ts{};
    std::string                                   key_id;
    std::array<std::uint8_t, KeyStore::sig_bytes> signature{};
    std::array<std::uint8_t, KeyStore::pk_bytes>  public_key{};

    std::string to_json() const;
    static Result<LedgerCheckpoint> from_json(std::string_view s);
};

Result<void> verify_checkpoint(const LedgerCheckpoint& cp);

// ---- Ledger --------------------------------------------------------------
//
// An append-only, Merkle-chained, Ed25519-signed event log persisted to a
// SQLite database. The chain head can be checkpointed externally for
// off-system attestation.

class Ledger {
public:
    Ledger(const Ledger&)            = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) noexcept;
    Ledger& operator=(Ledger&&) noexcept;
    ~Ledger();

    // Open or create a SQLite-backed ledger at the given filesystem path.
    // If a key is not provided one is generated and stored alongside the
    // SQLite file (0600).
    static Result<Ledger> open(std::filesystem::path path);
    static Result<Ledger> open(std::filesystem::path path, KeyStore key);

    // Append a new event. body must be a canonicalizable JSON value.
    Result<LedgerEntry> append(std::string event_type,
                               std::string actor,
                               nlohmann::json body,
                               std::string tenant = "");

    // One spec per entry in a batch.
    struct AppendSpec {
        std::string    event_type;
        std::string    actor;
        nlohmann::json body;
        std::string    tenant;
    };

    // Append N entries atomically. Either all entries land in the chain
    // (ordered, gap-free, prev_hash-linked) or none of them do — the
    // backend transaction rolls back on failure. Returns the appended
    // entries in seq-ascending order. Subscribers fire once per entry,
    // after the entire batch has committed.
    //
    // Empty input is a valid no-op; returns an empty vector.
    Result<std::vector<LedgerEntry>> append_batch(std::vector<AppendSpec> specs);

    // Read an entry by sequence number.
    Result<LedgerEntry> at(std::uint64_t seq) const;

    // Read just the timestamp at a given seq. Sugar over at(seq).header.ts
    // for callers that only need the time anchor without paying for the
    // full entry copy. Returns invalid_argument if seq == 0 or
    // seq > length().
    Result<Time> ts_at_seq(std::uint64_t seq) const;

    // Body byte size of the entry at a given seq. Sugar over
    // at(seq).body_json.size() for callers that only need the size and
    // not the bytes themselves. Returns invalid_argument if seq == 0 or
    // seq > length(). Useful for capacity probes that pin a specific
    // entry rather than scanning the whole chain.
    Result<std::uint64_t> body_byte_size_at(std::uint64_t seq) const;

    // Read the last n entries (most recent first).
    Result<std::vector<LedgerEntry>> tail(std::size_t n) const;

    // Read entries within [start, end) seq range.
    Result<std::vector<LedgerEntry>> range(std::uint64_t start, std::uint64_t end) const;

    // Read entries by ts_ns in [from, to). Half-open. Index-backed.
    Result<std::vector<LedgerEntry>> range_by_time(Time from, Time to) const;

    // Tenant-scoped reads. Filter to entries whose header.tenant matches
    // the supplied string. The empty tenant ("") is its own scope; passing
    // "" returns only those entries, not all entries. Uses an index on
    // (tenant, seq) for O(log n + k) on each backend.
    Result<std::vector<LedgerEntry>> tail_for_tenant(const std::string& tenant,
                                                     std::size_t n) const;
    Result<std::vector<LedgerEntry>> range_for_tenant(const std::string& tenant,
                                                      std::uint64_t start,
                                                      std::uint64_t end) const;

    // (oldest_seq, newest_seq) bounds of a tenant's entries within the chain.
    // Returns (0, 0) if no entries match. The empty tenant ("") is its own
    // scope. Useful for "show me where this tenant's chain lives" — callers
    // can plan paginated reads without first loading every entry. Implemented
    // via paginated tenant-scoped range queries (chunked at 1024) so memory
    // stays bounded for large chains.
    Result<std::pair<std::uint64_t, std::uint64_t>>
        seq_range_for_tenant(const std::string& tenant) const;

    // Verify the entire chain: each entry's prev_hash matches the previous,
    // each signature matches the registered public key, payload_hash matches.
    // Streams through entries one at a time so memory usage is O(1) regardless
    // of chain length.
    Result<void> verify() const;

    // True iff the chain has no gaps: every seq from 1 to length() is
    // present and prev_hash linkage is intact (no signature work). Wraps
    // verify() for callers who only want the bool — backend errors and
    // integrity failures both surface as false. Empty chains return true
    // (no gaps to find). Used by status pages and feature gates that
    // need a one-line health probe without a Result branch.
    bool is_chain_continuous() const;

    // Multi-threaded verify. Streams entries, verifies signatures and
    // payload-hash recomputation in parallel across `threads` workers,
    // then walks the chain sequentially in the calling thread to check
    // prev_hash continuity and gap-free seq.
    //
    // Equivalent semantically to verify(): same Result, same error
    // codes, same diagnostics. ~Nx faster on a chain large enough that
    // signature verification dominates (>~10k entries) when run on N
    // cores.
    //
    // threads=0 picks std::thread::hardware_concurrency() at runtime.
    // For tiny chains (<512 entries) falls back to single-threaded
    // verify() to avoid pool overhead.
    Result<void> verify_parallel(unsigned threads = 0) const;

    // Current chain head (hash of the most recent entry, or zero if empty).
    Hash          head() const;
    std::uint64_t length() const;

    // Public verification key bytes for this ledger's signer.
    std::array<std::uint8_t, KeyStore::pk_bytes> public_key() const;
    std::string                                  key_id() const;

    // Sign an arbitrary attestation (e.g., an evidence-bundle manifest) with
    // the same key that signs ledger entries. The signature can be verified
    // against public_key() with KeyStore::verify. This is intentionally
    // narrow: forging a ledger entry would still require matching seq,
    // prev_hash, and the canonical entry encoding, none of which this method
    // generates.
    std::array<std::uint8_t, KeyStore::sig_bytes> sign_attestation(Bytes message) const;

    // Produce a checkpoint for the current chain head. Self-contained
    // proof that this signing key (whose pubkey is embedded) attested
    // to the seq/head_hash pair at the returned ts.
    LedgerCheckpoint checkpoint() const;

    // ---- Stats / observability ------------------------------------------
    //
    // A summary view of the ledger suitable for dashboards, capacity
    // planning, and `status` endpoints. All fields are derivable from
    // existing API but assembling them in one O(1)-or-bounded call lets
    // operators avoid 5 round-trips per refresh.

    struct Stats {
        std::uint64_t entry_count{};
        Hash          head_hash{};
        std::uint64_t oldest_seq{};
        std::uint64_t newest_seq{};
        Time          oldest_ts{};
        Time          newest_ts{};
        std::uint64_t total_body_bytes{};   // sum of body_json lengths
        std::uint64_t avg_body_bytes{};     // total_body_bytes / entry_count, or 0
        std::string   key_id;

        std::string to_json() const;
    };

    Result<Stats> stats() const;

    // Per-tenant variant. Same shape as stats() but counts only entries
    // whose header.tenant matches the supplied string. The empty tenant
    // ("") is its own scope. Implemented via paginated tenant-scoped
    // range queries so memory stays bounded for large chains.
    Result<Stats> stats_for_tenant(const std::string& tenant) const;

    // Sum of body_json sizes (bytes) across the entire chain. Streams via
    // for_each; O(1) memory regardless of chain length. Distinct from the
    // on-disk SQLite file size — this is the *logical* content size, useful
    // for capacity planning that ignores SQLite page overhead, indexes, and
    // WAL. Empty chain returns 0.
    Result<std::uint64_t> cumulative_body_bytes() const;

    // Entries-per-second across the chain, measured as
    // (length - 1) / ((newest_ts - oldest_ts) seconds). Returns 0.0 when
    // length < 2 (no rate computable from a single point). Useful for
    // "how busy is this ledger?" probes and capacity dashboards. Two
    // single-row lookups (oldest + newest); cheap regardless of chain
    // length.
    Result<double> seq_density() const;

    // Count entries grouped by event_type. Returns a map keyed by the
    // header.event_type string. O(n) scan over the chain via for_each;
    // O(1) memory aside from the result map. Useful for dashboards
    // ("how many inferences vs drift events today?") and for spotting
    // unexpected event types from custom integrations.
    Result<std::unordered_map<std::string, std::uint64_t>>
        count_by_event_type() const;

    // Sorted list of every distinct header.event_type that has ever been
    // appended. O(n) scan via for_each into a set, copied to a sorted
    // vector. Empty chain returns the empty vector. Companion to
    // count_by_event_type — that one returns counts; this one is a cheap
    // enumeration for dashboards that need to populate event-type filter
    // dropdowns without also computing counts.
    Result<std::vector<std::string>> distinct_event_types() const;

    // ---- Forensic lookup ------------------------------------------------
    //
    // Find the entry whose body contains "inference_id" == id. Used by
    // incident response when a downstream report names a specific
    // inference and the ledger needs to be the source of truth. Returns
    // not_found if no such entry exists. O(n) scan via for_each — bounded
    // by chain length, run off the hot path.
    Result<LedgerEntry> find_by_inference_id(std::string_view id) const;

    // Return the last `n` entries whose header.actor matches the supplied
    // string, most-recent first. Used for "what did this clinician do
    // today?" style forensic queries; the system: actor strings
    // (system:drift_monitor, system:consent_registry) are also matchable
    // for cross-event-type tracing. O(n) scan via for_each, but stops
    // accumulating once n matches are found.
    //
    // Empty actor returns invalid_argument; n=0 returns the empty vector
    // (cheap no-op).
    Result<std::vector<LedgerEntry>> tail_by_actor(std::string_view actor,
                                                   std::size_t      n) const;

    // Most recent (largest seq) entry whose header.actor matches the
    // supplied string. Sugar over tail_by_actor(actor, 1). Empty actor
    // returns invalid_argument; no match returns not_found. Used by
    // dashboards that just want the latest-touch row per actor without
    // unpacking a single-element vector.
    Result<LedgerEntry> most_recent_for_actor(std::string_view actor) const;

    // Earliest (smallest seq) entry whose header.actor matches the
    // supplied string. Mirror of most_recent_for_actor; stops the
    // for_each scan on the first hit so cost is O(k) where k is the seq
    // of the first match. Empty actor returns invalid_argument; no
    // match returns not_found. Used by dashboards that show "first
    // seen" timestamps per actor and by replay paths anchored on an
    // actor's genesis row.
    Result<LedgerEntry> find_first_for_actor(std::string_view actor) const;

    // Return entries whose header.event_type matches `event_type`,
    // ordered by seq ascending. O(n) scan via for_each. Empty
    // event_type returns invalid_argument; unknown event_type returns
    // empty vector. Used by dashboards ("show me all drift.crossed
    // events") and by the consent replay path on Runtime restart.
    Result<std::vector<LedgerEntry>>
        range_by_event_type(std::string_view event_type) const;

    // Last `n` entries of a given event type, most recent first. Pairs
    // with range_by_event_type for streaming dashboards: range gives
    // the full set; tail gives just the latest.
    Result<std::vector<LedgerEntry>>
        tail_by_event_type(std::string_view event_type, std::size_t n) const;

    // Longest consecutive run of `event_type` entries in the chain (in
    // seq-ascending order). Useful for detecting bursts ("we had 50
    // drift.crossed events in a row"). Returns 0 if no entries match.
    // Empty event_type returns invalid_argument. O(n) scan via for_each;
    // O(1) memory.
    Result<std::uint64_t>
        longest_run_of_event_type(std::string_view event_type) const;

    // First-match lookup of an event_type — the oldest entry whose
    // header.event_type matches. Stops the for_each scan on the first
    // hit so cost is O(k) where k is the seq of the first match. Empty
    // event_type returns invalid_argument; no-match returns not_found.
    // Used by replay paths that need the genesis entry of a stream
    // (e.g. "find the first consent.granted for this tenant") and by
    // dashboards that show "first seen" timestamps per event class.
    Result<LedgerEntry>
        find_first_by_event_type(std::string_view event_type) const;

    // Cheap O(n)-worst-case existence check: does the chain hold any
    // entry with header.event_type == event_type. Stops the scan on
    // the first hit. Empty event_type returns false (no allocation,
    // no scan). Used by feature gates ("has this ledger ever seen a
    // drift.crossed?") that don't need the entries themselves.
    bool has_event_type(std::string_view event_type) const;

    // Cheap O(n)-worst-case existence check: does the chain hold any
    // entry with header.actor == actor. Stops the scan on the first
    // hit. Empty actor returns false (no allocation, no scan). Actor
    // analog of has_event_type — used by feature gates and audit
    // probes ("has this clinician ever touched the ledger?") that
    // don't need the entries themselves.
    bool any_actor_matches(std::string_view actor) const;

    // Return every entry whose header.actor matches the supplied
    // string, ordered by seq ascending. Complement to tail_by_actor
    // (which is most-recent-first and capped at n). O(n) scan via
    // for_each. Empty actor returns invalid_argument; unknown actor
    // returns the empty vector. Used by audit dashboards that need
    // the full activity history of one actor rather than just the
    // recent slice.
    Result<std::vector<LedgerEntry>>
        range_by_actor(std::string_view actor) const;

    // Composite filter: entries whose header.actor matches `actor` AND
    // whose header.ts falls in [from, to). Half-open interval.
    // Seq-ascending. Empty actor returns invalid_argument; from > to
    // returns invalid_argument. O(n) scan via for_each. Used by
    // forensic queries like "what did this clinician do during the
    // night shift?" — counterpart to range_for_patient_in_window but
    // scoped to actor instead of patient.
    Result<std::vector<LedgerEntry>>
        range_for_actor_in_window(std::string_view actor,
                                  Time from, Time to) const;

    // First `n` entries (oldest), seq-ascending. Complement to
    // tail(n) which returns the most-recent slice. n=0 returns the
    // empty vector (cheap no-op). Stops the for_each scan early once
    // n entries have been collected, so cost is O(n) regardless of
    // chain length.
    Result<std::vector<LedgerEntry>>
        oldest_n(std::size_t n) const;

    // Composite filter: entries whose header.event_type matches
    // event_type AND whose header.tenant matches tenant. Empty
    // event_type returns invalid_argument. Empty tenant ("") is its
    // own scope, matching only entries explicitly carrying it. O(n)
    // scan via for_each.
    Result<std::vector<LedgerEntry>>
        filter(std::string_view event_type, const std::string& tenant) const;

    // Sum of body_json sizes (bytes) for entries whose header.tenant
    // matches the supplied string. The empty tenant ("") is its own
    // scope. O(n) over the matching subset via paginated tenant-scoped
    // range queries (chunked at 1024) so memory stays bounded for
    // large chains. Useful for per-tenant capacity dashboards.
    Result<std::uint64_t> byte_size_for_tenant(const std::string& tenant) const;

    // Distinct tenant strings appearing in the chain, sorted alphabetically.
    // The empty tenant ("") is included if any entries carry it. O(n) scan
    // via for_each. Used by tenant-pickers in dashboards and by routines
    // that need to fan out per-tenant work without an a-priori tenant list.
    Result<std::vector<std::string>> tenants() const;

    // Distinct actor strings appearing in the chain, sorted alphabetically.
    // O(n) scan via for_each. Counterpart to tenants(); used by audit
    // dashboards to populate actor-filter dropdowns and by forensic tools
    // that need to enumerate "who has done anything in this ledger".
    Result<std::vector<std::string>> actors() const;

    // Count of distinct header.actor values across the chain. Streams
    // via for_each into a set; O(distinct actors) memory. Empty chain
    // returns 0. Counterpart to actors() — that returns the names; this
    // returns just the cardinality, cheaper for status pages and probes
    // that only need the number.
    Result<std::uint64_t> distinct_actors_count() const;

    // Inference-committed entries whose body's "model" field equals
    // `model_id`. O(n) scan; cheap substring prefilter before JSON parse,
    // mirroring range_by_patient. Used by model-scoped audit views ("show
    // me every inference run by model X"). Empty model_id returns
    // invalid_argument.
    Result<std::vector<LedgerEntry>>
        range_by_model(std::string_view model_id) const;

    // Cheap count of entries whose ts ∈ [from, to). O(n) scan via for_each
    // but only a counter is kept — no entry copies, O(1) memory. Half-open
    // interval matches range_by_time and tail_in_window. Used by rate
    // dashboards and SLO probes that just need a number, not the entries
    // themselves.
    Result<std::uint64_t> count_in_window(Time from, Time to) const;

    // First `n` entries (oldest), seq-ascending, scoped to a tenant.
    // Complement to tail_for_tenant (which is most-recent-first). The
    // empty tenant ("") is its own scope. n=0 returns the empty vector
    // (cheap no-op). Uses paginated tenant-scoped range queries
    // (chunked at 1024) so memory stays bounded for large chains.
    Result<std::vector<LedgerEntry>>
        oldest_n_for_tenant(const std::string& tenant, std::size_t n) const;

    // Composite filter: entries whose header.ts falls in [from, to)
    // AND whose header.event_type matches `event_type`. Half-open
    // interval. Empty event_type returns invalid_argument; from > to
    // returns invalid_argument. Seq-ascending. O(n) scan via for_each.
    // Used by SLO probes that need to bound the entries returned by
    // both time and class ("how many drift.crossed events in the last
    // hour, with bodies?").
    Result<std::vector<LedgerEntry>>
        events_between(Time from, Time to, std::string_view event_type) const;

    // Cheap O(n)-worst-case existence check: would
    // find_by_inference_id(id) succeed? Stops the scan on the first
    // hit. Empty id returns false (no allocation, no scan). Used by
    // feature gates and idempotency probes that don't need the entry
    // itself.
    bool has_inference_id(std::string_view id) const;

    // Compact JSON containing length, head_hash hex, key_id,
    // fingerprint, oldest_ts iso8601 (or "" if empty), newest_ts
    // iso8601 (or ""). Sugar for handing a third party a single
    // self-describing string. Different from attest() (which is the
    // structured Attestation; this is the text form expanded with
    // timestamp bounds).
    std::string attestation_json() const;

    // Single-line JSON object containing length, head_hex, key_id,
    // key_fingerprint (from KeyStore::fingerprint), and head_signature
    // (hex of sign_attestation(head.bytes)). Useful for status pages
    // that want a one-line proof-of-current-state without going through
    // the full Checkpoint struct: cheaper than checkpoint() (no
    // canonical encoding step, no embedded pubkey) but stronger than
    // attestation_json() (includes a real Ed25519 signature over the
    // current head bytes).
    std::string head_attestation_json() const;

    // Sum of body_json sizes (bytes) grouped by header.tenant. O(n) scan
    // via for_each; result map memory is O(distinct tenants). The empty
    // tenant ("") is its own bucket if any entries carry it. Complements
    // byte_size_for_tenant — that asks one tenant; this returns all in a
    // single pass for capacity dashboards and per-tenant billing rollups.
    Result<std::unordered_map<std::string, std::uint64_t>>
        byte_size_per_tenant() const;

    // Top `n` actors by entry count, most-active first. Ties break
    // alphabetically so output is deterministic across runs. n=0 returns
    // the empty vector (cheap no-op). O(n) scan via for_each into a
    // dedup map, then a partial sort over distinct actors. Used by audit
    // dashboards ("who's been busiest in this ledger?") and by anomaly
    // probes that flag unexpected actor concentration.
    Result<std::vector<std::pair<std::string, std::uint64_t>>>
        most_active_actors(std::size_t top_n) const;

    // Verify a sub-range of the chain [start, end). Same correctness
    // guarantees as verify(): prev_hash continuity, payload-hash match,
    // ed25519 signature match. Cheaper than verify() when only a subset
    // matters (e.g. evidence bundle attestation, post-restart spot
    // check). Returns invalid_argument if start >= end or end > length.
    Result<void> verify_range(std::uint64_t start, std::uint64_t end) const;

    // Single BLAKE2b-256 digest summarizing entries in [start, end).
    // Computed by feeding each entry's entry_hash() bytes (in seq-ascending
    // order) into a streaming Hasher. Two ledgers with byte-identical
    // content over the same range produce the same checksum, so replicas
    // can confirm they're in sync without shipping the entries themselves.
    // Returns invalid_argument for bad bounds (start >= end, end > length+1).
    // The empty range (start == end) and an empty chain both return
    // Hash::zero().
    Result<Hash> checksum_range(std::uint64_t start, std::uint64_t end) const;

    // Find the chain head as it existed at-or-before time `t`. Returns
    // {seq=0, head_hash=zero} for empty chains or for a `t` earlier than
    // the first entry's timestamp. Used to anchor retroactive checkpoints
    // (e.g. "what was the chain head one hour ago?").
    struct HistoricalHead {
        std::uint64_t seq{};
        Hash          head_hash{};
        Time          ts{};
    };
    Result<HistoricalHead> head_at_time(Time t) const;

    // Historical head as of a particular seq. Returns the entry_hash of
    // the entry at `seq` and the seq itself, or invalid_argument if
    // seq > length() or seq == 0.
    Result<HistoricalHead> head_at_seq(std::uint64_t seq) const;

    // Oldest entry (seq == 1). not_found on an empty chain. Cheap
    // single-row lookup; complement to newest_entry().
    Result<LedgerEntry> oldest_entry() const;

    // Duration from the oldest entry's ts to Time::now(). Returns 0ns
    // if the chain is empty (no oldest entry to anchor against).
    // Useful for retention dashboards ("how old is the oldest record
    // on this ledger?") and SLO probes that age out chains beyond a
    // threshold. Single-row lookup; cheap.
    Result<std::chrono::nanoseconds> age_of_oldest() const;

    // Newest entry (seq == length()). not_found on an empty chain.
    // Cheap single-row lookup; complement to oldest_entry(). Equivalent
    // to at(length()) but avoids the caller having to read length()
    // first and handle the empty case.
    Result<LedgerEntry> newest_entry() const;

    // Largest seq whose entry.ts <= t. Returns 0 for an empty chain
    // or for a `t` earlier than the first entry's timestamp. Useful
    // anchor for "what was the chain like at time T" queries — pairs
    // with head_at_time which returns the head_hash, while this returns
    // just the seq for cheaper time-based slicing.
    Result<std::uint64_t> seq_at_time(Time t) const;

    // Compact attestation that a specific entry at `seq` is part of the
    // current chain. `chain_to_head` is the sequence of entry_hashes
    // from seq+1 to head_seq, in seq-ascending order, so a verifier can
    // replay the prev_hash linkage from the named entry forward to the
    // head. Used for third-party attestations of single entries without
    // shipping the entire chain (or even neighboring bodies).
    //
    // seq == 0 or seq > length() returns invalid_argument.
    struct InclusionProof {
        std::uint64_t     seq{};
        Hash              entry_hash{};
        std::vector<Hash> chain_to_head;  // entry_hashes for seq+1 .. head_seq
        std::uint64_t     head_seq{};
        Hash              head_hash{};

        std::string to_json() const;
    };
    Result<InclusionProof> inclusion_proof(std::uint64_t seq) const;

    // Chain of entry_hashes from `seq` (inclusive) up to the chain head,
    // in seq-ascending order. Element 0 is the entry's own hash;
    // subsequent elements link forward to head. Useful for an external
    // verifier to confirm a specific entry is part of the current chain
    // without shipping the full bodies. Returns invalid_argument if
    // seq == 0 or seq > length(). Hashes are pulled by entry_hash() on
    // each entry in the range [seq, length()+1).
    Result<std::vector<Hash>> merkle_proof_path(std::uint64_t seq) const;

    // Last `n` entries whose header.ts falls in [from, to), most-recent
    // first. Half-open interval. n=0 returns the empty vector;
    // from > to returns invalid_argument. O(n) scan via for_each with
    // a bounded ring buffer; memory is O(min(n, matches)).
    Result<std::vector<LedgerEntry>>
        tail_in_window(Time from, Time to, std::size_t n) const;

    // Last `n` entries within the seq window [start, end), most-recent
    // first. Half-open. n=0 returns the empty vector; start >= end
    // returns the empty vector (cheap no-op, no error). Implementation:
    // pulls the seq range via select_range, keeps the last `n` via a
    // bounded ring buffer, then reverses. Memory is O(min(n, range)).
    // Pairs with tail_in_window for callers who already know the seq
    // bounds and want to skip the timestamp-based scan.
    Result<std::vector<LedgerEntry>>
        tail_in_seq_range(std::uint64_t start, std::uint64_t end,
                          std::size_t n) const;

    // Cheap O(1) existence check for a sequence number. True iff the
    // chain currently holds an entry at `seq`. noexcept; never reads
    // the backend.
    bool has_entry(std::uint64_t seq) const noexcept;

    // Cheap O(1) check: does the chain currently hold any entry whose
    // seq > `seq`. Computed via length() comparison; never reads the
    // backend. Used by replica-tail loops that want a "is there anything
    // new?" probe before issuing the heavier events_after_seq query.
    bool has_event_after_seq(std::uint64_t seq) const;

    // Compact attestation of the current chain head. Bundles the
    // length, head_hash, signing key id, and the key fingerprint
    // (a hashed alias of the public key, see KeyStore::fingerprint).
    // Cheaper than checkpoint() — no signature, no Ed25519 work — and
    // useful for status endpoints, dashboards, and bundle headers.
    struct Attestation {
        std::uint64_t length{};
        Hash          head{};
        std::string   key_id;
        std::string   fingerprint;

        std::string to_json() const;
    };
    Attestation attest() const;

    // Inference-committed entries whose body's "patient" field matches
    // the supplied PatientId. O(n) scan; cheap substring prefilter
    // before JSON parse. Used by patient-scoped audit views ("show me
    // every inference run on patient X").
    Result<std::vector<LedgerEntry>>
        range_by_patient(const std::string& patient) const;

    // Last `n` inference-committed entries whose body's "patient" field
    // matches the supplied PatientId, most-recent-first. Ring-buffer
    // scan — memory is O(min(n, matches)). Empty patient returns
    // invalid_argument; n=0 returns the empty vector. Pairs with
    // range_by_patient (oldest-first, full set) for streaming patient
    // audit views.
    Result<std::vector<LedgerEntry>>
        tail_for_patient(const std::string& patient, std::size_t n) const;

    // Inference-committed entries for a patient whose header.ts falls
    // in [from, to). Half-open. Seq-ascending. Empty patient or
    // from > to returns invalid_argument. Combined patient + time
    // window scan in a single pass for "what inferences ran on this
    // patient last week?" forensic queries.
    Result<std::vector<LedgerEntry>>
        range_for_patient_in_window(const std::string& patient,
                                    Time from, Time to) const;

    // Entries whose seq > after_seq, in seq-ascending order. Used by
    // replicas / followers tailing the chain to pull only the deltas
    // they have not yet observed. after_seq >= length() returns the
    // empty vector (caller is already caught up). after_seq == 0
    // returns the entire chain.
    Result<std::vector<LedgerEntry>>
        events_after_seq(std::uint64_t after_seq) const;

    // Content-address: return the entry_hash for the entry at `seq`.
    // seq == 0 or seq > length() returns invalid_argument. Cheap
    // single-row lookup; the hash is recomputed from the stored
    // entry rather than read from a cached column.
    Result<Hash> content_address(std::uint64_t seq) const;

    // ---- Subscription ---------------------------------------------------
    //
    // Register a callback that fires after each successful append, on the
    // appender's thread. Returned handle owns the registration; destroying
    // it unsubscribes. Multiple subscribers are supported and called in
    // registration order.
    //
    // Callbacks must not call back into Ledger::append from the same
    // thread (would re-enter the append mutex and deadlock). They MAY
    // hand the entry to a queue / thread-pool / external system.

    using Subscriber = std::function<void(const LedgerEntry&)>;

    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&) noexcept;
        Subscription& operator=(Subscription&&) noexcept;
        ~Subscription();

    private:
        friend class Ledger;
        Subscription(Ledger* ledger, std::uint64_t id) : ledger_(ledger), id_(id) {}
        Ledger*       ledger_ = nullptr;
        std::uint64_t id_     = 0;
    };

    Subscription subscribe(Subscriber cb);
    void         unsubscribe(std::uint64_t id);

private:
    Ledger() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---- JSONL export / import ----------------------------------------------
//
// One JSON object per line, emitted in seq-ascending order. Each line is
// the canonical-JSON form of a LedgerEntry: every header field, the body,
// the hex signature, the key id. Suitable for `jq`-driven auditing,
// cold-storage archive, or substrate-agnostic transfer.
//
// Importing into a fresh ledger preserves seq, prev_hash, signatures,
// entry hashes — i.e. the destination's verify() passes against the
// same KeyStore the source used. Refuses non-empty destinations.

struct LedgerExportStats {
    std::uint64_t entries_written = 0;
    Hash          last_entry_hash{};
};

struct LedgerImportStats {
    std::uint64_t entries_imported = 0;
    Hash          dest_head{};
};

class LedgerJsonl {
public:
    // Stream every entry in src, seq ASC, to out_path as JSONL. The file
    // is created (or truncated) and closed cleanly even on early exit.
    static Result<LedgerExportStats>
    export_to(const std::string& src_uri, const std::string& out_path);

    // Read JSONL from in_path, verify each entry's signature against the
    // supplied KeyStore's public key, and append to dst. Stops on the
    // first verification failure with an integrity error.
    static Result<LedgerImportStats>
    import_to(const std::string& in_path, const std::string& dst_uri, KeyStore key);
};

// ---- LedgerMigrator -----------------------------------------------------
//
// Copy a chain from one SQLite ledger to another, byte-for-byte. The
// destination receives the same entries in the same order with the same
// signatures, so the destination's verify() passes against the same public
// key the source used. Useful for cold-storage snapshots and chain
// relocation.

struct LedgerMigrationStats {
    std::uint64_t entries_copied = 0;
    Hash          source_head{};
    Hash          dest_head{};
};

class LedgerMigrator {
public:
    // Copy every entry from src to dst. dst must be empty; otherwise the
    // chain would fork at the first entry. Verifies the source mid-stream
    // and stops with an integrity error if the source chain is broken.
    static Result<LedgerMigrationStats>
    copy(const std::string& src_uri, const std::string& dst_uri, KeyStore key);
};

}  // namespace asclepius

#endif  // ASCLEPIUS_AUDIT_HPP
