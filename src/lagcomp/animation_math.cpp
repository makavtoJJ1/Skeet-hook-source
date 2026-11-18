#include "skeet/lagcomp/animation_math.hpp"
#include "skeet/resolver.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <immintrin.h>

namespace skeet::lagcomp {
namespace {
float absolute(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) & 0x7FFFFFFFu);
}
float negative(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) ^ 0x80000000u);
}
float normalize(float value) noexcept {
    if (std::isnan(value))
        return 0.0f;
    if (absolute(value) > 180.0f) {
        const float correction = round_half_away_from_zero(absolute(value / 360.0f)) * 360.0f;
        value = value < 0.0f ? value + correction : value - correction;
    }
    return value;
}
float angle_modulo(float value) noexcept {
    const float scale = std::bit_cast<float>(0x43360B61u);
    const auto integer = _mm_cvttss_si32(_mm_set_ss(value * scale));
    return float(std::uint32_t(integer) & 0xFFFFu) * std::bit_cast<float>(0x3BB40000u);
}
}

float advance_animation_yaw(const AnimationYawMotion& motion, float target_yaw,
                            float stationary_yaw, float elapsed, float current_yaw,
                            bool strict_bounds) noexcept {
    current_yaw = _mm_cvtss_f32(
        _mm_min_ss(_mm_max_ss(_mm_set_ss(current_yaw), _mm_set_ss(-360.0f)), _mm_set_ss(360.0f)));
    float delta = float(std::fmod(double(target_yaw - current_yaw), 360.0));
    if (target_yaw > current_yaw) {
        if (delta >= 180.0f)
            delta -= 360.0f;
    } else if (delta <= -180.0f) {
        delta += 360.0f;
    }
    float minimum, maximum;
    anti_aim::animation_yaw_limits(motion.bounds, minimum, maximum);
    if (delta > maximum)
        current_yaw = target_yaw - maximum;
    else if (delta < (strict_bounds ? minimum : negative(maximum)))
        // The server clamps the lower and upper model yaw limits independently.
        current_yaw = strict_bounds ? target_yaw - minimum : maximum + target_yaw;
    current_yaw = normalize(current_yaw);

    float rate;
    // Native JB treats unordered speed comparisons as the moving branch.
    if (!(motion.horizontal_speed_ec <= 0.1f) || !(absolute(motion.vertical_speed_f0) <= 100.0f)) {
        rate = motion.bounds.speed_fraction_11c * 20.0f + 30.0f;
    } else {
        target_yaw = stationary_yaw;
        rate = 100.0f;
    }
    float step = rate * elapsed;
    if (step < 0.0f)
        step = negative(step);
    const float target = angle_modulo(target_yaw);
    const float source = angle_modulo(current_yaw);
    delta = normalize(target - source);
    if (delta > step)
        return source + step;
    if (delta < negative(step))
        return source - step;
    return target;
}
} // namespace skeet::lagcomp
