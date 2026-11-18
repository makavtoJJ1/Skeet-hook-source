#include "post_firing_detail.hpp"
#include "skeet/rage/preamble_tail.hpp"
#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/acquisition_frame.hpp"
#include "skeet/rage/aim_fire_event.hpp"
#include "skeet/rage/pending_shot_publication.hpp"
#include "skeet/rage/shot_diagnostics.hpp"
#include "skeet/rage/jitter_log.hpp"
#include "skeet/rage/shot_log.hpp"
#include "skeet/rage/feedback_owner.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <limits>

namespace skeet::rage {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
std::int32_t sum(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
float ray_distance(const RageVector& center, const RageVector& origin, const RageVector& axis,
                   float bound) noexcept {
    const auto projection =
        ((center[1] - origin[1]) * axis[1] + (center[0] - origin[0]) * axis[0]) +
        (center[2] - origin[2]) * axis[2];
    if (projection < 0.0f || projection > bound)
        return std::numeric_limits<float>::max();
    const auto x = center[0] - (axis[0] * projection + origin[0]);
    const auto y = center[1] - (axis[1] * projection + origin[1]);
    const auto z = center[2] - (axis[2] * projection + origin[2]);
    return std::sqrt((x * x + y * y) + z * z);
}
void publish_shot(PreambleState&, PreambleResult& preamble, AcquisitionStorage& storage,
                  TargetCandidate& selected, void* local, const PostFiringBindings& b) {
    auto& frame = *preamble.frame;
    auto& command = *preamble.command;
    auto& evaluation = storage.evaluations[std::size_t(&selected - storage.candidates.data())];
    auto* target =
        engine::invoke<void*>(b.movement.targeting.entity_list, 3, selected.entity_index);
    std::int32_t diagnostic_id = 0;
    bool pending = false;
    if (target) {
        if (!frame.field_1a0)
            capture_shot_diagnostics(frame, local, target, b.diagnostics);
        const auto index = b.shot_index % 150;
        diagnostic_id = index;
        b.shot_index = sum(b.shot_index, 1);
        dispatch_aim_fire_event(b.scripts, b.players, frame, selected, evaluation, index);
        pending = publish_pending_shot(
            frame, synchronize_evaluation_state(selected, evaluation), index, b.pending_shots);
        if (b.pending_shots.jitter_fix) {
            if (const auto* feedback = feedback_owner_runtime(b.pending_shots.active))
                log_jitter_fire(*b.players.get(selected.entity_index), feedback->normal_log,
                                selected, evaluation, index, frame.command_number_ac, pending);
        }
        publish_accepted_shot_model(frame, selected, evaluation.record, b.shot_models);
        if (frame.field_1d4 == 1 && selected.primary.damage < selected.health) {
            const RageVector delta{selected.primary.position[0] - frame.origin_dc[0],
                                   selected.primary.position[1] - frame.origin_dc[1],
                                   selected.primary.position[2] - frame.origin_dc[2]};
            const auto inverse =
                1.0f /
                (std::sqrt((delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2]) +
                 std::numeric_limits<float>::epsilon());
            const RageVector direction{inverse * delta[0], inverse * delta[1], inverse * delta[2]};
            const auto limit = b.entity_limit;
            for (std::int32_t index = 1; index < limit; ++index) {
                auto* other = engine::invoke<void*>(b.movement.targeting.entity_list, 3, index);
                if (std::uint32_t(index) == selected.entity_index ||
                    !target_is_eligible(b.movement.targeting, other))
                    continue;
                auto center = read<RageVector>(other, b.movement.offsets.network_origin);
                auto* collideable = engine::invoke<void*>(other, 3);
                center[2] =
                    engine::invoke<const RageVector*>(collideable, 2)->at(2) * 0.5f + center[2];
                if (100.0f >
                    ray_distance(center, frame.origin_dc, direction, read<float>(&frame, 0x130))) {
                    frame.field_1dc = 12;
                    break;
                }
            }
        }
    }
    if (evaluation.requested_tick > 0) {
        const auto time = maximum_number(read<float>(&frame, 0x198), b.tick_correction);
        const auto ticks = _mm_cvtt_ss2si(_mm_set_ss(time / b.preamble.interval + 0.5f));
        const auto corrected = sum(evaluation.requested_tick, ticks);
        if (corrected <=
            sum(std::bit_cast<std::int32_t>(frame.field_18c), read<std::int32_t>(&frame, 0x144)))
            command.tick = corrected;
    }
    if (target)
        if (const auto* feedback = feedback_owner_runtime(b.pending_shots.active))
            log_shot_fire(feedback->spread_log_settings, feedback->normal_log, frame, command,
                          selected, evaluation, diagnostic_id, pending);
    auto& player = b.players.get(selected.entity_index)->native;
    write(&player, 0xA6, std::uint16_t(read<std::uint16_t>(&player, 0xA6) + 1u));
}
}
PreambleTailEntry continue_after_target_firing(PreambleState& state, PreambleResult& preamble,
                                               AcquisitionStorage& storage, TargetBatch& batch,
                                               const TargetFiringResult& firing, void* local,
                                               const PostFiringBindings& b) {
    auto& frame = *preamble.frame;
    auto& command = *preamble.command;
    const auto status = firing.status;
    if (status == TargetFiringStatus::recorded)
        publish_shot(state, preamble, storage, *firing.choice.candidate, local, b);
    const bool cleanup = status == TargetFiringStatus::recorded ||
                         status == TargetFiringStatus::post_angles_stopped ||
                         status == TargetFiringStatus::attack_button_only;
    if (cleanup && !b.preamble.settings.silent_aim.value)
        client::set_command_view_angles(b.command_angles, command.angles);
    if (status == TargetFiringStatus::recorded || status == TargetFiringStatus::retained_target ||
        status == TargetFiringStatus::readiness_stopped ||
        status == TargetFiringStatus::post_angles_stopped ||
        status == TargetFiringStatus::attack_button_only) {
        state.flags |= 0x80U;
        if (firing.choice.candidate->primary.chance != 1.0f)
            state.flags |= 0x100U;
    } else if (status == TargetFiringStatus::rejected_batch)
        state.flags |= 0x100U;
    const auto* leading =
        status == TargetFiringStatus::empty_batch ? b.prior_leading : batch.candidates[0];
    const auto* evaluation =
        leading ? &storage.evaluations[std::size_t(leading - storage.candidates.data())] : nullptr;
    if (b.preamble.settings.quick_stop.value &&
        hotkey_active(b.preamble.settings.quick_stop_key.value.binding, b.preamble.input) &&
        !(state.flags & 0x80U) && leading) {
        state.flags |= 8U;
        float chance = 0.0f;
        TargetFrameBindings current(b.prediction, b.scanner, frame, b.preamble.prediction_time,
                                    b.preamble.scheduler_flags);
        const auto final = b.final_target.with_scan(current.scanner);
        if (evaluate_final_target(frame, state, *leading, *evaluation, state.flags, final, chance))
            state.flags |= 0x180U;
        state.flags &= ~8U;
    }
    post_firing_detail::predict_stop(state, preamble, storage, local, b);
    finish_post_firing_movement(frame, command, leading, evaluation, true,
                                preamble.original_firing_bit, b.movement);
    return PreambleTailEntry::after_firing;
}
} // namespace skeet::rage
