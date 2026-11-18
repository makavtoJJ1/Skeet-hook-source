#include "skeet/resolver_runtime/jitter.hpp"
#include "skeet/lagcomp/record_validity.hpp"
#include "skeet/resolver_runtime/attempts.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace skeet::resolver_runtime {
namespace {
float yaw_distance(float a, float b) noexcept {
    return std::fabs(std::remainder(a - b, 360.0f));
}
JitterPoseEstimate closest_pose(float anchor, const std::array<float, 3>& attempts,
                                float confidence_margin) noexcept {
    float best = std::numeric_limits<float>::infinity(), next = best;
    std::uint8_t attempt = 0;
    for (std::uint8_t i = 0; i < attempts.size(); ++i) {
        const auto error = yaw_distance(attempts[i], anchor);
        if (error < best) {
            next = best;
            best = error;
            attempt = i;
        } else {
            next = std::min(next, error);
        }
    }
    const float margin = next - best;
    return {anchor, best, margin, margin >= confidence_margin ? attempt : std::uint8_t{0xff}};
}

JitterPoseEstimate estimate_record_pose(const lagcomp::LagRecord& record, float eye) noexcept {
    const auto& header = record.header;
    const float anchor = std::bit_cast<float>(header.player_history_180);
    if (!std::isfinite(eye) || !std::isfinite(anchor) || !std::isfinite(header.animation_yaw))
        return {};
    std::array<float, 3> attempts;
    for (std::uint8_t attempt = 0; attempt < attempts.size(); ++attempt) {
        attempts[attempt] = std::bit_cast<float>(record.alternatives[yaw_variant_index(attempt)].state[0]);
        if (!std::isfinite(attempts[attempt]))
            return {};
    }
    const float range = std::max(yaw_distance(eye, attempts[1]), yaw_distance(eye, attempts[2]));
    const float confidence = std::max(2.0f, range * 0.1f);
    if (!(range > 0.0f && range <= 90.0f) || yaw_distance(eye, anchor) > range + confidence)
        return {};
    const auto estimate = closest_pose(std::remainder(anchor, 360.0f), attempts, confidence);
    if (!estimate.valid() || yaw_distance(header.animation_yaw, anchor) - estimate.error < confidence)
        return {};
    const auto& variant = record.alternatives[yaw_variant_index(estimate.attempt)];
    if (!std::isfinite(std::bit_cast<float>(variant.state[1])) ||
        !complete_hitbox_matrices(static_cast<std::int32_t>(variant.matrices.size()), variant.matrices))
        return {};
    return estimate;
}
}
JitterPoseEstimate estimate_jitter_pose(float eye_yaw, float animation_yaw,
                                        float maximum_yaw) noexcept {
    if (!std::isfinite(eye_yaw) || !std::isfinite(animation_yaw) ||
        !std::isfinite(maximum_yaw) || maximum_yaw <= 0 || maximum_yaw > 90.0f)
        return {};
    // Use one reconstructed animation state. Compare with the same three
    // yaw attempts the resolver can publish and abstain near a tie.
    const auto confidence_margin = std::max(2.0f, maximum_yaw * 0.1f);
    const auto anchor = std::remainder(animation_yaw, 360.0f);
    const auto eye = std::remainder(eye_yaw, 360.0f);
    if (yaw_distance(eye, anchor) > maximum_yaw + confidence_margin)
        return {anchor};
    return closest_pose(anchor, {eye, eye - maximum_yaw, eye + maximum_yaw}, confidence_margin);
}
const char* jitter_decision_name(JitterDecision decision) noexcept {
    switch (decision) {
    case JitterDecision::animation_yaw: return "animation-yaw-prior";
    case JitterDecision::shot_feedback: return "retain-shot-hypothesis";
    case JitterDecision::environment: return "environment-evidence";
    case JitterDecision::movement: return "animation-movement-evidence";
    case JitterDecision::shared: return "shared-yaw-evidence";
    case JitterDecision::forced_yaw: return "forced-body-yaw";
    case JitterDecision::shot_record: return "target-shot-record";
    case JitterDecision::correction_disabled: return "correction-disabled";
    case JitterDecision::ambiguous_pose: return "no-clear-pose";
    case JitterDecision::unchanged_pose: return "stock-pose-retained";
    case JitterDecision::history_pose: return "history-animation-yaw";
    default: return "no-jitter-correction";
    }
}
void JitterTracking::remember(const JitterRecordEvidence& evidence) noexcept {
    if (evidence.tick < 0)
        return;
    for (auto& record : records)
        if (record.tick == evidence.tick) {
            record = evidence;
            return;
        }
    records[next_record++ % records.size()] = evidence;
}
const JitterRecordEvidence* JitterTracking::find(const lagcomp::LagRecordHeader& header) const noexcept {
    const auto yaw = std::bit_cast<float>(header.player_history_1ac);
    if (header.simulation_tick < 0 || !std::isfinite(yaw))
        return nullptr;
    for (const auto& record : records)
        if (record.tick == header.simulation_tick && record.receive_tick == header.receive_tick &&
            yaw_distance(record.observed_yaw, yaw) < 0.01f)
            return &record;
    return nullptr;
}

bool JitterRecordEvidence::applied_to(const lagcomp::LagRecordHeader& header) const noexcept {
    return applied() && estimate.attempt == header.resolver_mode;
}

void correct_jitter_history(lagcomp::RecordRing& history, JitterTracking& tracking) noexcept {
    if (history.count < 2 || history.count > history.records.size() ||
        (history.at(0).header.flags & 0x200u))
        return;
    // The current record can still be queued for bone building. All older
    // records already have their own capture-time alternatives.
    for (std::uint32_t age = 1; age < history.count; ++age) {
        auto& record = history.at(age);
        const auto& header = record.header;
        if (header.flags & 0x200u)
            break;
        if ((header.flags & 0x84u) != 0x84u || (header.flags & 0x6au) ||
            header.resolver_state != 0 || header.resolver_index > 2)
            continue;
        const auto* captured = tracking.find(header);
        if (captured && (captured->history_checked || captured->applied() ||
                         captured->decision == JitterDecision::forced_yaw ||
                         captured->decision == JitterDecision::shot_feedback))
            continue;
        if (lagcomp::record_precedes_discontinuity(history, header.simulation_tick))
            break;
        auto evidence = captured ? *captured : JitterRecordEvidence{};
        evidence.tick = header.simulation_tick;
        evidence.receive_tick = header.receive_tick;
        evidence.observed_yaw = std::bit_cast<float>(header.player_history_1ac);
        evidence.history_checked = true;
        const auto estimate = estimate_record_pose(record, evidence.observed_yaw);
        if (estimate.valid()) {
            select_yaw_variant(record, estimate.attempt);
            evidence.estimate = estimate;
            evidence.decision = JitterDecision::history_pose;
        }
        tracking.remember(evidence);
    }
}
} // namespace skeet::resolver_runtime
