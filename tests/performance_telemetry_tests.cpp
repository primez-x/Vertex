#include "sketch/performance_telemetry.hpp"
#include "support/noninteractive_errors.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace sketch;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void records_nearest_rank_p95_and_thresholds() {
    PerformanceTelemetry telemetry;
    for (int value = 1; value <= 20; ++value) {
        require(telemetry.record(PerformanceMetric::input, static_cast<double>(value)),
                "finite timing should be accepted");
    }
    const auto summary = telemetry.summary(PerformanceMetric::input);
    require(summary.sample_count == 20 && summary.dropped_sample_count == 0,
            "sample count should be retained");
    require(summary.has_samples && summary.p95_ms == 19.0 &&
                summary.threshold_ms == 50.0 && summary.within_threshold,
            "nearest-rank p95 or threshold is incorrect");
    require(std::string(performance_metric_name(PerformanceMetric::navigation)) == "navigation" &&
                performance_metric_threshold_ms(PerformanceMetric::navigation) == 16.7,
            "metric metadata is incorrect");
}

void invalid_values_are_ignored_without_mutation() {
    PerformanceTelemetry telemetry;
    require(!telemetry.record(PerformanceMetric::edit, -1.0),
            "negative timing should be rejected");
    require(!telemetry.record(PerformanceMetric::edit, std::numeric_limits<double>::quiet_NaN()),
            "NaN timing should be rejected");
    require(!telemetry.record(PerformanceMetric::edit, std::numeric_limits<double>::infinity()),
            "infinite timing should be rejected");
    const auto summary = telemetry.summary(PerformanceMetric::edit);
    require(summary.sample_count == 0 && !summary.has_samples && !summary.within_threshold,
            "invalid timing changed the metric");
}

void newest_samples_replace_oldest_after_bound() {
    PerformanceTelemetry telemetry;
    for (std::size_t value = 0; value != PerformanceTelemetry::max_samples_per_metric + 3; ++value) {
        require(telemetry.record(PerformanceMetric::save, static_cast<double>(value)),
                "bounded timing should be accepted");
    }
    const auto summary = telemetry.summary(PerformanceMetric::save);
    require(summary.sample_count == PerformanceTelemetry::max_samples_per_metric &&
                summary.dropped_sample_count == 3 && summary.p95_ms == 3894.0,
            "bounded timing did not retain the newest samples");
}

void reset_clears_all_metrics_and_accepts_fresh_samples() {
    PerformanceTelemetry telemetry;
    constexpr PerformanceMetric metrics[]{PerformanceMetric::navigation, PerformanceMetric::input,
                                          PerformanceMetric::edit, PerformanceMetric::open,
                                          PerformanceMetric::save};
    for (const auto metric : metrics) {
        for (std::size_t index = 0; index < PerformanceTelemetry::max_samples_per_metric + 1;
             ++index) {
            require(telemetry.record(metric, 10.0), "pre-reset timing should be accepted");
        }
        require(telemetry.summary(metric).dropped_sample_count == 1,
                "pre-reset metric should contain a dropped sample");
    }

    telemetry.reset();
    for (const auto metric : metrics) {
        const auto summary = telemetry.summary(metric);
        require(summary.sample_count == 0 && summary.dropped_sample_count == 0 &&
                    summary.p95_ms == 0.0 && !summary.has_samples && !summary.within_threshold,
                "reset should clear every metric and its dropped count");
    }

    require(telemetry.record(PerformanceMetric::input, 2.0),
            "fresh timing after reset should be accepted");
    const auto summary = telemetry.summary(PerformanceMetric::input);
    require(summary.sample_count == 1 && summary.dropped_sample_count == 0 &&
                summary.p95_ms == 2.0 && summary.has_samples && summary.within_threshold,
            "fresh timing should not include samples from before reset");
}

void scoped_measurement_records_a_duration() {
    PerformanceTelemetry telemetry;
    {
        ScopedPerformanceMeasurement scope(telemetry, PerformanceMetric::open);
        // A zero-duration scope is valid and keeps this test deterministic.
    }
    const auto summary = telemetry.summary(PerformanceMetric::open);
    require(summary.sample_count == 1 && summary.has_samples &&
                std::isfinite(summary.p95_ms) && summary.p95_ms >= 0.0,
            "scoped measurement did not record a finite duration");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        records_nearest_rank_p95_and_thresholds();
        invalid_values_are_ignored_without_mutation();
        newest_samples_replace_oldest_after_bound();
        reset_clears_all_metrics_and_accepts_fresh_samples();
        scoped_measurement_records_a_duration();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
