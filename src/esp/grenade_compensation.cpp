#include "skeet/esp/grenade_prediction.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace skeet::esp {
namespace {
constexpr double radians = std::numbers::pi / 180.0;
struct Vector {
    double x, y, z;
    Vector operator+(Vector b) const {
        return {x + b.x, y + b.y, z + b.z};
    }
    Vector operator-(Vector b) const {
        return {x - b.x, y - b.y, z - b.z};
    }
    Vector operator*(double s) const {
        return {x * s, y * s, z * s};
    }
};
double dot(Vector a, Vector b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vector forward(double pitch, double yaw) {
    const auto cp = std::cos(pitch * radians);
    return {cp * std::cos(yaw * radians), cp * std::sin(yaw * radians), -std::sin(pitch * radians)};
}
double launch_pitch(double pitch) {
    return pitch - (90.0 - std::abs(pitch)) / 9.0;
}
double command_pitch(Vector direction) {
    const auto pitch = std::atan2(-direction.z, std::hypot(direction.x, direction.y)) / radians;
    return (pitch + 10.0) * (pitch < -10.0 ? 9.0 / 8.0 : 9.0 / 10.0);
}

// All stationary yaw candidates on a pitch boundary. Restrict tan(yaw/2)
// to [-1,1] in two charts so roots near 180 degrees remain well conditioned.
using Polynomial = std::array<double, 5>;
double evaluate(const Polynomial& p, int degree, double x) {
    auto value = p[degree];
    while (degree-- > 0)
        value = value * x + p[degree];
    return value;
}
int roots(Polynomial p, int degree, std::array<double, 4>& output) {
    double scale = 0;
    for (int i = 0; i <= degree; ++i)
        scale = std::max(scale, std::abs(p[i]));
    if (scale == 0)
        return 0;
    for (auto& value : p)
        value /= scale;
    while (degree > 0 && std::abs(p[degree]) < 1e-14)
        --degree;
    if (degree == 0)
        return 0;
    if (degree == 1) {
        const auto value = -p[0] / p[1];
        if (value < -1 || value > 1)
            return 0;
        output[0] = value;
        return 1;
    }
    Polynomial derivative{};
    for (int i = 1; i <= degree; ++i)
        derivative[i - 1] = i * p[i];
    std::array<double, 4> critical{};
    const auto count = roots(derivative, degree - 1, critical);
    std::array<double, 6> edges{};
    edges[0] = -1;
    std::copy_n(critical.begin(), count, edges.begin() + 1);
    edges[count + 1] = 1;
    int found = 0;
    const auto append = [&](double value) {
        if (found < degree && (found == 0 || std::abs(value - output[found - 1]) > 1e-9))
            output[found++] = value;
    };
    for (int i = 0; i <= count; ++i) {
        auto low = edges[i], high = edges[i + 1];
        auto first = evaluate(p, degree, low);
        const auto last = evaluate(p, degree, high);
        if (std::abs(first) < 1e-12)
            append(low);
        if (std::signbit(first) != std::signbit(last) && first != 0 && last != 0) {
            for (int step = 0; step < 56; ++step) {
                const auto middle = (low + high) * 0.5;
                const auto value = evaluate(p, degree, middle);
                if (std::signbit(value) == std::signbit(first)) {
                    low = middle;
                    first = value;
                } else
                    high = middle;
            }
            append((low + high) * 0.5);
        }
    }
    if (std::abs(evaluate(p, degree, 1)) < 1e-12)
        append(1);
    return found;
}
}

GrenadeCompensation compensate_grenade_movement(Vector3 velocity, float speed,
                                                Vector3& angles) noexcept {
    if (!(speed > 0) || !std::isfinite(speed) || !std::isfinite(velocity.x) ||
        !std::isfinite(velocity.y) || !std::isfinite(velocity.z) || !std::isfinite(angles.x) ||
        !std::isfinite(angles.y))
        return GrenadeCompensation::invalid_input;

    const auto pitch = std::clamp(double(angles.x), -89.0, 89.0);
    const auto yaw = std::remainder(double(angles.y), 360.0);
    const auto desired = forward(launch_pitch(pitch), yaw);
    // Normalize by throw speed. Solve |k*d - w| = 1 for the real launch,
    // including the game's pitch remapping and 1.25 * player velocity.
    const Vector inherited{1.25 * velocity.x / speed, 1.25 * velocity.y / speed,
                           1.25 * velocity.z / speed};
    const auto along = dot(inherited, desired);
    const auto across = inherited - desired * along;
    const auto discriminant = 1.0 - dot(across, across);
    if (discriminant >= 0 && along + std::sqrt(discriminant) > 1e-10) {
        const auto aim = desired * std::sqrt(discriminant) - across;
        const auto solved_pitch = command_pitch(aim);
        if (solved_pitch >= -89.0 && solved_pitch <= 89.0) {
            angles.x = static_cast<float>(solved_pitch);
            angles.y = static_cast<float>(std::atan2(aim.y, aim.x) / radians);
            return GrenadeCompensation::exact;
        }
    }

    // The requested direction can be outside the reachable velocity sphere,
    // or require an illegal command pitch. Minimize angular error on the
    // reachable sphere and both pitch boundaries instead of producing NaN.
    double best = -2;
    const auto consider = [&](Vector aim) {
        const auto candidate_pitch = std::clamp(command_pitch(aim), -89.0, 89.0);
        const auto candidate_yaw = std::atan2(aim.y, aim.x) / radians;
        const auto launch = forward(launch_pitch(candidate_pitch), candidate_yaw) + inherited;
        const auto magnitude = std::sqrt(dot(launch, launch));
        if (magnitude <= 1e-10)
            return;
        const auto score = dot(launch, desired) / magnitude;
        if (score > best) {
            best = score;
            angles.x = static_cast<float>(candidate_pitch);
            angles.y = static_cast<float>(candidate_yaw);
        }
    };
    consider(desired);
    if (discriminant >= 0 && along + std::sqrt(discriminant) > 1e-10)
        consider(desired * std::sqrt(discriminant) - across);
    const auto inherited_length = std::sqrt(dot(inherited, inherited));
    if (inherited_length > 1) {
        const auto axis = inherited * (1.0 / inherited_length);
        auto tangent = desired - axis * dot(axis, desired);
        auto tangent_length = std::sqrt(dot(tangent, tangent));
        if (tangent_length < 1e-12) {
            const Vector basis = std::abs(axis.z) < 0.9 ? Vector{0, 0, 1} : Vector{1, 0, 0};
            tangent = basis - axis * dot(axis, basis);
            tangent_length = std::sqrt(dot(tangent, tangent));
        }
        const auto sine = 1.0 / inherited_length;
        consider(axis * -sine + tangent * (std::sqrt(1.0 - sine * sine) / tangent_length));
    }
    for (double boundary : {-89.0, 89.0}) {
        const auto radius = std::cos(launch_pitch(boundary) * radians);
        const auto z = -std::sin(launch_pitch(boundary) * radians);
        const auto vz = inherited.z + z;
        const auto constant = desired.x * inherited.x + desired.y * inherited.y + desired.z * vz;
        const auto norm =
            inherited.x * inherited.x + inherited.y * inherited.y + vz * vz + radius * radius;
        for (double sign : {1.0, -1.0}) {
            const auto a = sign * radius * desired.x, b = sign * radius * desired.y;
            const auto f = 2 * sign * radius * inherited.x;
            const auto g = 2 * sign * radius * inherited.y;
            const std::array<double, 3> n{constant + a, 2 * b, constant - a};
            const std::array<double, 3> q{norm + f, 2 * g, norm - f};
            std::array<double, 6> derivative{};
            // Derivative of n(t) / sqrt(q(t) * (1+t*t)); degree five cancels.
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    if (i) {
                        derivative[i + j - 1] += 2 * i * n[i] * q[j];
                        derivative[i + j + 1] += 2 * i * n[i] * q[j];
                    }
                    if (j) {
                        derivative[i + j - 1] -= j * n[i] * q[j];
                        derivative[i + j + 1] -= j * n[i] * q[j];
                    }
                    derivative[i + j + 1] -= 2 * n[i] * q[j];
                }
            Polynomial polynomial{};
            std::copy_n(derivative.begin(), 5, polynomial.begin());
            std::array<double, 4> stationary{};
            const auto count = roots(polynomial, 4, stationary);
            const auto sample = [&](double t) {
                const auto divisor = 1.0 + t * t;
                consider(
                    {sign * radius * (1.0 - t * t) / divisor, sign * radius * 2 * t / divisor, z});
            };
            sample(-1);
            sample(0);
            sample(1);
            for (int i = 0; i < count; ++i)
                sample(stationary[i]);
        }
    }
    return GrenadeCompensation::limited;
}
} // namespace skeet::esp
