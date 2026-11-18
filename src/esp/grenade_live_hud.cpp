#include "grenade_live_detail.hpp"
#include "detail.hpp"
#include "skeet/misc/grenade_command.hpp"
#include "skeet/client/command_recapture.hpp"
#include "skeet/render/backend.hpp"

namespace skeet::esp {
using namespace grenade_live_detail;
void GrenadeLiveScene::Storage::draw_warnings(ui::RenderQueue& queue,
                                              const GrenadeEspConfig& config,
                                              GrenadeWarningOwner& warnings) {
    if (!config.proximity_warning.value)
        return;
    const auto& b = bindings;
    const auto& o = raw.bindings().offsets;
    for (auto& record : warnings.records) {
        auto* local = b.local_player;
        if (!local || field<std::uint8_t>(local, offsets.life_state))
            continue;
        if (field<std::uint8_t>(local, offsets.movement_type) == 10) {
            const auto mode = field<std::int32_t>(local, offsets.observer_mode);
            if (mode == 4 || mode == 5)
                continue;
        }
        auto* entity = record.handle == 0xffffffffu ? nullptr : raw.by_handle(record.handle);
        if (!entity || (field<std::uint32_t>(entity, o.effects) & 0x20u)) {
            record.handle = 0xffffffffu;
            continue;
        }
        if ((record.type < 2 && field<std::int32_t>(entity, o.explosion_effect_tick) != 0) ||
            record.suppressed)
            continue;
        float distance;
        const auto local_origin = raw.live_origin(local);
        if (record.type == 2) {
            auto* inferno = record.handle == 0xffffffffu ? nullptr : raw.by_handle(record.handle);
            if (!inferno)
                continue;
            const auto center = field<Vector3>(inferno, o.origin);
            const auto count = field<std::int32_t>(inferno, o.fire_count);
            distance = std::sqrt(squared(difference(local_origin, center)));
            for (std::int32_t cell = 0; cell < count; ++cell) {
                if (!field<std::uint8_t>(inferno, o.fire_active + cell))
                    continue;
                const Vector3 point{
                    float(field<std::int32_t>(inferno, o.fire_x + cell * 4)) + center.x,
                    float(field<std::int32_t>(inferno, o.fire_y + cell * 4)) + center.y,
                    float(field<std::int32_t>(inferno, o.fire_z + cell * 4)) + center.z};
                const auto next = std::sqrt(squared(difference(point, local_origin)));
                distance = _mm_cvtss_f32(_mm_min_ss(_mm_set_ss(next), _mm_set_ss(distance)));
            }
        } else
            distance = std::sqrt(squared(difference(record.position, local_origin)));
        const GrenadeWarningLiveFrame frame{local,           b.renderer,
                                            b.tick_count,    b.queries.environment().tick_interval,
                                            b.interpolation, b.explosive_texture,
                                            b.fire_texture};
        draw_grenade_warning(queue, b.projection, record, distance, frame);
    }
}
void GrenadeLiveScene::Storage::draw(ui::RenderQueue& queue,
                                     const misc::GrenadePredictionConfig& settings,
                                     const GrenadeEspConfig& config, GrenadeTrajectoryOwner& visual,
                                     misc::GrenadeCommandState& command,
                                     GrenadeWarningOwner& warnings) {
    auto& smooth = command.trajectory_velocity;
    const auto target = command.live_velocity;
    if (settings.super_toss.value)
        smooth = target;
    else {
        smooth.x += (target.x - smooth.x) * 0.2f;
        smooth.y += (target.y - smooth.y) * 0.2f;
        smooth.z += (target.z - smooth.z) * 0.2f;
    }
    const auto& b = bindings;
    const auto& o = raw.bindings().offsets;
    if (engine::invoke<bool>(b.engine_client, 82) || engine::invoke<bool>(b.engine_client, 93) ||
        game_flag(offsets.game_player_override)) {
        warnings.records.clear();
        return;
    }
    auto trajectory = [&] {
        if (!config.trajectory.value)
            return;
        auto* selected = b.local_player;
        if (selected && field<std::uint8_t>(selected, offsets.life_state) &&
            field<std::uint8_t>(selected, offsets.movement_type) == 10) {
            const auto mode = field<std::int32_t>(selected, offsets.observer_mode);
            if (mode == 4 || mode == 5) {
                const auto handle = field<std::uint32_t>(selected, offsets.observer_target);
                if (handle != 0xffffffffu) {
                    auto* observed = raw.by_handle(handle);
                    if (observed && raw.player(observed))
                        selected = observed;
                }
            }
        }
        if (!selected || field<std::uint8_t>(selected, offsets.movement_type) == 10 ||
            field<std::uint8_t>(selected, offsets.life_state))
            return;
        const auto local = selected == b.local_player;
        if (!local && std::abs(field<float>(selected, offsets.view_angles)) > 50.0f)
            return;
        const auto handle = field<std::uint32_t>(selected, offsets.active_weapon);
        if (handle == 0xffffffffu)
            return;
        auto* weapon = raw.by_handle(handle);
        if (!weapon)
            return;
        const auto kind =
            static_cast<GrenadeKind>(field<std::int16_t>(weapon, offsets.weapon_definition));
        switch (kind) {
        case GrenadeKind::flash:
        case GrenadeKind::explosive:
        case GrenadeKind::smoke:
        case GrenadeKind::molotov:
        case GrenadeKind::decoy:
        case GrenadeKind::incendiary:
        case GrenadeKind::tactical:
        case GrenadeKind::diversion:
            break;
        default:
            return;
        }
        if (local) {
            if (field<std::int32_t>(weapon, offsets.weapon_state) == 190)
                return;
        } else if (field<float>(weapon, offsets.weapon_throw_time) > 0.0f)
            return;
        const auto strength = field<float>(weapon, offsets.weapon_strength);
        visual.path.points.clear();
        visual.path.bounces.clear();
        const auto cached = field<std::uint8_t>(selected, offsets.alternate_eye) != 0;
        Vector3 eye, velocity;
        float start_time = 0.0f;
        if (!cached) {
            velocity = field<Vector3>(selected, o.velocity);
            eye = raw.live_origin(selected);
            eye.z += grenade_player_view_height(field<float>(selected, offsets.duck_amount));
        } else {
            const auto origin = raw.live_origin(selected);
            const auto offset = field<Vector3>(selected, offsets.view_offset);
            eye = {origin.x + offset.x, origin.y + offset.y, origin.z + offset.z};
            // Use the same current predicted velocity as the release command.
            // Render-frame smoothing lags behind takeoff and landing.
            velocity = settings.super_toss.value ? field<Vector3>(selected, o.velocity)
                                                 : command.trajectory_velocity;
            const auto expected = std::bit_cast<std::int32_t>(std::uint32_t(b.command_count) +
                                                              std::uint32_t(b.last_outgoing));
            if ((velocity.x * velocity.x + velocity.y * velocity.y) + velocity.z * velocity.z >=
                    80.0f &&
                command.captured_command.number == expected) {
                const auto flags = field<std::uint32_t>(selected, offsets.player_flags);
                const auto position = raw.live_origin(selected);
                MovementSimulation simulation{};
                initialize_live_movement_simulation(
                    simulation, selected, {position.x, position.y, position.z},
                    {velocity.x, velocity.y, velocity.z}, flags, 0, b.movement_capture);
                auto count =
                    truncate(float(std::ceil(0.1f / b.queries.environment().tick_interval)));
                if (command.phase4_eligible)
                    count = 2;
                while (count-- > 0)
                    simulate_live_movement_tick(simulation, b.queries.movement(),
                                                b.movement_environment, selected,
                                                command.captured_command);
                eye = {simulation.position.x, simulation.position.y,
                       simulation.position.z + simulation.view_offset_z};
                velocity = {simulation.velocity.x, simulation.velocity.y, simulation.velocity.z};
            }
            start_time = b.queries.environment().tick_interval;
        }
        if (!settings.super_toss.value &&
            std::sqrt((velocity.x * velocity.x + velocity.y * velocity.y) +
                      velocity.z * velocity.z) < 10.0f)
            velocity = {};
        Vector3 angles;
        engine::invoke<void>(b.engine_client, 18, &angles);
        const auto speed = engine::invoke<float>(weapon, 452);
        const auto compensate =
            field<std::uint8_t>(selected, offsets.alternate_eye) && settings.super_toss.value;
        Vector3 recoil{};
        if (compensate) {
            const auto value =
                client::capture_command_recoil(selected, offsets.recoil, b.convars.recoil_scale);
            recoil = {value[0], value[1], value[2]};
        }
        GrenadeThrowInput input{
            eye,        velocity,           angles, recoil, strength, speed, 1.0f,
            compensate, identity(selected), false};
        const auto provider = raw.provider(b.trace_flag);
        visual.launch = prepare_grenade_throw(input, provider);
        visual.state.owner_entity = identity(selected);
        simulate_grenade(visual.state, kind, visual.launch.position, visual.launch.velocity,
                         start_time, 0.0f, b.queries.environment(), provider, &b.projection,
                         &visual.path);
        finalize_grenade(visual.state, provider);
        visual.drawing_position = visual.state.position;
        GrenadeScreenPoint endpoint;
        const auto visible = project_grenade_point(b.projection, visual.drawing_position, endpoint);
        GrenadeAuxiliaryResult presentation;
        if (kind == GrenadeKind::explosive || kind == GrenadeKind::flash)
            visual.state.position.z += 1.0f;
        if (kind == GrenadeKind::explosive) {
            const auto selected_damage =
                select_damage(visual.state.position, settings.minimum_damage);
            presentation.magnitude = truncate(selected_damage.damage);
            presentation.highlight = selected_damage.reached_threshold;
            if (presentation.magnitude > 0)
                presentation.annotation = detail::decimal(presentation.magnitude) + u" dmg";
        } else {
            presentation = evaluate_grenade_auxiliary(visual.state, auxiliary_context);
        }
        if (visible && presentation.magnitude > 0 && !presentation.annotation.empty())
            queue.text(truncate(endpoint.x - 4.0f), truncate(endpoint.y - 8.0f),
                       presentation.highlight ? 0xff15c28fu : 0xc8ffffffu, 0x18, 0,
                       presentation.annotation);
        if (presentation.draw_path)
            draw_active_grenade_path(queue, config, visual.path, visible ? &endpoint : nullptr,
                                     presentation.magnitude);
    };
    trajectory();
    draw_warnings(queue, config, warnings);
}
void GrenadeLiveScene::draw_prediction(ui::RenderQueue& queue,
                                       const misc::GrenadePredictionConfig& settings,
                                       const GrenadeEspConfig& config,
                                       GrenadeTrajectoryOwner& visual,
                                       misc::GrenadeCommandState& state,
                                       GrenadeWarningOwner& warnings) {
    storage_->draw(queue, settings, config, visual, state, warnings);
}
} // namespace skeet::esp
