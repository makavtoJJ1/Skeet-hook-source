#include "skeet/lagcomp/history_runtime.hpp"
#include "skeet/lagcomp/extrapolated_record.hpp"
#include "skeet/lagcomp/packet_capture.hpp"
#include "skeet/lagcomp/packet_direction.hpp"
#include "skeet/lagcomp/resolver_detection.hpp"
#include "skeet/lagcomp/receive_timing.hpp"
#include "skeet/anti_aim/body_yaw.hpp"
#include "skeet/resolver_runtime/build_validation.hpp"
#include "skeet/animation/record_pose_scope.hpp"
#include "skeet/menu/misc_menu.hpp"
#include <bit>
#include <cstring>
#include <xmmintrin.h>

namespace skeet::lagcomp {
namespace {
HistoryRecordingRuntime* installed_runtime;
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
std::int32_t subtract(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
std::uint32_t convar_integer(const void* object) noexcept {
    for (;;) {
        const auto* parent = read<const void*>(object, 0x1C);
        if (parent == object)
            break;
        object = parent;
    }
    return read<std::uint32_t>(object, 0x30) ^
           std::uint32_t(reinterpret_cast<std::uintptr_t>(object));
}
void clear_packets(client::PlayerState& owner) noexcept {
    write(&owner.native, 0x234, std::uint32_t(0));
    owner.native.field_238 = 0;
}
void set_absolute_origin(void* player, const Vector& value, const EntityStateRuntime& entity) {
    if (!entity.set_absolute_origin)
        return;
    const auto offset = entity.offsets.transform_guard_49d93e62 - 0x14u;
    const auto saved = read<std::uint32_t>(player, offset);
    write(player, offset, UINT32_MAX);
    entity.set_absolute_origin(player, value);
    write(player, offset, saved);
}
float maximum_yaw(const void* state) noexcept {
    if (!state)
        return 58.0f;
    const anti_aim::AnimationYawInput input{read<float>(state, 0xA4),  read<float>(state, 0xF8),
                                            read<float>(state, 0xFC),  read<float>(state, 0x11C),
                                            read<float>(state, 0x334), read<float>(state, 0x338)};
    float minimum, maximum;
    anti_aim::animation_yaw_limits(input, minimum, maximum);
    return maximum;
}
struct LastLayerRestore {
    std::byte* target = nullptr;
    std::uint32_t saved;
    LastLayerRestore(void* player, const EntityNetvars& offsets) {
        const auto* descriptor = static_cast<const std::byte*>(player) + offsets.animation_layers;
        if (read<std::int32_t>(descriptor, 0xC) > 12) {
            target = read<std::byte*>(descriptor, 0) + 0x2C0;
            saved = read<std::uint32_t>(target, 0);
            write(target, 0, std::uint32_t(0));
        }
    }
    ~LastLayerRestore() {
        if (target)
            write(target, 0, saved);
    }
};
void publish_live_fallback(client::PlayerState& owner, bool captured,
                           const HistoryRecordingBindings& bindings) {
    if (!owner.history.count ||
        owner.fallback.header.receive_tick <= owner.history.at(0).header.receive_tick)
        return;
    const auto& entity = bindings.entity;
    const auto receive_tick = sample_receive_tick(entity.engine, entity.receive_interval);
    // Native1677E reloads the current ring head after engine14;167AD reads
    // the live maximum-unlag word. No pre-call recording-state copy publishes
    // over changes made at that genuine engine boundary.
    auto& newest = owner.history.at(0);
    if (!captured) {
        const float seconds =
            float(receive_tick) * entity.interval - float(bindings.maximum_unlag_ms) * 0.001f;
        const auto truncated = _mm_cvtt_ss2si(_mm_set_ss(seconds));
        if (float(truncated) > float(newest.header.simulation_tick) * entity.interval) {
            std::memcpy(&newest, &owner.fallback, sizeof(LagRecord));
            owner.rebuilt_animation_by_slot[owner.history.first & 31u] = owner.fallback_animation_rebuilt;
            owner.jitter.remember(owner.jitter.fallback);
            owner.fallback.header.receive_tick = -1;
            return;
        }
    }
    owner.native.flags |= 0x100u;
}
}
void bind_history_recording_runtime(HistoryRecordingRuntime& runtime) noexcept {
    installed_runtime = &runtime;
}
#if defined(_M_IX86) || defined(__i386__)
void __fastcall execute_history_record_job(void* item) {
#else
void execute_history_record_job(void* item) {
#endif
    installed_runtime->execute_job(*static_cast<HistoryJobItem*>(item));
}
void HistoryRecordingRuntime::execute_job(const HistoryJobItem& item) {
    auto* player = reinterpret_cast<void*>(std::uintptr_t(item.player));
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    auto& owner = *bindings_.players.get(index);
    auto& record = *reinterpret_cast<LagRecord*>(std::uintptr_t(item.record));
    build_record(owner, player, item.speed_fraction, record, true);
}
bool HistoryRecordingRuntime::hypotheses_enabled(void* player) const {
    const auto& b = bindings_;
    if (!*b.configuration.recording_enabled)
        return false;
    if (convar_integer(b.hypotheses.game_type) == 6u) {
        const auto handle = std::uint32_t(b.hypotheses.selected_handle);
        if (handle == UINT32_MAX ||
            engine::invoke<void*>(b.entity.entity_list, 4, handle) != player)
            return false;
    }
    if (*b.configuration.resolver_enabled)
        return true;
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    const auto safe_point = read<std::uint32_t>(b.globals.adjustments.settings_base(),
                                                std::size_t(index) * 0x20 + 0x14);
    if (safe_point)
        return safe_point == 2u;
    if (hotkey_active(b.hypotheses.hotkey, b.hypotheses.keys))
        return true;
    if (std::uint32_t(*b.configuration.target_hitboxes) & b.hypotheses.active_mode_mask)
        return true;
    return b.hypotheses.physical_override && (b.hypotheses.input_flags & 8u);
}
void HistoryRecordingRuntime::build_live_hypotheses(void* player, client::PlayerState& owner,
                                                    float fraction, LagRecord& record) {
    const auto& b = bindings_;
    auto input = capture_record_hypothesis_input(player, owner, record, b.entity,
                                                  b.configuration.reworked());
    input.speed_fraction = fraction;
    resolver_runtime::build_hypotheses(record, input, player, b.bones, b.configuration.reworked());
}
void HistoryRecordingRuntime::build_record(client::PlayerState& owner, void* player,
                                           const float& fraction, LagRecord& record,
                                           bool deferred) {
    const auto& b = bindings_;
    if (deferred) {
        const auto offset = b.bones.fields.entity_flags_83a6cf2b;
        write(player, offset, read<std::uint32_t>(player, offset) | 8u);
        write(player, b.bones.fields.ik_mask_01d0bacc, UINT32_MAX);
    } else
        engine::invoke<const Vector*>(player, 11);
    const bool rebuild = b.configuration.reworked();
    if (rebuild)
        record.header.flags &= ~(4u | 0x80u);
    std::int32_t count;
    if (rebuild) {
        std::array<Matrix3x4, animation::maximum_bones> scratch;
        count = animation::build_captured_hitbox_matrices(player, record, b.bones, scratch,
                                                         record.matrices, 0x80300u);
    } else
        count = b.bones.build_resolver_hitbox_matrices(player, record.header.animation_yaw,
                                                       record.matrices);
    const bool complete = count > 0 && (!rebuild ||
                          resolver_runtime::complete_hitbox_matrices(count, record.matrices));
    if (complete)
        record.header.flags |= 4u;
    if (&record == &owner.fallback) {
        owner.fallback_animation_rebuilt.matrices = rebuild && complete;
    } else {
        for (std::uint32_t age = 0; age < owner.history.count; ++age) {
            const auto slot = (owner.history.first + age) & 31u;
            if (owner.history.records[slot] == &record) {
                owner.rebuilt_animation_by_slot[slot].matrices = rebuild && complete;
                break;
            }
        }
    }
    if (hypotheses_enabled(player))
        build_live_hypotheses(player, owner, fraction, record);
    if (deferred) {
        const auto offset = b.bones.fields.entity_flags_83a6cf2b;
        write(player, offset, read<std::uint32_t>(player, offset) & ~8u);
    }
}
void HistoryRecordingRuntime::publish_record(client::PlayerState& owner, void* player,
                                             const CachedHistoryFrame& frame, bool active,
                                             bool enemy, bool forced_yaw, bool changed, std::uint32_t flags,
                                             float fraction, bool animation_rebuilt, HistoryJobQueue& queue) {
    const auto& b = bindings_;
    auto state = owner.recording_state();
    const auto slot =
        prepare_history_capture(state, b.records, active, !enemy, frame.ticks.last, changed);
    owner.recording_state(state);
    if (!active || !enemy)
        return;
    const bool fallback = slot.kind == HistoryCaptureKind::fallback;
    auto* record = fallback ? &owner.fallback : slot.record;
    if (record) {
        LastLayerRestore layer(player, b.entity.offsets);
        b.entity.capture_record(*record, player, b.players);
        if (fallback)
            owner.fallback_animation_rebuilt = {false, animation_rebuilt};
        else
            owner.rebuilt_animation_by_slot[owner.history.first & 31u] = {false, animation_rebuilt};
        if (fallback)
            owner.jitter.fallback = {};
        if (b.configuration.jitter_fix || forced_yaw) {
            auto evidence = owner.jitter.current;
            if (evidence.tick != record->header.simulation_tick)
                evidence = {};
            evidence.tick = record->header.simulation_tick;
            evidence.receive_tick = record->header.receive_tick;
            evidence.observed_yaw = std::bit_cast<float>(record->header.player_history_1ac);
            if (forced_yaw)
                evidence.decision = resolver_runtime::JitterDecision::forced_yaw;
            if ((flags & 0x20u) && record->header.resolver_state == 2 && evidence.applied())
                evidence.decision = resolver_runtime::JitterDecision::movement;
            if (evidence.applied() && evidence.estimate.attempt != record->header.resolver_mode)
                evidence.decision = resolver_runtime::JitterDecision::not_applied;
            if (fallback)
                owner.jitter.fallback = evidence;
            else
                owner.jitter.remember(evidence);
        }
        const auto* previous = owner.history.count > 1 ? &owner.history.at(1) : nullptr;
        publish_record_flags(*record, previous, *b.configuration.recording_enabled, flags,
                             b.globals.shot_angles_active);
        if (!append_history_job(queue, b.globals.settings_menu.multithreading, player, fraction,
                                *record))
            build_record(owner, player, fraction, *record, false);
    }
    publish_live_fallback(owner, fallback, b);
    if (b.configuration.jitter_enabled() && !forced_yaw)
        resolver_runtime::correct_jitter_history(owner.history, owner.jitter);
}
void HistoryRecordingRuntime::record_players(void* local) {
    // Native4340C971 allocates both direction regions on this invocation's
    // stack, reusing them across its player loop, never as process state.
    HistoryRecordingScratch scratch;
    record_players(local, scratch);
}
void HistoryRecordingRuntime::record_players(void* local, HistoryRecordingScratch& scratch) {
    const auto& b = bindings_;
    const auto& entity = b.entity;
    auto* const player_table = b.globals.adjustments.settings_base();
    if (b.globals.animation_reset)
        *b.globals.animation_reset = 0;
    b.configuration.prediction_available = std::uint8_t(history_prediction_available(b.activation));
    bool active = false;
    if (!engine::invoke<bool>(entity.engine, 82) && !engine::invoke<bool>(entity.engine, 93))
        active = *b.activation.recording_enabled || history_configuration_active(b.activation);
    const auto local_index = engine::invoke<std::int32_t>(static_cast<std::byte*>(local) + 8, 10);
    const auto maximum = b.globals.maximum_players;
    HistoryJobQueue queue;
    initialize_history_job_queue(queue);
    for (std::int32_t index = 1; index <= maximum;
         index = std::bit_cast<std::int32_t>(std::uint32_t(index) + 1u)) {
        auto& owner = *b.players.get(std::uint32_t(index));
        auto* player = engine::invoke<void*>(entity.entity_list, 3, index);
        prune_history_records(owner.history, b.records, b.globals.history_clock, entity.interval);
        owner.publish_history();
        auto timeline = history_timeline(owner);
        prune_packet_samples(timeline, b.globals.history_clock, entity.interval);
        owner.native.field_238 = timeline.packet_count;
        std::array<std::byte, 0x158> information;
        if (!player || !engine::invoke<bool>(player, 158) ||
            !engine::invoke<bool>(b.configuration.engine, 8, index, information.data()) ||
            std::to_integer<unsigned>(information[0x13D]) ||
            engine::invoke<bool>(static_cast<std::byte*>(player) + 8, 9)) {
            clear_packets(owner);
            continue;
        }
        auto frame = prepare_cached_history_frame(owner, b.records, player, index == local_index,
                                                  entity, b.prelude);
        if (!frame.ready)
            continue;
        const auto max_yaw = maximum_yaw(frame.animation_state);
        const auto old_index = owner.native.resolver_index;
        auto receive = sample_receive_tick(entity.engine, entity.receive_interval);
        std::uint32_t flags = read<std::int32_t>(&owner.native, 0x1A4) == receive ||
                              (read<std::uint32_t>(player, b.fields.effects_38ca5228) & 8u) ||
                              read<std::uint8_t>(player, b.fields.field_043fa575) !=
                                  read<std::uint8_t>(player, b.fields.field_334b460c);
        receive = sample_receive_tick(entity.engine, entity.receive_interval);
        if (read<std::int32_t>(&owner.native, 0x1DC) == receive)
            flags |= 8u;
        std::int32_t shot = -1;
        bool accepted_shot = false;
        if (frame.ticks.count > 0) {
            owner.native.flags =
                (owner.native.flags & ~0x10u) | ((owner.native.flags >> 1) & 0x10u);
            const auto handle = read<std::uint32_t>(player, entity.offsets.weapon_handle);
            auto* weapon = handle == UINT32_MAX
                               ? nullptr
                               : engine::invoke<void*>(entity.entity_list, 4, handle);
            if (weapon) {
                const auto tick = time_to_tick(
                    read<float>(weapon, b.fields.weapon_shot_time_0176a381), entity.interval);
                owner.native.flags = (owner.native.flags & ~1u) |
                                     std::uint32_t(tick == subtract(frame.ticks.last, 1) ||
                                                   tick == frame.ticks.last);
                if (history_weapon_is_firearm(
                        read<std::uint16_t>(weapon, b.prelude.weapon_definition_5c9f0fbb)) &&
                    tick > frame.ticks.first && tick <= frame.ticks.last) {
                    owner.native.flags |= 0x20u;
                    flags |= 4u;
                    shot = tick;
                    accepted_shot = true;
                }
            }
            if (!accepted_shot)
                owner.native.flags &= ~0x20u;
        }
        if (read<std::int32_t>(&owner.native, 0x1D4) > 1 && frame.ticks.count) {
            const auto height = engine::invoke<const Vector*>(player, 77)->at(2);
            if (read<float>(&owner.native, 0x11C) != height) {
                write(&owner.native, 0x12C,
                      read<float>(player, entity.offsets.origin + 8) +
                          read<float>(&owner.native, 0x11C));
                write(&owner.native, 0x148,
                      read<float>(player, b.prelude.previous_time_1599541f) + entity.interval);
            }
        }
        if (flags & 1u)
            clear_packets(owner);
        //4340D50C supplies this invocation's saved local player to4339EF8C.
        const rage::TargetBindings targeting{local,
                                             entity.entity_list,
                                             b.targeting.game_type,
                                             b.targeting.teammates_are_enemies,
                                             b.globals.adjustments,
                                             b.targeting.offsets};
        const bool enemy = rage::target_is_enemy(targeting, player);
        const auto current_layers = read<std::array<AnimationLayerState, 13>>(&owner.native, 0x2FC);
        const auto previous_layers =
            read<std::array<AnimationLayerState, 13>>(&owner.native, 0x468);
        const bool changed = history_animation_layers_changed(current_layers, previous_layers);
        const auto animation_enabled =
            read<std::uint8_t>(player, b.fields.animation_enabled_8481ea04);
        const auto cached_enabled = read<std::uint8_t>(&owner.native, 0x17C);
        const auto previous_angles = read<Vector>(player, entity.offsets.eye_angles);
        const auto table_offset = std::uint32_t(index) << 5;
        owner.jitter.current = {};
        if (enemy && b.configuration.jitter_fix) {
            auto& evidence = owner.jitter.current;
            evidence.tick = frame.ticks.last;
            evidence.observed_yaw = owner.native.reference_yaw;
            if (!b.configuration.jitter_enabled())
                evidence.decision = resolver_runtime::JitterDecision::correction_disabled;
        }
        if (enemy && frame.ticks.count) {
            if (owner.history.count) {
                const auto tick = time_to_tick(read<float>(&owner.native, 0x110), entity.interval);
                if (tick < subtract(read<std::int32_t>(&owner.native, 0x168), 1) &&
                    tick < owner.history.at(0).header.simulation_tick) {
                    owner.native.flags |= 0x200u;
                    write(&owner.native, 0x1D8,
                          sample_receive_tick(entity.engine, entity.receive_interval));
                }
            }
            ResolverDetectionState detection{bool(player_table[table_offset + 6]),
                                             read<float>(&owner.native, 0x1C0),
                                             read<float>(&owner.native, 0x178),
                                             read<std::uint16_t>(&owner.native, 0xA6),
                                             {}};
            for (unsigned mode = 0; mode < 4; ++mode)
                detection.selection.attempt_by_mode[mode] =
                    owner.native.resolver_slots[mode].attempt;
            detection.selection.status = owner.native.resolver_status;
            const auto packets = history_timeline(owner);
            update_resolver_detection(
                detection, {frame.ticks.count, entity.interval, read<float>(&owner.native, 0x118),
                            owner.native.reference_yaw,
                            read<float>(player, entity.offsets.eye_angles), packets});
            player_table[table_offset + 6] = std::uint8_t(detection.enabled);
            write(&owner.native, 0x1C0, detection.yaw_duration_1c0);
            write(&owner.native, 0x178, detection.pitch_duration_178);
            for (unsigned mode = 0; mode < 4; ++mode)
                owner.native.resolver_slots[mode].attempt =
                    detection.selection.attempt_by_mode[mode];
            owner.native.resolver_status = detection.selection.status;
            if (detection.enabled)
                update_history_environment(
                    owner, player, scratch.environment,
                    {b.globals.local_origin, frame.ticks.count,
                     std::uint32_t(reinterpret_cast<std::uintptr_t>(&owner.native) + 0x5D4),
                     b.globals.current_time},
                    b.environment);
        }
        if (enemy)
            publish_history_eye_angles(owner, player, frame.ticks.count != 0,
                                       {bool(player_table[table_offset + 4]),
                                        read<std::int32_t>(player_table, table_offset + 0xC)},
                                       entity);
        capture_history_packet(owner, changed, player, entity,
                               b.velocity.reconstruction.fields.player_flags);
        if (index == local_index) {
            clear_history_records(owner.history, b.records);
            owner.publish_history();
            continue;
        }
        float fraction = frame.animation_speed_fraction;
        bool animation_rebuilt = false;
        if (animation_enabled != cached_enabled && changed) {
            const auto saved_origin = *engine::invoke<const Vector*>(player, 10);
            set_absolute_origin(player, read<Vector>(player, entity.offsets.origin), entity);
            update_history_yaw_side(owner, enemy, flags, b.globals.transition_enabled,
                                    b.yaw_side_fmod);
            const auto& publication_source = b.publication;
            const HistoryAnimationPublishBindings publication{
                entity,
                publication_source.fields,
                entity.engine,
                publication_source.local_time_entity,
                &b.globals.current_time,
                entity.receive_interval,
                b.globals.animation_reset,
                b.configuration,
                player_table,
                table_offset,
                publication_source.allow_zero_sequence,
                publication_source.movement_exponent,
                publication_source.candidate_attempts,
                publication_source.movement,
                publication_source.power,
                publication_source.arccosine,
                publication_source.atan2};
            if (!frame.animation_state) {
                const auto previous =
                    read<std::uint8_t>(player, b.fields.animation_enabled_8481ea04);
                write(player, b.fields.animation_enabled_8481ea04,
                      read<std::uint8_t>(&owner.native, 0x17C));
                engine::invoke<void>(player, 224);
                if (b.globals.animation_reset)
                    *b.globals.animation_reset = 0;
                write(player, b.fields.animation_enabled_8481ea04, previous);
                publish_history_animation_origin(owner, player, saved_origin, publication);
            } else {
                const HistoryAnimationPublishSaved saved{
                    &b.globals.animation_time, b.globals.animation_time, &b.globals.frame_time,
                    b.globals.frame_time, saved_origin};
                b.globals.frame_time = entity.interval;
                float layer_weight = read<float>(&owner.native, 0x394);
                if (enemy) {
                    update_history_packet_direction(owner, flags, frame.ticks.last,
                                                    entity.interval);
                    const auto& p = b.velocity.reconstruction;
                    const HistoryVelocityReconstructionBindings physics{b.players,
                                                                        p.trace,
                                                                        entity.entity_list,
                                                                        entity.interval,
                                                                        p.fields,
                                                                        p.cvars,
                                                                        p.classify,
                                                                        p.sequence_name,
                                                                        p.compare_sequence_name,
                                                                        p.jump_sequence_name,
                                                                        p.atan2,
                                                                        p.sine,
                                                                        p.cosine,
                                                                        p.trace_flag,
                                                                        flags};
                    capture_history_velocity(owner, player,
                                             {entity, physics, b.velocity.maximum_velocity_convar,
                                              b.velocity.landing_limit_convar});
                    const auto seed = prepare_history_animation(
                        owner, player, frame.animation_state, frame.ticks.count, flags, entity,
                        p.fields.player_flags, b.configuration.reworked());
                    const auto& resolver_source = b.resolver;
                    const HistoryResolverUpdateBindings resolver{
                        entity,
                        b.configuration,
                        player_table,
                        b.hypotheses.selected_handle,
                        resolver_source.local_time_entity,
                        b.globals.current_time,
                        b.globals.animation_time,
                        b.globals.animation_frame,
                        b.globals.shot_angles_active,
                        b.globals.animation_reset,
                        b.globals.local_origin,
                        resolver_source.pending_shots,
                        resolver_source.fields,
                        resolver_source.recoil_scale_convar,
                        physics,
                        resolver_source.animation,
                        resolver_source.environment};
                    const auto result = update_history_resolver(
                        owner, player, frame.animation_state, seed, frame.ticks, shot, flags,
                        {old_index, max_yaw, fraction, table_offset}, scratch.resolver, resolver);
                    layer_weight = result.animation_publish_layer_weight;
                    fraction = result.speed_fraction;
                    animation_rebuilt = result.animation_rebuilt;
                }
                publish_history_animation(owner, player, frame.animation_state, frame.ticks.last,
                                          frame.ticks.count, fraction, layer_weight, flags, saved,
                                          publication);
            }
        }
        publish_record(owner, player, frame, active, enemy, player_table[table_offset + 5] != 0,
                       changed, flags, fraction, animation_rebuilt, queue);
        publish_history_final_angles(player, previous_angles, entity,
                                     b.fields.angle_interpolation_fac21eaf);
    }
    dispatch_history_jobs(queue, b.jobs);
}
} // namespace skeet::lagcomp
