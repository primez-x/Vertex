#include "sketch/performance_telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sketch {

namespace {
constexpr std::array<double, 5> kThresholds{{16.7, 50.0, 250.0, 5000.0, 5000.0}};
}

const char* performance_metric_name(PerformanceMetric metric) noexcept {
    switch (metric) {
    case PerformanceMetric::navigation: return "navigation";
    case PerformanceMetric::input: return "input";
    case PerformanceMetric::edit: return "edit";
    case PerformanceMetric::open: return "open";
    case PerformanceMetric::save: return "save";
    }
    return "unknown";
}

double performance_metric_threshold_ms(PerformanceMetric metric) noexcept {
    return kThresholds[PerformanceTelemetry::metric_index(metric)];
}

std::size_t PerformanceTelemetry::metric_index(PerformanceMetric metric) noexcept {
    switch (metric) {
    case PerformanceMetric::navigation: return 0;
    case PerformanceMetric::input: return 1;
    case PerformanceMetric::edit: return 2;
    case PerformanceMetric::open: return 3;
    case PerformanceMetric::save: return 4;
    }
    return 0;
}

bool PerformanceTelemetry::record(PerformanceMetric metric, double elapsed_ms) noexcept {
    if (!std::isfinite(elapsed_ms) || elapsed_ms < 0.0) return false;
    try {
        std::lock_guard lock(mutex_);
        auto& samples = samples_[metric_index(metric)];
        if (samples.values.size() == max_samples_per_metric) {
            samples.values.erase(samples.values.begin());
            ++samples.dropped;
        }
        samples.values.push_back(elapsed_ms);
        return true;
    } catch (...) {
        return false;
    }
}

bool PerformanceTelemetry::record(
    PerformanceMetric metric, std::chrono::steady_clock::duration elapsed) noexcept {
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    return record(metric, elapsed_ms);
}

PerformanceMetricSummary PerformanceTelemetry::summary(PerformanceMetric metric) const noexcept {
    std::vector<double> values;
    std::size_t dropped = 0;
    try {
        {
            std::lock_guard lock(mutex_);
            const auto& samples = samples_[metric_index(metric)];
            values = samples.values;
            dropped = samples.dropped;
        }
        if (values.empty()) {
            return {0, dropped, 0.0, performance_metric_threshold_ms(metric), false, false};
        }
        std::sort(values.begin(), values.end());
        const auto rank = (95 * values.size() + 99) / 100;
        const auto p95 = values[rank - 1];
        const auto threshold = performance_metric_threshold_ms(metric);
        return {values.size(), dropped, p95, threshold, true, p95 <= threshold};
    } catch (...) {
        return {0, dropped, 0.0, performance_metric_threshold_ms(metric), false, false};
    }
}

void PerformanceTelemetry::reset() noexcept {
    try {
        std::lock_guard lock(mutex_);
        for (auto& samples : samples_) {
            samples.values.clear();
            samples.dropped = 0;
        }
    } catch (...) {
        // Diagnostic instrumentation must not interrupt authoring.
    }
}

ScopedPerformanceMeasurement::ScopedPerformanceMeasurement(
    PerformanceTelemetry& telemetry, PerformanceMetric metric) noexcept
    : telemetry_(&telemetry), metric_(metric), started_(std::chrono::steady_clock::now()) {}

ScopedPerformanceMeasurement::~ScopedPerformanceMeasurement() noexcept {
    if (telemetry_ == nullptr) return;
    (void)telemetry_->record(metric_, std::chrono::steady_clock::now() - started_);
}

}  // namespace sketch
