#include "post_firing_detail.hpp"
#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/acquisition_frame.hpp"
#include "skeet/movement/rage_stop.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>

namespace skeet::rage::post_firing_detail {
namespace {
struct SavedFrame {
    std::int32_t tick;
    RageVector origin, bone_origin, velocity;
    float duck_amount;
    std::uint32_t player_flags, flags;
};
MovementVector movement(const RageVector& value) noexcept {
    return {value[0], value[1], value[2]};
}
RageVector rage(const MovementVector& value) noexcept {
    return {value.x, value.y, value.z};
}
}
void predict_stop(PreambleState& state, PreambleResult& preamble, AcquisitionStorage& storage,
                  void* local, const PostFiringBindings& b) {
    auto& frame = *preamble.frame;
    auto& command = *preamble.command;
    const auto& settings = b.preamble.settings;
    if ((state.flags & 0x80U) || !settings.quick_stop.value ||
        !hotkey_active(settings.quick_stop_key.value.binding, b.preamble.input) ||
        !(std::uint32_t(settings.quick_stop_options.value) & 1U) || !frame.weapon_address ||
        !(frame.flags & 0x40000U) || frame.weapon_id_d8 == 31 || (state.flags & 3U))
        return;
    const auto slot = select_post_firing_candidate(state, storage, b.movement.remembered_handle);
    if (slot == no_rage_point)
        return;
    auto& candidate = storage.candidates[slot];
    auto& evaluation = storage.evaluations[slot];
    if (evaluation.requested_tick == -1)
        return;
    reset_scan_point(candidate.primary);
    evaluation.settings.words[0] = 1;
    if (frame.flags & 0x1000000U) {
        evaluation.settings.words[1] &= ~2U;
        evaluation.settings.words[2] &= ~2U;
    }
    if (!(evaluation.settings.words[1] | evaluation.settings.words[2]))
        return;
    state.flags |= 8U;
    auto* target =
        engine::invoke<void*>(b.movement.targeting.entity_list, 3, candidate.entity_index);
    b.players.get(candidate.entity_index); //native563EA, before frame/simulation reads
    const SavedFrame saved{frame.tick_base_c8, frame.origin_dc,      frame.origin_10c,
                           frame.velocity_150, frame.duck_amount_98, frame.player_flags_7c,
                           frame.flags};
    auto* weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
    const auto threshold = rage_stop_speed(weapon);
    MovementSimulation simulation;
    initialize_live_movement_simulation(
        simulation, local, movement(frame.origin_10c), movement(frame.velocity_150),
        frame.player_flags_7c, std::uint32_t(frame.tick_base_c8) + 1u, b.simulation_capture);
    std::int32_t ticks = 0;
    while (ticks < 14) {
        const auto squared = simulation.velocity.y * simulation.velocity.y +
                             simulation.velocity.x * simulation.velocity.x;
        if (threshold * threshold >= squared)
            break;
        auto copy = frame.previous_command;
        prepare_rage_stop_command(frame, local, copy, simulation, b.stop);
        simulate_live_movement_tick(simulation, b.collision, b.stop.environment, local, copy);
        ++ticks;
    }
    bool projected = false;
    if (ticks) {
        const auto x = simulation.position.x - frame.origin_10c[0];
        const auto y = simulation.position.y - frame.origin_10c[1];
        const auto z = simulation.position.z - frame.origin_10c[2];
        if (!(0.1f > ((y * y + x * x) + z * z))) {
            projected = true;
            frame.tick_base_c8 = std::bit_cast<std::int32_t>(simulation.mode);
            frame.origin_dc = {simulation.position.x, simulation.position.y,
                               simulation.view_offset_z + simulation.position.z};
            frame.origin_10c = rage(simulation.position);
            frame.velocity_150 = rage(simulation.velocity);
            frame.duck_amount_98 = simulation.duck_amount;
            frame.player_flags_7c = simulation.player_flags;
            frame.flags = (frame.flags & ~0x40000U) | ((simulation.player_flags & 1U) << 18);
            b.preamble.prediction_time = float(frame.tick_base_c8) * b.preamble.interval;
        }
    }
    if (evaluation.settings.option(0xDE) && (frame.flags & 0x400U) && !(state.flags & 3U) &&
        !(b.preamble.scheduler_flags & 8U)) {
        const auto network_index =
            engine::invoke<std::uint32_t>(static_cast<std::byte*>(target) + 8, 10);
        auto& player_state = *b.players.get(network_index);
        auto trace = b.prediction.trace;
        trace.context_flags = state.flags;
        trace.weapon_flags = state.weapon_flags;
        trace.weapon = weapon;
        const TargetPredictionBindings seed{trace,
                                            b.prediction.record_selection,
                                            b.prediction.engine_trace,
                                            b.prediction.local_maximum_speed,
                                            b.preamble.scheduler_flags,
                                            &command};
        TargetFrameBindings current(seed, b.scanner, frame, b.preamble.prediction_time,
                                    b.preamble.scheduler_flags);
        refine_acquisition_minimum_damage(frame, target, player_state, evaluation.settings,
                                          current.prediction, b.bone_runtime);
    }
    std::array<std::uint32_t, 3> completed{};
    TargetFrameBindings current(b.prediction, b.scanner, frame, b.preamble.prediction_time,
                                b.preamble.scheduler_flags);
    ScanEvaluation scan{candidate, evaluation.record, evaluation.source_record,
                        evaluation.settings.scan_policy(), evaluation.ballistic_evaluations};
    ScanRequest request{
        target, BallisticEntity(reinterpret_cast<std::uintptr_t>(target)), scan, 0, completed, 0};
    scan_candidate(current.scanner, request);
    if (completed[0]) {
        float chance = 1.0f;
        TargetFrameBindings after_scan(b.prediction, b.scanner, frame, b.preamble.prediction_time,
                                       b.preamble.scheduler_flags);
        const auto final = b.final_target.with_scan(after_scan.scanner);
        if (evaluate_final_target(frame, state, candidate, evaluation, state.flags, final,
                                  chance)) {
            state.flags |= 0x180U;
            frame.flags |= 0x10000U;
        }
    }
    state.flags &= ~8U;
    if (projected) {
        frame.tick_base_c8 = saved.tick;
        frame.origin_dc = saved.origin;
        frame.origin_10c = saved.bone_origin;
        frame.velocity_150 = saved.velocity;
        frame.duck_amount_98 = saved.duck_amount;
        frame.player_flags_7c = saved.player_flags;
        frame.flags = (frame.flags & ~0x40000U) | (saved.flags & 0x40000U);
        b.preamble.prediction_time = float(saved.tick) * b.preamble.interval;
    }
}
} // namespace skeet::rage::post_firing_detail
