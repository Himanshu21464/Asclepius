// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_TELEMETRY_HPP
#define ASCLEPIUS_TELEMETRY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "asclepius/core.hpp"

namespace asclepius {

// ---- Histogram ----------------------------------------------------------
//
// A binned distribution. Bins are uniform on [lo, hi]; values outside are
// clamped into the edge bins. Thread-safe.

class Histogram {
public:
    Histogram(double lo, double hi, std::size_t bins);

    // Move constructor / assignment. The contained `mutable std::mutex`
    // would otherwise IMPLICITLY DELETE both copy and move because mutexes
    // are non-movable and non-copyable. We need Histogram to be
    // move-constructible so callers can return one by value
    // (e.g. `Result<Histogram>` from audit-side getters that snapshot a
    // body-size distribution). The implementation locks the source mutex,
    // snapshots lo_/hi_/counts_/total_, leaves the moved-from instance
    // empty, and DEFAULT-CONSTRUCTS our own mutex (mutexes are
    // intentionally never moved). Both noexcept — the only operations are
    // primitive copies, vector move, and a lock that propagates only on
    // catastrophic OS failure.
    Histogram(Histogram&& other) noexcept;
    Histogram& operator=(Histogram&& other) noexcept;

    // Copying is still NOT supported — defining the move operations
    // implicitly suppresses the implicit copy ones, but we declare them
    // explicitly here so the contract is unambiguous at call sites.
    Histogram(const Histogram&)            = delete;
    Histogram& operator=(const Histogram&) = delete;

    void observe(double value);

    std::size_t        bin_count() const noexcept;
    std::vector<double> normalized() const;
    std::uint64_t      total() const noexcept;
    double             lo() const noexcept;
    double             hi() const noexcept;

    // Empirical mean computed from bin midpoints weighted by counts.
    // Returns 0.0 if total()==0.
    double mean() const;

    // Empirical population stddev (divides by N, not N-1). Returns 0.0
    // if total()==0 or 1.
    double stddev() const;

    // Empirical population variance (square of stddev). Uses bin
    // midpoints weighted by counts. Returns 0.0 if total()==0 or 1.
    double variance() const;

    // Interquartile range = quantile(0.75) - quantile(0.25). Returns
    // 0.0 on empty histograms.
    double iqr() const;

    // Normalized 3rd central moment (Pearson's moment skewness):
    // E[(X - mean)^3] / stddev^3. Positive values indicate a right tail,
    // negative a left tail, zero a symmetric distribution. Returns 0.0
    // if stddev == 0 or total() < 2.
    double skewness() const;

    // Lowest bin midpoint that has any count. Returns lo() if the
    // histogram is empty (total()==0). Bin midpoints are computed as
    // lo + bin_w * (i + 0.5).
    double min() const;

    // Highest bin midpoint that has any count. Returns hi() if the
    // histogram is empty (total()==0).
    double max() const;

    // Sum of bin_midpoint * count across all bins. Returns 0.0 if the
    // histogram is empty (total()==0). Equivalent to mean() * total(),
    // but computed directly without the divide so it composes naturally
    // when callers want a raw weighted total.
    double sum() const;

    // max() - min(). Returns 0.0 if the histogram is empty (total()==0).
    // Useful as a one-shot "spread" diagnostic that doesn't allocate.
    double range() const;

    // Multiply every bin count by `factor`, rounded down (floor) to
    // integer counts, and update total_ to match the post-scale sum.
    // Used for downsampling — e.g. when merging two baselines and we
    // want to weight one of them less heavily before merge(). Returns
    // invalid_argument when factor < 0. factor == 0 effectively clears
    // (every bin and total_ become 0). factor == 1.0 is a no-op
    // (subject to the floor, which only matters for the boundary case
    // of factor barely below 1.0). Locked.
    Result<void> scale_by(double factor);

    // Add another histogram's bin counts into this one. Returns
    // invalid_argument if bin_count(), lo(), or hi() don't match.
    // Acquires both mutexes via std::lock to avoid deadlock.
    Result<void> merge(const Histogram& other);

    // Explicit deep copy. Returns a NEW Histogram with the same lo/hi/
    // bins/counts/total. Distinct from the move constructor — both
    // operands stay valid afterwards. Useful when callers need a
    // snapshot they can mutate without affecting the source (e.g. a
    // dashboard that wants to scale_by() or merge() into a working
    // copy without disturbing the live distribution). Locks the source
    // under mu_ so the snapshot is consistent under concurrent
    // observe(); the destination's mutex is default-constructed
    // (mutexes are intentionally never copied).
    Histogram clone() const;

    // Returns a NEW Histogram = *this + other (bin-wise sum). Mirrors
    // merge() (which mutates *this) — this version is non-mutating, so
    // both operands stay unchanged. If bin layouts differ
    // (bin_count/lo/hi mismatch) → returns an empty Histogram with
    // *this's geometry (lo/hi/bins from *this, all-zero counts). This
    // matches scale_by(0)'s "geometry preserved, counts dropped"
    // semantics rather than propagating an Error, since the operation
    // is informational/exploratory rather than transactional.
    // Acquires both mutexes via std::scoped_lock to avoid deadlock.
    Histogram with_added(const Histogram& other) const;

    // Replace this histogram's contents with a copy of `other`'s
    // (lo/hi/bins/counts/total). Acquires both mutexes via std::lock to
    // avoid deadlock. After this returns, *this == other content-wise.
    // Self-assignment is a no-op. Distinct from merge() — this REPLACES
    // rather than adds.
    void reset_to(const Histogram& other);

    // Returns the value at quantile q in [0, 1] from the empirical
    // distribution. q is clamped to [0, 1]. q=0 returns lo, q=1 returns hi,
    // q=0.5 the median. Within the matching bin we linearly interpolate
    // between the bin's lower and upper edges using the partial cumulative
    // mass attributable to that bin. An empty histogram (total()==0)
    // returns 0.0.
    double quantile(double q) const;

    // Wrapper over quantile(p / 100.0). p is interpreted as a percentile
    // in [0, 100] and is clamped into that range. p=50 returns the median.
    // Empty histograms return 0.0 (matching quantile()).
    double percentile(double p) const;

    // Convenience wrapper over percentile(99.0). The 99th percentile is
    // the canonical "tail latency" cutoff that operator dashboards lean
    // on; it deserves its own name so call sites read self-documenting
    // rather than as a magic number. Empty histograms return 0.0
    // (matching quantile()).
    double p99() const;

    // Sugar wrapper over percentile(50.0). The median; common-enough on
    // operator dashboards that it deserves its own name alongside p99().
    // Empty histograms return 0.0 (matching quantile()). Distinct from
    // median() in name only — both call quantile(0.5) under the hood and
    // are bit-for-bit identical; use whichever reads more naturally at
    // the call site.
    double p50() const;

    // Sugar wrapper over percentile(90.0). The 90th percentile is the
    // canonical "warning band" cutoff that operator dashboards reach for
    // when they want a less-noisy companion to p99() — values above p90
    // cover the slow 10% of traffic without being dominated by the very
    // long tail. Empty histograms return 0.0 (matching quantile()).
    double p90() const;

    // Sugar wrapper over percentile(95.0). The 95th percentile is the
    // industry-default "warning ceiling" between p90 and p99 — operators
    // typically dashboard p50/p95/p99 as the canonical triple. Giving it
    // its own name keeps call sites self-documenting. Empty histograms
    // return 0.0 (matching quantile()).
    double p95() const;

    // Returns lo + bin_width * (i + 0.5) — the value at the center of
    // bin i. Saturates: i >= bin_count() collapses to the last bin's
    // midpoint (which equals hi() - bin_width/2). Useful when callers
    // want to know what value a specific bin represents without
    // reverse-engineering the formula. Locked.
    double bin_midpoint(std::size_t i) const;

    // Rough 95% normal-approximation confidence interval over the
    // empirical distribution. Returns (mean - 1.96*stddev,
    // mean + 1.96*stddev). Empty histograms return (0.0, 0.0). The
    // result is "rough" because the normal-CI assumption only holds
    // when the empirical distribution is itself near-normal — for
    // long-tailed or multimodal data this should be read as a
    // diagnostic spread rather than a probabilistic guarantee. Both
    // bounds are computed under one lock so a concurrent observe()
    // can't tear the pair.
    std::pair<double, double> confidence_interval_95() const;

    // Index of the bin with the highest count (the modal bin). Ties are
    // broken by smallest index. Returns 0 on an empty histogram (since
    // every bin has count 0, the smallest index wins). Useful for
    // dashboards that want to highlight the most-populated band of a
    // distribution without unpacking the full counts vector.
    std::size_t nth_largest_bin() const;

    // Largest single-bin observation count. Returns 0 on an empty
    // histogram. Distinct from nth_largest_bin() (which returns the
    // modal bin's INDEX) — this returns the modal bin's COUNT. Pairs
    // with total() to compute concentration ratios without two locks.
    std::uint64_t observed_max_bin_count() const;

    // Heuristic shape probe: true iff no single bin holds more than
    // `max_share` of the total mass. Defaults to 0.4 (no bin has more
    // than 40% of observations). max_share is clamped to [0, 1]. An
    // empty histogram (total()==0) is vacuously balanced and returns
    // true. Useful as a one-shot dashboard predicate to flag
    // "everything is collapsing into one bin" pathologies without
    // unpacking the counts vector.
    bool is_balanced(double max_share = 0.4) const;

    // Convenience wrapper over quantile(0.5). Returns 0.0 on an empty
    // histogram (matching quantile()).
    double median() const;

    // True iff total() == 0. Locked. Cheap predicate for callers that
    // want to gate work without computing a full snapshot.
    bool is_empty() const noexcept;

    // Drop all observations: zero counts and total_, but keep bins/lo/hi
    // intact. Distinct from rotating into a new histogram — same
    // instance, just emptied. Locked.
    void clear();

    // Number of bins with at least one observation. Locked.
    std::size_t nonzero_bin_count() const;

    // Raw observation count in bin i. Returns Error::invalid_argument if
    // i >= bin_count(). Lets callers introspect a single bin without
    // copying the full counts_ vector or normalizing. Locked.
    Result<std::uint64_t> bin_at(std::size_t i) const;

    // Cumulative count up through bin i (inclusive). Returns
    // Error::invalid_argument if i >= bin_count(). Equivalent to summing
    // bin_at(0..i) but acquires the lock once and reads counts_ directly,
    // so it composes naturally with cdf() without paying for normalization.
    // Locked.
    Result<std::uint64_t> cumulative_at(std::size_t i) const;

    // Returns (min observed midpoint, max observed midpoint). For an empty
    // histogram returns (lo(), hi()) — matching min()/max() sentinel
    // semantics. Wraps both reads under a single lock so the returned pair
    // is a consistent snapshot (a concurrent observe() between two separate
    // min()/max() calls could produce a min that's higher than the prior
    // max, which this avoids).
    std::pair<double, double> observed_range() const;

    // Cumulative distribution function: for each bin i, the fraction of
    // total observations that fall at or below that bin's upper edge.
    // Length == bin_count(). The last entry is 1.0 for any non-empty
    // histogram. Empty histograms (total()==0) return all-zeros.
    std::vector<double> cdf() const;

    // Population Stability Index: sum_i (p_i - q_i) * ln(p_i / q_i).
    // Conventionally interpreted as <0.10 stable, 0.10–0.25 minor drift,
    // >0.25 significant drift.
    static double psi(const Histogram& reference, const Histogram& current);

    // Two-sample Kolmogorov-Smirnov statistic on the empirical CDFs.
    static double ks(const Histogram& reference, const Histogram& current);

    // Earth Mover's Distance (1D).
    static double emd(const Histogram& reference, const Histogram& current);

private:
    double                     lo_;
    double                     hi_;
    std::vector<std::uint64_t> counts_;
    mutable std::mutex         mu_;
    std::uint64_t              total_ = 0;
};

// ---- DriftReport / Severity --------------------------------------------

enum class DriftSeverity : std::uint8_t {
    none   = 0,
    minor  = 1,
    moder  = 2,
    severe = 3,
};

const char* to_string(DriftSeverity) noexcept;

struct DriftReport {
    std::string   feature;
    double        psi          = 0.0;
    double        ks_statistic = 0.0;
    double        emd          = 0.0;
    DriftSeverity severity     = DriftSeverity::none;
    std::uint64_t reference_n  = 0;
    std::uint64_t current_n    = 0;
    Time          computed_at;
};

// ---- DriftMonitor -------------------------------------------------------
//
// Maintains per-feature reference + current histograms and computes drift
// metrics on demand. Use register_feature() once at startup with baseline
// values (held-out validation set, or the prior production window).

class DriftMonitor {
public:
    DriftMonitor();
    ~DriftMonitor();
    DriftMonitor(const DriftMonitor&)            = delete;
    DriftMonitor& operator=(const DriftMonitor&) = delete;

    // Register a feature with its baseline distribution. lo/hi/bins
    // configure the histogram resolution.
    Result<void> register_feature(std::string         name,
                                  std::vector<double> baseline,
                                  double              lo   = 0.0,
                                  double              hi   = 1.0,
                                  std::size_t         bins = 20);

    // Record an observation against a registered feature.
    Result<void> observe(std::string_view feature, double value);

    // Convenience: equivalent to calling observe() n times with the same
    // value. Holds the lock once. No-op if the feature is unregistered
    // (silent — distinct from observe() which returns not_found). Used
    // for "we just got a batch of similar values, fold them in" patterns.
    // The alert sink fires at most ONCE per call (after the batch lands),
    // based on the post-batch severity.
    void observe_uniform(std::string_view feature, double value, std::size_t n);

    // Bulk observation: equivalent to calling observe() once per value,
    // but holds the lock once for the whole batch. The alert sink fires
    // at most ONCE per call (after the batch lands), based on the
    // post-batch severity. Returns not_found if the feature was never
    // registered. An empty span is a valid no-op.
    Result<void> observe_batch(std::string_view          feature,
                               std::span<const double>   values);

    // Compute drift reports for all registered features at the current moment.
    std::vector<DriftReport> report() const;

    // Single-feature variant of report(): compute psi/ks/emd for one
    // registered feature. Returns Error::not_found if the feature was
    // never registered. Useful for dashboards / sidecars that want to
    // probe one feature's drift without the cost of a full report().
    Result<DriftReport> report_for_feature(std::string_view feature) const;

    // Reset the current window — typically called daily or on alert.
    void rotate();

    // Severity classification thresholds (exposed for tests/calibration).
    static DriftSeverity classify(double psi) noexcept;

    // Alert sink: callback fired whenever a feature's classified severity
    // CROSSES (rises above) a threshold during observe(). Once-per-crossing,
    // not once-per-observation — the monitor remembers the last reported
    // severity and only fires when the new severity is strictly higher.
    // The runtime uses this to append drift.crossed entries to the ledger
    // automatically; libraries can use it for paging or dashboards.
    using AlertSink = std::function<void(const DriftReport&)>;
    void set_alert_sink(AlertSink sink, DriftSeverity threshold = DriftSeverity::moder);

    // Reconfigure the alert threshold without changing the installed
    // sink. Useful for runtime threshold tuning — e.g. an operator
    // dialing the threshold up to "severe" during a scheduled rollout
    // to silence "minor"/"moder" pages. NOTE: this DOES NOT clear
    // last_severity_; a feature that has already crossed (and recorded
    // its high-water severity) will not re-fire when the threshold
    // moves. Call clear_alerts() separately if you want the alert
    // ladder reset alongside the threshold change.
    void set_alert_threshold(DriftSeverity severity);

    // True iff a non-empty AlertSink is currently installed. Used for
    // "is this monitor wired to the ledger?" checks. Locked.
    bool has_alert_sink() const noexcept;

    // Names of all registered features, in unspecified order. Used by
    // dashboards to enumerate what the monitor is watching without
    // computing a full report().
    std::vector<std::string> list_features() const;

    // Number of registered features. O(1).
    std::size_t feature_count() const;

    // Count of registered features that have at least one current-window
    // observation. Distinct from feature_count() (total registered):
    // dashboards use this to gate "are we receiving traffic on every
    // feature we're watching?" without enumerating list_features() and
    // probing observation_count() per name. Locked.
    std::size_t feature_count_observed() const;

    // Cheap predicate: true iff a feature with this name has been
    // registered. Locked, but does not allocate or compute drift.
    bool has_feature(std::string_view name) const noexcept;

    // Sugar wrapper over has_feature(). Reads more naturally on call
    // sites that frame registration as the predicate ("is this feature
    // registered?") rather than ownership ("does the monitor have this
    // feature?"). noexcept; on internal failure (lock or string
    // construction throw) returns false rather than propagating —
    // matching has_feature().
    bool is_registered(std::string_view feature) const noexcept;

    // Returns the current-window observation count for a feature. Returns
    // not_found if the feature was never registered. Useful for sidecars
    // that want to gate report() on a minimum sample size.
    Result<std::uint64_t> observation_count(std::string_view feature) const;

    // Sum of current-window observation counts across all registered
    // features. Returns 0 if no features are registered. Pairs with
    // feature_count() and feature_count_observed() as a "how much
    // traffic has the monitor seen this window?" probe — distinct from
    // observation_count(name) which returns the per-feature total. Used
    // by aggregate health dashboards that want a single scalar without
    // walking list_features() and summing per-feature counts. Locked
    // once for the whole sweep so the result is a consistent snapshot.
    std::uint64_t observation_total() const;

    // Returns the reference (baseline) window's total observation count.
    // Pairs with observation_count(), which returns the current window
    // total. Returns not_found if the feature was never registered.
    Result<std::uint64_t> baseline_count(std::string_view feature) const;

    // Name of the feature with the largest current PSI across all
    // registered features. Returns Error::not_found if no features
    // are registered. Ties (equal PSI to within bitwise equality) are
    // broken by alphabetical name to keep the result deterministic
    // for dashboards and tests.
    Result<std::string> most_drifted_feature() const;

    // (feature_name, full DriftReport) for the feature with the
    // highest current PSI. Distinct from most_drifted_feature() which
    // returns just the name — this returns the full report so dashboards
    // can show severity, ks, emd, n's alongside the name without a
    // second lookup. Returns Error::not_found if no features are
    // registered. Ties on PSI broken by alphabetical name (matching
    // most_drifted_feature() semantics) so the result is deterministic.
    Result<std::pair<std::string, DriftReport>> worst_psi_feature() const;

    // Aggregate health snapshot for the monitor. total_observations is
    // the sum of current-window observation counts across all features.
    // max_severity is the worst current PSI severity across features
    // (DriftSeverity::none if no features are registered).
    struct Summary {
        std::size_t   feature_count;
        std::uint64_t total_observations;
        DriftSeverity max_severity;
    };
    Summary summary() const;

    // True iff any registered feature's current PSI severity classifies
    // as severe (NaN PSI is treated as severe via classify()). Returns
    // false on empty monitors. Locked.
    bool any_severe() const;

    // Names of features whose CURRENT severity classifies as severe.
    // Sugar over report() filtered by severity == DriftSeverity::severe,
    // returned as an alphabetically-sorted vector so dashboards / tests
    // get a deterministic order. Empty when no features are registered
    // or when none classify as severe. Distinct from any_severe()
    // (cheap bool) and from list_features() (every name regardless of
    // severity).
    std::vector<std::string> list_severe_features() const;

    // Cheap predicate: any registered feature currently classifying as
    // severe? Stops on the first severe feature rather than enumerating
    // the full set, so it's strictly cheaper than
    // !list_severe_features().empty(). noexcept; on internal failure
    // (lock or PSI throw) returns false rather than propagating —
    // matching the conservative degradation of has_alert_sink() /
    // is_registered().
    bool has_any_severe() const noexcept;

    // Reset just the current-window histogram for one feature, leaving
    // the reference (baseline) intact. Per-feature variant of rotate(),
    // which clears all features. Returns not_found if the feature was
    // never registered.
    Result<void> reset(std::string_view feature);

    // Reset every feature's current window AND clear the per-feature
    // last_severity_ tracking map. This is the all-features analogue of
    // reset(name): both clear the current window, and reset_all() also
    // clears alert tracking so the alert sink can re-fire from the
    // bottom of the threshold ladder.
    //
    // Distinct from rotate(): rotate() only rebuilds the current
    // histograms (preserving baselines) but DOES NOT clear
    // last_severity_, so a feature that was already at "severe" stays
    // marked as severe until clear_alerts() is called separately. Use
    // reset_all() when an operator wants a full fresh start — both data
    // and alert state — rather than just rotating the data window.
    void reset_all();

    // Reset the internal last_severity_ tracking map. Used after an
    // operator acknowledges drift alerts so the alert sink can fire
    // again on the next severity rise (per-crossing semantics are
    // preserved, but the "previous" severity is forgotten).
    void clear_alerts();

    // Compute the current PSI severity for a single registered feature.
    // Returns Error::not_found if the feature was never registered.
    Result<DriftSeverity> feature_severity(std::string_view feature) const;

    // Up-to-`n` historical drift snapshots for one feature, ordered oldest
    // first. The current implementation returns the single current snapshot
    // (matching `report_for_feature`) and ignores `n` — future versions will
    // maintain a per-feature ring buffer of past reports populated either on
    // severity crossings or on telemetry calls. Returns an empty vector if
    // the feature was never registered (a "show me trend" call that no-trend
    // is fine — empty rather than not_found).
    std::vector<DriftReport> trend_for_feature(std::string_view feature,
                                               std::size_t      n) const;

private:
    struct FeatureState;
    std::unordered_map<std::string, std::unique_ptr<FeatureState>> features_;
    mutable std::mutex                                             mu_;

    // Alert sink + threshold + last-fired severity per feature.
    AlertSink                                       alert_sink_;
    DriftSeverity                                   alert_threshold_ = DriftSeverity::moder;
    std::unordered_map<std::string, DriftSeverity>  last_severity_;
};

// ---- Generic counters / histograms registry ----------------------------

class MetricRegistry {
public:
    void  inc(std::string_view name, std::uint64_t delta = 1);

    // Alias for inc(name, delta). Some operators prefer the explicit
    // verb when the call site is incrementing a domain counter rather
    // than recording a tick.
    void  add(std::string_view name, std::uint64_t delta);

    // Verbose alias for inc(name, delta). Some operator codebases
    // prefer the explicit name at the call site even though the
    // semantics — create-on-first-use, then increment — are identical
    // to inc(). Trivial forward.
    void  increment_or_create(std::string_view name, std::uint64_t delta = 1);

    // Record a measurement into a histogram. Buckets are exponential with
    // bases at the supplied list (le upper bounds). If a histogram of this
    // name already exists, value is added; otherwise it's created with the
    // default latency-shaped bucket set:
    //   0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, +Inf
    void  observe(std::string_view name, double value);

    std::uint64_t count(std::string_view name) const;

    // Safer variant of count(name): returns Error::not_found if no
    // counter with that name has been registered. count() returns 0
    // silently for both 0-valued and missing counters; this disambiguates.
    // Histograms are NOT counters and do not satisfy this lookup.
    Result<std::uint64_t> counter_value(std::string_view name) const;

    // Same as count(name) but returns `default_value` (instead of 0) when
    // the counter doesn't exist. Distinguishes "missing" from "0" without
    // forcing the caller through a Result. Returns the counter value if it
    // exists; histograms still satisfy this and return their observation
    // count (matching count()'s fall-through). Locked.
    std::uint64_t counter_with_default(std::string_view name,
                                       std::uint64_t    default_value) const;

    // Quantile of a registered histogram by name, computed from its
    // bucket counts via linear interpolation across the bucket containing
    // the quantile. Returns 0.0 if the histogram doesn't exist or has no
    // observations. q is clamped to [0, 1].
    double histogram_quantile(std::string_view name, double q) const;

    // Cheap predicate: true iff a counter with this name has been
    // registered (regardless of its current value). Locked, but does
    // not allocate the way list_counters() / counter_snapshot() do.
    bool has_counter(std::string_view name) const noexcept;

    // Cheap predicate: true iff a histogram with this name has been
    // observe()'d. Distinct from has_counter() — counters and
    // histograms occupy independent name spaces. Locked, no alloc.
    bool has_histogram(std::string_view name) const noexcept;

    // Cheap discoverability predicate: true iff a counter OR histogram
    // with this name has been observed. Equivalent to
    // `has_counter(name) || has_histogram(name)`, but acquires the
    // mutex once. noexcept; on internal failure (e.g. lock or string
    // construction throw) returns false rather than propagating —
    // matching has_counter()/has_histogram().
    bool has(std::string_view name) const noexcept;

    // Read the count of a registered histogram by name. Returns not_found
    // if no histogram of that name has been observe()'d. Distinct from
    // count(), which returns 0 for unknown names and also matches counters.
    Result<std::uint64_t> histogram_count(std::string_view name) const;

    // Read the running sum of a registered histogram by name. Returns
    // not_found if no histogram of that name has been observe()'d.
    Result<double> histogram_sum(std::string_view name) const;

    // JSON-shaped snapshot.
    std::string snapshot_json() const;

    // Prometheus 0.0.4 text exposition format. Emits HELP + TYPE pair
    // followed by sample lines per metric. Counters get one sample;
    // histograms get one _bucket{le="…"} line per cumulative bucket plus
    // _sum and _count. Names sanitized to [a-zA-Z_:][a-zA-Z0-9_:]*. Output
    // is suitable for serving at /metrics with Content-Type:
    // text/plain; version=0.0.4; charset=utf-8.
    std::string snapshot_prometheus() const;

    // Names of all registered counters, in unspecified order. For health
    // dashboards and operator tooling that wants to enumerate what's
    // being measured without parsing the prometheus exposition.
    std::vector<std::string> list_counters() const;

    // Names of all registered counters, sorted alphabetically. Unlike
    // list_counters() (unspecified order), this is suitable for stable
    // JSON dumps and diff-friendly snapshots.
    std::vector<std::string> all_counter_names() const;

    // Sorted vector of counter names whose name starts with `prefix`.
    // An empty prefix matches every counter, which makes this
    // equivalent to all_counter_names() (= list_counters() but sorted).
    // Useful for "everything under this subsystem prefix" dashboards
    // that want the names rather than the per-counter sum that
    // sum_counters_with_prefix() returns. Locked.
    std::vector<std::string> counter_names_with_prefix(std::string_view prefix) const;

    // Sorted vector of counter names whose value is STRICTLY GREATER
    // than `threshold`. Used by "show me the noisy counters" probes —
    // operators reach for this to narrow a snapshot down to the high-
    // count entries without parsing the prometheus exposition or
    // diff'ing a counter_snapshot(). A threshold of 0 returns every
    // non-zero counter; std::uint64_t max() conceptually returns the
    // empty set. Sorted alphabetically for stable / diff-friendly
    // output. Locked.
    std::vector<std::string> counters_above(std::uint64_t threshold) const;

    // Reset a single counter to zero. Used when a sidecar wants to
    // emit per-deploy metric resets (e.g. on restart). Returns not_found
    // if no such counter exists. Histograms are not affected.
    Result<void> reset(std::string_view name);

    // Zero every counter to 0 while keeping the names registered.
    // Distinct from clear(), which drops everything (counters AND
    // histograms). This matches DriftMonitor::reset_all() semantics for
    // counters: data zeroed, names preserved. Histograms are NOT affected
    // (use reset_histograms() for that). Locked.
    void reset_all_counters();

    // Reset every counter whose name starts with `prefix` to 0,
    // returning the number of counters reset. An empty prefix matches
    // every counter, which makes this equivalent to reset_all_counters()
    // in effect (and the return is the total counter count). Names are
    // preserved (data zeroed only) so subsequent counter_value() probes
    // continue to succeed — mirrors reset_all_counters() semantics
    // scoped to a prefix. Used to scope-reset families like "inference."
    // or "drift." without affecting unrelated subsystem counters.
    // Histograms are NOT affected. Locked.
    std::size_t reset_counter_pattern(std::string_view prefix);

    // Drop all counters and histograms. Used by tests and by live deploy
    // resets that want to start from a clean slate. Mutex-protected.
    void clear();

    // Drop all histograms while leaving counters untouched. Used by
    // tests and by live deploy resets that want to keep counter trends
    // (e.g. inferences_total) but reset latency histograms after a
    // model swap or config change. Mutex-protected.
    void reset_histograms();

    // Number of registered counters in the registry (not the sum of their
    // values, just the count of distinct names). Useful for health
    // probes that want to assert "telemetry is wired up."
    std::size_t counter_count() const;

    // Sum of all counter values across the registry. Used as a "global
    // event count" probe for liveness dashboards. Locked.
    std::uint64_t counter_total() const;

    // Sum of all counter values whose name starts with `prefix`. An
    // empty prefix matches every counter, which makes this equivalent
    // to counter_total(). Useful for "all *_total counters" or
    // "everything under this subsystem prefix" dashboards that want a
    // single scalar without enumerating names. Locked.
    std::uint64_t sum_counters_with_prefix(std::string_view prefix) const;

    // Largest counter value across the registry. Returns 0 on an empty
    // registry (consistent with counter_total()). Pairs with
    // counter_total() as a "what's the worst single counter saying?"
    // probe for sidecars that want to gate on a hot counter without
    // enumerating the full snapshot.
    std::uint64_t counter_max() const;

    // Smallest counter value across the registry. Returns
    // Error::not_found if no counters have been registered. Distinct
    // from counter_max() (which returns 0 silently on empty), because
    // 0 is itself a legal counter value — the not_found result lets
    // callers distinguish "no counters" from "the smallest counter is
    // 0." Useful as a "is every counter at least N?" gate when paired
    // with a min-threshold check on the result.
    Result<std::uint64_t> counter_min() const;

    // Sum of histogram observation counts across all registered
    // histograms. Distinct from counter_total() (which sums counter
    // values) and from histogram_count(name) (which returns the
    // observation count for a single histogram). Returns 0 on an empty
    // registry or when every histogram has zero observations. Locked.
    std::uint64_t histogram_count_total() const;

    // Snapshot of all counters as a map of {name: value}. O(n) copy;
    // suitable for diff'ing across two points in time without parsing
    // the prometheus exposition.
    std::unordered_map<std::string, std::uint64_t> counter_snapshot() const;

    // Per-counter delta from a prior counter_snapshot()-shaped baseline.
    // For every counter currently in this registry, the entry is
    // (current - baseline_or_0). For every counter present in baseline
    // but no longer in the registry, the entry is -baseline (a negative
    // delta indicating the counter was reset/dropped). Used to compute
    // "what changed since this snapshot." Signed so deltas survive resets.
    std::unordered_map<std::string, std::int64_t>
    diff(const std::unordered_map<std::string, std::uint64_t>& baseline) const;

    // Sum of absolute deltas across the union of (current ∪ baseline)
    // counter names. Equivalent to summing |v| over diff(baseline).values().
    // Used to summarize "how much activity happened between two snapshots"
    // as a single scalar — useful for dashboards that want one number for
    // a sparkline rather than a per-counter table.
    std::uint64_t
    counter_diff_total(const std::unordered_map<std::string, std::uint64_t>& baseline) const;

    // Convenience ratio between two registered counters, returning
    // count(numerator) / count(denominator) as a double. Returns
    // Error::not_found if either counter is unregistered (using
    // counter_value semantics — a missing counter is distinct from a
    // 0-valued one). Returns Error::invalid_argument if the denominator
    // resolves to 0 to avoid division-by-zero. Useful for derived rates
    // such as "blocked_input / inference_attempts" without having to
    // unpack a snapshot.
    Result<double> ratio(std::string_view numerator,
                         std::string_view denominator) const;

    // True iff the registry contains no counters AND no histograms.
    // Cheap predicate gated by the registry mutex — used by health
    // probes that want to assert telemetry is wired up before scraping.
    bool is_empty() const noexcept;

    // Sugar wrapper over `!is_empty()`. True iff the registry contains
    // any counter or any histogram. Reads more naturally on call sites
    // framed as "do we have anything to scrape?" rather than "is this
    // empty?". noexcept; on internal failure (e.g. lock throw) returns
    // false, matching the conservative degradation of is_empty().
    bool has_any() const noexcept;

private:
    struct Hist {
        std::vector<double>        buckets;     // upper bounds, ascending
        std::vector<std::uint64_t> bucket_counts;  // count_of(value <= buckets[i])
        std::uint64_t              count = 0;
        double                     sum   = 0.0;
    };

    mutable std::mutex                                mu_;
    std::unordered_map<std::string, std::uint64_t>    counters_;
    std::unordered_map<std::string, Hist>             histograms_;
};

// ============================================================================
// Round 92 — CalibrationMonitor
// ============================================================================
//
// Tracks empirical sensitivity / specificity / PPV / NPV against a configured
// target floor and surfaces a below-floor signal when the empirical value
// drops below the floor by more than `tolerance`. Used by products like
// TriageMate where the substrate guarantees a sensitivity target the
// deployment commits to (e.g. ≥99% for fracture-rule-out).
//
// Vendor-neutral: the kernel does not know what diagnostic the model is
// performing; it just keeps four counters (TP/FP/TN/FN) and reports
// derived rates. Callers feed Outcome events from their own ground-truth
// pipeline.

class CalibrationMonitor {
public:
    enum class Outcome : std::uint8_t {
        true_positive  = 1,
        false_positive = 2,
        true_negative  = 3,
        false_negative = 4,
    };

    struct Targets {
        double sensitivity_floor = 0.0;  // [0, 1]
        double specificity_floor = 0.0;  // [0, 1]
        double tolerance         = 0.0;  // [0, 1] — alarm when below floor by more than this
    };

    CalibrationMonitor();
    explicit CalibrationMonitor(Targets t);

    void record(Outcome);
    void record_n(Outcome, std::size_t n);

    std::size_t tp() const noexcept;
    std::size_t fp() const noexcept;
    std::size_t tn() const noexcept;
    std::size_t fn() const noexcept;
    std::size_t total() const noexcept;

    // Returns 0..1 or NaN if the relevant denominator is zero.
    double sensitivity() const noexcept;  // TP / (TP + FN)
    double specificity() const noexcept;  // TN / (TN + FP)
    double ppv()         const noexcept;  // TP / (TP + FP)
    double npv()         const noexcept;  // TN / (TN + FN)
    double accuracy()    const noexcept;  // (TP + TN) / total

    // True iff sensitivity OR specificity is below the configured floor by
    // more than tolerance, AND total observations >= min_samples (else
    // the result is not statistically meaningful and we return false).
    bool is_below_floor(std::size_t min_samples = 30) const noexcept;

    // Per-axis variants of is_below_floor.
    bool sensitivity_below_floor(std::size_t min_samples = 30) const noexcept;
    bool specificity_below_floor(std::size_t min_samples = 30) const noexcept;

    Targets targets() const noexcept;
    void    set_targets(Targets);

    void reset();

    // Aggregate snapshot for /healthz dashboards.
    struct Snapshot {
        std::size_t tp;
        std::size_t fp;
        std::size_t tn;
        std::size_t fn;
        std::size_t total;
        double      sensitivity;
        double      specificity;
        double      ppv;
        double      npv;
        double      accuracy;
        bool        below_floor;
    };
    Snapshot snapshot(std::size_t min_samples = 30) const noexcept;

    // Single-line ASCII summary. Format:
    //   "tp=<n> fp=<n> tn=<n> fn=<n> sens=<f> spec=<f>"
    // No trailing newline.
    std::string summary_string() const;

private:
    mutable std::mutex mu_;
    std::size_t        tp_      = 0;
    std::size_t        fp_      = 0;
    std::size_t        tn_      = 0;
    std::size_t        fn_      = 0;
    Targets            targets_{};
};

const char* to_string(CalibrationMonitor::Outcome) noexcept;

}  // namespace asclepius

#endif  // ASCLEPIUS_TELEMETRY_HPP
