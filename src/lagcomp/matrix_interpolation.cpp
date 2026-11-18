#include "skeet/lagcomp/matrix_interpolation.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include <bit>
#include <cstring>

namespace skeet::lagcomp {
namespace {
constexpr float radians = 0.01745329238474369f;
float add(float first, float second) noexcept {
    // Commuting SSE operands changes the selected NaN payload. Keep the native
    // destination operand explicit even when the compiler vectorizes this loop.
    __asm__ __volatile__("addss %1, %0" : "+x"(first) : "x"(second));
    return first;
}
float multiply(float first, float second) noexcept {
    __asm__ __volatile__("mulss %1, %0" : "+x"(first) : "x"(second));
    return first;
}
Matrix3x4 rotation_matrix(const Vector& angles, const Vector& origin, const RotationMath& math) {
    const float sy = math.sine(angles[1] * radians), cy = math.cosine(angles[1] * radians);
    const float sp = math.sine(angles[0] * radians), cp = math.cosine(angles[0] * radians);
    const float sr = math.sine(angles[2] * radians), cr = math.cosine(angles[2] * radians);
    const float crcy = cr * cy, crsy = cr * sy, srcy = sr * cy, srsy = sr * sy;
    return {{{cp * cy, srcy * sp - crsy, crcy * sp + srsy, origin[0]},
             {cp * sy, srsy * sp + crcy, crsy * sp - srcy, origin[1]},
             {-sp, sr * cp, cr * cp, origin[2]}}};
}
Matrix3x4 concatenate(const Matrix3x4& parent, const Matrix3x4& local) noexcept {
    Matrix3x4 result;
    for (unsigned row = 0; row != 3; ++row)
        for (unsigned column = 0; column != 4; ++column)
            result.rows[row][column] =
                add(add(add(multiply(parent.rows[row][2], local.rows[2][column]),
                            multiply(parent.rows[row][1], local.rows[1][column])),
                        multiply(parent.rows[row][0], local.rows[0][column])),
                    column == 3 ? parent.rows[row][3] : 0.0f);
    return result;
}
Matrix3x4 inverse_rotation(float yaw, const Vector& origin, HistoryMatrixScratch& scratch,
                           bool primary, const RotationMath& math) {
    const auto rotation = rotation_matrix({0.0f, yaw, 0.0f}, {}, math);
    Matrix3x4 inverse;
    for (unsigned row = 0; row != 3; ++row)
        for (unsigned column = 0; column != 3; ++column)
            inverse.rows[row][column] = rotation.rows[column][row];
    (void)primary;
    // R^-1 * -origin must use this rotation, never prior scratch contents.
    for (unsigned row = 0; row < 3; ++row)
        inverse.rows[row][3] =
            -(inverse.rows[row][0] * origin[0] + inverse.rows[row][1] * origin[1] +
              inverse.rows[row][2] * origin[2]);
    for (unsigned i = 0; i != 3; ++i) {
        scratch.inverse_row1[i] = inverse.rows[1][i];
        scratch.inverse_row2[i] = inverse.rows[2][i];
    }
    return inverse;
}
Vector interpolated_angles(float older, float newer, float fraction, const RotationMath& math) {
    if (older == newer)
        return {0.0f, older, 0.0f};
    const auto first = history_angle_quaternion({0.0f, older, 0.0f}, math);
    const auto second = history_angle_quaternion({0.0f, newer, 0.0f}, math);
    return history_quaternion_angles(
        slerp_history_quaternion(first, align_history_quaternion(first, second), fraction, math),
        math);
}
void transform(HitboxMatrices& destination, const HitboxMatrices& source,
               const Matrix3x4& relative) noexcept {
    for (unsigned i = 0; i != 19; ++i)
        destination[i] = concatenate(relative, source[i]);
}
}
float interpolate_history_record(LagRecord& destination, const LagRecord& newer,
                                 const LagRecord& older, float requested_time, float interval,
                                 const HistoryMatrixScratch& incoming, std::uint32_t& output_flags,
                                 const RotationMath& math, bool use_captured_endpoint_pose) {
    Vector angles;
    const float fraction = interpolate_history_fields(destination, newer, older, requested_time,
                                                      interval, math, &angles);
    if (use_captured_endpoint_pose && fraction == 1.0f) {
        // Accuracy boost can reach this path at the exact newer tick.
        destination.header.animation_yaw = newer.header.animation_yaw;
        destination.header.flags = (destination.header.flags & ~0x84u) | (newer.header.flags & 0x84u);
        destination.animation = newer.animation;
        destination.matrices = newer.matrices;
        if (newer.header.flags & 0x80u)
            destination.alternatives = newer.alternatives;
    } else if (older.header.flags & 4u) {
        HistoryMatrixScratch scratch;
        std::memcpy(&scratch, &incoming, sizeof scratch);
        const auto inverse =
            inverse_rotation(older.header.animation_yaw, older.header.origin, scratch, true, math);
        transform(destination.matrices, older.matrices,
                  concatenate(rotation_matrix(angles, destination.header.origin, math), inverse));
        if ((older.header.flags & newer.header.flags & 0x80u) != 0) {
            for (unsigned i = 0; i != 3; ++i) {
                const float old_yaw = std::bit_cast<float>(older.alternatives[i].state[0]);
                const float new_yaw = std::bit_cast<float>(newer.alternatives[i].state[0]);
                const auto alternative_angles =
                    interpolated_angles(old_yaw, new_yaw, fraction, math);
                const auto alternative_inverse =
                    inverse_rotation(old_yaw, older.header.origin, scratch, false, math);
                transform(destination.alternatives[i].matrices, older.alternatives[i].matrices,
                          concatenate(
                              rotation_matrix(alternative_angles, destination.header.origin, math),
                              alternative_inverse));
            }
        } else
            destination.header.flags &= ~0x80u;
    }
    output_flags |= 0x10u;
    destination.header.field_00 = 0;
    return fraction;
}
} // namespace skeet::lagcomp
