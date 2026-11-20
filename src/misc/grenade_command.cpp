#include "skeet/misc/grenade_command.hpp"
#include "skeet/client/command_recapture.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/movement/collision.hpp"
#include "skeet/movement/simulation_capture.hpp"
#include "skeet/movement/simulation_environment.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <limits>

namespace skeet::misc {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
std::int32_t add(std::int32_t first, std::int32_t second) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(first) + std::uint32_t(second));
}
std::int32_t truncate(float value) noexcept {
    return _mm_cvttss_si32(_mm_set_ss(value));
}
float clamp(float value, float low, float high) noexcept {
    return _mm_cvtss_f32(
        _mm_min_ss(_mm_max_ss(_mm_set_ss(value), _mm_set_ss(low)), _mm_set_ss(high)));
}
std::uint32_t identity(const void* entity) noexcept {
    return std::uint32_t(reinterpret_cast<std::uintptr_t>(entity));
}
bool supported(std::uint32_t definition) noexcept {
    // Constructor+1DC/+1E0 words {0,0x17800}: flash,HE,smoke,molotov,incendiary.
    return definition < 64 && ((std::uint64_t(0x17800u) << 32) & (std::uint64_t(1) << definition));
}
esp::Vector3 vector(const std::array<float, 3>& value) noexcept {
    return {value[0], value[1], value[2]};
}
float squared(esp::Vector3 value) noexcept {
    return (value.x * value.x + value.y * value.y) + value.z * value.z;
}
esp::Vector3 subtract(esp::Vector3 first, esp::Vector3 second) noexcept {
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}
void movement(client::Command& command, esp::Vector3 value) noexcept {
    command.movement.forward_move = value.x;
    command.movement.side_move = value.y;
    command.movement.up_move = value.z;
}
std::int32_t after_throw_tick(float time, float interval) noexcept {
    const auto tick = truncate(time / interval + 0.5f);
    return time < float(tick) * interval ? tick : add(tick, 1);
}
std::uint8_t& send_packet(client::CommandFrame& frame) noexcept {
    return *reinterpret_cast<std::uint8_t*>(std::uintptr_t(frame.send_packet_address));
}
float throw_speed(void* weapon) {
    const auto base = engine::invoke<float>(weapon, 452);
    return clamp(base * 0.8999999761581421f, 15.0f, 750.0f);
}
}

bool GrenadeCommandRuntime::release_enabled(InputBitmap input) const noexcept {
    return config.automatic_release.value && hotkey_active(config.release_key.value.binding, input);
}
bool GrenadeCommandRuntime::release_is_recent() const noexcept {
    const auto difference =
        std::uint32_t(state.released_command.tick) - std::uint32_t(*bindings.tick_count);
    const auto sign = std::bit_cast<std::int32_t>(difference) >> 31;
    const auto absolute =
        std::bit_cast<std::int32_t>((difference ^ std::uint32_t(sign)) - std::uint32_t(sign));
    return absolute < truncate(0.25f / *bindings.tick_interval + 0.5f);
}
void* GrenadeCommandRuntime::active_weapon(void* player) const {
    const auto handle = field<std::uint32_t>(player, bindings.offsets.active_weapon);
    return handle == 0xffffffffu ? nullptr : engine::invoke<void*>(bindings.entity_list, 4, handle);
}
const client::Command*
GrenadeCommandRuntime::previous_command(const client::Command& command) const noexcept {
    const auto number = std::uint32_t(command.number) - 1u;
    const auto* commands = field<const client::Command*>(bindings.input, 0xF0);
    const auto* previous = commands + number % 150u;
    return std::uint32_t(previous->number) == number ? previous : nullptr;
}

void GrenadeCommandRuntime::phase1(void* player, const client::Command& command) {
    state.captured_command = command;
    if (!(field<std::uint32_t>(player, bindings.offsets.player_flags) & 1u))
        movement(state.captured_command, {});
}

void GrenadeCommandRuntime::phase2(void* player, void* weapon, client::Command& command,
                                   std::array<float, 3>& angles, InputBitmap input) {
    if (!release_enabled(input) || !weapon ||
        !supported(field<std::uint16_t>(weapon, bindings.offsets.weapon_definition)))
        return;
    const auto pin = field<std::uint8_t>(weapon, bindings.offsets.grenade_pin_pulled);
    const auto throw_time = field<float>(weapon, bindings.offsets.grenade_throw_time);
    if (!release_is_recent())
        return;
    bool apply = false;
    if (!pin && throw_time > 0.0f) {
        apply = field<std::int32_t>(player, bindings.offsets.player_tick_base) <=
                after_throw_tick(throw_time, *bindings.tick_interval);
    }
    if (!apply) {
        if (command.number != state.release_command_number) {
            if (command.number == add(state.release_command_number, 1) || pin ||
                !(throw_time > 0.0f) ||
                float(field<std::int32_t>(player, bindings.offsets.player_tick_base)) *
                        *bindings.tick_interval <=
                    throw_time)
                return;
        }
    }
    angles = state.released_command.angles;
    command.movement.buttons = state.released_command.movement.buttons & ~0x801u;
    command.movement.side_move = state.released_command.movement.side_move;
    command.movement.forward_move = state.released_command.movement.forward_move;
    command.angles[0] = state.released_command.angles[0];
    command.angles[1] = state.released_command.angles[1];
    client::set_command_view_angles(bindings.view_angles, command.angles);
}

void GrenadeCommandRuntime::observer0(void* player, client::Command& command, InputBitmap input) {
    if (!release_enabled(input) || *bindings.observer_suppressed ||
        field<std::uint8_t>(player, bindings.offsets.player_life_state))
        return;
    auto* weapon = active_weapon(player);
    if (!weapon || !supported(field<std::uint16_t>(weapon, bindings.offsets.weapon_definition)) ||
        field<std::uint8_t>(weapon, bindings.offsets.grenade_pin_pulled))
        return;
    const auto throw_time = field<float>(weapon, bindings.offsets.grenade_throw_time);
    if (!(throw_time > 0.0f) || !release_is_recent() ||
        field<std::int32_t>(player, bindings.offsets.player_tick_base) >
            after_throw_tick(throw_time, *bindings.tick_interval))
        return;
    command.movement.buttons &= ~0x801u;
    command.angles = state.released_command.angles;
    client::set_command_view_angles(bindings.view_angles, command.angles);
    engine::invoke<void>(bindings.view_angles.prediction, 13, command.angles.data());
}

void GrenadeCommandRuntime::reassess_warnings(void* player) {
    if (!bindings.esp_config->proximity_warning.value)
        return;
    auto& live = bindings.scene.live;
    for (auto& record : bindings.warning_owner->records) {
        if (record.handle == 0xffffffffu || !live.engine().entity_for_handle(record.handle)) {
            record.handle = 0xffffffffu;
            continue;
        }
        live.assess_warning(record, player, *bindings.command_buttons);
    }
    bindings.warning_owner->command_buttons = *bindings.command_buttons;
}

void GrenadeCommandRuntime::update_remote_warnings(void* player, std::uintptr_t incoming) {
    bindings.scene.live.update_remote_warnings(*bindings.warning_owner, *bindings.esp_config,
                                               player, incoming);
}

void GrenadeCommandRuntime::process_automatic_release(client::CommandFrame& frame, void* player,
                                                      void* weapon, client::Command& command,
                                                      InputBitmap input) {
    if (!weapon || !supported(std::uint32_t(frame.weapon_id_d8)) || !release_enabled(input) ||
        !(command.movement.buttons & 0x801u) ||
        ((frame.secondary_flags & 1u) && (!frame.choked_15c || !(frame.secondary_flags & 2u))) ||
        !field<std::uint8_t>(weapon, bindings.offsets.grenade_pin_pulled))
        return;
    auto& scene = bindings.scene;
    auto& live = scene.live;
    const auto trace = live.trace();
    MovementTraceFlagGuard trace_guard(trace.trace_flag);
    auto eye = vector(frame.origin_10c);
    eye.z += frame.height_164;
    auto velocity = vector(frame.velocity_150);
    const auto prediction_time = *bindings.current_time;
    const auto prediction_limit = prediction_time + 0.10000000149011612f;
    if (frame.scalar_c0 > 0.0f) {
        const auto& context = scene.movement;
        MovementSimulation simulation{};
        initialize_live_movement_simulation(
            simulation, player, {frame.origin_10c[0], frame.origin_10c[1], frame.origin_10c[2]},
            {velocity.x, velocity.y, velocity.z},
            field<std::uint32_t>(player, bindings.offsets.player_flags), 0, context.capture);
        const auto maximum = std::numeric_limits<float>::max();
        MovementFeedback feedback{0, 0, 0, 0, 0, maximum, 0.0f, maximum};
        MovementFeedback saved{};
        //4338EBA0/EC88 test the same optional global434759B0 on save/restore.
        if (scene.release_movement) {
            saved = *scene.movement_feedback;
            feedback = *scene.movement_feedback;
        }
        auto number = command.number;
        auto time = prediction_time + *bindings.tick_interval;
        std::int32_t ordinal = 0;
        for (;;) {
            auto next = frame.previous_command;
            next.number = number = add(number, 1);
            //433A1975 returns immediately for the absent movement owner.
            if (scene.release_movement)
                synthesize_release_command(*scene.release_movement, player, weapon, next,
                                           simulation, feedback);
            simulate_live_movement_tick(simulation, live.movement(), context.environment, player,
                                        next);
            // Native COMISS/JBE also continues for an unordered clock value.
            if (time > prediction_limit || ((frame.secondary_flags & 2u) && ordinal == 0))
                break;
            ++ordinal;
            time += *bindings.tick_interval;
        }
        if (scene.release_movement)
            *scene.movement_feedback = saved;
        eye = {simulation.position.x, simulation.position.y,
               simulation.position.z + simulation.view_offset_z};
        velocity = {simulation.velocity.x, simulation.velocity.y, simulation.velocity.z};
    }
    auto strength = 0.5f;
    if (command.movement.buttons & 1u)
        strength = 1.0f;
    if (command.movement.buttons & 0x800u)
        strength -= 0.5f;
    const auto previous_strength = field<float>(weapon, bindings.offsets.grenade_strength);
    const auto change = strength - previous_strength;
    const auto step = *bindings.tick_interval * 1.2999999523162842f;
    if (change > step)
        strength = previous_strength + step;
    else if (-step > change)
        strength = previous_strength - step;
    const auto throw_velocity = engine::invoke<float>(weapon, 452);
    const auto compensate = field<std::uint8_t>(player, bindings.offsets.player_alternate_eye) &&
                            config.super_toss.value;
    std::array<float, 3> recoil{};
    if (compensate)
        recoil = client::capture_command_recoil(player, bindings.offsets.player_recoil,
                                                bindings.recoil_scale_convar);
    esp::GrenadeThrowInput throw_input{eye,
                                       velocity,
                                       vector(frame.previous_command.angles),
                                       vector(recoil),
                                       strength,
                                       throw_velocity,
                                       1.0f,
                                       compensate,
                                       identity(player)};
    throw_input.zero_slow_movement = false;
    const auto launch = esp::prepare_grenade_throw(throw_input, trace);
    esp::GrenadeState prediction{};
    prediction.owner_entity = identity(player);
    esp::simulate_grenade(prediction, static_cast<esp::GrenadeKind>(frame.weapon_id_d8),
                          launch.position, launch.velocity, 0.0f, 0.0f, live.environment(), trace);
    esp::finalize_grenade(prediction, trace);
    std::int32_t magnitude = 0;
    bool accepted = false;
    switch (prediction.kind) {
    case esp::GrenadeKind::explosive: {
        prediction.position.z += 1.0f;
        const auto damage = live.select_damage(prediction.position, config.minimum_damage);
        magnitude = truncate(damage.damage);
        accepted = damage.reached_threshold;
        break;
    }
    case esp::GrenadeKind::flash:
        prediction.position.z += 1.0f;
        magnitude =
            esp::evaluate_grenade_flash(prediction.position, identity(player), live.auxiliary())
                .percent;
        accepted = magnitude > 75;
        break;
    case esp::GrenadeKind::smoke:
        if (prediction.extinguished) {
            magnitude = 1;
            accepted = true;
        }
        break;
    case esp::GrenadeKind::molotov:
    case esp::GrenadeKind::incendiary:
        esp::evaluate_grenade_fire_distance(prediction, live.auxiliary(), magnitude, accepted);
        break;
    default:
        break;
    }
    if (magnitude > 0 && accepted) {
        command.movement.buttons &= ~0x801u;
        state.released_command = frame.previous_command;
        state.released_command.tick = *bindings.tick_count;
    }
}

void GrenadeCommandRuntime::process_throw_release(client::CommandFrame& frame, void* player,
                                                  void* weapon, client::Command& command,
                                                  InputBitmap input) {
    if (!weapon || frame.weapon_kind_124 != 9)
        return;
    if (*bindings.release_choke_enabled && (frame.secondary_flags & 1u) &&
        field<std::uint8_t>(weapon, bindings.offsets.grenade_pin_pulled)) {
        const auto* previous = previous_command(command);
        if (previous && (previous->movement.buttons & 0x801u) &&
            !(command.movement.buttons & 0x801u) && (frame.secondary_flags & 2u) &&
            frame.choked_15c) {
            state.release_command_number = add(command.number, 1);
            send_packet(frame) = 1;
            frame.choke_limit_fc = frame.choked_15c;
        }
    }
    if (command.number == state.release_command_number) {
        send_packet(frame) = 0;
        frame.secondary_flags |= 4u;
    } else if (command.number == add(state.release_command_number, 1)) {
        send_packet(frame) = 1;
        frame.choke_limit_fc = frame.choked_15c;
    }
    if (command.number != state.release_command_number) {
        if (command.number == add(state.release_command_number, 1) ||
            field<std::uint8_t>(weapon, bindings.offsets.grenade_pin_pulled))
            return;
        const auto time = field<float>(weapon, bindings.offsets.grenade_throw_time);
        if (!(time > 0.0f) ||
            float(field<std::int32_t>(player, bindings.offsets.player_tick_base)) *
                    *bindings.tick_interval <=
                time)
            return;
    }
    frame.secondary_flags |= 8u;
    if (release_enabled(input) && supported(std::uint32_t(frame.weapon_id_d8)) &&
        release_is_recent()) {
        command.angles[0] = state.released_command.angles[0];
        command.angles[1] = state.released_command.angles[1];
    }
    if (config.super_toss.value) {
        const auto recoil = client::capture_command_recoil(player, bindings.offsets.player_recoil,
                                                           bindings.recoil_scale_convar);
        command.angles[0] -= recoil[0];
        command.angles[1] -= recoil[1];
        command.angles[2] -= recoil[2];
        const auto velocity = field<esp::Vector3>(player, bindings.offsets.player_velocity);
        if (squared(velocity) > 4.0f) {
            auto angles = vector(command.angles);
            const auto speed = throw_speed(weapon);
            const auto strength =
                clamp(field<float>(weapon, bindings.offsets.grenade_strength), 0.0f, 1.0f);
            esp::compensate_grenade_movement(
                velocity, (strength * 0.699999988079071f + 0.30000001192092896f) * speed, angles);
            command.angles = {angles.x, angles.y, angles.z};
        }
    }
    *bindings.command_buttons = (*bindings.command_buttons & 0xfffffbffu) | 0x200u;
}

void GrenadeCommandRuntime::phase4(client::CommandFrame& frame, void* player,
                                   client::Command& command, InputBitmap input) {
    state.phase4_eligible = (frame.secondary_flags & 1u) != 0;
    auto* weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
    if (*bindings.release_choke_enabled && weapon && frame.weapon_kind_124 == 9 &&
        (frame.secondary_flags & 1u) &&
        field<std::uint8_t>(weapon, bindings.offsets.grenade_pin_pulled)) {
        const auto* previous = previous_command(command);
        if (previous && (previous->movement.buttons & 0x801u) &&
            !(command.movement.buttons & 0x801u) &&
            (!frame.choked_15c || !(frame.secondary_flags & 2u))) {
            command.movement.buttons =
                (command.movement.buttons & ~0x801u) | (previous->movement.buttons & 0x801u);
            if (send_packet(frame))
                send_packet(frame) = 0;
        }
    }
    reassess_warnings(player);
    process_automatic_release(frame, player, weapon, command, input);
    process_throw_release(frame, player, weapon, command, input);
}

void GrenadeCommandRuntime::phase7(const client::CommandFrame& frame, void* player) {
    if (*bindings.command_buttons & 0x80u) {
        const auto maximum = engine::invoke<float>(player, 276);
        const auto scalar = field<float>(player, bindings.offsets.player_maximum_speed);
        const auto magnitude = minimum_number(scalar, maximum) * 0.3400000035762787f - 1.0f;
        const auto value = state.captured_movement();
        const auto scale =
            magnitude / (std::sqrt(squared(value)) + std::numeric_limits<float>::epsilon());
        movement(state.captured_command, {value.x * scale, scale * value.y, scale * value.z});
    }
    if (*bindings.command_buttons & 1u)
        movement(state.captured_command, {});
    state.live_velocity = field<esp::Vector3>(player, bindings.offsets.player_velocity);
    const auto origin = vector(frame.origin_10c);
    const auto delta = subtract(state.previous_origin, origin);
    if ((delta.y * delta.y + delta.x * delta.x) + delta.z * delta.z > 10000.0f)
        state.trajectory_velocity = state.live_velocity;
    state.previous_origin = origin;
}

} // namespace skeet::misc
