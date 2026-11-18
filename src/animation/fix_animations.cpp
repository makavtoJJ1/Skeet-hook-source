#include "skeet/animation/fix_animations.hpp"
#include "skeet/animation/state_copy.hpp"
#include "skeet/client/entity_update_dispatch.hpp"
#include "skeet/effects/player_opacity.hpp"
#include "skeet/prediction/history.hpp"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <xmmintrin.h>

namespace skeet::animation {
namespace {
FixAnimationsRuntime* active_runtime;
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
template <class Result, class... Arguments>
Result original(void* object, std::size_t slot, Arguments... arguments) {
    const auto* metadata =
        static_cast<const engine::VtableHookMetadata*>((*static_cast<void***>(object))[-2]);
    return reinterpret_cast<engine::MemberFunction<Result, Arguments...>>(
        metadata->original_table[slot])(object, arguments...);
}
void original_six(void* object, std::size_t slot, const std::array<std::uintptr_t, 6>& a) {
    original<void>(object, slot, a[0], a[1], a[2], a[3], a[4], a[5]);
}
void* entity_from(void* receiver, std::size_t bias) noexcept {
    return receiver ? static_cast<std::byte*>(receiver) - bias : nullptr;
}
bool command_mode(const FixAnimationGlobals& globals) noexcept {
    return globals.command_mode || globals.alternate_command_mode;
}
bool interpolation_enabled(const void* variable) noexcept {
    for (;;) {
        const auto* parent = read<const void*>(variable, 0x1C);
        if (parent == variable)
            break;
        variable = parent;
    }
    const auto word = read<std::uint32_t>(variable, 0x2C) ^
                      std::uint32_t(reinterpret_cast<std::uintptr_t>(variable));
    return std::bit_cast<float>(word) != 0.0f;
}
std::int32_t subtract(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
std::int32_t truncate(float value) noexcept {
    return _mm_cvttss_si32(_mm_set_ss(value));
}
std::int32_t receive_tick(const FixAnimationsBindings& b) {
    return lagcomp::receive_time_to_tick(engine::invoke<float>(b.entity.engine, 14),
                                         b.globals.receive_interval);
}
// Complete4332FC35: max with explicit NaN and opposite-sign handling. This
// preserves the selected operand's signed zero and NaN payload.
float native_maximum(float first, float second) noexcept {
    if (std::isnan(first))
        return second;
    if (std::isnan(second))
        return first;
    if (std::signbit(first) != std::signbit(second))
        return std::signbit(first) ? second : first;
    return _mm_cvtss_f32(_mm_max_ss(_mm_set_ss(second), _mm_set_ss(first)));
}
void write_animation_frame(void* player, const FixAnimationsBindings& b) {
    write(player, b.fields.animation_frame_74019d87, b.globals.animation_frame);
}
void copy_absolute_angles_to_engine(void* player, const FixAnimationsBindings& b) {
    auto* result = engine::invoke<std::byte*>(player, 0x194 / 4);
    write(result, 0xC, read<RageVector>(player, b.bones.fields.angles_3b1f6ecc));
}
}

bool FixAnimationsRuntime::animation_owner_changed(void* player) const {
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    const auto* owner = bindings.players.get(index);
    return read<std::uint8_t>(&owner->native, 0x17C) !=
           read<std::uint8_t>(player, bindings.fields.client_animation_8481ea04);
}

void FixAnimationsRuntime::build_transformations(void* player,
                                                 std::array<std::uintptr_t, 6> arguments) {
    if (bindings.globals.enabled) {
        const auto offset = bindings.bones.fields.setup_flags_bb78dc43;
        write(player, offset, read<std::uint32_t>(player, offset) | 4u);
    }
    original_six(player, 190, arguments);
}
void FixAnimationsRuntime::standard_blending_rules(void* player, void* studio,
                                                   RageVector* positions, BoneQuaternion* rotations,
                                                   float time) {
    if (bindings.globals.enabled) {
        const auto offset = bindings.bones.fields.setup_flags_bb78dc43;
        write(player, offset, read<std::uint32_t>(player, offset) | 4u);
    }
    original<void>(player, 207, studio, positions, rotations, time);
}
void FixAnimationsRuntime::extra_bone_processing(void* player,
                                                 std::array<std::uintptr_t, 6> arguments) {
    if (!bindings.globals.enabled)
        original_six(player, 198, arguments);
}

void FixAnimationsRuntime::calculate_view(void* player, RageVector* origin, RageVector* angles,
                                          float* near_plane, float* far_plane,
                                          float* field_of_view) {
    const auto& b = bindings;
    void** animation_slot = nullptr;
    void* saved_animation = nullptr;
    if (b.globals.enabled && (command_mode(b.globals) || b.globals.clear_animation_state)) {
        animation_slot = reinterpret_cast<void**>(static_cast<std::byte*>(player) +
                                                  b.bones.fields.animation_state_3d52a1a7);
        saved_animation = *animation_slot;
        *animation_slot = nullptr;
    }
    void* height_slot = nullptr;
    std::uint32_t saved_height = 0;
    const auto mode = read<std::int32_t>(player, b.fields.observer_mode_2f4e97c4);
    if (mode == 1 || mode == 2) {
        write(player, b.fields.observer_target_3b704b41 + 4, 76.0f);
        height_slot = static_cast<std::byte*>(player) + b.fields.view_offset_c246315e + 8;
        saved_height = read<std::uint32_t>(height_slot, 0);
        write(height_slot, 0, std::uint32_t(0));
    }
    original<void>(player, 277, origin, angles, near_plane, far_plane, field_of_view);
    const auto current_mode = read<std::int32_t>(player, b.fields.observer_mode_2f4e97c4);
    if ((current_mode == 1 || current_mode == 2) && engine::invoke<bool>(b.input, 32, -1)) {
        write(b.input, 0xAC, (*angles)[0]);
        write(b.input, 0xB0, (*angles)[1]);
    }
    if (height_slot)
        write(height_slot, 0, saved_height);
    if (animation_slot)
        *animation_slot = saved_animation;
}

bool FixAnimationsRuntime::setup_bones(void* renderable, Matrix3x4* output, std::int32_t maximum,
                                       std::uint32_t mask, float time) {
    const auto& b = bindings;
    auto* player = entity_from(renderable, 4);
    if ((b.globals.command_flags & 0x20u) && !output && maximum == -1 && mask == 0x200u)
        return true;
    if (b.globals.enabled) {
        write_animation_frame(player, b);
        write(player, b.bones.fields.setup_flags_bb78dc43, 4u);
    }
    if (b.globals.invalidate_bones && (mask & 0x100u)) {
        const auto* state = read<const void*>(player, b.bones.fields.animation_state_3d52a1a7);
        if (state) {
            const auto offset = b.entity.offsets.field_0471e13c;
            const auto counter = read<std::int32_t>(player, offset);
            if (counter && read<std::int32_t>(state, 0x70) > counter)
                write(player, offset, std::uint32_t(0));
        }
    }
    struct SavedField {
        void* destination = nullptr;
        std::uint32_t word;
    };
    std::array<SavedField, 5> fields;
    auto save = [&](std::size_t index, void* destination, std::uint32_t value) {
        fields[index] = {destination, read<std::uint32_t>(destination, 0)};
        write(destination, 0, value);
    };
    void* transient_slot = nullptr;
    std::uint32_t saved_transient = 0;
    const auto number = b.globals.last_outgoing;
    const auto history =
        prediction::CommandHistoryView::from_owner(b.globals.command_history_owner);
    if (const auto* state = history.find(number)) {
        auto* collision = engine::invoke<void*>(player, 3);
        auto* maximum_bounds = engine::invoke<RageVector*>(collision, 2);
        save(0, &(*maximum_bounds)[2], read<std::uint32_t>(state, 0x84));
        save(1, static_cast<std::byte*>(player) + b.entity.offsets.field_432b1f5b,
             read<std::uint32_t>(state, 0x7C));
        save(2, static_cast<std::byte*>(player) + b.entity.offsets.field_0d560fac,
             read<std::uint32_t>(state, 0x80));
        save(3, static_cast<std::byte*>(player) + b.fields.view_offset_c246315e + 8,
             read<std::uint32_t>(state, 0x28));
        const float clock = float(subtract(read<std::int32_t>(state, 0), 1)) * b.globals.interval;
        save(4, &b.globals.current_time, std::bit_cast<std::uint32_t>(clock));
        transient_slot = static_cast<std::byte*>(player) + b.fields.transient_6e9852d6;
        saved_transient = read<std::uint32_t>(transient_slot, 0);
        write(transient_slot, 0, read<std::uint32_t>(state, 0x78));
    }
    const auto replacement = b.globals.setup_angles;
    auto* eye_slot = static_cast<std::byte*>(player) + b.fields.eye_angles_c1b66ee8 + 4;
    const auto saved_eye = read<RageVector>(eye_slot, 0);
    write(eye_slot, 0, replacement);
    const auto result = original<bool>(renderable, 13, output, maximum, mask, time);
    write(eye_slot, 0, saved_eye);
    if (transient_slot)
        write(transient_slot, 0, saved_transient);
    for (auto cursor = fields.rbegin(); cursor != fields.rend(); ++cursor)
        if (cursor->destination)
            write(cursor->destination, 0, cursor->word);
    return result;
}

void FixAnimationsRuntime::restore_animation_backup(void* player) {
    const auto& b = bindings;
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    auto** buckets = b.globals.backup_buckets;
    auto** bucket = buckets + index % b.globals.backup_bucket_count;
    auto* node = *bucket;
    while (node && node->entity_index != index)
        node = node->next;
    if (!node) {
        bucket = buckets + b.globals.backup_bucket_count;
        node = *bucket;
    }
    if (node == buckets[b.globals.backup_bucket_count])
        return;
    bool restore = false;
    if (b.globals.backup_mode == 2 && !read<std::uint8_t>(player, b.fields.life_state_9d920e35)) {
        const auto acknowledged = engine::invoke<std::int32_t>(b.entity.engine, 205);
        restore =
            node->acknowledged_command == acknowledged && node->frame == b.globals.prediction_frame;
    }
    if (restore) {
        if (auto* state = read<void*>(player, b.bones.fields.animation_state_3d52a1a7)) {
            const auto pointers = read<std::array<std::uint32_t, 4>>(state, 0x5C);
            std::memcpy(state, node->animation.data(), 0x348);
            write(state, 0x5C, pointers[0]);
            write(state, 0x68, pointers[3]);
            write(state, 0x64, pointers[2]);
            write(state, 0x60, pointers[1]);
        }
        std::memcpy(b.bones.pose_parameters(player).data(), node->poses.data(), sizeof node->poses);
        const auto current = read<RageVector>(player, b.bones.fields.angles_3b1f6ecc);
        if (node->angles[0] != current[0] || node->angles[1] != current[1] ||
            node->angles[2] != current[2]) {
            if (b.invalidate_physics)
                b.invalidate_physics(player, 2);
            write(player, b.bones.fields.angles_3b1f6ecc, node->angles);
        }
        engine::invoke<void>(player, 112);
        write(player, b.entity.offsets.field_0d560fac, node->collision_time);
    }
    if (*bucket == node)
        *bucket = node->next;
    else {
        auto* previous = *bucket;
        while (previous->next != node)
            previous = previous->next;
        previous->next = node->next;
    }
    release_animation_state_vector(node->animation.data(), b.allocator);
    std::uint32_t displacement;
    std::memcpy(&displacement, reinterpret_cast<std::byte*>(node) - 8, 4);
    b.memory.free(b.memory.process_heap, 0, reinterpret_cast<std::byte*>(node) - 16 - displacement);
    --b.globals.backup_count;
}

void FixAnimationsRuntime::networkable_pre_update(void* networkable, std::int32_t update) {
    const auto& b = bindings;
    auto* player = entity_from(networkable, 8);
    const auto index = engine::invoke<std::uint32_t>(networkable, 10);
    auto& owner = b.players.get(index)->native;
    client::capture_serialized_player_state(owner, player, b.data_update_fields);
    if (index == engine::invoke<std::uint32_t>(b.entity.engine, 12) &&
        read<std::uint8_t>(player, b.data_update_fields.local_serialized_d070684c))
        b.data_update_observers.capture_forward(player, update);
    // Original433E129A wraps the full433B7613 capture before engine slot6.
    original<void>(networkable, 6, update);
}

void FixAnimationsRuntime::networkable_view(void* networkable, std::int32_t update) {
    const auto& b = bindings;
    auto* player = entity_from(networkable, 8);
    const auto index = engine::invoke<std::uint32_t>(networkable, 10);
    auto& owner = b.players.get(index)->native;
    if (!update)
        restore_animation_backup(player);
    write(&owner, 0x118, read<std::uint32_t>(player, b.fields.body_yaw_a2a8247f));
    bool changed_view = false;
    RageVector saved_view;
    if (!engine::invoke<bool>(b.entity.engine, 82) && !engine::invoke<bool>(b.entity.engine, 93) &&
        interpolation_enabled(b.interpolation_cvar)) {
        b.data_update_observers.finish_reverse(player, update);
        const auto current = read<RageVector>(player, b.bones.fields.angles_3b1f6ecc);
        const auto& desired = b.globals.view_angles;
        if (current[0] != desired[0] || current[1] != desired[1] || current[2] != desired[2]) {
            if (b.invalidate_physics)
                b.invalidate_physics(player, 2);
            write(player, b.bones.fields.angles_3b1f6ecc, b.globals.view_angles);
        }
        engine::invoke<void>(b.entity.engine, 18, &saved_view);
        engine::invoke<void>(b.entity.engine, 19, &b.globals.view_angles);
        changed_view = true;
        if (b.globals.enabled) {
            write(player, b.bones.fields.setup_flags_bb78dc43, 4u);
            copy_absolute_angles_to_engine(player, b);
        }
    }
    original<void>(networkable, 7, update);
    if (changed_view)
        engine::invoke<void>(b.entity.engine, 19, &saved_view);
}

void FixAnimationsRuntime::thinkable_observers(void* thinkable) {
    const auto& b = bindings;
    auto* player = entity_from(thinkable, 0xC);
    b.globals.thinking = 1;
    if (b.globals.engine_timestamp) {
        const auto stamp = b.globals.timestamp();
        *b.globals.engine_timestamp = stamp;
    }
    if (b.globals.enabled) {
        write_animation_frame(player, b);
        write(player, b.bones.fields.setup_flags_bb78dc43, 4u);
    }
    original<void>(thinkable, 1);
    const auto observers = b.update_observers.entries();
    for (auto* item : observers) {
        auto* observer = static_cast<network::UpdateObserver*>(item);
        observer->vtable->post_view(observer, player);
    }
    b.globals.thinking = 0;
}
void FixAnimationsRuntime::thinkable_opacity(void* thinkable) {
    auto* player = entity_from(thinkable, 0xC);
    if (bindings.globals.enabled && animation_owner_changed(player))
        write_animation_frame(player, bindings);
    original<void>(thinkable, 1);
    effects::apply_player_opacity(player, bindings.opacity);
}

void FixAnimationsRuntime::receive_layer(const void* data, void* layer, void* output) {
    const auto& b = bindings;
    auto* player = read<void*>(layer, 0x30);
    if (player && engine::invoke<bool>(player, 158)) {
        const auto* begin =
            read<const std::byte*>(player, b.bones.fields.animation_layers_b538bba5);
        const auto layer_index = (static_cast<std::byte*>(layer) - begin) / 0x38;
        const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
        auto& owner = b.players.get(index)->native;
        write(&owner, 0x46C + layer_index * 0x1C, read<std::uint32_t>(data, 8));
        if (b.globals.local_player == player && command_mode(b.globals) &&
            !read<std::uint8_t>(b.globals.local_player, b.fields.life_state_9d920e35))
            return;
        auto* state = read<void*>(player, b.bones.fields.animation_state_3d52a1a7);
        std::uint8_t* slot = nullptr;
        std::uint8_t saved = 0;
        if (b.globals.enabled && state) {
            slot = static_cast<std::uint8_t*>(state) + 4;
            saved = *slot;
            *slot = 1;
        }
        b.original_layer_proxy(data, layer, output);
        if (slot)
            *slot = saved;
    } else
        b.original_layer_proxy(data, layer, output);
}

void FixAnimationsRuntime::networkable_history(void* networkable, std::int32_t update) {
    const auto& b = bindings;
    auto* player = entity_from(networkable, 8);
    const auto index = engine::invoke<std::uint32_t>(networkable, 10);
    auto& owner = b.players.get(index)->native;
    write(&owner, 0x118, read<std::uint32_t>(player, b.fields.body_yaw_a2a8247f));
    bool changed = false;
    for (std::size_t word = 0; word != 91; ++word) {
        if (read<std::uint32_t>(&owner, 0x2FC + word * 4) !=
            read<std::uint32_t>(&owner, 0x468 + word * 4)) {
            changed = true;
            break;
        }
    }
    const auto simulation_offset = b.fields.simulation_time_d5fd2e2c;
    write(player, simulation_offset, read<std::uint32_t>(&owner, 0x10C));
    const bool received_clock = b.history_rebuild && b.globals.local_player &&
                                player != b.globals.local_player;
    if (!read<std::uint8_t>(player, b.fields.life_state_9d920e35)) {
        const auto current = read<float>(player, simulation_offset);
        const auto previous = read<float>(player, b.fields.previous_time_1599541f);
        bool reconcile = !received_clock && current == read<float>(&owner, 0x198);
        if (!received_clock && !reconcile) {
            const auto again = engine::invoke<std::int32_t>(networkable, 10);
            if (lagcomp::time_to_tick(current, b.globals.interval) % 100 == 0) {
                const auto now = receive_tick(b);
                const auto offset = again % b.globals.clock_offset;
                auto old_bucket = subtract(read<std::int32_t>(&owner, 0x168), offset);
                auto new_bucket = subtract(now, offset);
                old_bucket = subtract(old_bucket, old_bucket % b.globals.clock_window);
                new_bucket = subtract(new_bucket, new_bucket % b.globals.clock_window);
                reconcile = old_bucket != new_bucket;
            }
        }
        if (reconcile) {
            // Complete43393C16, including each live comparison before reading
            // the interval. The animation-cycle alternative below is the
            // native correction when the grounded replicated-time test fails.
            bool replicated = false;
            if (read<float>(&owner, 0x524) != 0.0f &&
                (read<std::uint32_t>(player, b.entity.offsets.player_flags) & 1u)) {
                const auto value = read<float>(player, b.fields.replicated_time_d3d4230d);
                if (value != read<float>(&owner, 0x154) &&
                    std::fabs(value - previous) <= b.globals.interval * 16.0f)
                    replicated = true;
            }
            if (replicated) {
                write(&owner, 0x198, read<std::uint32_t>(player, simulation_offset));
                write(player, simulation_offset,
                      read<std::uint32_t>(player, b.fields.replicated_time_d3d4230d));
            } else {
                const auto new_cycle = read<float>(&owner, 0x5B4);
                auto old_cycle = read<float>(&owner, 0x448);
                const auto difference = new_cycle - old_cycle;
                const auto live_current = read<float>(player, simulation_offset);
                const auto span = truncate((live_current - previous) / b.globals.interval + 0.5f);
                std::int32_t ticks = span;
                if (difference != 0.0f && span != 1 && span != 2) {
                    auto rate = read<float>(&owner, 0x444);
                    if (difference < 0.0f) {
                        ticks = 0;
                        std::int32_t attempts = 0;
                        for (;;) {
                            ++ticks;
                            const float next = b.globals.interval * rate + old_cycle;
                            if (next >= 0.999969482421875f)
                                rate = read<float>(&owner, 0x5B0);
                            old_cycle = b.globals.interval * rate + old_cycle;
                            if (truncate(old_cycle) > 0)
                                break;
                            ++attempts;
                            if (attempts >= 16)
                                break;
                        }
                        if (attempts < 16) {
                            old_cycle -= float(truncate(old_cycle));
                            const float remaining =
                                (new_cycle - old_cycle) / read<float>(&owner, 0x5B0);
                            ticks = add(ticks, truncate(remaining / b.globals.interval + 0.5f));
                        }
                    } else
                        ticks = truncate((difference / rate) / b.globals.interval + 0.5f);
                    if (ticks < span) {
                        write(&owner, 0x198, live_current);
                        write(player, simulation_offset,
                              float(ticks) * b.globals.interval + previous);
                    }
                } else {
                    write(&owner, 0x198, live_current);
                    write(player, simulation_offset, previous);
                }
            }
        }
        if (changed) {
            write(&owner, 0x1D4, read<std::uint32_t>(&owner, 0x1D4) + 1u);
            const auto tick =
                lagcomp::time_to_tick(read<float>(player, simulation_offset), b.globals.interval);
            const auto stale = tick < subtract(receive_tick(b), 1);
            const auto old = read<std::uint32_t>(&owner, 0);
            write(&owner, 0, (old & ~8u) | (stale ? 8u : 0u));
            if (!received_clock) {
                const float minimum =
                    read<float>(player, simulation_offset) - b.globals.interval * 16.0f;
                write(player, b.fields.previous_time_1599541f,
                      native_maximum(read<float>(player, b.fields.previous_time_1599541f), minimum));
            }
        }
    } else {
        write(&owner, 0x1D4, std::uint32_t(0));
        write(&owner, 0, read<std::uint32_t>(&owner, 0) & ~8u);
    }
    write(&owner, 0x110, read<std::uint32_t>(player, simulation_offset));
    if (changed)
        write(&owner, 0x168, receive_tick(b));

    if (b.globals.command_mode && !read<std::uint8_t>(player, b.fields.life_state_9d920e35) &&
        !read<std::uint8_t>(player, b.fields.suppressed_cde290d2)) {
        write(player, b.fields.client_animation_8481ea04, std::uint8_t(0));
        if (b.globals.enabled && animation_owner_changed(player))
            write(player, b.bones.fields.setup_flags_bb78dc43, 4u);
    } else {
        write(player, b.fields.client_animation_8481ea04, read<std::uint8_t>(&owner, 0x17C));
        if (read<std::uint8_t>(player, b.fields.life_state_9d920e35) == 2 &&
            read<std::uint8_t>(player, b.fields.move_type_7501cb91) == 10) {
            const auto mode = read<std::int32_t>(player, b.fields.observer_mode_2f4e97c4);
            if (mode == 4 || mode == 5) {
                const auto handle = read<std::uint32_t>(player, b.fields.observer_target_3b704b41);
                const auto target_index = handle & 0xFFFFu;
                if (handle != UINT32_MAX && target_index - 1u <= 63u) {
                    auto* target = engine::invoke<void*>(b.entity.entity_list, 4, handle);
                    if (target && engine::invoke<bool>(target, 158) &&
                        engine::invoke<bool>(static_cast<std::byte*>(target) + 8, 9) &&
                        b.resource && *b.resource &&
                        read<std::uint8_t>(*b.resource,
                                           b.fields.resource_alive_d266b537 + target_index)) {
                        const auto source =
                            read<RageVector>(player, b.bones.fields.bone_origin_a9ee6062);
                        auto& geometry = b.players.get(target_index)->geometry;
                        geometry.origin = {source[0], source[1], source[2]};
                        geometry.center = geometry.origin;
                        const auto* minimum = engine::invoke<const RageVector*>(target, 76);
                        const auto* maximum = engine::invoke<const RageVector*>(target, 77);
                        geometry.center.z += ((*maximum)[2] + (*minimum)[2]) * 0.5f;
                        geometry.eye_height =
                            read<float>(target, b.entity.offsets.pose_parameters + 4) * 18.0f +
                            58.0f;
                        geometry.pose_adjustment =
                            read<float>(target, b.entity.offsets.pose_parameters + 0x30) * 6.0f;
                        const auto second = *engine::invoke<const RageVector*>(target, 77);
                        geometry.lower = {geometry.center.x, geometry.center.y,
                                          geometry.center.z - second[2] * 0.6f};
                        geometry.upper = {geometry.center.x, geometry.center.y,
                                          (geometry.origin.z + geometry.eye_height) -
                                              geometry.pose_adjustment};
                        geometry.alpha = 1.0f;
                        geometry.last_live_tick = receive_tick(b);
                    }
                }
            }
        }
    }
    std::array<std::byte, 0x158> information;
    if (engine::invoke<bool>(b.entity.engine, 8, index, information.data())) {
        auto* settings = b.player_settings + std::size_t(index) * 0x20;
        const auto identity = read<std::uint32_t>(information.data(), 8);
        if (read<std::uint32_t>(settings, 0) != identity) {
            write(settings, 0, identity);
            std::memset(settings + 8, 0, 0x18);
        }
    }
    if (b.globals.enabled && animation_owner_changed(player)) {
        copy_absolute_angles_to_engine(player, b);
        const auto offset = b.fields.simulation_time_d5fd2e2c;
        write(player, offset - 4, read<std::uint32_t>(player, offset - 8));
    }
    const auto current = read<float>(player, b.fields.simulation_time_d5fd2e2c);
    float temporary = current;
    if (current - read<float>(player, b.fields.previous_time_1599541f) != 0.0f) {
        const auto interval = b.globals.interval;
        const float minimum = engine::invoke<float>(b.entity.engine, 14) - interval;
        if (minimum > read<float>(player, b.fields.simulation_time_d5fd2e2c)) {
            const auto clock = engine::invoke<float>(b.entity.engine, 14);
            temporary = clock + (interval + interval);
        }
    }
    auto* simulation = static_cast<std::byte*>(player) + b.fields.simulation_time_d5fd2e2c;
    const auto saved = read<std::uint32_t>(simulation, 0);
    write(simulation, 0, temporary);
    original<void>(networkable, 7, update);
    if (!update)
        restore_animation_backup(player);
    (void)read<std::uint32_t>(player, b.bones.fields.effects_38ca5228);
    write(simulation, 0, saved);
}

#if defined(_M_IX86) || defined(__i386__)
#define SKEET_FIX_ENTRY __fastcall
#else
#define SKEET_FIX_ENTRY
#endif
namespace {
void SKEET_FIX_ENTRY transformations_entry(void* player, void*, std::uintptr_t a, std::uintptr_t b,
                                           std::uintptr_t c, std::uintptr_t d, std::uintptr_t e,
                                           std::uintptr_t f) {
    active_runtime->build_transformations(player, {a, b, c, d, e, f});
}
void SKEET_FIX_ENTRY blending_entry(void* player, void*, void* studio, RageVector* positions,
                                    BoneQuaternion* rotations, float time) {
    active_runtime->standard_blending_rules(player, studio, positions, rotations, time);
}
void SKEET_FIX_ENTRY extra_bones_entry(void* player, void*, std::uintptr_t a, std::uintptr_t b,
                                       std::uintptr_t c, std::uintptr_t d, std::uintptr_t e,
                                       std::uintptr_t f) {
    active_runtime->extra_bone_processing(player, {a, b, c, d, e, f});
}
void SKEET_FIX_ENTRY view_entry(void* player, void*, RageVector* origin, RageVector* angles,
                                float* near_plane, float* far_plane, float* field_of_view) {
    active_runtime->calculate_view(player, origin, angles, near_plane, far_plane, field_of_view);
}
bool SKEET_FIX_ENTRY bones_entry(void* renderable, void*, Matrix3x4* output, std::int32_t maximum,
                                 std::uint32_t mask, float time) {
    return active_runtime->setup_bones(renderable, output, maximum, mask, time);
}
void SKEET_FIX_ENTRY history_entry(void* networkable, void*, std::int32_t update) {
    active_runtime->networkable_history(networkable, update);
}
void SKEET_FIX_ENTRY network_view_entry(void* networkable, void*, std::int32_t update) {
    active_runtime->networkable_view(networkable, update);
}
void SKEET_FIX_ENTRY network_pre_update_entry(void* networkable, void*, std::int32_t update) {
    active_runtime->networkable_pre_update(networkable, update);
}
void SKEET_FIX_ENTRY think_observers_entry(void* thinkable, void*) {
    active_runtime->thinkable_observers(thinkable);
}
void SKEET_FIX_ENTRY think_opacity_entry(void* thinkable, void*) {
    active_runtime->thinkable_opacity(thinkable);
}
void layer_proxy_entry(const void* data, void* layer, void* output) {
    active_runtime->receive_layer(data, layer, output);
}
}
void FixAnimationsRuntime::prepare(FixAnimationHook kind,
                                   engine::VtableHookMetadata& metadata) noexcept {
    active_runtime = this;
    switch (kind) {
    case FixAnimationHook::player:
        metadata.replacement_table[190] = reinterpret_cast<void*>(&transformations_entry);
        metadata.replacement_table[207] = reinterpret_cast<void*>(&blending_entry);
        metadata.replacement_table[198] = reinterpret_cast<void*>(&extra_bones_entry);
        metadata.replacement_table[277] = reinterpret_cast<void*>(&view_entry);
        break;
    case FixAnimationHook::renderable:
        metadata.replacement_table[13] = reinterpret_cast<void*>(&bones_entry);
        break;
    case FixAnimationHook::networkable_history:
        metadata.replacement_table[7] = reinterpret_cast<void*>(&history_entry);
        break;
    case FixAnimationHook::networkable_view:
        metadata.replacement_table[6] = reinterpret_cast<void*>(&network_pre_update_entry);
        metadata.replacement_table[7] = reinterpret_cast<void*>(&network_view_entry);
        break;
    case FixAnimationHook::thinkable_observers:
        metadata.replacement_table[1] = reinterpret_cast<void*>(&think_observers_entry);
        break;
    case FixAnimationHook::thinkable_opacity:
        metadata.replacement_table[1] = reinterpret_cast<void*>(&think_opacity_entry);
        break;
    }
}
engine::VtableHookNode* FixAnimationsRuntime::install(FixAnimationHook kind,
                                                      engine::VtableHookRegistry& hooks,
                                                      void* receiver,
                                                      engine::VtableHookMetadata& metadata) {
    prepare(kind, metadata);
    return engine::install_vtable_hook(hooks, receiver, metadata);
}
void* FixAnimationsRuntime::native_replacement_entry(std::size_t, std::uint32_t entry) {
    void* replacement;
    switch (entry) {
    case 0x43330F73u:
        replacement = reinterpret_cast<void*>(&transformations_entry);
        break;
    case 0x4335C639u:
        replacement = reinterpret_cast<void*>(&blending_entry);
        break;
    case 0x43388201u:
        replacement = reinterpret_cast<void*>(&extra_bones_entry);
        break;
    case 0x4335B798u:
        replacement = reinterpret_cast<void*>(&view_entry);
        break;
    case 0x433E80F7u:
        replacement = reinterpret_cast<void*>(&bones_entry);
        break;
    case 0x4337DE0Bu:
        replacement = reinterpret_cast<void*>(&history_entry);
        break;
    case 0x433E129Au:
        replacement = reinterpret_cast<void*>(&network_pre_update_entry);
        break;
    case 0x433B7093u:
        replacement = reinterpret_cast<void*>(&network_view_entry);
        break;
    case 0x433FD3BFu:
        replacement = reinterpret_cast<void*>(&think_observers_entry);
        break;
    case 0x43383CA2u:
        replacement = reinterpret_cast<void*>(&think_opacity_entry);
        break;
    default:
        throw std::invalid_argument("Unknown captured FixAnimations entry");
    }
    return replacement;
}
void FixAnimationsRuntime::bind_native_replacement(engine::VtableHookMetadata& metadata,
                                                   std::size_t slot, std::uint32_t entry) {
    auto* replacement = native_replacement_entry(slot, entry);
    active_runtime = this;
    metadata.replacement_table[slot] = replacement;
}
void FixAnimationsRuntime::uninstall(engine::VtableHookRegistry& hooks, void* receiver) {
    engine::uninstall_vtable_hook(hooks, receiver);
}
void FixAnimationsRuntime::install_layer_proxy(void** slot) {
    active_runtime = this;
    proxy_slot_ = slot;
    saved_proxy_ = *slot;
    bindings.original_layer_proxy = reinterpret_cast<AnimationLayerProxy>(saved_proxy_);
    std::atomic_ref<void*>(*slot).exchange(reinterpret_cast<void*>(&layer_proxy_entry),
                                           std::memory_order_seq_cst);
}
void FixAnimationsRuntime::restore_layer_proxy() {
    if (proxy_slot_)
        std::atomic_ref<void*>(*proxy_slot_).exchange(saved_proxy_, std::memory_order_seq_cst);
    proxy_slot_ = nullptr;
}
#undef SKEET_FIX_ENTRY
} // namespace skeet::animation
