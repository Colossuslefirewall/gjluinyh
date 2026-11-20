#include "modules/tracking/upgrade_core.hpp"
#include "modules/tracking/comparison_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kBaseTime = 10.0;
constexpr double kDuration = 4.0;
constexpr double kObservationAge = 0.015;
constexpr double kLeadMs = 20.0;
constexpr double kMaxLead = 20.0;
constexpr int kWarmupSlots = 12;

enum class Scenario {
    StaticJitter, Linear, Reversal, CameraStep, Dropout,
    TimestampAnomalies, IdentityReset, BoxResize
};
enum class TimestampIssue { None, Duplicate, OutOfOrder };
enum class Method { LegacyRaw, LegacyKalman, CmpRaw, CmpKalman, CmpEma, CmpSunone };

struct Point { double x, y; };
struct Sample {
    double elapsed = 0, timestamp = 0, delivery = 0;
    Point measured;
    float width = 80, height = 100;
    bool hasDetection = true, reset = false;
    TimestampIssue issue = TimestampIssue::None;
};
struct Metrics {
    std::uint64_t attempts = 0, valid = 0, skipped = 0, rejectedProjection = 0;
    std::uint64_t timestampAnomalies = 0, rejectedInput = 0, staleAfterReject = 0;
    std::uint64_t timed = 0;
    long double squaredError = 0;
    double maxError = 0, maxCorrection = 0;
    std::vector<double> cpuUs;
};

const char* Name(Scenario s) {
    switch (s) {
    case Scenario::StaticJitter: return "static_jitter";
    case Scenario::Linear: return "linear";
    case Scenario::Reversal: return "reversal";
    case Scenario::CameraStep: return "camera_step";
    case Scenario::Dropout: return "dropout";
    case Scenario::TimestampAnomalies: return "duplicate_out_of_order";
    case Scenario::IdentityReset: return "identity_reset";
    case Scenario::BoxResize: return "box_resize";
    }
    return "unknown";
}

const char* Name(Method m) {
    switch (m) {
    case Method::LegacyRaw: return "stable_legacy_raw";
    case Method::LegacyKalman: return "stable_legacy_kalman";
    case Method::CmpRaw: return "comparison_raw";
    case Method::CmpKalman: return "comparison_kalman";
    case Method::CmpEma: return "comparison_velocity_ema";
    case Method::CmpSunone: return "comparison_sunone";
    }
    return "unknown";
}

Point Truth(Scenario scenario, double t) {
    switch (scenario) {
    case Scenario::StaticJitter: return {0, 0};
    case Scenario::Linear: return {20 + 35 * t, -10 + 12 * t};
    case Scenario::Reversal:
        return t <= 2 ? Point{-80 + 45 * t, 15 + 8 * t}
                      : Point{10 - 45 * (t - 2), 31 - 8 * (t - 2)};
    case Scenario::CameraStep:
        return {15 + (t >= 2 ? 140.0 : 0.0), -20 - (t >= 2 ? 90.0 : 0.0)};
    case Scenario::Dropout: return {-30 + 28 * t, 25 - 9 * t};
    case Scenario::TimestampAnomalies: return {12 + 30 * t, -18 + 6 * t};
    case Scenario::IdentityReset:
        return t < 2 ? Point{-120 + 20 * t, 50 + 5 * t}
                     : Point{-70 - 30 * (t - 2), 65 + 8 * (t - 2)};
    case Scenario::BoxResize: return {-20 + 25 * t, 30 + 10 * t};
    }
    return {};
}

bool IsDropout(double t) {
    return (t >= 1.0 && t < 1.45) || (t >= 2.15 && t < 2.23);
}

std::vector<Sample> MakeSamples(Scenario scenario, int hz, std::uint32_t seed) {
    const int count = static_cast<int>(std::lround(kDuration * hz));
    const int duplicateIndex = hz;
    const int outOfOrderIndex = 2 * hz;
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, scenario == Scenario::StaticJitter ? 2.5 : 1.5);
    std::vector<Sample> samples;
    samples.reserve(count);
    for (int i = 0; i < count; ++i) {
        Sample s;
        s.elapsed = static_cast<double>(i) / hz;
        const Point truth = Truth(scenario, s.elapsed);
        s.measured = {truth.x + noise(rng), truth.y + noise(rng)};
        s.timestamp = kBaseTime + s.elapsed;
        s.delivery = s.timestamp + kObservationAge;
        s.reset = scenario == Scenario::IdentityReset && i == 2 * hz;
        s.hasDetection = scenario != Scenario::Dropout || !IsDropout(s.elapsed);
        if (scenario == Scenario::BoxResize && s.elapsed >= 2) {
            s.width = 240;
            s.height = 300;
        }
        if (scenario == Scenario::TimestampAnomalies && i == duplicateIndex) {
            s.timestamp -= 1.0 / hz;
            s.delivery = kBaseTime + s.elapsed + kObservationAge;
            s.issue = TimestampIssue::Duplicate;
        } else if (scenario == Scenario::TimestampAnomalies && i == outOfOrderIndex) {
            s.timestamp -= 4.0 / hz;
            s.delivery = kBaseTime + s.elapsed + kObservationAge;
            s.issue = TimestampIssue::OutOfOrder;
        }
        samples.push_back(s);
    }
    return samples;
}

double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p * values.size()));
    return values[std::max<std::size_t>(1, rank) - 1];
}

// Execute the same single-selected-track projection branches used by tracking.cpp.
Metrics Run(Scenario scenario, int hz, Method method, bool prediction) {
    constexpr std::array<std::uint32_t, 3> seeds{0x51A7u, 0xB10Cu, 0xA17E5u};
    Metrics total;
    for (std::uint32_t seed : seeds) {
        const auto samples = MakeSamples(scenario, hz, seed);
        albion::upgrade::Options options;
        options.processNoise = 300;
        options.measurementNoise = 9;
        options.predictionMs = static_cast<float>(kLeadMs);
        options.maxLead = static_cast<float>(kMaxLead);
        options.prediction = prediction;
        options.kalman = method == Method::LegacyKalman;
        options.comparisonMode = method == Method::CmpRaw ? 1 :
                                 method == Method::CmpKalman ? 2 :
                                 method == Method::CmpEma ? 3 :
                                 method == Method::CmpSunone ? 4 : 0;
        albion::upgrade::Tracker legacy;
        albion::comparison::Tracker comparison;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Sample& s = samples[i];
            if (s.reset) {
                legacy.Reset();
                comparison.Reset();
            }
            std::vector<Detection> detections;
            if (s.hasDetection) {
                detections.push_back(Detection{
                    static_cast<float>(s.measured.x - s.width * .5),
                    static_cast<float>(s.measured.y - s.height * .5),
                    s.width, s.height, .9f, 0});
            }
            if (s.issue != TimestampIssue::None) {
                ++total.timestampAnomalies;
                if (method != Method::LegacyRaw && method != Method::LegacyKalman)
                    ++total.rejectedInput;
            }
            const auto begin = Clock::now();
            const albion::upgrade::Track* legacyTrack = nullptr;
            const albion::comparison::Track* comparisonTrack = nullptr;
            if (method == Method::LegacyRaw || method == Method::LegacyKalman) {
                legacy.Update(detections, s.timestamp, options, -1);
                for (const auto& track : legacy.Tracks())
                    if (track.observed && track.box.score >= 0) { legacyTrack = &track; break; }
            } else {
                comparison.Update(detections, s.timestamp, options, -1, 0);
                for (const auto& track : comparison.Tracks())
                    if (track.observed && track.box.score >= 0) { comparisonTrack = &track; break; }
            }

            bool hadTrack = false, rejected = false;
            double outputX = 0, outputY = 0, rawX = 0, rawY = 0;
            if (legacyTrack) {
                hadTrack = true;
                rawX = legacyTrack->box.x + legacyTrack->box.w * .5;
                rawY = legacyTrack->box.y + legacyTrack->box.h * .5;
                outputX = rawX;
                outputY = rawY;
                double vx = legacyTrack->vx, vy = legacyTrack->vy;
                if (method == Method::LegacyKalman) {
                    outputX += legacyTrack->fx.x - rawX;
                    outputY += legacyTrack->fy.x - rawY;
                    vx = legacyTrack->fx.v;
                    vy = legacyTrack->fy.v;
                }
                if (prediction) {
                    const double horizon = std::clamp(s.delivery - s.timestamp, 0.0, .15) + kLeadMs * .001;
                    double dx = vx * horizon, dy = vy * horizon;
                    const double magnitude = std::hypot(dx, dy);
                    if (magnitude > kMaxLead && magnitude > 0) {
                        dx *= kMaxLead / magnitude;
                        dy *= kMaxLead / magnitude;
                    }
                    outputX += dx;
                    outputY += dy;
                }
            } else if (comparisonTrack) {
                hadTrack = true;
                rawX = comparisonTrack->box.x + comparisonTrack->box.w * .5;
                rawY = comparisonTrack->box.y + comparisonTrack->box.h * .5;
                const int mode = options.comparisonMode - 1;
                const double rawDistance = std::hypot(rawX + 1000, rawY + 1000);
                const auto projected = comparisonTrack->motion.Project(
                    mode, rawX, rawY, s.delivery, kLeadMs, kMaxLead,
                    prediction, mode != 0, rawDistance);
                if (!projected.valid) rejected = true;
                else { outputX = projected.x; outputY = projected.y; }
            }
            const auto end = Clock::now();
            if (i >= kWarmupSlots) {
                total.cpuUs.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
                ++total.timed;
            }
            ++total.attempts;
            if (hadTrack && !rejected) {
                const double evalElapsed = (s.delivery - kBaseTime) + (prediction ? kLeadMs * .001 : 0);
                const Point truth = Truth(scenario, evalElapsed);
                if (!std::isfinite(outputX) || !std::isfinite(outputY)) {
                    ++total.rejectedProjection;
                } else {
                    const double error = std::hypot(outputX - truth.x, outputY - truth.y);
                    total.squaredError += static_cast<long double>(error) * error;
                    total.maxError = std::max(total.maxError, error);
                    total.maxCorrection = std::max(total.maxCorrection,
                                                   std::hypot(outputX - rawX, outputY - rawY));
                    ++total.valid;
                    if (s.issue != TimestampIssue::None &&
                        method != Method::LegacyRaw && method != Method::LegacyKalman)
                        ++total.staleAfterReject;
                }
            } else if (rejected) {
                ++total.rejectedProjection;
            } else {
                ++total.skipped;
            }
        }
    }
    return total;
}

void PrintMetric(double value) {
    if (std::isfinite(value)) std::cout << value;
    else std::cout << "NA";
}
} // namespace

int main() {
    constexpr std::array<Scenario, 8> scenarios{
        Scenario::StaticJitter, Scenario::Linear, Scenario::Reversal, Scenario::CameraStep,
        Scenario::Dropout, Scenario::TimestampAnomalies, Scenario::IdentityReset, Scenario::BoxResize};
    constexpr std::array<int, 4> rates{30, 60, 144, 240};
    constexpr std::array<Method, 6> methods{
        Method::LegacyRaw, Method::LegacyKalman, Method::CmpRaw,
        Method::CmpKalman, Method::CmpEma, Method::CmpSunone};
    std::cerr << "Synthetic estimator-only benchmark; GCC/C++ runtime timing is not application latency. "
              << "3 fixed seeds; 4s per seed; first " << kWarmupSlots
              << " slots excluded from CPU percentiles; p99 aggregates at least "
              << (static_cast<int>(kDuration * 30) - kWarmupSlots) * 3 << " timings per row.\n"
              << "Normal timestamp age=" << (kObservationAge * 1000) << "ms; prediction lead="
              << kLeadMs << "ms; prediction-on truth is evaluated at delivery+lead; off at delivery. "
              << "RMSE excludes no-output frames only because no coordinate exists; skipped/rejected counts are explicit.\n";
    std::cout << "scenario,hz,method,prediction,seeds,attempts,valid_outputs,skipped_no_output,"
                 "rejected_projections,timestamp_anomalies,rejected_inputs,stale_outputs_after_reject,"
                 "rmse_px,max_error_px,max_correction_px,"
                 "cpu_samples,cpu_p50_us,cpu_p95_us,cpu_p99_us\n";
    std::cout << std::fixed << std::setprecision(4);
    for (Scenario scenario : scenarios) for (int hz : rates)
        for (Method method : methods) for (bool prediction : {false, true}) {
            const Metrics m = Run(scenario, hz, method, prediction);
            std::cout << Name(scenario) << ',' << hz << ',' << Name(method) << ','
                      << (prediction ? "on" : "off") << ",3," << m.attempts << ',' << m.valid << ','
                      << m.skipped << ',' << m.rejectedProjection << ',' << m.timestampAnomalies << ','
                      << m.rejectedInput << ',' << m.staleAfterReject << ',';
            PrintMetric(m.valid ? std::sqrt(static_cast<double>(m.squaredError / m.valid))
                                : std::numeric_limits<double>::quiet_NaN());
            std::cout << ',';
            PrintMetric(m.valid ? m.maxError : std::numeric_limits<double>::quiet_NaN());
            std::cout << ',';
            PrintMetric(m.valid ? m.maxCorrection : std::numeric_limits<double>::quiet_NaN());
            std::cout << ',' << m.timed << ',';
            PrintMetric(Percentile(m.cpuUs, .50)); std::cout << ',';
            PrintMetric(Percentile(m.cpuUs, .95)); std::cout << ',';
            PrintMetric(Percentile(m.cpuUs, .99)); std::cout << '\n';
        }
    return 0;
}
