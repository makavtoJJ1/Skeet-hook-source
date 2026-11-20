#include "skeet/rage/single_target.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/rage_damage.hpp"
#include "skeet/lagcomp/live_record_attachments.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <immintrin.h>
#include <utility>

namespace skeet::rage {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T result;
    std::memcpy(&result, static_cast<const std::byte*>(object) + offset, sizeof result);
    return result;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
float minimum(float a, float b) noexcept {
    return _mm_cvtss_f32(_mm_min_ss(_mm_set_ss(a), _mm_set_ss(b)));
}
float maximum(float a, float b) noexcept {
    return _mm_cvtss_f32(_mm_max_ss(_mm_set_ss(a), _mm_set_ss(b)));
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
std::int32_t shots_to_kill(std::int32_t health, std::int32_t damage) noexcept {
    if (damage <= 0)
        return 0x7FFFFFFF;
    const auto shots = add(add(health, -1), damage) / damage;
    return shots > 1 ? shots : 1;
}
// Exact434701D0 filter:43382057 returnsfalse,433686FC returns1.
class PredictionWorldFilter {
public:
    virtual bool should_hit(void*, std::uint32_t) {
        return false;
    }
    virtual std::int32_t trace_type() {
        return 1;
    }
};
struct alignas(16) PredictionRay {
    std::array<float, 4> start, delta, start_offset, extents;
    std::uint32_t transform;
    bool is_ray, swept;
};
static_assert(sizeof(PredictionRay) == 0x50);
static_assert(offsetof(PredictionRay, is_ray) == 0x44);
bool convar_nonzero(const void* object) noexcept {
    for (;;) {
        const auto parent = field<const void*>(object, 0x1C);
        if (parent == object)
            break;
        object = parent;
    }
    return field<std::uint32_t>(object, 0x30) !=
           std::uint32_t(reinterpret_cast<std::uintptr_t>(object));
}
void* weapon(const client::CommandFrame& frame) noexcept {
    return reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
}
float point_scale(const client::CommandFrame& frame, void* target,
                  const AcquisitionTargetSettings& settings, const TargetPreparationBindings& b) {
    const auto& offsets = b.offsets;
    const auto* pose = static_cast<const std::byte*>(target) + offsets.pose_parameters;
    const float duck_pose = field<float>(pose, 0xC);
    const float limit = 0.9f - (duck_pose - 0.0f) * 0.199999988079071044921875f;
    if (!(b.state.flags & 4U))
        return minimum(limit, 0.9f);
    float scale;
    if (b.weapon_settings.multipoint_scale.value == 24) {
        const auto target_origin = field<RageVector>(target, b.targeting.offsets.origin);
        const auto* local_origin = engine::invoke<const RageVector*>(b.targeting.local_player, 10);
        const RageVector delta{target_origin[0] - (*local_origin)[0],
                               target_origin[1] - (*local_origin)[1],
                               target_origin[2] - (*local_origin)[2]};
        if ((b.state.flags & 0x200U) && convar_nonzero(b.accuracy_convar)) {
            const auto* wrapper = field<const std::byte*>(target, offsets.studio_header);
            const std::byte* hitbox = nullptr;
            if (wrapper) {
                const auto* header = field<const std::byte*>(wrapper, 0);
                const auto index = field<std::int32_t>(target, offsets.hitbox_set);
                const auto* set =
                    header + field<std::int32_t>(header, 0xB0) + std::ptrdiff_t(index) * 12;
                if (field<std::int32_t>(set, 4))
                    hitbox = set + field<std::int32_t>(set, 8) + 0x44;
            }
            const auto bone = hitbox ? field<std::int32_t>(hitbox, 0) : -1;
            const auto* cache =
                field<const std::byte*>(target, offsets.bone_cache) + std::ptrdiff_t(bone) * 0x30;
            const float x = field<float>(cache, 0), y = field<float>(cache, 0x10),
                        z = field<float>(cache, 0x20);
            const float planar = std::sqrt(x * x + y * y);
            float angle = float(std::atan2(double(-z), double(planar))) * 57.2957763671875f + 90.0f;
            angle = normalize_movement_angle(angle);
            float fraction = minimum(maximum(angle - 40.0f, 0.0f), 40.0f) / 40.0f;
            if (duck_pose != 1.0f)
                fraction += ((1.0f - duck_pose) - fraction) * 0.5f;
            if (delta[2] > 100.0f)
                fraction += (0.9700000286102294921875f - fraction) * 0.5f;
            scale = minimum(maximum(fraction, 0.1f), 0.9700000286102294921875f);
            if (std::isnan(scale))
                scale = 0.699999988079071044921875f;
        } else {
            const float length =
                std::sqrt((delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2]);
            scale = minimum(
                maximum(1.0f - (length * std::bit_cast<float>(frame.field_160)) / 5.0f, 0.3f),
                0.8f);
        }
    } else {
        scale = float(std::bit_cast<std::int32_t>(
                    std::uint32_t(b.weapon_settings.multipoint_scale.value) - 1U)) *
                0.01f;
        if (settings.words[1] != 2U && duck_pose >= 0.99f)
            scale = minimum(0.699999988079071044921875f, scale);
        else if (field<float>(target, offsets.velocity + 8) != 0.0f)
            scale *= 0.9f;
    }
    return minimum(scale, limit);
}
}
float acquisition_point_scale(const client::CommandFrame& frame, void* target,
                              const AcquisitionTargetSettings& settings,
                              const TargetPreparationBindings& bindings) {
    return point_scale(frame, target, settings, bindings);
}
void* select_single_target(const SingleTargetSelection& request) {
    void* selected = nullptr;
    float best = request.minimum_cosine;
    for (std::int32_t index = 1; index <= request.maximum_clients; ++index) {
        void* target = engine::invoke<void*>(request.targeting.entity_list, 3, index);
        if (!target_is_eligible(request.targeting, target))
            continue;
        const auto origin = field<RageVector>(target, request.targeting.offsets.origin);
        const auto* collision = engine::invoke<const RageVector*>(target, 77);
        const float x = origin[0] - request.origin[0], y = origin[1] - request.origin[1];
        const float z = (((*collision)[2] - 8.0f) + origin[2]) - request.origin[2];
        const float length = std::sqrt((x * x + y * y) + z * z);
        const float score =
            ((request.forward[1] * y + x * request.forward[0]) + request.forward[2] * z) /
            (length + 0.00000011920928955078125f);
        if (score > best) {
            selected = target;
            best = score;
        }
    }
    return selected;
}
bool target_history_yaw_changed(void* target, const client::PlayerState& player,
                                std::ptrdiff_t animation_state_offset) noexcept {
    const auto* animation = field<const std::byte*>(target, animation_state_offset);
    const float yaw = animation ? field<float>(animation, 0x80) : player.native.reference_yaw;
    const auto count = player.history.count < 3 ? player.history.count : 3;
    for (std::uint32_t n = 0; n < count; ++n) {
        const auto& record = player.history.at(n).header;
        if (record.flags & 0x200U)
            break;
        if (record.flags & 8U)
            continue;
        const float difference = yaw - record.animation_yaw;
        if (std::isnan(difference))
            continue;
        if (std::fabs(normalize_movement_angle(difference)) > 60.0f)
            return true;
    }
    return false;
}
void prepare_acquisition_target_settings(const client::CommandFrame& frame, void* target,
                                         client::PlayerState& player, std::uint32_t entity_index,
                                         const players::Adjustments& adjustments,
                                         AcquisitionTargetSettings& settings,
                                         const TargetPreparationBindings& b) {
    if (settings.words[0] && player.history.count) {
        bool retain = false;
        if (b.remembered_target_handle != -1) {
            const auto* remembered =
                engine::invoke<void*>(b.targeting.entity_list, 4, b.remembered_target_handle);
            retain = remembered == target && !(b.state.previous_targets[(entity_index & 63U) >> 5] &
                                               (1U << (entity_index & 31U)));
        }
        if (!retain)
            settings.words[0] = 1;
    } else
        settings.words[0] = 0;
    const auto health = engine::invoke<std::int32_t>(target, 122);
    const auto raw = std::bit_cast<std::int32_t>(settings.words[5]);
    float factor = 0;
    std::int32_t source_limit = 0;
    if (raw == 0) {
        factor = engine::invoke<float>(weapon(frame), 433, std::int32_t(0));
        source_limit = engine::invoke<std::int32_t>(weapon(frame), 431);
    }
    settings.words[5] =
        std::uint32_t(rage_minimum_damage(raw, health, b.state.flags, factor, source_limit));
    if (settings.words[4])
        settings.words[3] = std::bit_cast<std::uint32_t>(point_scale(frame, target, settings, b));
    const auto origin = field<RageVector>(target, b.targeting.offsets.origin);
    const float x = origin[0] - frame.origin_10c[0], y = origin[1] - frame.origin_10c[1],
                z = origin[2] - frame.origin_10c[2];
    const float distance = (y * y + x * x) + z * z;
    if (distance > 4000000.0f) {
        settings.words[4] &= ~0x2F0U;
        if (settings.words[1] != 0x200U)
            settings.words[1] &= ~0x200U;
    }
    if (distance > 1000000.0f)
        settings.words[4] &= ~0x200U;
    auto countdown = field<std::int32_t>(&player.native, 0xF8);
    if (countdown > 0) {
        settings.option(0xDD, 1);
        if (!(b.state.flags & 9U)) {
            --countdown;
            write(&player.native, 0xF8, countdown);
        }
    }
    if (adjustments.prefer_body_aim == 1)
        settings.option(0xDD, 0);
    else if (adjustments.prefer_body_aim == 2 || adjustments.prefer_body_aim == 3)
        settings.option(0xDD, 1);
    const bool force_body = adjustments.prefer_body_aim == 3 ||
                            hotkey_active(b.global.force_body_aim.value.binding, b.input);
    const bool restricted =
        (b.state.flags & 0x400U) &&
        field<std::uint16_t>(weapon(frame), b.targeting.offsets.weapon_identifier) != 31;
    if (force_body && !restricted) {
        settings.words[1] &= 0xC;
        settings.words[2] &= 0xC;
        if (!(settings.words[1] | settings.words[2]))
            settings.words[1] = 0xC;
    }
    if (adjustments.safe_point == 2) {
        settings.words[2] |= settings.words[1];
        settings.words[1] = 0;
    }
    if (!settings.words[1] && settings.words[2])
        settings.words[2] &= ~0x200U;
    if (adjustments.correction_active && b.global.prefer_safe_point.value &&
        (settings.words[1] & 0xC) == 0xC && 0.5f > field<float>(&player.native, 0x51C) &&
        field<std::int32_t>(target, b.targeting.offsets.team) == 2 &&
        field<float>(target, b.targeting.offsets.duck_amount) >= 0.8f &&
        (field<std::uint8_t>(target, b.targeting.offsets.movement_flags) & 1U))
        settings.words[4] &= ~8U;
    if (restricted) {
        settings.words[1] &= 2;
        settings.words[2] &= 2;
        settings.words[4] &= 2;
        settings.option(0xDD, 0);
        settings.option(0xDF, 0);
    }
}
AcquisitionTargetTicks acquisition_target_ticks(const client::CommandFrame& frame, void* target,
                                                client::PlayerState& player,
                                                AcquisitionTargetSettings& settings,
                                                const TargetPreparationBindings& b,
                                                const RecordCandidateBindings& timing) {
    const auto maximum = settings.record_limit();
    AcquisitionTargetTicks result;
    const auto predicted = requested_pose_record_tick(frame, target, player, timing);
    const auto finish = [&]() -> AcquisitionTargetTicks {
        if (timing.rebuild_pose && player.history.count) {
            const auto newest = player.history.at(0).header.simulation_tick;
            std::erase_if(result.values, [&](int tick) { return tick > newest; });
        }
        if (result.values.size() > maximum)
            result.values.resize(maximum);
        // Only accepted records consume scan slots. Keep the measured forecast first.
        if (predicted) {
            result.values.insert(result.values.begin(), *predicted);
            if (settings.words[0])
                result.descriptor_flags = 4;
        }
        return std::move(result);
    };
    const auto window = lagcomp::history_tick_window(
        {timing.current_time, field<float>(&frame, 0x198), timing.interpolation, timing.interval,
         timing.maximum_unlag_ms, field<std::int32_t>(&frame, 0xD4),
         std::int32_t(frame.field_18c)});
    const auto clamp_newest = [&](std::int32_t first, std::int32_t last) {
        if (last >= first) {
            const auto newest = player.history.at(0).header.simulation_tick;
            if (newest < first)
                return first;
            return newest > last ? last : newest;
        }
        return last;
    };
    if (settings.words[0]) {
        append_target_record_ticks(frame, target, player, maximum, timing, result.values);
        if (!result.values.empty()) {
            if (result.values.size() >= 2)
                std::qsort(result.values.data(), result.values.size(), sizeof(std::int32_t),
                           [](const void* left, const void* right) {
                               return std::bit_cast<std::int32_t>(
                                   std::uint32_t(*static_cast<const std::int32_t*>(right)) -
                                   std::uint32_t(*static_cast<const std::int32_t*>(left)));
                           });
            result.descriptor_flags = 4;
        } else {
            if (!player.history.count)
                return finish();
            const auto first = window.corrected > window.first ? window.corrected : window.first;
            std::int32_t stop_tick = player.history.at(0).header.simulation_tick;
            for (std::uint32_t n = 0; n < player.history.count; ++n) {
                const auto& record = player.history.at(n).header;
                stop_tick = record.simulation_tick;
                if (record.flags & 0x200U)
                    break;
            }
            if (window.last < stop_tick)
                return finish();
            const auto* layers = field<const std::byte*>(target, b.offsets.animation_layers);
            if (0.00150000001303851604461669921875f > field<float>(layers, 0x178)) {
                if (target_history_yaw_changed(target, player, b.offsets.animation_state))
                    settings.option(0xDD, 1);
            } else {
                const auto receive_tick = player.history.at(0).header.receive_tick;
                const long double clock = engine::invoke<double>(timing.engine_client, 14);
                const float rounded =
                    float(clock / static_cast<long double>(timing.receive_interval) + 0.5L);
                const auto tick = _mm_cvttss_si32(_mm_set_ss(rounded));
                const auto difference =
                    std::bit_cast<std::int32_t>(std::uint32_t(tick) - std::uint32_t(receive_tick));
                if (difference > 2)
                    return finish();
            }
            result.values.push_back(clamp_newest(first, window.last));
        }
    }
    if (result.values.empty()) {
        if ((b.state.flags & 1U) || !player.history.count)
            return finish();
        result.values.push_back(clamp_newest(window.first, window.last));
    }
    return finish();
}
namespace {
bool prediction_damage_gate(const client::CommandFrame& frame, const client::PlayerState& player,
                            const AcquisitionTargetSettings& settings,
                            const TargetPredictionBindings& b) {
    const auto& trace = b.trace;
    if (!settings.option(0xDE) || !(frame.flags & 0x400U) || (trace.context_flags & 3U) ||
        (b.scheduler_flags & 8U) || !player.history.count || (frame.flags & 0x8000000U))
        return false;
    if (frame.weapon_id_d8 == 64 && !(trace.weapon_flags & 1U))
        return false;
    if ((frame.flags & 0x102000U) || !(frame.flags & 0x40000U) || (frame.flags & 0x4000U))
        return false;
    return !(20.0f > frame.scalar_c0 && !(frame.previous_command.movement.buttons & 0x618U));
}
void refine_prediction_record(const client::CommandFrame& frame, void* target,
                              AcquisitionTargetSettings& settings,
                              const TargetPredictionBindings& b, const lagcomp::LagRecord& record) {
    const auto& trace = b.trace;
    auto query = prepare_target_bullet(trace.weapon, trace.offsets.weapon_item, true);
    anti_aim::TargetTraceFilter engine_filter(trace.trace_filter_vtable, trace.trace_entities);
    const auto filter = engine_filter.binding();
    RageVector aim = record.header.origin;
    aim[2] = record.header.collision_max_z * 0.5f + aim[2];
    trace_ballistic_bullet(trace.ballistics, trace.source, frame.origin_dc, aim, filter, query);
    if (!query.point_count)
        return;
    const auto target_entity = BallisticEntity(reinterpret_cast<std::uintptr_t>(target));
    const auto damage = [&] {
        const auto info = trace.ballistics.entity_info(target_entity);
        return final_bullet_damage(query.damage, query.armor_ratio,
                                   std::bit_cast<std::int32_t>(query.reserved), query.weapon_id, 3,
                                   info.armor, info.helmet);
    };
    const auto health = engine::invoke<std::int32_t>(target, 122);
    const auto present_damage = damage();
    const auto present_shots = shots_to_kill(health, present_damage);
    if (present_shots <= 1)
        return;
    const auto angles = field<MovementAngles>(&frame, 0x118);
    MovementVector forward, right;
    movement_angle_vectors(angles, &forward, &right, nullptr);
    const auto flatten = [](MovementVector& v) {
        const auto inverse =
            1.0f / (std::sqrt((v.x * v.x + v.y * v.y) + 0.0f * 0.0f) + 0.00000011920928955078125f);
        v = {v.x * inverse, v.y * inverse, 0.0f * inverse};
    };
    flatten(forward);
    flatten(right);
    const auto forward_move =
        b.command ? b.command->movement.forward_move : field<float>(&frame, 0x24);
    const auto side_move = b.command ? b.command->movement.side_move : field<float>(&frame, 0x28);
    const float x = side_move * right.x + forward_move * forward.x;
    const float y = side_move * right.y + forward_move * forward.y;
    void* local = reinterpret_cast<void*>(std::uintptr_t(trace.source));
    float speed;
    if (b.command) {
        // Post-firing4336961E calls player450 before reading3EB71843.
        const auto maximum = engine::invoke<float>(local, 276);
        speed = minimum(field<float>(local, b.local_maximum_speed), maximum) *
                    0.3400000035762786865234375f -
                1.0f;
    } else {
        speed =
            minimum(field<float>(local, b.local_maximum_speed), engine::invoke<float>(local, 276)) *
                0.3400000035762786865234375f -
            1.0f;
    }
    const auto inverse =
        speed / (std::sqrt((x * x + y * y) + 0.0f * 0.0f) + 0.00000011920928955078125f);
    const float time = engine::invoke<float>(trace.weapon, 433, std::int32_t(0)) * 0.75f;
    const RageVector projected{(x * inverse) * time + frame.origin_dc[0],
                               (y * inverse) * time + frame.origin_dc[1],
                               (0.0f * inverse) * time + frame.origin_dc[2]};
    PredictionRay ray{};
    ray.start = {frame.origin_dc[0] - 16.0f, frame.origin_dc[1] - 16.0f, frame.origin_dc[2] + 0.0f,
                 0};
    for (unsigned axis = 0; axis < 3; ++axis)
        ray.delta[axis] = projected[axis] - frame.origin_dc[axis];
    ray.start_offset = {16.0f, 16.0f, -0.0f, 0};
    ray.extents = {0, 0, 2.0f, 0};
    ray.is_ray = false;
    ray.swept = ((ray.delta[0] * ray.delta[0] + ray.delta[1] * ray.delta[1]) +
                 ray.delta[2] * ray.delta[2]) != 0.0f;
    BallisticTrace world_trace;
    PredictionWorldFilter world_filter;
    {
        MovementTraceFlagGuard guard(trace.trace_flag);
        engine::invoke<void>(b.engine_trace, 5, &ray, std::uint32_t(1), &world_filter,
                             &world_trace);
    }
    if (!trace_ballistic_bullet(trace.ballistics, trace.source, world_trace.end, aim, filter,
                                query))
        return;
    const auto future_damage = damage();
    if (future_damage <= 0)
        return;
    const auto future_shots = shots_to_kill(health, future_damage);
    if (present_shots <= future_shots)
        return;
    if (future_shots >= 2 && (b.scheduler_flags & 0x8000U) && frame.field_1d4 == 1)
        return;
    auto numerator = health;
    const auto remainder = health % future_shots;
    if (remainder)
        numerator = add(health, future_shots - remainder);
    auto required = numerator / future_shots;
    if (required > future_damage)
        required = future_damage;
    if (frame.field_1d4 == 1 && engine::invoke<bool>(trace.weapon, 430)) {
        if (add(present_damage, future_damage) > health)
            return;
        const auto remaining = add(health, -future_damage);
        required = remaining > 1 ? remaining : 1;
    }
    const auto old = std::bit_cast<std::int32_t>(settings.words[5]);
    if (required > old)
        settings.words[5] = std::uint32_t(required);
}
}
void refine_acquisition_minimum_damage(const client::CommandFrame& frame, void* target,
                                       client::PlayerState& player,
                                       AcquisitionTargetSettings& settings,
                                       const TargetPredictionBindings& b) {
    if (!prediction_damage_gate(frame, player, settings, b))
        return;
    lagcomp::LagRecord record;
    std::memcpy(&record, &player.history.at(0), sizeof record);
    const auto& selection = b.record_selection;
    lagcomp::refresh_record_attachments(selection.players, target, record, selection.preparation,
                                        selection.attachments);
    refine_prediction_record(frame, target, settings, b, record);
}
void refine_acquisition_minimum_damage(const client::CommandFrame& frame, void* target,
                                       client::PlayerState& player,
                                       AcquisitionTargetSettings& settings,
                                       const TargetPredictionBindings& b,
                                       const animation::BoneRuntime& runtime) {
    if (!prediction_damage_gate(frame, player, settings, b))
        return;
    lagcomp::LagRecord record;
    std::memcpy(&record, &player.history.at(0), sizeof record);
    const auto& selection = b.record_selection;
    lagcomp::refresh_record_attachments(selection.players, target, record, selection.preparation,
                                        selection.attachments, runtime);
    refine_prediction_record(frame, target, settings, b, record);
}
bool acquire_single_target(PreambleState& state, PreambleResult& preamble,
                           AcquisitionStorage& storage, const SingleTargetBindings& bindings) {
    if (!(state.flags & 9U)) {
        state.target_mask.fill(0);
        state.previous_targets.fill(0);
    }
    auto& frame = *preamble.frame;
    const auto& preparation = bindings.preparation;
    void* target = select_single_target({preparation.targeting, bindings.maximum_clients,
                                         frame.origin_dc, field<RageVector>(&frame, 0x1A8),
                                         std::bit_cast<float>(preamble.scan_template[7])});
    if (!target)
        return false;
    const auto entity = engine::invoke<std::uint32_t>(static_cast<std::byte*>(target) + 8, 10);
    auto& player = *bindings.records.players.get(entity);
    AcquisitionTargetSettings settings{preamble.scan_template};
    prepare_acquisition_target_settings(frame, target, player, entity,
                                        bindings.adjustments.records[entity].adjustments, settings,
                                        preparation);
    auto trace = bindings.prediction.trace;
    trace.context_flags = state.flags;
    trace.weapon_flags = state.weapon_flags;
    trace.weapon = weapon(frame);
    const TargetPredictionBindings prediction{trace,
                                              bindings.prediction.record_selection,
                                              bindings.prediction.engine_trace,
                                              bindings.prediction.local_maximum_speed,
                                              bindings.prediction.scheduler_flags,
                                              bindings.prediction.command};
    refine_acquisition_minimum_damage(frame, target, player, settings, prediction,
                                      bindings.records.bone_runtime);
    auto ticks = acquisition_target_ticks(frame, target, player, settings, preparation,
                                          bindings.record_candidates);
    prepare_and_submit_target_records(state, preamble, storage, target, player, ticks.values,
                                      settings, ticks.descriptor_flags, bindings.records);
    finish_target_acquisition(state, storage, bindings.jobs, bindings.records.players,
                              bindings.maximum_clients, bindings.records.settings_menu);
    return true;
}
} // namespace skeet::rage
