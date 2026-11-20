#include "skeet/client/hitboxes.hpp"
#include "skeet/engine/studio_hitbox_set.hpp"
#include "skeet/lagcomp/entity_state.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace skeet::client {
namespace {
template <class T> T read(const void* object, std::uint32_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::uint32_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
struct Model {
    std::span<const std::byte> descriptors;
    std::array<std::uint32_t, 19> bones;
    std::int32_t count;
    animation::BoneModel bone_model() const noexcept {
        return {true,  0,  true, false,
                false, {}, {},   {bones.data(), static_cast<std::size_t>(count)}};
    }
};
bool model_for(const void* entity, const ClientHitboxOffsets& offsets, Model& model) noexcept {
    const auto wrapper = read<const void*>(entity, offsets.studio_wrapper);
    if (!wrapper)
        return false;
    const auto studio = read<const std::byte*>(wrapper, 0);
    if (!studio)
        return false;
    const auto* set = engine::studio_hitbox_set(studio, read<std::int32_t>(entity, offsets.hitbox_set));
    if (!set)
        return false;
    model.count = std::min(read<std::int32_t>(set, 4), static_cast<int>(model.bones.size()));
    if (model.count <= 0)
        return false;
    const auto* descriptors = set + read<std::int32_t>(set, 8);
    model.descriptors = {descriptors, static_cast<std::size_t>(model.count) * 0x44};
    for (std::int32_t i = 0; i < model.count; ++i) {
        model.bones[i] = read<std::uint32_t>(descriptors, i * 0x44);
        if (model.bones[i] >= animation::maximum_bones)
            return false;
    }
    return true;
}
RageLocalHitbox local_hitbox(const std::byte* descriptor) noexcept {
    return {read<std::int32_t>(descriptor, 4),  read<RageVector>(descriptor, 8),
            read<RageVector>(descriptor, 0x14), read<RageVector>(descriptor, 0x24),
            read<float>(descriptor, 0x30),      descriptor};
}
// SSE keeps the destination operand's NaN payload. C++ permits commuting an
// add/multiply even without fast-math, so retain that observable native order.
bool nan(float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x7FFFFFFFu) > 0x7F800000u;
}
float quiet(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) | 0x00400000u);
}
float add(float a, float b) noexcept {
    if (nan(a))
        return quiet(a);
    if (nan(b))
        return quiet(b);
    return a + b;
}
float subtract(float a, float b) noexcept {
    if (nan(a))
        return quiet(a);
    if (nan(b))
        return quiet(b);
    return a - b;
}
float multiply(float a, float b) noexcept {
    if (nan(a))
        return quiet(a);
    if (nan(b))
        return quiet(b);
    return a * b;
}
float negate(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) ^ 0x80000000u);
}
Matrix3x4 overlay_matrix(const Matrix3x4& bone, const RageVector& angles,
                         const RageTrigonometry& trig) {
    constexpr float radians = 0.01745329238474369f;
    const auto yaw = multiply(angles[1], radians), pitch = multiply(angles[0], radians),
               roll = multiply(angles[2], radians);
    const auto sy = trig.sine(yaw), cy = trig.cosine(yaw), sp = trig.sine(pitch),
               cp = trig.cosine(pitch), sr = trig.sine(roll), cr = trig.cosine(roll);
    const auto crcy = multiply(cr, cy), crsy = multiply(cr, sy), srcy = multiply(sr, cy),
               srsy = multiply(sr, sy);
    const Matrix3x4 rotation{
        {{multiply(cp, cy), subtract(multiply(srcy, sp), crsy), add(multiply(crcy, sp), srsy), 0},
         {multiply(cp, sy), add(multiply(srsy, sp), crcy), subtract(multiply(crsy, sp), srcy), 0},
         {negate(sp), multiply(sr, cp), multiply(cr, cp), 0}}};
    Matrix3x4 result;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column)
            result.rows[row][column] =
                add(add(add(multiply(bone.rows[row][2], rotation.rows[2][column]),
                            multiply(bone.rows[row][1], rotation.rows[1][column])),
                        multiply(bone.rows[row][0], rotation.rows[0][column])),
                    column == 3 ? bone.rows[row][3] : 0.0f);
    return result;
}
}
void draw_client_hitbox(const ClientHitboxRuntime& runtime, const RageLocalHitbox& hitbox,
                        const Matrix3x4& bone, float duration, std::uint32_t color) {
    const auto matrix = overlay_matrix(bone, hitbox.rotation, runtime.trigonometry);
    if (hitbox.radius > 0.0f) {
        RageVector minimum, maximum;
        for (unsigned row = 0; row < 3; ++row) {
            const auto& m = matrix.rows[row];
            minimum[row] =
                add(add(add(multiply(m[0], hitbox.minimum[0]), multiply(m[1], hitbox.minimum[1])),
                        multiply(m[2], hitbox.minimum[2])),
                    m[3]);
            maximum[row] =
                add(add(add(multiply(m[0], hitbox.maximum[0]), multiply(m[1], hitbox.maximum[1])),
                        multiply(m[2], hitbox.maximum[2])),
                    m[3]);
        }
        using Capsule =
            engine::MemberFunction<void, const RageVector&, const RageVector&, const float&,
                                   std::int32_t, std::int32_t, std::int32_t, std::int32_t, float,
                                   std::int32_t, std::int32_t>;
        const auto table = *static_cast<void***>(runtime.debug_overlay);
        reinterpret_cast<Capsule>(table[23])(runtime.debug_overlay, minimum, maximum, hitbox.radius,
                                             color & 255, (color >> 8) & 255, (color >> 16) & 255,
                                             color >> 24, duration, 1, 1);
        return;
    }
    constexpr float degrees = 57.2957763671875f;
    const float horizontal = std::sqrt(add(multiply(matrix.rows[0][0], matrix.rows[0][0]),
                                           multiply(matrix.rows[1][0], matrix.rows[1][0])));
    RageVector angles;
    if (horizontal > 0.001) {
        angles[1] = multiply(
            static_cast<float>(runtime.arc_tangent(matrix.rows[1][0], matrix.rows[0][0])), degrees);
        angles[0] =
            multiply(static_cast<float>(runtime.arc_tangent(negate(matrix.rows[2][0]), horizontal)),
                     degrees);
        angles[2] = multiply(
            static_cast<float>(runtime.arc_tangent(matrix.rows[2][1], matrix.rows[2][2])), degrees);
    } else {
        angles[1] = multiply(
            static_cast<float>(runtime.arc_tangent(negate(matrix.rows[0][1]), matrix.rows[1][1])),
            degrees);
        angles[0] =
            multiply(static_cast<float>(runtime.arc_tangent(negate(matrix.rows[2][0]), horizontal)),
                     degrees);
        angles[2] = 0.0f;
    }
    const RageVector origin{matrix.rows[0][3], matrix.rows[1][3], matrix.rows[2][3]};
    const std::uint32_t transparent = 0;
    using Box = engine::MemberFunction<void, const RageVector&, const RageVector&,
                                       const RageVector&, const RageVector&, const std::uint32_t&,
                                       const std::uint32_t&, float>;
    const auto table = *static_cast<void***>(runtime.debug_overlay);
    reinterpret_cast<Box>(table[21])(runtime.debug_overlay, origin, hitbox.minimum, hitbox.maximum,
                                     angles, transparent, color, duration);
}

void draw_client_hitboxes(const ClientHitboxRuntime& runtime, void* entity,
                          const ClientHitboxesRequest& request) {
    auto* cached = runtime.players.get(request.entity_index);
    if (!cached)
        return;
    auto& player = *cached;
    auto& ring = player.history;
    auto tick = request.has_tick
                    ? request.tick
                    : lagcomp::time_to_tick(read<float>(entity, runtime.offsets.simulation_time),
                                            runtime.interval);
    if (ring.count) {
        const auto newest = ring.at(0).header.simulation_tick;
        const auto oldest = ring.at(ring.count - 1).header.simulation_tick;
        tick = newest < oldest ? newest : std::clamp(tick, oldest, newest);
    }
    Model model{};
    if (!model_for(entity, runtime.offsets, model))
        return;
    if (tick == -1)
        tick = runtime.current_tick;
    const lagcomp::LagRecord* record = nullptr;
    for (std::uint32_t i = 0; i < ring.count; ++i) {
        const auto* candidate = &ring.at(i);
        if (candidate->header.simulation_tick <= tick && (candidate->header.flags & 4)) {
            record = candidate;
            break;
        }
    }
    HitboxMatrices matrices;
    if (!record) {
        const auto* descriptor = static_cast<const std::byte*>(entity) + runtime.offsets.bone_cache;
        const auto count = read<std::int32_t>(descriptor, 0xC);
        const auto* bones = read<const Matrix3x4*>(descriptor, 0);
        if (!bones || count <= 0)
            return;
        if (animation::select_hitbox_bone_matrices(model.bone_model(),
                {bones, static_cast<std::size_t>(count)}, matrices) != model.count)
            return;
    } else {
        matrices = record->matrices;
        const auto& offsets = runtime.offsets;
        auto* collideable = engine::invoke<void*>(entity, 3);
        auto* maximum = collideable ? engine::invoke<RageVector*>(collideable, 2) : nullptr;
        if (!maximum)
            return;
        const auto yaw = read<float>(entity, offsets.absolute_angles + 4);
        write(entity, offsets.absolute_angles + 4,
              request.flags & 1 ? request.yaw : player.native.reference_yaw);
        const auto max_z = (*maximum)[2];
        (*maximum)[2] = record->header.collision_max_z;
        const auto origin = read<RageVector>(entity, offsets.origin),
                   absolute_origin = read<RageVector>(entity, offsets.absolute_origin);
        write(entity, offsets.origin, record->header.origin);
        write(entity, offsets.absolute_origin, record->header.origin);
        const auto flags = read<std::uint32_t>(entity, offsets.entity_flags);
        write(entity, offsets.entity_flags, flags & 0xFFFF07FFu);
        const auto field_432 = read<float>(entity, offsets.field_432b1f5b),
                   field_0d = read<float>(entity, offsets.field_0d560fac);
        auto height = read<float>(&player.native, 0x12C),
             stamp = read<float>(&player.native, 0x148);
        const auto& newest = ring.at(0).header;
        if (tick < newest.simulation_tick &&
            record->header.collision_max_z != newest.collision_max_z) {
            height = add(newest.origin[2], newest.collision_max_z);
            stamp = runtime.current_time;
        }
        write(entity, offsets.field_432b1f5b, height);
        write(entity, offsets.field_0d560fac, stamp);
        std::array<Matrix3x4, animation::maximum_bones> bones;
        lagcomp::scatter_record_hitbox_matrices(matrices, model.bone_model().hitbox_bones, bones);
        if (runtime.update_attachments) {
            const auto readable = read<std::uint32_t>(entity, offsets.bone_accessor + 8);
            const auto writable = read<std::uint32_t>(entity, offsets.bone_accessor + 12);
            write(entity, offsets.bone_accessor + 8, 0xFFF00u);
            write(entity, offsets.bone_accessor + 12, 0xFFF00u);
            runtime.update_attachments(entity, bones.data(), 0x300);
            write(entity, offsets.bone_accessor + 12, writable);
            write(entity, offsets.bone_accessor + 8, readable);
        }
        animation::select_hitbox_bone_matrices(model.bone_model(), bones, matrices);
        write(entity, offsets.field_432b1f5b, field_432);
        write(entity, offsets.field_0d560fac, field_0d);
        write(entity, offsets.entity_flags, flags);
        write(entity, offsets.origin, origin);
        write(entity, offsets.absolute_origin, absolute_origin);
        (*maximum)[2] = max_z;
        write(entity, offsets.absolute_angles + 4, yaw);
    }
    for (std::int32_t i = 0; i < model.count; ++i)
        if (request.hitbox_mask & (1u << (i & 31)))
            draw_client_hitbox(runtime, local_hitbox(model.descriptors.data() + i * 0x44),
                               matrices[i], request.duration, request.color);
}
} // namespace skeet::client
