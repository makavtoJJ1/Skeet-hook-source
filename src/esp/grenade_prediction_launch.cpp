#include "skeet/esp/grenade_prediction.hpp"
#include "skeet/movement/collision.hpp"
#include <bit>
#include <cmath>
#include <immintrin.h>

namespace skeet::esp {
namespace {
constexpr auto radians = std::bit_cast<float>(0x3c8efa35u);
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
GrenadeLaunch prepare_grenade_throw(const GrenadeThrowInput& input, GrenadeTraceProvider provider) {
    auto velocity = input.player_velocity;
    if (!input.compensate_movement && input.zero_slow_movement && length(velocity) < 10.0f)
        velocity = {};
    auto angles = input.view_angles;
    const auto strength = clamp(input.strength, 0, 1);
    const auto speed = (strength * 0.7f + 0.3f) * clamp(input.throw_velocity * 0.9f, 15, 750);
    if (input.compensate_movement) {
        angles.x -= input.aim_punch.x * input.recoil_scale;
        angles.y -= input.aim_punch.y * input.recoil_scale;
        angles.z -= input.aim_punch.z * input.recoil_scale;
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
