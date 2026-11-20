#include "skeet/lagcomp/entity_state.hpp"
#include "skeet/engine/studio_hitbox_set.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include "skeet/lagcomp/receive_timing.hpp"
#include "skeet/client/player_state.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>
#include <xmmintrin.h>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::uint32_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
template <class T> void write(void* object, std::uint32_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
}
void capture_player_animation(AnimationState& destination, void* player,
                              const EntityNetvars& offsets) {
    const auto* descriptor = static_cast<std::byte*>(player) + offsets.animation_layers;
    capture_animation(destination, read<void*>(descriptor, 0), read<std::int32_t>(descriptor, 0xC),
                      static_cast<float*>(static_cast<void*>(static_cast<std::byte*>(player) +
                                                             offsets.pose_parameters)));
}
void restore_player_animation(const AnimationState& source, void* player,
                              const EntityNetvars& offsets) {
    auto* descriptor = static_cast<std::byte*>(player) + offsets.animation_layers;
    restore_animation(source, read<void*>(descriptor, 0), read<std::int32_t>(descriptor, 0xC),
                      static_cast<float*>(static_cast<void*>(static_cast<std::byte*>(player) +
                                                             offsets.pose_parameters)));
}
void set_transform(void* player, const EntityNetvars& offsets, SetAbsoluteVector function,
                   const Vector& value) {
    if (function) {
        const auto guard_offset = offsets.transform_guard_49d93e62 - 0x14u;
        const auto guard = read<std::uint32_t>(player, guard_offset);
        write(player, guard_offset, UINT32_MAX);
        function(player, value);
        write(player, guard_offset, guard);
    }
}
void scatter_live_hitbox_matrices(const HitboxMatrices& matrices, void* player,
                                  const EntityHitboxModelOffsets& offsets,
                                  std::span<Matrix3x4> bones) noexcept {
    const auto wrapper = read<const void*>(player, std::uint32_t(offsets.studio_header));
    if (!wrapper)
        return;
    const auto header = read<const void*>(wrapper, 0);
    if (!header)
        return;
    const auto index = read<std::int32_t>(player, std::uint32_t(offsets.hitbox_set));
    const auto set = engine::studio_hitbox_set(static_cast<const std::byte*>(header), index);
    if (!set)
        return;
    const auto native_count = read<std::int32_t>(set, 4);
    const auto count = native_count < 19 ? native_count : 19;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto descriptor = set + read<std::int32_t>(set, 8) + std::ptrdiff_t(i) * 0x44;
        const auto bone = read<std::uint32_t>(descriptor, 0);
        if (bone < bones.size())
            bones[bone] = matrices[std::size_t(i)];
    }
}
}
float animation_speed_factor(const void* state) noexcept {
    const auto clamp = [](float value) {
        return _mm_cvtss_f32(
            _mm_min_ss(_mm_max_ss(_mm_set_ss(value), _mm_setzero_ps()), _mm_set_ss(1.0f)));
    };
    //4333DB01 may supply its own unwritten null-animation local state. Its
    // native scalar observations remain actual opaque stack-word loads.
    const auto scalar = [&](std::size_t offset) {
        return engine::read_opaque_native_float(static_cast<const std::byte*>(state) + offset);
    };
    const float running = scalar(0x11C) * -0.3f - 0.2f;
    float factor = running * clamp(scalar(0xF8)) + 1.0f;
    const float duck = scalar(0xA4);
    if (duck > 0.0f) {
        const float speed = duck * clamp(scalar(0xFC));
        factor = speed * (0.5f - factor) + factor;
    }
    return factor;
}
void scatter_record_hitbox_matrices(const HitboxMatrices& matrices,
                                    std::span<const std::uint32_t> hitbox_bones,
                                    std::span<Matrix3x4> bones) noexcept {
    for (std::size_t i = 0; i < hitbox_bones.size() && i < 19; ++i)
        if (hitbox_bones[i] < bones.size())
            bones[hitbox_bones[i]] = matrices[i];
}
namespace {
struct CaptureHistoryView {
    const void* field_1ac;
    const void* field_180;
    const std::uint8_t* index;
    const std::uint8_t* state;
    const std::byte* modes;
    std::uint32_t mode_stride;
};
void capture_record_fields(const EntityStateRuntime& runtime, LagRecord& record, void* player,
                           const CaptureHistoryView& history) {
    const auto& offsets = runtime.offsets;
    const auto& interval = runtime.interval;
    auto& out = record.header;
    out.flags = 0;
    out.eye_pitch = read<float>(player, offsets.eye_angles);
    out.player_history_1ac = read<std::uint32_t>(history.field_1ac, 0);
    out.player_history_180 = read<std::uint32_t>(history.field_180, 0);
    out.simulation_tick = time_to_tick(read<float>(player, offsets.simulation_time), interval);
    if (out.simulation_tick ==
        time_to_tick(read<float>(player, offsets.old_simulation_time), interval))
        out.flags = 0x100;
    out.field_a2a8247f = read<float>(player, offsets.field_a2a8247f);
    out.origin = read<Vector>(player, offsets.origin);
    out.collision_max_z = engine::invoke<const Vector*>(player, 77)->at(2);
    out.player_flags = read<std::uint32_t>(player, offsets.player_flags);
    out.velocity = read<Vector>(player, offsets.velocity);
    out.field_fb7e3404 = read<std::uint32_t>(player, offsets.field_fb7e3404);
    out.duck_amount = read<float>(player, offsets.duck_amount);
    const auto handle = read<std::uint32_t>(player, offsets.weapon_handle);
    void* weapon =
        handle == UINT32_MAX ? nullptr : engine::invoke<void*>(runtime.entity_list, 4, handle);
    out.weapon_field_6777c9ce = weapon ? read<float>(weapon, offsets.weapon_field_6777c9ce) : 0.0f;
    out.receive_tick = sample_receive_tick(runtime.engine, runtime.receive_interval);
    out.command_tick = corrected_command_tick(runtime.command_tick, runtime.previous_command,
                                              runtime.current_command);
    const auto* animation = read<void*>(player, offsets.animation_state);
    out.animation_yaw =
        animation ? read<float>(animation, 0x80) : engine::invoke<const Vector*>(player, 11)->at(1);
    out.resolver_index = *history.index;
    out.resolver_mode =
        read<std::uint8_t>(history.modes, std::uint32_t(*history.index) * history.mode_stride);
    out.resolver_state = *history.state;
    out.animation_speed_factor = animation ? animation_speed_factor(animation) : 1.0f;
    capture_player_animation(record.animation, player, offsets);
}
}
void EntityStateRuntime::capture_record(LagRecord& record, void* player,
                                        const PlayerCaptureState& history) const {
    // Separate semantic entry: its caller supplies the proven history view.
    // As in the original, a valid player has a nonzero networkable index.
    (void)engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    capture_record_fields(*this, record, player,
                          {&history.field_1ac, &history.field_180, &history.resolver_index,
                           &history.resolver_state,
                           reinterpret_cast<const std::byte*>(history.resolver_modes.data()), 1});
}
void EntityStateRuntime::capture_record(LagRecord& record, void* player,
                                        client::PlayerStateCache& players) const {
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    auto& owner = *players.get(index);
    capture_record_fields(*this, record, player,
                          {&owner.native.reference_yaw, &owner.native.capture_field_180,
                           &owner.native.resolver_index, &owner.native.resolver_status,
                           reinterpret_cast<const std::byte*>(owner.native.resolver_slots.data()),
                           0x24});
}
void EntityStateRuntime::capture_snapshot(EntitySnapshot& snapshot, void* player) const {
    auto& out = snapshot.state;
    out.capture_frame = frame;
    out.handle = *engine::invoke<const std::uint32_t*>(player, 2);
    out.player_flags = read<std::uint32_t>(player, offsets.player_flags);
    out.simulation_time = read<float>(player, offsets.simulation_time);
    out.absolute_yaw = engine::invoke<const Vector*>(player, 11)->at(1);
    out.collision_max_z = engine::invoke<const Vector*>(player, 77)->at(2);
    out.velocity = read<Vector>(player, offsets.velocity);
    out.origin = read<Vector>(player, offsets.origin);
    out.absolute_origin = *engine::invoke<const Vector*>(player, 10);
    capture_player_animation(out.animation, player, offsets);
    auto* cache = static_cast<std::byte*>(player) + offsets.bone_cache;
    out.readable_bones = read<std::uint32_t>(player, offsets.bone_accessor + 8);
    out.writable_bones = read<std::uint32_t>(player, offsets.bone_accessor + 0xC);
    out.recent_bone_counter = read<std::uint32_t>(player, offsets.recent_bone_counter);
    out.bone_count = read<std::int32_t>(cache, 0xC);
    if (out.bone_count > std::int32_t(snapshot.bones.size()))
        throw std::length_error("Snapshot bone storage is smaller than the live cache");
    const auto* bones = read<const Matrix3x4*>(cache, 0);
    for (std::int32_t i = 0; i < out.bone_count; ++i)
        snapshot.bones[std::size_t(i)] = bones[i];
}
void EntityStateRuntime::restore_snapshot(const EntitySnapshot& snapshot) const {
    const auto& saved = snapshot.state;
    void* player =
        saved.handle == UINT32_MAX ? nullptr : engine::invoke<void*>(entity_list, 4, saved.handle);
    if (!player)
        throw std::logic_error("Snapshot recorded entity must remain valid");
    std::uint8_t prior_override = 0;
    if (transform_override) {
        prior_override = *transform_override;
        *transform_override = 1;
    }
    write(player, offsets.player_flags, saved.player_flags);
    write(player, offsets.simulation_time, saved.simulation_time);
    set_transform(player, offsets, set_absolute_angles, Vector{0.0f, saved.absolute_yaw, 0.0f});
    auto* collideable = engine::invoke<void*>(player, 3);
    Vector maximum = *engine::invoke<const Vector*>(collideable, 2);
    if (maximum[2] != saved.collision_max_z) {
        const auto first = read<std::uint32_t>(player, offsets.field_432b1f5b);
        const auto second = read<std::uint32_t>(player, offsets.field_0d560fac);
        maximum[2] = saved.collision_max_z;
        const auto* minimum = engine::invoke<const Vector*>(player, 76);
        if (set_collision_bounds) {
            auto* current = engine::invoke<void*>(player, 3);
            set_collision_bounds(current, *minimum, maximum);
        }
        collideable = engine::invoke<void*>(player, 3);
        auto* current_maximum = const_cast<Vector*>(engine::invoke<const Vector*>(collideable, 2));
        (*current_maximum)[2] = saved.collision_max_z;
        write(player, offsets.field_0d560fac, second);
        write(player, offsets.field_432b1f5b, first);
    }
    write(player, offsets.velocity, saved.velocity);
    set_transform(player, offsets, set_absolute_origin, saved.absolute_origin);
    write(player, offsets.origin, saved.origin);
    restore_player_animation(saved.animation, player, offsets);
    auto* cache = static_cast<std::byte*>(player) + offsets.bone_cache;
    if (!saved.bone_count || read<std::int32_t>(cache, 0xC) < saved.bone_count) {
        write(player, offsets.bone_accessor + 8, std::uint32_t(0));
        write(player, offsets.recent_bone_counter, std::uint32_t(0));
        write(player, offsets.last_bone_setup_time, std::uint32_t(0xFF7FFFFF));
        write(player, offsets.field_0471e13c, std::uint32_t(0));
        write(player, offsets.effects_bb78dc43,
              read<std::uint32_t>(player, offsets.effects_bb78dc43) & 0xFFFFFFFDu);
    } else {
        write(player, offsets.bone_accessor + 8, saved.readable_bones);
        write(player, offsets.bone_accessor + 0xC, saved.writable_bones);
        write(player, offsets.recent_bone_counter, saved.recent_bone_counter);
        auto* bones = read<Matrix3x4*>(cache, 0);
        for (std::int32_t i = 0; i < saved.bone_count; ++i)
            bones[i] = snapshot.bones[std::size_t(i)];
    }
    if (transform_override)
        *transform_override = prior_override;
}
void EntityStateRuntime::apply_record(void* player, const LagRecord& record,
                                      std::span<const std::uint32_t> hitbox_bones,
                                      const std::uint32_t* bone_counter) const {
    apply_record_inputs(player, record, hitbox_bones, bone_counter, nullptr);
}
void EntityStateRuntime::apply_record_live_model(void* player, const LagRecord& record,
                                                 const EntityHitboxModelOffsets& model,
                                                 const std::uint32_t* bone_counter) const {
    apply_record_inputs(player, record, {}, bone_counter, &model);
}
void EntityStateRuntime::apply_record_inputs(void* player, const LagRecord& record,
                                             std::span<const std::uint32_t> hitbox_bones,
                                             const std::uint32_t* bone_counter,
                                             const EntityHitboxModelOffsets* model) const {
    const auto& saved = record.header;
    std::uint8_t prior_override = 0;
    if (transform_override) {
        prior_override = *transform_override;
        *transform_override = 1;
    }
    write(player, offsets.player_flags, saved.player_flags);
    write(player, offsets.simulation_time, float(saved.simulation_tick) * interval);
    set_transform(player, offsets, set_absolute_angles, Vector{0.0f, saved.animation_yaw, 0.0f});
    auto* collideable = engine::invoke<void*>(player, 3);
    auto* maximum = const_cast<Vector*>(engine::invoke<const Vector*>(collideable, 2));
    if ((*maximum)[2] != saved.collision_max_z) {
        collideable = engine::invoke<void*>(player, 3);
        maximum = const_cast<Vector*>(engine::invoke<const Vector*>(collideable, 2));
        (*maximum)[2] = saved.collision_max_z;
    }
    write(player, offsets.velocity, saved.velocity);
    set_transform(player, offsets, set_absolute_origin, saved.origin);
    write(player, offsets.origin, saved.origin);
    restore_player_animation(record.animation, player, offsets);
    auto* cache = static_cast<std::byte*>(player) + offsets.bone_cache;
    const auto bone_count = read<std::int32_t>(cache, 0xC);
    auto* cache_data = read<Matrix3x4*>(cache, 0);
    if (!(saved.flags & 4u) || !cache_data || bone_count <= 0) {
        write(player, offsets.bone_accessor + 8, std::uint32_t(0));
        write(player, offsets.recent_bone_counter, std::uint32_t(0));
        write(player, offsets.last_bone_setup_time, std::uint32_t(0xFF7FFFFF));
        write(player, offsets.field_0471e13c, std::uint32_t(0));
        write(player, offsets.effects_bb78dc43,
              read<std::uint32_t>(player, offsets.effects_bb78dc43) & 0xFFFFFFFDu);
    } else {
        std::span<Matrix3x4> bones{cache_data, static_cast<std::size_t>(bone_count)};
        if (model)
            scatter_live_hitbox_matrices(record.matrices, player, *model, bones);
        else
            scatter_record_hitbox_matrices(record.matrices, hitbox_bones, bones);
        write(player, offsets.bone_accessor + 8, std::uint32_t(0xFFF00));
        write(player, offsets.bone_accessor + 0xC, std::uint32_t(0xFFF00));
        if (bone_counter)
            write(player, offsets.recent_bone_counter, *bone_counter);
    }
    if (transform_override)
        *transform_override = prior_override;
}
EntityRecordScope::EntityRecordScope(const EntityStateRuntime& owner, void* entity,
                                     std::span<Matrix3x4> saved_bones) noexcept
    : player(entity), flags(0), snapshot{EntitySnapshotHeader{}, saved_bones}, runtime(owner) {
    record.header.field_00 = 0;
    snapshot.state.capture_frame = -1;
    snapshot.state.handle = UINT32_MAX;
}
void EntityRecordScope::apply(const LagRecord& value, std::span<const std::uint32_t> hitbox_bones,
                              const std::uint32_t* bone_counter) {
    if (snapshot.state.capture_frame != runtime.frame)
        runtime.capture_snapshot(snapshot, player);
    std::memcpy(&record, &value, sizeof(record));
    runtime.apply_record(player, record, hitbox_bones, bone_counter);
}
void EntityRecordScope::apply_live_model(const LagRecord& value,
                                         const EntityHitboxModelOffsets& model,
                                         const std::uint32_t* bone_counter) {
    if (snapshot.state.capture_frame != runtime.frame)
        runtime.capture_snapshot(snapshot, player);
    std::memcpy(&record, &value, sizeof record);
    runtime.apply_record_live_model(player, record, model, bone_counter);
}
void EntityRecordScope::restore() {
    if (snapshot.state.capture_frame == runtime.frame) {
        runtime.restore_snapshot(snapshot);
        snapshot.state.capture_frame = -1;
    }
}
} // namespace skeet::lagcomp
