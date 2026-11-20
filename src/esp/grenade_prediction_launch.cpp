#include "skeet/esp/grenade_prediction.hpp"
#include "skeet/movement/collision.hpp"
#include <bit>
#include <cmath>
#include <immintrin.h>
#include <limits>

namespace skeet::esp {
namespace {
constexpr auto radians = std::bit_cast<float>(0x3c8efa35u);
constexpr auto degrees = std::bit_cast<float>(0x42652ee0u);
float length(Vector3 v) {
    return std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
}
Vector3 forward(Vector3 angles) {
    const auto yaw = angles.y * radians, pitch = angles.x * radians;
    const auto sy = std::sin(yaw), cy = std::cos(yaw);
    const auto sp = std::sin(pitch), cp = std::cos(pitch);
    return {cy * cp, cp * sy, -sp};
}
float clamp(float value, float lower, float upper) {
    return _mm_cvtss_f32(
        _mm_min_ss(_mm_max_ss(_mm_set_ss(value), _mm_set_ss(lower)), _mm_set_ss(upper)));
}
}
void compensate_grenade_movement(Vector3 velocity, float speed, Vector3& angles) noexcept {
    const auto direction = forward(angles);
    const auto inverse_direction =
        1.0f / (length(direction) + std::numeric_limits<float>::epsilon());
    const Vector3 unit{inverse_direction * direction.x, inverse_direction * direction.y,
                       inverse_direction * direction.z};
    const Vector3 opposite{-velocity.x, -velocity.y, -velocity.z};
    const auto velocity_length = length(velocity);
    const auto inverse_opposite = 1.0f / (length(opposite) + std::numeric_limits<float>::epsilon());
    const auto alignment =
        ((opposite.y * inverse_opposite) * unit.y + (opposite.x * inverse_opposite) * unit.x) +
        (opposite.z * inverse_opposite) * unit.z;
    const auto travel =
        (std::sqrt((((25.0f * alignment) * alignment) * velocity_length) * velocity_length +
                   (16.0f * speed) * speed - (25.0f * velocity_length) * velocity_length) -
         (5.0f * velocity_length) * alignment) *
        0.25f;
    const auto reciprocal_speed = 1.0f / speed;
    const Vector3 result{(travel * unit.x - velocity.x * 1.25f) * reciprocal_speed,
                         (travel * unit.y - velocity.y * 1.25f) * reciprocal_speed,
                         (travel * unit.z - velocity.z * 1.25f) * reciprocal_speed};
    float pitch, yaw = 0;
    if (result.y == 0.0f && result.x == 0.0f)
        pitch = result.z <= 0 ? 90.0f : 270.0f;
    else {
        yaw = static_cast<float>(
                  std::atan2(static_cast<double>(result.y), static_cast<double>(result.x))) *
              degrees;
        const auto horizontal = std::sqrt(result.x * result.x + result.y * result.y);
        pitch = static_cast<float>(
                    std::atan2(static_cast<double>(-result.z), static_cast<double>(horizontal))) *
                degrees;
    }
    if (std::isfinite(pitch) && std::isfinite(yaw)) {
        angles.x = pitch;
        angles.y = yaw;
    }
}
GrenadeLaunch prepare_grenade_throw(const GrenadeThrowInput& input, GrenadeTraceProvider provider) {
    auto velocity = input.player_velocity;
    if (input.zero_slow_movement && length(velocity) < 10.0f)
        velocity = {};
    auto angles = input.view_angles;
    const auto strength = clamp(input.strength, 0, 1);
    const auto speed = (strength * 0.7f + 0.3f) * clamp(input.throw_velocity * 0.9f, 15, 750);
    if (input.compensate_movement) {
        angles.x -= input.aim_punch.x * input.recoil_scale;
        angles.y -= input.aim_punch.y * input.recoil_scale;
        angles.z -= input.aim_punch.z * input.recoil_scale;
        if ((velocity.x * velocity.x + velocity.y * velocity.y) + velocity.z * velocity.z > 4.0f)
            compensate_grenade_movement(velocity, speed, angles);
    }
    if (angles.x > 90.0f)
        angles.x -= 360.0f;
    else if (angles.x < -90.0f)
        angles.x += 360.0f;
    angles.x -= ((90.0f - std::abs(angles.x)) * 10.0f) / 90.0f;
    MovementTraceFlagGuard trace_guard(provider.trace_flag);
    const auto direction = forward(angles);
    Vector3 origin{input.eye_position.x, input.eye_position.y,
                   (input.eye_position.z - 12.0f) + strength * 12.0f};
    const Vector3 delta{(direction.x * 22.0f + origin.x) - origin.x,
                        (direction.y * 22.0f + origin.y) - origin.y,
                        (direction.z * 22.0f + origin.z) - origin.z};
    const auto hit =
        provider.trace(provider.context,
                       {origin,
                        delta,
                        {2, 2, 2},
                        (delta.y * delta.y + delta.x * delta.x) + delta.z * delta.z != 0,
                        false},
                       0x208400bu, {input.owner_entity, 0xffffffffu, 13});
    origin = {hit.end.x - direction.x * 6.0f, hit.end.y - direction.y * 6.0f,
              hit.end.z - direction.z * 6.0f};
    velocity = {speed * direction.x + velocity.x * 1.25f, speed * direction.y + velocity.y * 1.25f,
                speed * direction.z + velocity.z * 1.25f};
    return {origin, velocity, angles, strength, speed};
}
} // namespace skeet::esp
