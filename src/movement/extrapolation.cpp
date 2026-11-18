#include "skeet/movement/extrapolation.hpp"
#include "skeet/movement/extrapolation_command.hpp"
#include "skeet/movement/extrapolation_model.hpp"
#include "skeet/movement/steering_math.hpp"
#include "skeet/animation/state_copy.hpp"
#include "skeet/engine/interface_call.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace skeet {
namespace {
template <class T> T read(const void* object, std::uint32_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::uint32_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
MovementVector movement_vector(const lagcomp::Vector& value) noexcept {
    return {value[0], value[1], value[2]};
}

}

static std::int32_t extrapolate_record(void* player, client::PlayerState& history,
                                       lagcomp::LagRecord& record, std::int32_t ticks,
                                       const MovementExtrapolationBindings& bindings,
                                       const MovementExtrapolationLiveBindings* live, bool rebuild_pose) {
    if (ticks <= 0)
        return 0;
    if (ticks > (rebuild_pose ? maximum_pose_extrapolation_ticks : 32))
        return rebuild_pose ? -1 : 0;
    void* const original_state = read<void*>(player, bindings.offsets.animation_state);
    if (!original_state)
        return rebuild_pose ? -1 : 0;

    const auto& seed = record.header;
    MovementSimulation simulation{};
    std::optional<ExtrapolatedMotion> fitted;
    if (rebuild_pose) {
        const float interval = *bindings.interval;
        const auto last_update = read<float>(original_state, 0x6C);
        if (!bindings.animation_engine.update_state || !bindings.allocator ||
            !(interval > 0.0f) || !std::isfinite(interval) || !std::isfinite(last_update) ||
            seed.simulation_tick > std::numeric_limits<std::int32_t>::max() - ticks ||
            history.history.count < 2 || seed.simulation_tick != history.history.at(0).header.simulation_tick ||
            seed.origin != history.history.at(0).header.origin ||
            lagcomp::time_to_tick(read<float>(player, bindings.offsets.simulation_time), interval) !=
                seed.simulation_tick ||
            std::fabs(last_update - float(seed.simulation_tick) * interval) > interval * 1.01f ||
            !std::isfinite(seed.eye_pitch) || !std::isfinite(seed.animation_yaw) ||
            !std::isfinite(std::bit_cast<float>(seed.player_history_1ac)) ||
            !std::isfinite(seed.collision_max_z) || seed.collision_max_z <= 0.0f)
            return -1;
        initialize_live_movement_simulation(
            simulation, player, movement_vector(seed.origin), movement_vector(seed.velocity),
            seed.player_flags, std::uint32_t(seed.simulation_tick) + 1u, live->capture);
        if (simulation.movement_type != 2 || simulation.duck_amount != seed.duck_amount ||
            simulation.ducking || simulation.hull_maximum.z != seed.collision_max_z)
            return -1;
        fitted = fit_extrapolated_motion(history.history, simulation.rules, ticks);
        if (!fitted)
            return -1;
    }

    lagcomp::AnimationState saved_animation;
    animation::capture_entity_animation(saved_animation, player, bindings.offsets);
    if (rebuild_pose && (record.animation.layer_count <= 0 ||
                         record.animation.layer_count != saved_animation.layer_count))
        return -1;
    const auto saved_angles = *engine::invoke<const lagcomp::Vector*>(player, 11);
    alignas(8) std::array<std::byte, animation::copied_animation_state_size> animation_state;
    std::memcpy(animation_state.data(), original_state, animation_state.size());
    if (rebuild_pose) {
        animation::initialize_animation_state_vector(animation_state.data());
        write(animation_state.data(), 0x6C, float(seed.simulation_tick) * *bindings.interval);
        write(animation_state.data(), 0x80, seed.animation_yaw);
        animation::restore_entity_animation(record.animation, player, bindings.offsets);
    }
    write(player, bindings.offsets.animation_state, static_cast<void*>(animation_state.data()));

    //43385AFD independently obtains the actual player's cache. Its raw path
    //must not reuse the owner or snapshot supplied for a different target.
    client::Command command{};
    bool inferred;
    if (rebuild_pose) {
        inferred = true;
        if (seed.duck_amount == 1.0f)
            command.movement.buttons |= duck_button;
        else if (read<std::uint8_t>(player, bindings.field_7090d8dd) ||
                 read<std::int32_t>(player, bindings.field_52899df3) == 1)
            command.movement.buttons |= speed_button;
    } else if (live) {
        const MovementExtrapolationCommandOffsets fields{
            bindings.offsets.velocity, bindings.offsets.duck_amount, bindings.offsets.player_flags,
            bindings.field_7090d8dd,   bindings.field_52899df3,      bindings.field_d3d4230d};
        inferred = reconstruct_live_extrapolation_command(command, player, *bindings.players,
                                                          fields, live->environment);
    } else {
        const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(player) + 8, 10);
        if (index == 0)
            throw std::logic_error("Movement extrapolation requires a player index");
        auto& command_history = *bindings.players->get(std::uint32_t(index));
        const MovementExtrapolationCommandPlayer command_player{
            read<MovementVector>(player, bindings.offsets.velocity),
            read<float>(player, bindings.offsets.duck_amount),
            read<std::uint32_t>(player, bindings.offsets.player_flags),
            read<std::uint8_t>(player, bindings.field_7090d8dd),
            read<std::int32_t>(player, bindings.field_52899df3),
            read<float>(player, bindings.field_d3d4230d)};
        inferred = reconstruct_extrapolation_command(
            command, command_history.history, read<float>(&command_history.native, 0x110),
            read<std::int32_t>(&command_history.native, 0x168), command_player,
            bindings.environment.value().physics);
    }
    std::int32_t result = 0;
    if (inferred) {
        if (live && !rebuild_pose)
            initialize_live_movement_simulation(
                simulation, player, movement_vector(record.header.origin),
                movement_vector(record.header.velocity), record.header.player_flags,
                std::uint32_t(record.header.simulation_tick) + 1u, live->capture);
        else if (!live)
            initialize_movement_simulation(
                simulation, bindings.player_handle.value(), movement_vector(record.header.origin),
                movement_vector(record.header.velocity), record.header.player_flags,
                std::uint32_t(record.header.simulation_tick) + 1u, bindings.snapshot.value(),
                bindings.rules.value());
        lagcomp::Vector angles{read<float>(&history.native, 0x1a8), history.native.reference_yaw,
                               0.0f};
        float intermediate_yaw = engine::invoke<const lagcomp::Vector*>(player, 11)->at(1);
        if (rebuild_pose) {
            angles = {seed.eye_pitch, std::bit_cast<float>(seed.player_history_1ac), 0.0f};
        } else if ((history.native.flags & 0x20u) && history.history.count >= 2) {
            const auto& previous = history.history.at(1).header;
            angles[0] = previous.eye_pitch;
            angles[1] = std::bit_cast<float>(previous.player_history_1ac);
            intermediate_yaw = previous.animation_yaw;
        }
        const auto saved_reset = rebuild_pose && bindings.animation_reset ? *bindings.animation_reset : 0;
        bool complete = true;
        for (std::int32_t i = 0; i < ticks; ++i) {
            if (rebuild_pose && !fitted->coast) {
                auto physics = capture_movement_command_physics(player, live->environment);
                if (simulation.player_flags & on_ground) {
                    const auto ground = capture_movement_ground_weapon_physics(
                        player, command.movement, simulation.player_flags, live->environment);
                    physics.ducking = ground.duck;
                    physics.weapon_speed_enabled = ground.physics.weapon_speed_enabled;
                    physics.has_weapon = ground.physics.has_weapon;
                    if (physics.has_weapon) {
                        physics.weapon_max_speed = ground.physics.weapon_max_speed;
                        physics.weapon_mode_count = ground.physics.weapon_mode_count;
                        physics.weapon_mode = ground.physics.weapon_mode;
                    }
                }
                MovementAngles movement_angles{};
                command.movement.forward_move = 0.0f;
                if (!extrapolated_motion_command(simulation, *fitted, physics, command.movement,
                                                 movement_angles)) {
                    complete = false;
                    break;
                }
                command.angles[1] = movement_angles.yaw;
            }
            float* const clock = bindings.current_time;
            const float saved_time = *clock;
            *clock = static_cast<float>(std::bit_cast<std::int32_t>(simulation.mode)) *
                     (*bindings.interval);
            const auto before = simulation.player_flags;
            const auto tick = simulation.mode;
            if (live)
                simulate_live_movement_tick(simulation, *bindings.collision, live->environment,
                                            player, command);
            else
                simulate_movement_tick(simulation, *bindings.collision,
                                       bindings.environment.value(), bindings.player_handle.value(),
                                       command.movement,
                                       {command.angles[0], command.angles[1], command.angles[2]});
            if (rebuild_pose &&
                (simulation.mode != tick + 1u || simulation.trace.all_solid ||
                 !std::isfinite(simulation.position.x) || !std::isfinite(simulation.position.y) ||
                 !std::isfinite(simulation.position.z) || !std::isfinite(simulation.velocity.x) ||
                 !std::isfinite(simulation.velocity.y) || !std::isfinite(simulation.velocity.z) ||
                 (((before ^ simulation.player_flags) & on_ground) && i != ticks - 1))) {
                *clock = saved_time;
                complete = false;
                break;
            }
            auto update_angles = angles;
            if (!rebuild_pose && i != ticks - 1)
                update_angles[1] = intermediate_yaw;

            const auto velocity = read<MovementVector>(player, bindings.offsets.velocity);
            write(player, bindings.offsets.velocity, simulation.velocity);
            const auto velocity_7a = read<MovementVector>(player, bindings.velocity_7a2d60c1);
            write(player, bindings.velocity_7a2d60c1, simulation.velocity);
            const auto guard_offset = bindings.offsets.transform_guard_49d93e62 - 0x14u;
            const auto guard = read<std::uint32_t>(player, guard_offset);
            const auto origin_4e = read<MovementVector>(player, bindings.origin_4e66bb34);
            write(player, bindings.origin_4e66bb34, simulation.position);
            const auto origin_2a = read<MovementVector>(player, bindings.origin_2a671b42);
            write(player, bindings.origin_2a671b42, simulation.position);
            const auto flags = read<std::uint32_t>(player, bindings.offsets.player_flags);
            write(player, bindings.offsets.player_flags, simulation.player_flags);
            const auto duck = rebuild_pose ? read<float>(player, bindings.offsets.duck_amount) : 0.0f;
            const auto simulation_time =
                rebuild_pose ? read<float>(player, bindings.offsets.simulation_time) : 0.0f;
            if (rebuild_pose) {
                write(player, bindings.offsets.duck_amount, simulation.duck_amount);
                write(player, bindings.offsets.simulation_time, *clock);
            }
            write(player, guard_offset, UINT32_MAX);
            update_angles[1] = normalize_movement_angle(update_angles[1]);
            const animation::AnimationUpdateTiming timing{
                *bindings.animation_frame, *clock, *bindings.interval, bindings.animation_reset};
            animation::update_animation_state(animation_state.data(), update_angles, timing,
                                              bindings.animation_engine);
            if (rebuild_pose) {
                write(player, bindings.offsets.simulation_time, simulation_time);
                write(player, bindings.offsets.duck_amount, duck);
                complete = read<float>(animation_state.data(), 0x6C) == *clock;
            }
            write(player, bindings.offsets.player_flags, flags);
            write(player, bindings.origin_2a671b42, origin_2a);
            write(player, bindings.origin_4e66bb34, origin_4e);
            write(player, guard_offset, guard);
            write(player, bindings.velocity_7a2d60c1, velocity_7a);
            write(player, bindings.offsets.velocity, velocity);
            *clock = saved_time;
            if (!complete)
                break;
        }
        if (rebuild_pose && bindings.animation_reset)
            *bindings.animation_reset = static_cast<std::uint8_t>(saved_reset);
        const float yaw = read<float>(animation_state.data(), 0x80);
        const float speed_factor = lagcomp::animation_speed_factor(animation_state.data());
        if (complete && (!rebuild_pose || (std::isfinite(yaw) && std::isfinite(speed_factor)))) {
            record.header.simulation_tick = add(record.header.simulation_tick, ticks);
            if (rebuild_pose) {
                record.header.field_00 = 0;
                record.header.animation_yaw = yaw;
            } else {
                record.header.receive_tick = add(record.header.receive_tick, ticks);
            }
            record.header.origin = {simulation.position.x, simulation.position.y, simulation.position.z};
            record.header.duck_amount = simulation.duck_amount;
            record.header.player_flags = simulation.player_flags;
            record.header.collision_max_z =
                rebuild_pose ? simulation.hull_maximum.z : (simulation.player_flags & 2u) ? 54.0f : 72.0f;
            record.header.velocity = {simulation.velocity.x, simulation.velocity.y, simulation.velocity.z};
            record.header.animation_speed_factor = speed_factor;
            animation::capture_entity_animation(record.animation, player, bindings.offsets);
            record.header.flags &= 0xffffff7bu;
            result = ticks;
        } else {
            result = -1;
        }
        animation::restore_entity_animation(saved_animation, player, bindings.offsets);
        if (bindings.set_absolute_angles)
            bindings.set_absolute_angles(player, saved_angles);
    }
    write(player, bindings.offsets.animation_state, original_state);
    animation::release_animation_state_vector(animation_state.data(), bindings.allocator);
    return result;
}

std::int32_t extrapolate_lag_record(void* player, client::PlayerState& history,
                                    lagcomp::LagRecord& record, std::int32_t ticks,
                                    const MovementExtrapolationBindings& bindings) {
    return extrapolate_record(player, history, record, ticks, bindings, nullptr, false);
}
std::int32_t extrapolate_lag_record(void* player, client::PlayerState& history,
                                    lagcomp::LagRecord& record, std::int32_t ticks,
                                    const MovementExtrapolationBindings& bindings,
                                    const MovementExtrapolationLiveBindings& live, bool rebuild_pose) {
    return extrapolate_record(player, history, record, ticks, bindings, &live, rebuild_pose);
}

} // namespace skeet
