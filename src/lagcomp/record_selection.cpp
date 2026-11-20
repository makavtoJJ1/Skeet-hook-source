#include "skeet/lagcomp/record_selection.hpp"
#include "skeet/client/player_state.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
[[gnu::always_inline]] inline float add(float first, float second) noexcept {
    __asm__ __volatile__("addss %1, %0" : "+x"(first) : "x"(second));
    return first;
}
[[gnu::always_inline]] inline float multiply(float first, float second) noexcept {
    __asm__ __volatile__("mulss %1, %0" : "+x"(first) : "x"(second));
    return first;
}
float distance_squared(const Vector& first, const Vector& second) noexcept {
    const float y = first[1] - second[1], x = first[0] - second[0], z = first[2] - second[2];
    return add(add(multiply(y, y), multiply(x, x)), multiply(z, z));
}
Vector angles_between(float older, float newer, float fraction, const RotationMath& math) {
    if (older == newer)
        return {0.0f, older, 0.0f};
    const auto first = history_angle_quaternion({0.0f, older, 0.0f}, math);
    const auto second = history_angle_quaternion({0.0f, newer, 0.0f}, math);
    const auto aligned = align_history_quaternion(first, second);
    return history_quaternion_angles(slerp_history_quaternion(first, aligned, fraction, math),
                                     math);
}
Matrix3x4 angle_matrix(const Vector& angles, const Vector& origin, const RotationMath& math) {
    constexpr float radians = 0.01745329238474369f;
    const float sy = math.sine(angles[1] * radians), cy = math.cosine(angles[1] * radians);
    const float sp = math.sine(angles[0] * radians), cp = math.cosine(angles[0] * radians);
    const float sr = math.sine(angles[2] * radians), cr = math.cosine(angles[2] * radians);
    const float srcy = sr * cy, srsy = sr * sy, crcy = cr * cy, crsy = cr * sy;
    return {{{cp * cy, srcy * sp - crsy, crcy * sp + srsy, origin[0]},
             {cp * sy, srsy * sp + crcy, crsy * sp - srcy, origin[1]},
             {-sp, sr * cp, cr * cp, origin[2]}}};
}
Matrix3x4 concatenate(const Matrix3x4& left, const Matrix3x4& right) {
    Matrix3x4 result;
    for (unsigned row = 0; row != 3; ++row)
        for (unsigned column = 0; column != 4; ++column)
            result.rows[row][column] =
                add(add(add(multiply(left.rows[row][2], right.rows[2][column]),
                            multiply(left.rows[row][1], right.rows[1][column])),
                        multiply(left.rows[row][0], right.rows[0][column])),
                    column == 3 ? left.rows[row][3] : 0.0f);
    return result;
}
Matrix3x4 correction_matrix(float old_yaw, const Vector& old_origin, const Vector& angles,
                            const Vector& origin, const RotationMath& math,
                            RecordInterpolationScratch& scratch, bool alternative) {
    const auto old = angle_matrix({0.0f, old_yaw, 0.0f}, old_origin, math);
    Matrix3x4 inverse;
    for (unsigned row = 0; row != 3; ++row)
        for (unsigned column = 0; column != 3; ++column)
            inverse.rows[row][column] = old.rows[column][row];
    (void)alternative;
    // Translation belongs to the freshly transposed rotation.
    for (unsigned row = 0; row < 3; ++row)
        inverse.rows[row][3] =
            -(inverse.rows[row][0] * old_origin[0] + inverse.rows[row][1] * old_origin[1] +
              inverse.rows[row][2] * old_origin[2]);
    for (unsigned axis = 0; axis != 3; ++axis) {
        scratch.inverse_y[axis] = std::bit_cast<std::uint32_t>(inverse.rows[1][axis]);
        scratch.inverse_z[axis] = std::bit_cast<std::uint32_t>(inverse.rows[2][axis]);
    }
    return concatenate(angle_matrix(angles, origin, math), inverse);
}
void rebase(HitboxMatrices& destination, const HitboxMatrices& source,
            const Matrix3x4& correction) {
    for (unsigned index = 0; index != 19; ++index)
        destination[index] = concatenate(correction, source[index]);
}
}
RecordChoice choose_history_record(const RecordRing& ring, const Vector& cached_origin,
                                   std::int32_t requested_tick, float interval) noexcept {
    RecordChoice choice;
    auto previous = cached_origin;
    const float requested_time = float(requested_tick) * interval;
    for (std::uint32_t ordinal = 0; ordinal != ring.count; ++ordinal) {
        const auto& record = ring.at(ordinal);
        if (distance_squared(record.header.origin, previous) > 4096.0f)
            return {};
        previous = record.header.origin;
        choice = {&record, ordinal};
        if (requested_time >= float(record.header.simulation_tick) * interval)
            break;
    }
    return choice;
}
RecordInterpolation interpolate_selected_record(LagRecord& destination, const RecordRing& ring,
                                                const RecordChoice& choice,
                                                std::int32_t requested_tick,
                                                const HistoryTickWindow& window, float interval,
                                                const RotationMath& math,
                                                RecordInterpolationScratch& scratch) {
    const auto* older = history_interpolation_source(ring, choice.ordinal, *choice.record,
                                                     requested_tick, 0, window);
    if (older == nullptr)
        return {};
    const auto& newer = *choice.record;
    const float requested_time = float(window.last) * interval;
    destination.header.simulation_tick = time_to_tick(requested_time, interval);
    const float old_time = float(older->header.simulation_tick) * interval;
    const float fraction = _mm_cvtss_f32(_mm_min_ss(
        _mm_max_ss(_mm_set_ss((requested_time - old_time) /
                              (float(newer.header.simulation_tick) * interval - old_time)),
                   _mm_setzero_ps()),
        _mm_set_ss(1.0f)));
    const auto angles =
        angles_between(older->header.animation_yaw, newer.header.animation_yaw, fraction, math);
    destination.header.animation_yaw = angles[1];
    destination.header.player_flags = newer.header.player_flags;
    for (unsigned axis = 0; axis != 3; ++axis) {
        destination.header.origin[axis] =
            (newer.header.origin[axis] - older->header.origin[axis]) * fraction +
            older->header.origin[axis];
        destination.header.velocity[axis] =
            (newer.header.velocity[axis] - older->header.velocity[axis]) * fraction +
            older->header.velocity[axis];
    }
    if ((older->header.flags & 4U) != 0) {
        rebase(destination.matrices, older->matrices,
               correction_matrix(older->header.animation_yaw, older->header.origin, angles,
                                 destination.header.origin, math, scratch, false));
        if ((older->header.flags & newer.header.flags & 0x80U) == 0)
            destination.header.flags &= ~0x80U;
        else {
            for (unsigned variant = 0; variant != 3; ++variant) {
                const auto old_yaw = std::bit_cast<float>(older->alternatives[variant].state[0]);
                const auto new_yaw = std::bit_cast<float>(newer.alternatives[variant].state[0]);
                const auto variant_angles = angles_between(old_yaw, new_yaw, fraction, math);
                rebase(destination.alternatives[variant].matrices,
                       older->alternatives[variant].matrices,
                       correction_matrix(old_yaw, older->header.origin, variant_angles,
                                         destination.header.origin, math, scratch, true));
            }
        }
    }
    return {true, fraction, angles};
}
void refresh_record_attachments(client::PlayerStateCache& players, void* target, LagRecord& record,
                                const EntityPreparationContext& preparation,
                                const RecordAttachmentContext& attachments) {
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(target) + 8, 10);
    auto& cached = players.get(std::uint32_t(index))->native;
    const auto& offsets = preparation.entity.offsets;
    const auto old_432 = read<std::uint32_t>(target, offsets.field_432b1f5b);
    const auto old_0d = read<std::uint32_t>(target, offsets.field_0d560fac);
    const auto old_angles = read<std::array<std::uint32_t, 2>>(target, offsets.eye_angles);
    prepare_entity_angles(players, target, record, true, preparation);
    record.header.field_50 = read<float>(target, offsets.eye_angles + 4);
    auto* collision = engine::invoke<void*>(target, 3);
    auto* maximum = engine::invoke<Vector*>(collision, 2);
    const auto old_height = (*maximum)[2];
    (*maximum)[2] = record.header.collision_max_z;
    const auto old_duck = read<std::uint32_t>(target, offsets.duck_amount);
    write(target, offsets.duck_amount, cached.cached_duck_amount);
    const auto old_6e = read<std::uint32_t>(target, attachments.field_6e9852d6);
    const auto old_2a = read<Vector>(target, attachments.field_2a671b42);
    write(target, attachments.field_2a671b42, record.header.origin);
    const auto old_4e = read<Vector>(target, attachments.field_4e66bb34);
    write(target, attachments.field_4e66bb34, record.header.origin);
    const auto old_flags = read<std::uint32_t>(target, attachments.field_83a6cf2b);
    write(target, attachments.field_83a6cf2b, old_flags & 0xFFFFF7FFu);
    if (record.header.flags & 4u)
        legit::refresh_hitbox_attachments(target, attachments.model.decoded(), record.matrices,
                                          attachments.scratch, offsets.bone_accessor,
                                          attachments.update);
    if (record.header.flags & 0x80u)
        for (auto& alternative : record.alternatives)
            legit::refresh_hitbox_attachments(target, attachments.model.decoded(),
                                              alternative.matrices, attachments.scratch,
                                              offsets.bone_accessor, attachments.update);
    write(target, attachments.field_83a6cf2b, old_flags);
    write(target, attachments.field_4e66bb34, old_4e);
    write(target, attachments.field_2a671b42, old_2a);
    write(target, attachments.field_6e9852d6, old_6e);
    write(target, offsets.duck_amount, old_duck);
    (*maximum)[2] = old_height;
    write(target, offsets.eye_angles, old_angles);
    write(target, offsets.field_0d560fac, old_0d);
    write(target, offsets.field_432b1f5b, old_432);
}
bool select_and_apply_history_record(EntityRecordScope& scope,
                                     const RecordSelectionContext& context,
                                     std::int32_t requested_tick, std::uint32_t unused_options) {
    (void)unused_options; // No read of EBP+10 in the original entry.
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(scope.player) + 8, 10);
    auto& cached = *context.players.get(std::uint32_t(index));
    scope.flags = 0;
    const auto choice = choose_history_record(cached.history, cached.native.cached_origin,
                                              requested_tick, context.preparation.entity.interval);
    if (!choice.record)
        return false;
    const auto& runtime = context.preparation.entity;
    if (scope.snapshot.state.capture_frame != runtime.frame)
        runtime.capture_snapshot(scope.snapshot, scope.player);
    std::memcpy(&scope.record, choice.record, sizeof scope.record);
    const auto interpolated = interpolate_selected_record(
        scope.record, cached.history, choice, requested_tick, history_tick_window(context.timing),
        runtime.interval, context.rotation, context.interpolation_scratch);
    if (interpolated.used)
        scope.flags |= 0x10u;
    const auto& model = context.attachments.model.decoded();
    const auto hitboxes =
        model.studio_header ? model.hitbox_bones : std::span<const std::uint32_t>{};
    runtime.apply_record(scope.player, scope.record, hitboxes, context.current_bone_counter);
    // Generic43344C0C always has the nonnull native timing context forwarded
    // through43327659 to4341DDA0.
    auto preparation = context.preparation;
    preparation.current_timing = &context.timing;
    refresh_record_attachments(context.players, scope.player, scope.record, preparation,
                               context.attachments);
    if (scope.record.header.flags & 4u) {
        const auto& offsets = runtime.offsets;
        auto* bones = read<Matrix3x4*>(scope.player, offsets.bone_cache);
        const auto count = read<int>(scope.player, offsets.bone_cache + 0xc);
        if (bones && count > 0)
            scatter_record_hitbox_matrices(scope.record.matrices, hitboxes,
                                           {bones, static_cast<std::size_t>(count)});
        write(scope.player, offsets.bone_accessor + 8, 0xFFF00u);
        write(scope.player, offsets.bone_accessor + 12, 0xFFF00u);
        if (context.current_bone_counter)
            write(scope.player, offsets.recent_bone_counter, *context.current_bone_counter);
    }
    return true;
}
} // namespace skeet::lagcomp
