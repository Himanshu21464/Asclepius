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

    // Add another histogram's bin counts into this one. Returns
    // invalid_argument if bin_count(), lo(), or hi() don't match.
    // Acquires both mutexes via std::lock to avoid deadlock.
    Result<void> merge(const Histogram& other);

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

    // Bulk observation: equivalent to calling observe() once per value,
    // but holds the lock once for the whole batch. The alert sink fires
    // at most ONCE per call (after the batch lands), based on the
    // post-batch severity. Returns not_found if the feature was never
    // registered. An empty span is a valid no-op.
    Result<void> observe_batch(std::string_view          feature,
                               std::span<const double>   values);

    // Compute drift reports for all registered features at the current moment.
    std::vector<DriftReport> report() const;

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

    // True iff a non-empty AlertSink is currently installed. Used for
    // "is this monitor wired to the ledger?" checks. Locked.
    bool has_alert_sink() const noexcept;

    // Names of all registered features, in unspecified order. Used by
    // dashboards to enumerate what the monitor is watching without
    // computing a full report().
    std::vector<std::string> list_features() const;

    // Number of registered features. O(1).
    std::size_t feature_count() const;

    // Cheap predicate: true iff a feature with this name has been
    // registered. Locked, but does not allocate or compute drift.
    bool has_feature(std::string_view name) const noexcept;

    // Returns the current-window observation count for a feature. Returns
    // not_found if the feature was never registered. Useful for sidecars
    // that want to gate report() on a minimum sample size.
    Result<std::uint64_t> observation_count(std::string_view feature) const;

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

    // Reset just the current-window histogram for one feature, leaving
    // the reference (baseline) intact. Per-feature variant of rotate(),
    // which clears all features. Returns not_found if the feature was
    // never registered.
    Result<void> reset(std::string_view feature);

    // Reset the internal last_severity_ tracking map. Used after an
    // operator acknowledges drift alerts so the alert sink can fire
    // again on the next severity rise (per-crossing semantics are
    // preserved, but the "previous" severity is forgotten).
    void clear_alerts();

    // Compute the current PSI severity for a single registered feature.
    // Returns Error::not_found if the feature was never registered.
    Result<DriftSeverity> feature_severity(std::string_view feature) const;

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

    // Reset a single counter to zero. Used when a sidecar wants to
    // emit per-deploy metric resets (e.g. on restart). Returns not_found
    // if no such counter exists. Histograms are not affected.
    Result<void> reset(std::string_view name);

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

    // Number of registered histograms. Named `_total` to avoid clashing
    // with the existing histogram_count(name) reader, which returns the
    // observation count for a single histogram.
    std::size_t histogram_count_total() const;

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

}  // namespace asclepius

#endif  // ASCLEPIUS_TELEMETRY_HPP
