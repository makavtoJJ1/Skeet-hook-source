#include "skeet/client/command_eye_adjustment.hpp"
#include "skeet/engine/studio_hitbox_set.hpp"
#include "skeet/rage/firing.hpp"
#include "skeet/animation/bone_runtime.hpp"
#include "skeet/engine/build_eye_adjustment_bindings.hpp"
#include "skeet/network/packet_control.hpp"
#include "skeet/movement/steering_math.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>
#include <type_traits>

namespace skeet::client {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
float negate(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) ^ 0x80000000U);
}
bool recent_selected_command(std::int32_t current, std::int32_t previous) noexcept {
    const auto difference = std::uint32_t(current) - std::uint32_t(previous);
    const auto sign = (difference & 0x80000000U) ? UINT32_MAX : 0U;
    return std::bit_cast<std::int32_t>((difference ^ sign) - sign) < 10;
}
template <class Context>
float selected_point_pitch(const CommandFrame& frame, const Context& context) {
    const auto& selected = *context.selected_point;
    const float x = selected[0] - frame.origin_dc[0];
    const float y = selected[1] - frame.origin_dc[1];
    const float z = selected[2] - frame.origin_dc[2];
    const float x_square = x * x, y_square = y * y, z_square = z * z;
    const float inverse =
        1.0f / (std::sqrt((x_square + y_square) + z_square) + 0.00000011920928955078125f);
    const float normalized_x = inverse * x, normalized_y = inverse * y, normalized_z = inverse * z;
    float pitch;
    if (normalized_y == 0.0f && normalized_x == 0.0f)
        pitch = normalized_z > 0.0f ? -90.0f : 90.0f;
    else {
        const float horizontal =
            std::sqrt(normalized_x * normalized_x + normalized_y * normalized_y);
        pitch = static_cast<float>(context.atan2(static_cast<double>(negate(normalized_z)),
                                                 static_cast<double>(horizontal))) *
                57.2957763671875f;
    }
    return pitch - frame.recoil_angles_9c[0];
}
bool build_eye_bones(void* player, const CommandEyeAdjustmentContext& context) {
    // Complete 433F8EEA: the renderable's slot 2 supplies angles in EDX;
    // decoded A9EE6062 supplies the origin in XMM1/XMM2/XMM3.
    const auto angles = *engine::invoke<const RageVector*>(static_cast<std::byte*>(player) + 4, 2);
    const auto origin = read<RageVector>(player, context.offsets.bone_origin);
    auto* matrices = read<Matrix3x4*>(player, context.offsets.common.bone_cache);
    const animation::BoneRequest request{angles, origin, 0.0f, 0x300U, -1, 0U, true};
    return animation::build_bone_matrices(
        context.bone_state, context.bone_model, request,
        std::span<Matrix3x4>(matrices, context.bone_model.parents.size()), context.bone_engine,
        context.trigonometry);
}
bool build_eye_bones(void* player, const CommandEyeAdjustmentSourceContext& context) {
    const auto angles = *engine::invoke<const RageVector*>(static_cast<std::byte*>(player) + 4, 2);
    const auto origin = read<RageVector>(player, context.offsets.bone_origin);
    auto* matrices = read<Matrix3x4*>(player, context.offsets.common.bone_cache);
    const animation::BoneRequest request{angles, origin, 0.0f, 0x300U, -1, 0U, true};
    return context.bones.build_bone_matrices(player, request, matrices);
}
template <class Context>
bool correct_head_height(CommandFrame& frame, void* player, const Context& context) {
    const auto& offsets = context.offsets;
    const auto* wrapper = read<const void*>(player, offsets.studio_header);
    if (!wrapper)
        return false;
    const auto* header = read<const std::byte*>(wrapper, 0);
    if (!header)
        return false;
    const auto set_index = read<std::int32_t>(player, offsets.hitbox_set);
    const auto* set = engine::studio_hitbox_set(header, set_index);
    if (!set || read<std::int32_t>(set, 4) <= 0)
        return false;
    const auto* boxes = set + read<std::int32_t>(set, 8);
    if (!boxes)
        return false;
    const auto bone = read<std::int32_t>(boxes, 0);
    if (bone < 0)
        return false;
    const auto& common = offsets.common;
    if (bone < read<std::int32_t>(player, common.bone_cache + 0xC)) {
        const auto* matrices = read<const Matrix3x4*>(player, common.bone_cache);
        if (!matrices)
            return false;
        frame.origin_dc[2] = rage::correct_firing_eye_height(matrices[bone], frame.origin_dc[2]);
    }
    return true;
}
}

template <class Context>
void adjust_eye(CommandFrame& frame, void* player, const Context& context) {
    if (!*context.enabled && !*context.alternate_enabled)
        return;
    const auto& offsets = context.offsets;
    const auto& common = offsets.common;
    const auto saved_pose = read<std::uint32_t>(player, common.pose_parameters + 0x30);
    const auto origin = *engine::invoke<const std::array<float, 3>*>(player, 10);
    const auto view = read<std::array<float, 3>>(player, offsets.view_offset);
    frame.origin_dc = {origin[0] + view[0], origin[1] + view[1], origin[2] + view[2]};

    write(player, common.bone_accessor + 8, std::uint32_t(0));
    write(player, offsets.overridden_property, std::uint32_t(0));
    write(player, common.last_bone_setup_time, std::uint32_t(0xFF7FFFFF));
    write(player, common.field_0471e13c, std::uint32_t(0));
    const auto setup_flags = read<std::uint32_t>(player, common.effects_bb78dc43) & ~2U;
    write(player, common.effects_bb78dc43, setup_flags);
    if constexpr (std::is_same_v<Context, CommandEyeAdjustmentContext>) {
        // The standalone model mirrors these native writes. The production
        // BoneRuntime reads the same fields directly from the actual player.
        context.bone_state.readable_bones = 0;
        context.bone_state.setup_flags = setup_flags;
    }

    const auto* animation = read<const void*>(player, common.animation_state);
    const bool animation_ready =
        animation && rage::firing_animation_ready(
                         animation, read<std::uint8_t>(player, offsets.animation_guard));
    frame.flags &= ~0x400000U;
    if (frame.weapon_address && !(frame.flags & 0x40U) && *context.enabled &&
        *context.pitch_enabled && !(frame.flags & 0x200U)) {
        const auto* weapon = reinterpret_cast<const void*>(std::uintptr_t(frame.weapon_address));
        if (network::classified_packet_weapon(
                read<std::uint16_t>(weapon, offsets.weapon_identifier),
                context.weapon_classification)) {
            float pitch;
            if ((frame.flags & 8U) && animation_ready) {
                pitch = -90.0f;
                frame.flags |= 0x400000U;
            } else if (recent_selected_command(frame.command_number_ac, *context.selected_command))
                pitch = selected_point_pitch(frame, context);
            else
                pitch = std::bit_cast<float>(frame.field_b4);
            pitch = normalize_movement_angle(pitch) +
                    read<float>(player, offsets.player_pitch_adjustment);
            if (pitch > 180.0f)
                pitch -= 360.0f;
            pitch = _mm_cvtss_f32(
                _mm_min_ss(_mm_max_ss(_mm_set_ss(pitch), _mm_set_ss(-90.0f)), _mm_set_ss(90.0f)));
            const auto* wrapper = read<const void*>(player, offsets.studio_header);
            if (wrapper && read<const void*>(wrapper, 0)) {
                float normalized = pitch;
                if constexpr (std::is_same_v<Context, CommandEyeAdjustmentContext>)
                    context.convert_pose_parameter(wrapper, 12, pitch, normalized);
                else
                    context.pose.convert(wrapper, 12, pitch, normalized);
                write(player, common.pose_parameters + 0x30, normalized);
            }
        }
    }

    const bool built = build_eye_bones(player, context);
    bool update_height = true;
    if (built) {
        frame.flags |= 0x20U;
        write(player, common.bone_accessor + 8, std::uint32_t(0xFFF00));
        write(player, common.bone_accessor + 0xC, std::uint32_t(0xFFF00));
        if constexpr (std::is_same_v<Context, CommandEyeAdjustmentContext>)
            context.bone_state.readable_bones = context.bone_state.writable_bones = 0xFFF00U;
        if (context.replacement_property)
            write(player, offsets.overridden_property, *context.replacement_property);
        if (animation_ready)
            update_height = correct_head_height(frame, player, context);
    }
    // Native invalid studio/hitbox paths skip this frame write entirely.
    if (update_height)
        frame.view_height_84 = frame.origin_dc[2] - frame.origin_10c[2];
    write(player, common.pose_parameters + 0x30, saved_pose);
}
void adjust_command_eye(CommandFrame& frame, void* player, Command&,
                        const CommandEyeAdjustmentContext& context) {
    adjust_eye(frame, player, context);
}
void adjust_command_eye(CommandFrame& frame, void* player, Command&,
                        const CommandEyeAdjustmentSourceContext& context) {
    adjust_eye(frame, player, context);
}
} // namespace skeet::client
