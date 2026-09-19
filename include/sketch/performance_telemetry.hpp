#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <vector>

namespace sketch {

enum class PerformanceMetric {
    navigation,
    input,
    edit,
    open,
    save,
};

struct PerformanceMetricSummary final {
    std::size_t sample_count{};
    std::size_t dropped_sample_count{};
    double p95_ms{};
    double threshold_ms{};
    bool has_samples{};
    bool within_threshold{};
};

[[nodiscard]] const char* performance_metric_name(PerformanceMetric metric) noexcept;
[[nodiscard]] double performance_metric_threshold_ms(PerformanceMetric metric) noexcept;

// Bounded, process-local timing collection for real application operations.
// Samples are intentionally diagnostic only: callers must supply the workload
// and reference-machine identity before using the summaries as qualification
// evidence. The recorder keeps the newest samples once its bound is reached.
class PerformanceTelemetry final {
public:
    static constexpr std::size_t max_samples_per_metric = 4096;

    PerformanceTelemetry() = default;
    PerformanceTelemetry(const PerformanceTelemetry&) = delete;
    PerformanceTelemetry& operator=(const PerformanceTelemetry&) = delete;

    // Invalid values are ignored and return false so instrumentation can never
    // interrupt authoring. Valid samples are measured in milliseconds.
    [[nodiscard]] bool record(PerformanceMetric metric, double elapsed_ms) noexcept;

    [[nodiscard]] bool record(
        PerformanceMetric metric,
        std::chrono::steady_clock::duration elapsed) noexcept;

    [[nodiscard]] PerformanceMetricSummary summary(PerformanceMetric metric) const noexcept;

    // Clear all metric samples and dropped counts. Active scopes may still record.
    void reset() noexcept;

    [[nodiscard]] static std::size_t metric_index(PerformanceMetric metric) noexcept;

private:
    struct Samples final {
        std::vector<double> values;
        std::size_t dropped{};
    };

    mutable std::mutex mutex_;
    std::array<Samples, 5> samples_;
};

class ScopedPerformanceMeasurement final {
public:
    ScopedPerformanceMeasurement(PerformanceTelemetry& telemetry,
                                 PerformanceMetric metric) noexcept;
    ScopedPerformanceMeasurement(const ScopedPerformanceMeasurement&) = delete;
    ScopedPerformanceMeasurement& operator=(const ScopedPerformanceMeasurement&) = delete;
    ~ScopedPerformanceMeasurement() noexcept;

private:
    PerformanceTelemetry* telemetry_{};
    PerformanceMetric metric_{PerformanceMetric::input};
    std::chrono::steady_clock::time_point started_{};
};

}  // namespace sketch
