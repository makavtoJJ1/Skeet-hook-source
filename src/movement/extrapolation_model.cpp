#include "skeet/movement/extrapolation_model.hpp"
#include "skeet/movement/simulation_velocity.hpp"
#include "skeet/movement/steering_math.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace skeet {
namespace {
constexpr float velocity_tolerance = 2.0f;
constexpr std::uint32_t ground = 1u;

float length_xy(float x, float y) noexcept {
    return std::sqrt(x * x + y * y);
}

MovementVector after_friction(MovementVector velocity, const MovementSimulationRules& rules) noexcept {
    const float speed = length_xy(velocity.x, velocity.y);
    if (speed < 0.1f)
        return velocity;
    const float loss = rules.friction * std::max(rules.stop_speed, speed) * rules.interval_per_tick;
    const float scale = std::max(0.0f, speed - loss) / speed;
    return {velocity.x * scale, velocity.y * scale, 0.0f};
}

std::optional<ExtrapolatedMotion> fit_span(const lagcomp::LagRecordHeader& older,
                                          const lagcomp::LagRecordHeader& newer,
                                          const MovementSimulationRules& rules) noexcept {
    const auto ticks = std::int64_t(newer.simulation_tick) - older.simulation_tick;
    if (ticks < 1 || ticks > 8 || ((older.flags | newer.flags) & 0x300u) ||
        ((older.player_flags ^ newer.player_flags) & 7u) ||
        older.duck_amount != newer.duck_amount ||
        (newer.duck_amount != 0.0f && newer.duck_amount != 1.0f))
        return std::nullopt;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(older.origin[axis]) || !std::isfinite(newer.origin[axis]) ||
            !std::isfinite(older.velocity[axis]) || !std::isfinite(newer.velocity[axis]))
            return std::nullopt;

    const float dt = rules.interval_per_tick;
    const float duration = float(ticks) * dt;
    const float dx = newer.origin[0] - older.origin[0];
    const float dy = newer.origin[1] - older.origin[1];
    const float dz = newer.origin[2] - older.origin[2];
    if (dx * dx + dy * dy + dz * dz > 4096.0f)
        return std::nullopt;
    const float position_tolerance = 0.25f + duration * velocity_tolerance;
    const bool grounded = (newer.player_flags & ground) != 0;
    if (!grounded) {
        const float fall = rules.gravity * duration;
        if (std::fabs(newer.velocity[2] - (older.velocity[2] - fall)) > velocity_tolerance ||
            std::fabs(dz - (older.velocity[2] - fall * 0.5f) * duration) > position_tolerance)
            return std::nullopt;
    } else if (std::fabs(dz) > position_tolerance ||
               std::fabs(older.velocity[2]) > velocity_tolerance ||
               std::fabs(newer.velocity[2]) > velocity_tolerance) {
        return std::nullopt;
    }

    MovementVector coast_velocity{older.velocity[0], older.velocity[1], 0.0f};
    MovementVector coast_delta{};
    for (std::int64_t i = 0; i < ticks; ++i) {
        if (grounded) {
            coast_velocity = after_friction(coast_velocity, rules);
            if (length_xy(coast_velocity.x, coast_velocity.y) < 1.0f)
                coast_velocity = {};
        }
        coast_delta.x += coast_velocity.x * dt;
        coast_delta.y += coast_velocity.y * dt;
    }
    if (length_xy(coast_delta.x - dx, coast_delta.y - dy) <= position_tolerance &&
        length_xy(coast_velocity.x - newer.velocity[0], coast_velocity.y - newer.velocity[1]) <=
            velocity_tolerance)
        return ExtrapolatedMotion{{}, true};

    const MovementVector step{(newer.velocity[0] - older.velocity[0]) / float(ticks),
                              (newer.velocity[1] - older.velocity[1]) / float(ticks), 0.0f};
    // Collision-free XY integrates the velocity after each acceleration step.
    const float ramp_time = (float(ticks) + 1.0f) * 0.5f;
    if (length_xy(dx - (older.velocity[0] + step.x * ramp_time) * duration,
                  dy - (older.velocity[1] + step.y * ramp_time) * duration) > position_tolerance)
        return std::nullopt;
    const float approach = step.x * newer.velocity[0] + step.y * newer.velocity[1];
    const float sideways = step.x * newer.velocity[1] - step.y * newer.velocity[0];
    const bool braking = grounded && approach < 0.0f && std::fabs(sideways) <= -approach * 0.05f;
    return ExtrapolatedMotion{step, false, braking};
}
}

std::optional<int> pose_receive_age(std::int32_t now, std::int32_t received,
                                    float receive_interval, float simulation_interval) noexcept {
    if (!(receive_interval > 0) || !(simulation_interval > 0) ||
        !std::isfinite(receive_interval) || !std::isfinite(simulation_interval))
        return std::nullopt;
    const auto elapsed = std::int64_t(now) - received;
    const double ticks = double(elapsed) * receive_interval / simulation_interval;
    if (ticks < 0 || ticks >= maximum_pose_extrapolation_ticks + 0.5)
        return std::nullopt;
    return int(std::round(ticks));
}

std::optional<ExtrapolatedMotion> fit_extrapolated_motion(
    const lagcomp::RecordRing& ring, const MovementSimulationRules& rules,
    std::int32_t ticks) noexcept {
    if (ticks < 1 || ticks > maximum_pose_extrapolation_ticks || ring.count < 2 ||
        !(rules.interval_per_tick > 0.0f) || !std::isfinite(rules.interval_per_tick) ||
        !(rules.gravity >= 0.0f) || !std::isfinite(rules.gravity) ||
        !(rules.friction >= 0.0f) || !std::isfinite(rules.friction) ||
        !(rules.stop_speed >= 0.0f) || !std::isfinite(rules.stop_speed))
        return std::nullopt;
    const auto& newest = ring.at(0).header;
    const auto& previous = ring.at(1).header;
    auto latest = fit_span(previous, newest, rules);
    if (!latest || ticks == 1)
        return latest;
    if (ring.count < 3)
        return latest->coast && ticks == 2 ? latest : std::nullopt;
    const auto& older = ring.at(2).header;
    const auto earlier = fit_span(older, previous, rules);
    if (!earlier || earlier->coast != latest->coast)
        return std::nullopt;
    if (!latest->coast) {
        if (length_xy(earlier->velocity_step.x - latest->velocity_step.x,
                      earlier->velocity_step.y - latest->velocity_step.y) * float(ticks) >
            velocity_tolerance * 2.0f)
            return std::nullopt;
        const float span = float(std::int64_t(newest.simulation_tick) - older.simulation_tick);
        latest->velocity_step = {(newest.velocity[0] - older.velocity[0]) / span,
                                 (newest.velocity[1] - older.velocity[1]) / span, 0.0f};
    }
    return latest;
}

bool extrapolated_motion_command(const MovementSimulation& state, const ExtrapolatedMotion& motion,
                                const MovementPhysics& physics, UserCommand& command,
                                MovementAngles& angles) noexcept {
    if (motion.coast)
        return true;
    if (!(physics.player_speed_limit > 0.0f) || !std::isfinite(physics.player_speed_limit) ||
        !(physics.player_max_speed > 0.0f) || !std::isfinite(physics.player_max_speed))
        return false;
    MovementVector target{state.velocity.x + motion.velocity_step.x,
                          state.velocity.y + motion.velocity_step.y, 0.0f};
    if (motion.braking &&
        target.x * motion.velocity_step.x + target.y * motion.velocity_step.y >= 0.0f)
        target = {};
    const auto baseline = (state.player_flags & ground) ? after_friction(state.velocity, state.rules)
                                                       : state.velocity;
    const MovementVector impulse{target.x - baseline.x, target.y - baseline.y, 0.0f};
    angles.yaw = planar_movement_yaw(impulse);
    const float impulse_size = length_xy(impulse.x, impulse.y);
    if (!std::isfinite(impulse_size))
        return false;
    if (impulse_size <= 0.1f)
        return true;

    const MovementVector direction{impulse.x / impulse_size, impulse.y / impulse_size, 0.0f};
    const auto projected_velocity = [&](float input) {
        auto trial = state;
        command.forward_move = input;
        prepare_movement_simulation_command(trial, trial.player, physics.player_speed_limit,
                                            physics.player_max_speed, command);
        prepare_movement_simulation_velocity(trial, physics, command, angles, false);
        const float projection = (trial.velocity.x - baseline.x) * direction.x +
                                 (trial.velocity.y - baseline.y) * direction.y;
        return std::pair{projection, trial.velocity};
    };
    // Raw forward input is clipped by the same speed, stamina and duck rules as
    // the full simulation. Find the smallest input that reaches the impulse.
    float lower = 0.0f, upper = 450.0f;
    for (int i = 0; i < 12; ++i) {
        const float middle = (lower + upper) * 0.5f;
        if (projected_velocity(middle).first < impulse_size)
            lower = middle;
        else
            upper = middle;
    }
    const auto [projection, velocity] = projected_velocity(upper);
    return std::isfinite(projection) &&
           length_xy(velocity.x - target.x, velocity.y - target.y) <= velocity_tolerance;
}

} // namespace skeet
