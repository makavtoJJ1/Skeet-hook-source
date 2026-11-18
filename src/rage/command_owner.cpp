#include "skeet/rage/command_owner.hpp"
#include "skeet/rage/acquisition_frame.hpp"
#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/post_firing.hpp"
#include "skeet/legit/runtime.hpp"
#include <algorithm>
#include "targeting_pipeline_detail.hpp"
#include <bit>
#include <cstring>

namespace skeet::rage {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
FiringState firing_state(const client::CommandFrame& frame, std::uint8_t& send_packet) noexcept {
    FiringState result;
    result.flags = frame.flags;
    result.weapon_id = frame.weapon_id_d8;
    result.marker = frame.command_number_ac;
    result.previous_tick = frame.tick_base_c8;
    result.current_tick = std::bit_cast<std::int32_t>(frame.field_1d8);
    result.field_fc = frame.choke_limit_fc;
    result.field_15c = frame.choked_15c;
    result.field_1d4 = std::bit_cast<std::int32_t>(frame.field_1d4);
    result.field_198 = field<float>(&frame, 0x198);
    result.reference_angles = {frame.previous_command.angles[0], frame.previous_command.angles[1],
                               frame.previous_command.angles[2]};
    result.recoil = frame.recoil_angles_9c;
    result.origin = frame.origin_dc;
    result.bone_origin = frame.origin_10c;
    result.field_84 = frame.view_height_84;
    result.field_a8 = frame.player_field_a8;
    result.field_164 = frame.height_164;
    result.special_threshold = frame.postpone_fire_1e4;
    result.angle_step = frame.maximum_angle_step_1e8;
    result.send_packet = &send_packet;
    return result;
}
void publish_firing_state(client::CommandFrame& frame, const FiringState& state) noexcept {
    // These are the frame members written by readiness/angles/commit. The
    // remaining projection fields are inputs and retain their native bytes.
    frame.flags = state.flags;
    frame.tick_base_c8 = state.previous_tick;
    frame.origin_dc = state.origin;
    frame.view_height_84 = state.field_84;
}

}
void process_rage_command_phase2(CommandOwner& owner, void* player, void* weapon,
                                 client::Command& command, std::array<float, 3>& incoming,
                                 std::uint8_t& send_packet) {
    process_rage_command_phase2(owner.state, player, weapon, command, incoming, send_packet,
                                owner.phase2);
}
void process_rage_phase6(CommandOwner& owner, const client::CommandFrame& frame,
                         const client::Command& command) {
    process_rage_phase6(frame, command, owner.phase6);
}
void capture_rage_command_frame(CommandOwner& owner, const client::CommandFrame& frame) noexcept {
    capture_rage_command_frame(owner.state, frame);
}
void process_rage_command(CommandOwner& owner, client::CommandFrame& original, void* player,
                          client::Command& command, std::uint8_t& send_packet) {
    auto& state = owner.state;
    auto& storage = owner.acquisition;
    auto preamble = prepare_rage_command(state, original, command, player, storage, owner.preamble);
    switch (preamble.continuation) {
    case PreambleContinuation::no_weapon:
        finish_rage_command(state, preamble, original, player, owner.tail,
                            PreambleTailEntry::return_only);
        return;
    case PreambleContinuation::inactive:
        finish_rage_command(state, preamble, original, player, owner.tail,
                            PreambleTailEntry::actions);
        return;
    case PreambleContinuation::ammunition:
        finish_rage_command(state, preamble, original, player, owner.tail,
                            PreambleTailEntry::after_firing);
        return;
    case PreambleContinuation::single_target:
    case PreambleContinuation::multiple_targets:
        break;
    }
    auto& frame = *preamble.frame;
    auto& selected_command = *preamble.command;
    AcquisitionFrameBindings current(state, preamble, owner.preamble.prediction_time,
                                     owner.preamble.yaw_target.handle,
                                     owner.preamble.scheduler_flags, owner.single_target,
                                     owner.multiple_targets, owner.settings_menu);
    const bool single = preamble.continuation == PreambleContinuation::single_target;
    bool acquired = true;
    if (single)
        acquired = acquire_single_target(state, preamble, storage, current.single);
    else
        acquire_multiple_targets(state, preamble, storage, current.multiple);
    auto& scanner = single ? current.single.records.scanner : current.multiple.records.scanner;
    TargetBatch batch;
    // A missing single selection jumps directly to54908, bypassing the
    // completion traversal, job drain and per-player cleanup.
    if (acquired)
        batch = rank_completed_target_candidates(storage.candidates, state.completed, state.flags,
                                                 state.target_mask, scanner.targeting);
    const auto final_target = owner.final_target.with_scan(scanner);
    auto choice = select_acquired_target_for_shot(batch, storage, frame, state, final_target);
    RageShotCommitState commit{state.flags, owner.preamble.scheduler_flags,
                               owner.preamble.selected_marker, send_packet};
    TargetFiringResult firing{
        batch.count ? TargetFiringStatus::rejected_batch : TargetFiringStatus::empty_batch, choice};
    if (choice.candidate) {
        const auto& evaluation = acquisition_evaluation_for(storage, *choice.candidate);
        const TargetFiringTiming timing{evaluation.requested_tick,
                                        evaluation.record.header.simulation_tick};
        auto firing_frame = firing_state(frame, send_packet);
        RageShotState shot{
            frame.flags, frame.weapon_id_d8, frame.postpone_fire_1e4, {}, frame.command_number_ac};
        auto readiness = owner.readiness;
        readiness.tick_interval = owner.preamble.interval;
        readiness.correction_time = 0.5f;
        readiness.enabled = owner.preamble.settings.automatic_fire.value != 0;
        readiness.mode_enabled = owner.preamble.settings.silent_aim.value != 0;
        readiness.send_mode = owner.preamble.send_mode;
        readiness.committed_marker = owner.preamble.selected_marker;
        readiness.command_marker = owner.preamble.current_sequence;
        if (frame.weapon_id_d8 == 64)
            readiness.revolver_gate =
                field<float>(reinterpret_cast<const void*>(std::uintptr_t(frame.weapon_address)),
                             owner.phase2.offsets.fire_state.next_secondary);
        readiness.scheduler_flags = commit.context_flags;
        const FiringTargetData target{*choice.candidate, timing.field_b4, timing.field_13c};
        const auto ready =
            evaluate_firing_readiness(firing_frame, target, preamble.firing, owner.firing_shared,
                                      selected_command, readiness);
        if (ready == FiringReadiness::retained_target)
            firing = {TargetFiringStatus::retained_target, choice};
        else if (ready == FiringReadiness::stopped)
            firing = {TargetFiringStatus::readiness_stopped, choice};
        else {
            auto post = owner.post_angles;
            post.player = player;
            post.enabled = owner.preamble.settings.automatic_fire.value != 0;
            post.send_mode = owner.preamble.send_mode;
            post.recoil_option = owner.preamble.settings.remove_recoil.value != 0;
            post.compensate_recoil =
                legit::recoil_compensation_enabled(
                    owner.tail.legit_settings,
                    std::uint32_t(std::int32_t(std::int8_t(frame.weapon_kind_124))),
                    owner.preamble.input) ||
                !post.recoil_option;
            commit.command_flags = owner.preamble.scheduler_flags;
            firing = targeting_detail::aim_and_commit_target(
                choice, firing_frame, preamble.firing, selected_command, shot, commit, readiness,
                post,
                single ? current.single.records.bone_runtime
                       : current.multiple.records.bone_runtime);
        }
        publish_firing_state(frame, firing_frame);
        state.flags = commit.context_flags;
        if (firing.status == TargetFiringStatus::recorded) {
            std::memcpy(reinterpret_cast<std::byte*>(&frame) + 0xE8, shot.selected_aim_point.data(),
                        sizeof shot.selected_aim_point);
            owner.preamble.selected_marker = commit.last_committed_marker;
            owner.preamble.scheduler_flags = commit.command_flags;
        }
    }
    const auto& base = owner.post_firing;
    const auto& movement = base.movement;
    const PostFiringMovementBindings live_movement{owner.preamble.settings,
                                                   owner.preamble.input,
                                                   state.flags,
                                                   owner.preamble.scheduler_flags,
                                                   state.weapon_flags,
                                                   state.target_mask,
                                                   scanner.targeting,
                                                   movement.offsets,
                                                   scanner.ballistics,
                                                   scanner.source,
                                                   scanner.weapon_item_offset,
                                                   scanner.trace_filter_vtable,
                                                   scanner.trace_entities,
                                                   movement.prediction,
                                                   owner.preamble.prediction_time,
                                                   owner.preamble.yaw_target.handle};
    const auto& branch_prediction =
        single ? current.single.prediction : current.multiple.prediction;
    auto trace = branch_prediction.trace;
    trace.context_flags = state.flags;
    trace.weapon_flags = state.weapon_flags;
    trace.weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
    const TargetPredictionBindings prediction{trace,
                                              branch_prediction.record_selection,
                                              branch_prediction.engine_trace,
                                              branch_prediction.local_maximum_speed,
                                              owner.preamble.scheduler_flags,
                                              &selected_command};
    const PostFiringBindings continuation{owner.preamble,
                                          live_movement,
                                          base.players,
                                          base.command_angles,
                                          final_target,
                                          base.scripts,
                                          base.diagnostics,
                                          base.pending_shots,
                                          base.shot_models,
                                          base.shot_index,
                                          base.entity_limit,
                                          base.tick_correction,
                                          nullptr,
                                          base.stop,
                                          base.simulation_capture,
                                          base.collision,
                                          prediction,
                                          scanner,
                                          single ? current.single.records.bone_runtime
                                                 : current.multiple.records.bone_runtime};
    const auto tail =
        continue_after_target_firing(state, preamble, storage, batch, firing, player, continuation);
    finish_rage_command(state, preamble, original, player, owner.tail, tail);
}
} // namespace skeet::rage
