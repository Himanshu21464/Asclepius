// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Asclepius Contributors
#ifndef ASCLEPIUS_TELEMETRY_HPP
#define ASCLEPIUS_TELEMETRY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

    // Lowest bin midpoint that has any count. Returns lo() if the
    // histogram is empty (total()==0). Bin midpoints are computed as
    // lo + bin_w * (i + 0.5).
    double min() const;

    // Highest bin midpoint that has any count. Returns hi() if the
    // histogram is empty (total()==0).
    double max() const;

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

    // Reset just the current-window histogram for one feature, leaving
    // the reference (baseline) intact. Per-feature variant of rotate(),
    // which clears all features. Returns not_found if the feature was
    // never registered.
    Result<void> reset(std::string_view feature);

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

    // Record a measurement into a histogram. Buckets are exponential with
    // bases at the supplied list (le upper bounds). If a histogram of this
    // name already exists, value is added; otherwise it's created with the
    // default latency-shaped bucket set:
    //   0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, +Inf
    void  observe(std::string_view name, double value);

    std::uint64_t count(std::string_view name) const;

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

    // Number of registered histograms. Named `_total` to avoid clashing
    // with the existing histogram_count(name) reader, which returns the
    // observation count for a single histogram.
    std::size_t histogram_count_total() const;

    // Snapshot of all counters as a map of {name: value}. O(n) copy;
    // suitable for diff'ing across two points in time without parsing
    // the prometheus exposition.
    std::unordered_map<std::string, std::uint64_t> counter_snapshot() const;

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
