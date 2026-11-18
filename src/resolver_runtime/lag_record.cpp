#include "skeet/resolver_runtime/attempts.hpp"
#include "skeet/lagcomp/records.hpp"
#include <bit>
#include <cmath>

namespace skeet::resolver_runtime {
namespace {
void apply_variant(lagcomp::LagRecord& record, std::size_t index, std::uint8_t attempt) noexcept {
    const auto& variant = record.alternatives[index];
    float pose = (std::bit_cast<float>(variant.state[1]) - -60.0f) / 120.0f + 0.0f;
    pose = pose > 0.0f ? pose : 0.0f;
    pose = pose < 1.0f ? pose : 1.0f;
    record.animation.pose_parameters[11] = pose;
    record.header.animation_yaw = std::bit_cast<float>(variant.state[0]);
    copy_hitbox_matrices(variant.matrices, record.matrices);
    record.header.resolver_mode = attempt;
}
}

void select_yaw_variant(lagcomp::LagRecord& record, std::uint8_t attempt) noexcept {
    apply_variant(record, yaw_variant_index(attempt), attempt);
}

void select_variant(lagcomp::LagRecord& record, std::uint8_t attempt) noexcept {
    auto& header = record.header;
    const auto mode = header.resolver_index;
    if ((header.flags & 0x80U) == 0 || mode == 0 || mode > 3)
        return;
    const auto cycle = attempt % 3;
    if (mode < 3) {
        select_yaw_variant(record, attempt);
    } else {
        float delta = std::bit_cast<float>(header.player_history_1ac) -
                      std::bit_cast<float>(header.player_history_180);
        if (std::isnan(delta))
            delta = 0.0f;
        else if (std::fabs(delta) > 180.0f) {
            const float turns = round_half_away_from_zero(std::fabs(delta / 360.0f));
            const float correction = turns * 360.0f;
            delta = delta < 0.0f ? delta + correction : delta - correction;
        }
        const bool positive = delta > 0.0f;
        const auto index = cycle == 0 ? (positive ? 0 : 2) : cycle == 1 ? (positive ? 2 : 0) : 1;
        apply_variant(record, index, attempt);
    }
}

void advance_pending_attempt(lagcomp::LagRecord& record, std::uint32_t options) noexcept {
    if ((options & 4U) != 0 && (record.header.flags & 0xc0U) == 0xc0U) {
        record.header.flags &= ~0x40U;
        select_variant(record, static_cast<std::uint8_t>(record.header.resolver_mode + 1U));
    }
}

std::uint32_t propagate_attempt(LagRecordHistory& history, std::uint8_t mode,
                                std::uint8_t previous_attempt, std::uint8_t new_attempt) noexcept {
    if (previous_attempt == new_attempt)
        return 0;
    std::uint32_t processed = 0;
    for (std::uint32_t index = 0; index != history.count; ++index) {
        auto& record = *history.records[(history.first + index) & 31U];
        auto& header = record.header;
        if ((header.flags & 0x22U) != 0 || header.resolver_index != mode ||
            header.resolver_mode != previous_attempt)
            break;
        select_variant(record, new_attempt);
        header.flags = (header.flags & ~0x40U) | 2U;
        ++processed;
        if ((header.flags & 0x200U) != 0)
            break;
    }
    return processed;
}

void apply_feedback(LagRecordHistory& history, lagcomp::LagRecord& source,
                    std::uint8_t previous_attempt, std::uint8_t new_attempt) noexcept {
    propagate_attempt(history, source.header.resolver_index, previous_attempt, new_attempt);
    source.header.flags |= 2U;
}

void publish_feedback(SelectionState& state, LagRecordHistory& history, lagcomp::LagRecord& source,
                      std::uint8_t previous_attempt, std::uint8_t new_attempt) noexcept {
    const auto mode = source.header.resolver_index;
    state.attempt_by_mode[mode] = new_attempt;
    state.status = 1;
    if (mode == 1)
        state.attempt_by_mode[2] = new_attempt;
    else if (mode == 2)
        state.attempt_by_mode[1] = new_attempt;
    apply_feedback(history, source, previous_attempt, new_attempt);
}

} // namespace skeet::resolver_runtime
