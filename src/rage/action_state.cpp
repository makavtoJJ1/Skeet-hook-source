#include "action_detail.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/lagcomp/update_timing.hpp"
#include "skeet/rage_safety.hpp"
#include "skeet/rage/scanning_geometry.hpp"
#include "skeet/rage/feedback_owner.hpp"
#include "skeet/anti_aim/targets.hpp"
#include "skeet/client/target_refresh_runtime_owner.hpp"
#include "skeet/movement/extrapolation.hpp"
#include "../rage_math.hpp"
#include "../rage_point_math.hpp"
#include <bit>
#include <immintrin.h>

namespace skeet::rage {
namespace {
using action_detail::read;
struct ActionHistoryFrame {
    lagcomp::HistoryTickTiming timing;
    lagcomp::EntityPreparationContext preparation;
    lagcomp::ExtrapolatedRecordBindings extrapolation;
    lagcomp::HistorySelectionBindings history;
    const client::CommandBudgetBindings& live_budget;
    ActionHistoryFrame(const lagcomp::HistorySelectionBindings& seed,
                       const client::CommandFrame& frame, float clock,
                       const client::CommandBudgetBindings& budget)
        : timing{clock,
                 read<float>(&frame, 0x198),
                 seed.interpolation,
                 seed.extrapolation.preparation.entity.interval,
                 seed.maximum_unlag_ms,
                 read<std::int32_t>(&frame, 0xD4),
                 std::bit_cast<std::int32_t>(frame.field_18c)},
          preparation(seed.extrapolation.preparation),
          extrapolation{seed.extrapolation.players,     preparation,
                        seed.extrapolation.attachments, seed.extrapolation.bones,
                        seed.extrapolation.movement,    seed.extrapolation.prediction_limit,
                        seed.extrapolation.validate_record, seed.extrapolation.rebuild_pose},
          history{extrapolation, seed.alternative,   seed.rotation,        seed.matrix_scratch,
                  clock,         seed.interpolation, seed.maximum_unlag_ms, seed.jitter_fix,
                  seed.animation_rebuild, seed.resolver_corrections},
          live_budget(budget) {
        preparation.global_frame = std::bit_cast<std::uint32_t>(clock);
        preparation.current_timing =
            seed.extrapolation.preparation.current_timing ? &timing : nullptr;
    }
    ActionHistoryFrame(const ActionHistoryFrame&) = delete;
    ActionHistoryFrame& operator=(const ActionHistoryFrame&) = delete;
};
void refresh_target(const client::CommandFrame& frame, client::Command& command,
                    const TargetBindings& targeting, const ActionStateBindings& bindings) {
    if (auto* live = std::get_if<std::reference_wrapper<const client::TargetRefreshRuntimeOwner>>(
            &bindings.preamble.target_refresh)) {
        live->get().refresh(bindings.preamble.yaw_target, targeting.local_player, command,
                            frame.previous_command.movement.buttons, read<float>(&frame, 0x11C));
        return;
    }
    const auto& refresh = std::get<std::reference_wrapper<const anti_aim::TargetRefreshContext>>(
                              bindings.preamble.target_refresh)
                              .get();
    const auto& base = refresh.scored;
    const auto& seed = base.common;
    const anti_aim::TargetSelectorContext common{targeting,
                                                 seed.engine_client,
                                                 seed.max_clients,
                                                 seed.histories,
                                                 seed.offsets,
                                                 seed.timing,
                                                 seed.prediction_limit,
                                                 seed.interval,
                                                 seed.receive_interval,
                                                 seed.collision,
                                                 seed.ballistics,
                                                 seed.trace_flag};
    const anti_aim::TargetScoredContext scored{
        {common, command, frame.previous_command.movement.buttons, read<float>(&frame, 0x11C),
         base.player_rules, base.dormant_health, base.offsets, base.geometry_offsets,
         base.local_pose_origin, base.local_pose, base.trigonometry, base.sequence,
         base.clock_owner, base.local_time_offset, base.trace_filter_vtable, base.trace_entities},
        bindings.live_movement.capture,
        bindings.live_movement.environment};
    anti_aim::refresh_yaw_target(bindings.preamble.yaw_target, {scored, refresh.selector_convar});
}
bool accepts_point(const action_detail::Point& point, const client::CommandFrame& frame,
                   const scan_detail::Geometry& geometry, std::int32_t requested_group,
                   std::int32_t minimum_damage, void* local, void* target,
                   const ActionStateBindings& bindings) {
    auto* weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
    auto query = prepare_target_bullet(weapon, bindings.scan.weapon_item_offset,
                                       bindings.preamble.settings.automatic_penetration.value != 0);
    const auto delta = rage_point_detail::subtract(point.position, frame.origin_dc);
    const auto direction = rage_point_detail::normalize(delta, rage_point_detail::length(delta));
    if (point.flags & 1U) {
        RageVector shot;
        for (std::size_t axis = 0; axis != 3; ++axis)
            shot[axis] = query.range * direction[axis];
        const auto multiplier =
            rage_detail::hitgroup_multipliers[std::uint32_t(requested_group) & 15U];
        if (!rage_point_safe({frame.origin_dc, shot}, requested_group, multiplier, geometry.views(),
                             bindings.scan.box_clipper))
            return false;
    }
    RageVector extended;
    for (std::size_t axis = 0; axis != 3; ++axis)
        extended[axis] = (direction[axis] * 120.0f + point.position[axis]) - frame.origin_dc[axis];
    const auto projection = intersect_scan_hitboxes({frame.origin_dc, extended}, geometry.primary,
                                                    bindings.scan.box_clipper);
    if (!projection.found || geometry.primary[projection.hitbox].hitgroup != requested_group)
        return false;
    // Native publishes this projected intersection into its temporary trace;
    // the ensuing point-to-point bullet call still receives the descriptor.
    anti_aim::TargetTraceFilter native_filter(bindings.scan.trace_filter_vtable,
                                              bindings.scan.trace_entities, 0x600400BU, 0x600400BU,
                                              1.0f);
    const auto filter = native_filter.binding();
    const auto source = BallisticEntity(reinterpret_cast<std::uintptr_t>(local));
    if (!trace_ballistic_bullet(bindings.scan.ballistics, source, frame.origin_dc, point.position,
                                filter, query))
        return false;
    const auto info = bindings.scan.ballistics.entity_info(
        BallisticEntity(reinterpret_cast<std::uintptr_t>(target)));
    const auto damage = final_bullet_damage(
        query.damage, query.armor_ratio, std::bit_cast<std::int32_t>(query.reserved),
        query.weapon_id, requested_group, info.armor, info.helmet);
    return damage >= minimum_damage && damage >= 0;
}
}
void update_action_state(esp::HitboxHighlightState& output, client::CommandFrame& frame,
                         void* local, client::Command& command,
                         const ActionStateBindings& bindings) {
    if (!bindings.active)
        return;
    for (auto& entry : output.entries)
        entry.flags = 0;
    output.target_handle = -1;
    if (!bindings.visualization.enabled.value && !bindings.visualization.safe_point.value)
        return;
    if (!frame.weapon_address || frame.weapon_id_d8 == 31)
        return;
    auto* weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
    const auto& seed = bindings.target_preparation.targeting;
    const TargetBindings targeting{
        seed.local_player,           seed.entity_list, seed.game_type, seed.teammates_are_enemies,
        bindings.player_adjustments, seed.offsets};
    if (!lagcomp::history_weapon_is_firearm(
            read<std::uint16_t>(weapon, targeting.offsets.weapon_identifier)))
        return;
    refresh_target(frame, command, targeting, bindings);
    const auto handle = bindings.preamble.yaw_target.handle;
    auto* target = handle == -1 ? nullptr : engine::invoke<void*>(targeting.entity_list, 4, handle);
    if (!target_is_eligible(targeting, target))
        return;
    const auto movement = read<std::uint8_t>(target, targeting.offsets.movement_type);
    if (movement == 8 || movement == 10 ||
        (read<std::uint8_t>(target, targeting.offsets.movement_flags) & 0x40U))
        return;
    const auto initial_model = scan_detail::model(target, bindings.scan.model_offsets);
    if (!initial_model.header || initial_model.hitboxes.empty())
        return;
    AcquisitionTargetSettings settings{
        prepare_rage_scan_template(bindings.state, frame, bindings.preamble)};
    const SpreadParameters spread{std::uint32_t(frame.weapon_id_d8),
                                  std::int32_t(frame.field_b0),
                                  read<float>(&frame, 0xD0),
                                  read<float>(&frame, 0xC4),
                                  std::bit_cast<float>(frame.field_100),
                                  read<std::int32_t>(&frame, 0x19C),
                                  bindings.preamble.shotgun_patterns,
                                  frame.weapon_kind_124};
    prepare_spread_cache(bindings.state.spread, spread, std::bit_cast<float>(frame.field_160),
                         std::uint32_t(frame.command_number_ac),
                         (bindings.state.flags & 0x10U) != 0, bindings.preamble.spread_tables,
                         bindings.preamble.random, bindings.preamble.pattern,
                         bindings.preamble.trigonometry);
    const auto networkable = static_cast<std::byte*>(target) + 8;
    const auto initial_index = engine::invoke<std::uint32_t>(networkable, 10);
    const auto health = engine::invoke<std::int32_t>(target, 122);
    PendingTargetDamage pending;
    const auto pending_present =
        pending_target_damage(feedback_owner_state(bindings.feedback_owner), pending);
    (void)scan_target_health(health, initial_index, pending_present ? &pending : nullptr);
    const auto index = engine::invoke<std::uint32_t>(networkable, 10);
    action_detail::prepare_settings(frame, target, index, settings, bindings);
    if (!(settings.words[1] | settings.words[2]))
        return;
    const auto shifted_origins = action_detail::origins(frame, target, bindings);
    const auto history_index = engine::invoke<std::uint32_t>(networkable, 10);
    auto& player = *bindings.scan.players.get(history_index);
    bindings.state.flags |= 1U;
    const auto requested_tick =
        player.history.count
            ? player.history.at(0).header.simulation_tick
            : lagcomp::time_to_tick(
                  read<float>(target, bindings.scan.preparation.entity.offsets.simulation_time),
                  bindings.preamble.interval);
    lagcomp::LagRecord record;
    record.header.field_00 = 0;
    std::uint32_t record_flags = 0;
    // The selected historical frame and readiness can both change the live
    // prediction clock after the acquisition bindings were constructed.
    // Own every refreshed reference until the complete history call returns.
    const ActionHistoryFrame selection(bindings.history, frame, bindings.preamble.prediction_time,
                                       bindings.live_budget);
    if (!lagcomp::select_target_history_record(
            frame, target, requested_tick, 8, record, record_flags, selection.history,
            bindings.bone_runtime, bindings.live_movement, selection.live_budget)) {
        bindings.state.flags &= ~1U;
        return;
    }
    // The original prepares one query before geometry and a fresh one for
    // every descriptor; its range is used by the three-hypothesis safety rays.
    (void)prepare_target_bullet(weapon, bindings.scan.weapon_item_offset,
                                bindings.preamble.settings.automatic_penetration.value != 0);
    const auto original_origin = frame.origin_dc;
    const auto safe_mask = bindings.visualization.safe_point.value && (record.header.flags & 0x80U)
                               ? settings.words[2]
                               : 0U;
    const auto model = scan_detail::model(target, bindings.scan.model_offsets);
    scan_detail::Geometry geometry;
    geometry.primary =
        prepare_rage_scan_hitboxes(model.hitboxes, record.matrices, bindings.scan.trigonometry);
    if (safe_mask)
        for (std::size_t i = 0; i != 3; ++i)
            geometry.alternatives[i] = prepare_rage_scan_hitboxes(
                model.hitboxes, record.alternatives[i].matrices, bindings.scan.trigonometry);
    for (auto& entry : output.entries) {
        if (entry.hitbox >= std::int32_t(initial_model.hitboxes.size()))
            continue;
        const auto index = std::uint32_t(entry.hitbox);
        const auto group = initial_model.hitboxes[index].hitgroup;
        const auto group_bit = 1U << (std::uint32_t(group) & 31U);
        if (!(group_bit & (safe_mask | settings.words[1])))
            continue;
        for (const auto& origin : shifted_origins) {
            frame.origin_dc = origin;
            const auto points = action_detail::points(
                frame, settings, record, model, geometry, index, (safe_mask & group_bit) != 0,
                bindings.visualization.enabled.value != 0, bindings);
            bool accepted = false;
            for (const auto& point : points) {
                if (!accepts_point(point, frame, geometry, group,
                                   std::bit_cast<std::int32_t>(settings.words[5]), local, target,
                                   bindings))
                    continue;
                entry.flags |= point.highlight;
                accepted = true;
                break;
            }
            if (accepted)
                break;
        }
        if (entry.flags) {
            const auto reference = engine::invoke<const std::uint32_t*>(target, 2);
            output.target_handle = std::bit_cast<std::int32_t>(*reference);
        }
    }
    frame.origin_dc = original_origin;
    bindings.state.flags &= ~1U;
}
} // namespace skeet::rage
