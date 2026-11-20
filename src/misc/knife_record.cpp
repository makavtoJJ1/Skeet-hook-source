#include "skeet/misc/knife_record.hpp"
#include "skeet/lagcomp/packet_capture.hpp"
#include "skeet/lagcomp/live_record_attachments.hpp"
#include "skeet/lagcomp/live_timeline.hpp"
#include "skeet/movement/extrapolation.hpp"
#include "skeet/animation/bone_runtime.hpp"
#include <bit>
#include <cstring>

namespace skeet::misc {
namespace {
template <class T> T field(const void* object, std::uint32_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void store(void* object, std::uint32_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
std::int32_t decrement(std::int32_t value) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(value) - 1U);
}
void set_transform(void* player, const lagcomp::EntityStateRuntime& entity,
                   lagcomp::SetAbsoluteVector function, const lagcomp::Vector& value) {
    if (!function)
        return;
    const auto offset = entity.offsets.transform_guard_49d93e62 - 0x14U;
    const auto saved = field<std::uint32_t>(player, offset);
    store(player, offset, UINT32_MAX);
    function(player, value);
    store(player, offset, saved);
}
void publish_bone_cache(lagcomp::EntityRecordScope& scope,
                        const lagcomp::ExtrapolatedRecordBindings& bindings,
                        const std::uint32_t* current_bone_counter,
                        const animation::BoneRuntime& bones) {
    const auto& entity = bindings.preparation.entity;
    const auto& offsets = entity.offsets;
    const auto cache = field<Matrix3x4*>(scope.player, offsets.bone_cache);
    const auto count = field<int>(scope.player, offsets.bone_cache + 0xc);
    if (cache && count > 0)
        bones.scatter_hitbox_bone_matrices(scope.player, scope.record.matrices,
                                           {cache, static_cast<std::size_t>(count)});
    store(scope.player, offsets.bone_accessor + 8, 0xFFF00U);
    store(scope.player, offsets.bone_accessor + 12, 0xFFF00U);
    if (current_bone_counter)
        store(scope.player, offsets.recent_bone_counter, *current_bone_counter);
}
void invalidate_bone_cache(void* player, const lagcomp::EntityNetvars& offsets) noexcept {
    store(player, offsets.bone_accessor + 8, 0U);
    store(player, offsets.recent_bone_counter, 0U);
    store(player, offsets.last_bone_setup_time, 0xFF7FFFFFU);
    store(player, offsets.field_0471e13c, 0U);
    store(player, offsets.effects_bb78dc43,
          field<std::uint32_t>(player, offsets.effects_bb78dc43) & ~2U);
}
}

void apply_extrapolated_knife_record(lagcomp::EntityRecordScope& scope, client::CommandFrame& frame,
                                     const lagcomp::ExtrapolatedRecordBindings& bindings,
                                     const std::uint32_t* current_bone_counter,
                                     const animation::BoneRuntime& bones,
                                     const MovementExtrapolationLiveBindings& live_movement,
                                     const client::CommandBudgetBindings& budget) {
    const auto& entity = bindings.preparation.entity;
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(scope.player) + 8, 10);
    auto& owner = *bindings.players.get(index);
    if (scope.snapshot.state.capture_frame != entity.frame)
        entity.capture_snapshot(scope.snapshot, scope.player);
    const auto timeline = lagcomp::history_timeline(owner);
    bool capture = timeline.record_count == 0;
    if (!capture) {
        const auto tick = lagcomp::time_to_tick(timeline.simulation_time, entity.interval);
        capture = tick < decrement(timeline.last_update_tick) &&
                  tick < timeline.at(0).header.simulation_tick;
    }
    if (capture) {
        entity.capture_record(scope.record, scope.player, bindings.players);
        scope.flags |= 0x20U;
    } else
        std::memcpy(&scope.record, &timeline.at(0), sizeof scope.record);

    lagcomp::HistoryTickTiming current{};
    current.tick_d4 = field<std::int32_t>(&frame, 0xD4);
    current.tick_18c = std::bit_cast<std::int32_t>(frame.field_18c);
    const lagcomp::MissingTickTiming timing{
        current.tick_d4, current.tick_18c, entity.interval, entity.receive_interval, 0,
        entity.engine,   scope.player};
    const auto ticks =
        lagcomp::estimate_missing_ticks(lagcomp::history_timeline(owner), timing, budget);
    std::int32_t changed = 0;
    if (ticks > 0) {
        changed = extrapolate_lag_record(scope.player, owner, scope.record, ticks,
                                         bindings.movement, live_movement);
        if (changed < 0)
            return;
        if (changed > 0)
            scope.flags |= 0x20U;
    }

    const auto applied_flags = scope.flags & 0x60U;
    const auto& offsets = entity.offsets;
    const auto& record = scope.record;
    const auto& saved = record.header;
    std::uint8_t prior_override = 0;
    if (entity.transform_override) {
        prior_override = *entity.transform_override;
        *entity.transform_override = 1;
    }
    store(scope.player, offsets.player_flags, saved.player_flags);
    store(scope.player, offsets.simulation_time, float(saved.simulation_tick) * entity.interval);
    set_transform(scope.player, entity, entity.set_absolute_angles,
                  {0.0f, saved.animation_yaw, 0.0f});
    auto* collideable = engine::invoke<void*>(scope.player, 3);
    auto* maximum =
        const_cast<lagcomp::Vector*>(engine::invoke<const lagcomp::Vector*>(collideable, 2));
    if ((*maximum)[2] != saved.collision_max_z) {
        collideable = engine::invoke<void*>(scope.player, 3);
        maximum =
            const_cast<lagcomp::Vector*>(engine::invoke<const lagcomp::Vector*>(collideable, 2));
        (*maximum)[2] = saved.collision_max_z;
    }
    store(scope.player, offsets.velocity, saved.velocity);
    set_transform(scope.player, entity, entity.set_absolute_origin, saved.origin);
    store(scope.player, offsets.origin, saved.origin);
    auto* layers = static_cast<std::byte*>(scope.player) + offsets.animation_layers;
    auto* poses = static_cast<float*>(
        static_cast<void*>(static_cast<std::byte*>(scope.player) + offsets.pose_parameters));
    lagcomp::restore_animation(record.animation, field<void*>(layers, 0),
                               field<std::int32_t>(layers, 12), poses);
    if (!applied_flags) {
        if ((saved.flags & 4U) && field<std::int32_t>(scope.player, offsets.bone_cache + 12))
            publish_bone_cache(scope, bindings, current_bone_counter, bones);
        else
            invalidate_bone_cache(scope.player, offsets);
    }
    if (entity.transform_override)
        *entity.transform_override = prior_override;

    if (changed > 0) {
        lagcomp::build_extrapolated_record_matrices(scope.player, scope.record, bones,
                                                    bindings.attachments.scratch);
        if (!(scope.flags & 0x40U)) {
            const auto input =
                lagcomp::capture_record_hypothesis_input(scope.player, owner, scope.record, entity);
            resolver_runtime::build_hypotheses(scope.record, input, scope.player, bones);
        }
    }
    auto preparation = bindings.preparation;
    preparation.current_timing = &current;
    lagcomp::refresh_record_attachments(bindings.players, scope.player, scope.record, preparation,
                                        bindings.attachments, bones);
    if (scope.record.header.flags & 4U)
        publish_bone_cache(scope, bindings, current_bone_counter, bones);
}
} // namespace skeet::misc
