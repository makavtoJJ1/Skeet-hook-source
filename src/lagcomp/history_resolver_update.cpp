#include "skeet/lagcomp/history_resolver_update.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include "skeet/client/player_state.hpp"
#include "skeet/lagcomp/packet_capture.hpp"
#include "skeet/lagcomp/prediction_scope.hpp"
#include "skeet/resolver.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>
#include <optional>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
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
        const float turns = round_half_away_from_zero(absolute(value / 360.0f)) * 360.0f;
        value = value < 0.0f ? value + turns : value - turns;
    }
    return value;
}
std::int32_t truncate(float value) noexcept {
    return _mm_cvttss_si32(_mm_set_ss(value));
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(a) +
                                       std::bit_cast<std::uint32_t>(b));
}
std::int32_t subtract(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(a) -
                                       std::bit_cast<std::uint32_t>(b));
}
float clamp_unit(float value) noexcept {
    return _mm_cvtss_f32(
        _mm_min_ss(_mm_max_ss(_mm_set_ss(value), _mm_setzero_ps()), _mm_set_ss(1.0f)));
}
float convar(const void* value) noexcept {
    return std::bit_cast<float>(read_prediction_convar(const_cast<void*>(value)));
}
float local_time(const HistoryResolverUpdateBindings& bindings) noexcept {
    if (!bindings.local_time_entity || !*bindings.local_time_entity)
        return 0.0f;
    return read<float>(*bindings.local_time_entity, bindings.fields.local_time_9f8e7f71);
}
struct Suppression {
    std::uint8_t* target;
    std::uint8_t saved = 0;
    explicit Suppression(std::uint8_t* value) : target(value) {
        if (target) {
            saved = *target;
            *target = 0;
        }
    }
    void restore() {
        if (target) {
            *target = saved;
            target = nullptr;
        }
    }
    ~Suppression() {
        restore();
    }
};
resolver_runtime::LagRecordHistory history_view(client::PlayerState& owner) noexcept {
    return {owner.history.records, owner.history.first, owner.history.count};
}
//43412A95..434137FE: this caller uses model descriptors0/3 without the
// nineteen-hitbox cap used by the independent aiming-model reader.
bool feedback_from_environment(client::PlayerState& owner, void* player, std::uint8_t mode,
                               std::uint8_t previous_attempt, float clock,
                               HistoryResolverScratch& scratch,
                               const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    Suppression suppression(bindings.environment.suppression_flag);
    const auto origin = read<RageVector>(player, bindings.entity.offsets.origin);
    const auto probe =
        resolver_runtime::probe_environment(bindings.local_origin, origin, bindings.environment);
    if (!probe.selected)
        return false;
    RageVector direction;
    if (probe.damage[0] == 0 && probe.damage[1] == 0)
        for (unsigned axis = 0; axis < 3; ++axis)
            direction[axis] =
                engine::read_opaque_native_float(scratch.probe_direction.data() + axis);
    else {
        direction = probe.direction;
        scratch.probe_direction = direction;
    }
    const auto* wrapper = read<const void*>(player, bindings.fields.studio_wrapper);
    if (!wrapper)
        return false;
    const auto* header = read<const std::byte*>(wrapper, 0);
    if (!header)
        return false;
    const auto* set =
        header + read<std::int32_t>(header, 0xB0) +
        std::ptrdiff_t(read<std::int32_t>(player, bindings.fields.hitbox_set_c2b6fb10)) * 12;
    if (read<std::int32_t>(set, 4) < (bindings.configuration.resolver_reworked() ? 4 : 1))
        return false;
    const auto* descriptors = set + read<std::int32_t>(set, 8);
    std::array<resolver_runtime::EnvironmentCapsule, 2> hitboxes;
    constexpr std::size_t indices[]{0, 3};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* box = descriptors + indices[i] * 0x44;
        const auto radius = read<float>(box, 0x30);
        if (!(radius > 0.0f))
            return false;
        hitboxes[i] = {std::uint32_t(indices[i]), read<RageVector>(box, 8),
                       read<RageVector>(box, 0x14), radius};
    }
    const auto& source = owner.history.at(0);
    const float yaw_delta =
        state.reference_yaw - std::bit_cast<float>(source.header.player_history_1ac);
    const auto& old_origin = source.header.origin;
    const bool rebase = yaw_delta != 0.0f || old_origin[0] != origin[0] ||
                        old_origin[1] != origin[1] || old_origin[2] != origin[2];
    for (auto& axis : direction)
        axis = negative(axis);
    const RageVector eye{bindings.local_origin[0], bindings.local_origin[1],
                         bindings.local_origin[2] + 64.0f};
    const auto selection =
        rebase ? resolver_runtime::score_environment_hypotheses(
                     source, hitboxes, direction, eye, {yaw_delta, old_origin, origin},
                     bindings.environment.trigonometry, bindings.configuration.resolver_reworked())
               : resolver_runtime::score_environment_hypotheses(source, hitboxes, direction, eye,
                                                                bindings.configuration.resolver_reworked());
    if (!selection.valid())
        return false;
    const auto attempt = resolver_runtime::classify_winning_pose(selection.pose_yaw);
    state.resolver_slots[mode].attempt = attempt;
    state.probe_time = clock;
    state.last_update_tick =
        corrected_command_tick(bindings.entity.command_tick, bindings.entity.previous_command,
                               bindings.entity.current_command);
    suppression.restore();
    state.resolver_status = 1;
    if (mode == 1)
        state.resolver_slots[2].attempt = attempt;
    else if (mode == 2)
        state.resolver_slots[1].attempt = attempt;
    const bool strict = bindings.configuration.resolver_reworked();
    const auto history_attempt = strict ? source.header.resolver_mode : previous_attempt;
    if ((!strict || source.header.resolver_index == mode) && history_attempt != attempt) {
        auto history = history_view(owner);
        resolver_runtime::propagate_attempt(history, mode, history_attempt, attempt);
    }
    return true;
}
//43411CBD..434138D9, before the selected mode's animation-yaw policy.
void update_inline_feedback(client::PlayerState& owner, void* player, std::uint8_t& mode,
                            SimulationTickSpan span, std::uint32_t& flags,
                            HistoryResolverScratch& scratch,
                            const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(player) + 8, 10);
    if (!bindings.player_table[std::size_t(index) * 0x20 + 6])
        return;
    if (!(state.flags & 0x40u) && read<float>(&state, 4) == local_time(bindings)) {
        state.flags |= 0x40u;
        const auto delta = normalize(state.reference_yaw - read<float>(&state, 8));
        if (absolute(delta) < 60.0f) {
            mode = std::uint8_t(!(state.flags & 4u)) + 1;
            state.resolver_slots[mode].attempt = resolver_runtime::classify_winning_pose(delta);
            state.resolver_status = 3;
            return;
        }
    }
    if (state.resolver_status == 3 || (flags & 1u))
        return;
    const auto selected = bindings.selected_entity;
    if (selected == -1 || engine::invoke<void*>(bindings.entity.entity_list, 4, selected) != player)
        return;
    if (span.count <= 1 &&
        (owner.history.count <= 1 || subtract(owner.history.at(0).header.simulation_tick,
                                              owner.history.at(1).header.simulation_tick) <= 1))
        return;
    const auto selected_index = selected == -1 ? 0x1FFFu : std::uint32_t(std::uint16_t(selected));
    const auto pending_shots = std::span<const HistoryEvaluation>(bindings.pending_shots);
    if (resolver_runtime::has_pending_environment_shot(pending_shots, selected_index))
        return;
    if (mode != 1 && mode != 2)
        return;
    const auto previous = state.resolver_slots[mode].attempt;
    const auto clock = local_time(bindings);
    if (clock == state.probe_time &&
        (state.probe_count >= 2 || absolute(bindings.current_time - state.probe_stamp) < 5.0f))
        return;
    if (!owner.history.count)
        return;
    if (clock == state.reference_time && state.resolver_status == 2) {
        const auto origin = read<RageVector>(player, bindings.entity.offsets.origin);
        const float y = state.reference_origin[1] - origin[1],
                    x = state.reference_origin[0] - origin[0],
                    z = state.reference_origin[2] - origin[2];
        if (10000.0f > (y * y + x * x) + z * z)
            return;
    }
    if (!(owner.history.at(0).header.flags & 0x80u))
        return;
    if (feedback_from_environment(owner, player, mode, previous, clock, scratch, bindings))
        flags |= 0x10u;
}
bool recent_shot_matches_pose(const client::NativePlayerStateLayout& state, std::uint8_t mode,
                             const resolver_runtime::JitterPoseEstimate& estimate,
                             float maximum_yaw, float now) {
    const auto offset = std::size_t(mode) * 0x24;
    const float fired = read<float>(&state, 0x14 + offset);
    const float fired_yaw = read<float>(&state, 0x18 + offset);
    const float age = now - fired;
    const float yaw_change = std::fabs(std::remainder(state.reference_yaw - fired_yaw, 360.0f));
    if (!(fired > 0 && age >= 0 && age <= 0.25f && yaw_change <= 4.0f))
        return false;

    const auto attempt = state.resolver_slots[mode].attempt % 3u;
    const float delta = attempt == 1 ? -maximum_yaw : attempt == 2 ? maximum_yaw : 0.0f;
    const float old_error = std::fabs(std::remainder(
        state.reference_yaw + delta - estimate.anchor, 360.0f));
    // Keep fresh evidence only while the current observation fits that pose.
    // A body-yaw change can invalidate it even when the eye yaw repeats.
    return old_error - estimate.error <= std::max(3.0f, maximum_yaw * 0.15f);
}
void choose_resolver_yaw(client::PlayerState& owner, void* player, void* animation_state,
                         SimulationTickSpan span, std::int32_t shot_tick, std::uint32_t& flags,
                         const HistoryResolverInput& input, HistoryResolverScratch& scratch,
                         const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    const auto angles = bindings.entity.offsets.eye_angles;
    const auto* table = bindings.player_table + input.player_table_offset;
    const float previous_animation_yaw = read<float>(animation_state, 0x80);
    using resolver_runtime::JitterDecision;
    auto& evidence = owner.jitter.current;
    const bool evaluate_jitter = bindings.configuration.jitter_enabled() &&
                                 evidence.tick == span.last && span.count >= 1 && span.count <= 16 &&
                                 !(flags & 5u);
    if (evaluate_jitter) {
        evidence.estimate = resolver_runtime::estimate_jitter_pose(
            state.reference_yaw, std::bit_cast<float>(state.capture_field_180), input.maximum_yaw);
        if (!evidence.estimate.valid())
            evidence.decision = JitterDecision::ambiguous_pose;
    }
    const bool use_prior = evaluate_jitter && evidence.estimate.valid();
    if (table[5]) {
        write(animation_state, 0x80, state.reference_yaw - float(read<std::int32_t>(table, 8)));
        evidence.decision = JitterDecision::forced_yaw;
    } else {
        if (table[6] || use_prior) {
            if (table[6] && !table[4] && bindings.configuration.prediction_available) {
                float pitch = read<float>(player, angles);
                const auto delta =
                    absolute(normalize((read<float>(player, bindings.fields.recoil_pitch_66146802) +
                                        read<float>(&state, 0x1A8)) -
                                       180.0f));
                if (delta < 0.1f) {
                    pitch = -180.0f;
                    if (read<std::uint8_t>(&state, 0xA4) &&
                        absolute(read<float>(&state, 0x108)) == 180.0f)
                        pitch = read<float>(&state, 0x108);
                    else if (read<std::uint16_t>(&state, 0xA6) % 11 == 10)
                        pitch = 180.0f;
                }
                write(player, angles, pitch);
            }
            std::uint8_t mode = 0, attempt = 0;
            const bool stock_selects_pose = table[6] &&
                (span.count > 1 || read<std::uint8_t>(&state, 0xA3) > 1);
            if (stock_selects_pose || use_prior)
                mode = (flags & 4u) && bindings.shot_angles_active
                           ? 3
                           : std::uint8_t(!(state.flags & 4u)) + 1;
            update_inline_feedback(owner, player, mode, span, flags, scratch, bindings);
            if (use_prior && mode > 0 && mode < 3) {
                auto& decision = evidence.decision;
                if (state.resolver_status != 0) {
                    decision = state.resolver_status == 1   ? JitterDecision::environment
                               : state.resolver_status == 2 ? JitterDecision::movement
                                                            : JitterDecision::shared;
                } else if (recent_shot_matches_pose(state, mode, evidence.estimate,
                                                    input.maximum_yaw, bindings.current_time)) {
                    decision = JitterDecision::shot_feedback;
                } else {
                    auto& chosen = state.resolver_slots[mode].attempt;
                    const bool changed = !stock_selects_pose || chosen % 3u != evidence.estimate.attempt;
                    if (changed)
                        chosen = evidence.estimate.attempt;
                    decision = changed ? JitterDecision::animation_yaw : JitterDecision::unchanged_pose;
                }
            } else if (flags & 4u) {
                evidence.decision = JitterDecision::shot_record;
            }
            if (mode == 0) {
                mode = input.previous_resolver_index;
                attempt = read<std::uint8_t>(&state, 0xA2);
            } else if (mode < 3) {
                const auto index =
                    engine::invoke<std::int32_t>(static_cast<std::byte*>(player) + 8, 10);
                attempt = state.resolver_slots[mode].attempt % 3u;
                if (attempt == 0) {
                    if (evidence.applied() || bindings.player_table[std::size_t(index) * 0x20 + 6] ||
                        read<std::int32_t>(&state, std::size_t(mode) * 0x24 + 0x20) > 1 ||
                        absolute(bindings.current_time -
                                 read<float>(&state, std::size_t(mode) * 0x24 + 0x1C)) <= 2.0f)
                        write(animation_state, 0x80, state.reference_yaw);
                    else if (input.previous_resolver_index != mode ||
                             read<std::uint8_t>(&state, 0xA2) != 0)
                        write(animation_state, 0x80, std::bit_cast<float>(state.capture_field_180));
                } else if (attempt == 1) {
                    write(animation_state, 0x80, state.reference_yaw - input.maximum_yaw);
                    write(player, angles + 4, normalize(state.reference_yaw - 120.0f));
                } else if (attempt == 2) {
                    write(animation_state, 0x80, state.reference_yaw + input.maximum_yaw);
                    write(player, angles + 4, normalize(state.reference_yaw + 120.0f));
                }
            } else if (mode == 3) {
                bool retain_center = false;
                if (span.count == 1 || (bindings.shot_angles_active && (flags & 4u) &&
                                        shot_tick == add(span.first, 1))) {
                    const auto difference = state.reference_yaw - read<float>(&state, 0x114);
                    retain_center =
                        std::isnan(difference) || absolute(normalize(difference)) < 120.0f;
                }
                if (!retain_center) {
                    const auto delta = normalize(state.reference_yaw -
                                                 std::bit_cast<float>(state.capture_field_180));
                    attempt = read<std::uint8_t>(&state, 0x7C) % 3u;
                    if (attempt == 2)
                        write(animation_state, 0x80, state.reference_yaw);
                    else if ((attempt == 0 && delta > 0.0f) || (attempt == 1 && !(delta > 0.0f))) {
                        write(animation_state, 0x80, state.reference_yaw + input.maximum_yaw);
                        write(player, angles + 4, normalize(state.reference_yaw + 120.0f));
                    } else {
                        write(animation_state, 0x80, state.reference_yaw - input.maximum_yaw);
                        write(player, angles + 4, normalize(state.reference_yaw - 120.0f));
                    }
                }
            }
            write(&state, 0xA2, attempt);
            write(&state, 0xA3, std::uint8_t(span.count));
            state.resolver_index = mode;
        }
        if (read<float>(&state, 0x524) > 0.0f)
            write(player, bindings.entity.offsets.field_a2a8247f, read<float>(player, angles + 4));
    }
    const auto yaw = read<float>(animation_state, 0x80);
    if (previous_animation_yaw != yaw)
        write(animation_state, 0x80, normalize(yaw));
}

struct StepFields {
    std::uint32_t flags;
    RageVector angles;
    float recoil, lower_body_yaw, duck;
    Vector velocity, absolute_origin;
};
StepFields capture_step_fields(void* player,
                               const HistoryResolverUpdateBindings& bindings) noexcept {
    const auto& o = bindings.entity.offsets;
    return {read<std::uint32_t>(player, o.player_flags),
            read<RageVector>(player, o.eye_angles),
            read<float>(player, bindings.fields.recoil_pitch_66146802),
            read<float>(player, o.field_a2a8247f),
            read<float>(player, o.duck_amount),
            read<Vector>(player, o.velocity),
            read<Vector>(player, bindings.fields.absolute_origin_4e66bb34)};
}
void publish_step_fields(void* player, const StepFields& value,
                         const HistoryResolverUpdateBindings& bindings) noexcept {
    const auto& o = bindings.entity.offsets;
    write(player, o.player_flags, value.flags);
    write(player, o.eye_angles, value.angles);
    write(player, bindings.fields.recoil_pitch_66146802, value.recoil);
    write(player, o.field_a2a8247f, value.lower_body_yaw);
    write(player, o.duck_amount, value.duck);
    write(player, o.velocity, value.velocity);
    write(player, bindings.fields.absolute_origin_4e66bb34, value.absolute_origin);
}
std::int32_t animation_step_count(const void* state, float interval) noexcept {
    const auto current = read<float>(state, 0x5B4);
    auto cycle = read<float>(state, 0x448), rate = read<float>(state, 0x444);
    const float delta = current - cycle;
    std::int32_t ticks = 0;
    // COMISS zero,delta / JBE includes an unordered delta in this arm.
    if (!(delta < 0.0f))
        ticks = truncate((delta / rate) / interval + 0.5f);
    else
        for (std::int32_t i = 0; i < 16; ++i) {
            ticks = add(ticks, 1);
            if (rate * interval + cycle >= 0x1.fffcp-1f)
                rate = read<float>(state, 0x5B0);
            cycle = rate * interval + cycle;
            const auto integer = truncate(cycle);
            if (integer > 0) {
                const auto rest = truncate(
                    ((current - (cycle - float(integer))) / read<float>(state, 0x5B0)) / interval +
                    0.5f);
                ticks = add(ticks, rest);
                break;
            }
        }
    return ticks < 1 ? 1 : ticks > 16 ? 16 : ticks;
}
void step_stock_animation(client::PlayerState& owner, void* player, void* animation_state,
                            SimulationTickSpan span, std::int32_t shot_tick, std::uint32_t flags,
                            HistoryResolverUpdateResult& result,
                            const HistoryResolverUpdateBindings& bindings) {
    auto& state = owner.native;
    const auto& o = bindings.entity.offsets;
    auto* descriptor = static_cast<std::byte*>(player) + o.animation_layers;
    const auto count = read<std::int32_t>(descriptor, 0xC);
    const auto copy_count = count > 13 ? 13 : count;
    for (std::int32_t i = 0; i < copy_count; ++i)
        std::memcpy(static_cast<std::byte*>(read<void*>(descriptor, 0)) + std::size_t(i) * 0x38 +
                        0x14,
                    reinterpret_cast<const std::byte*>(&state) + 0x3A4 +
                        std::size_t(i) * 0x1C,
                    0x1C);
    constexpr std::size_t animation_offsets[]{0x98, 0x19C, 0x190, 0x198, 0x180};
    constexpr std::size_t owner_offsets[]{0x3BC, 0x3C4, 0x3CC, 0x3D8, 0x458};
    for (std::size_t i = 0; i < 5; ++i)
        write(animation_state, animation_offsets[i], read<std::uint32_t>(&state, owner_offsets[i]));
    for (std::int32_t i = 0; i < copy_count; ++i)
        write(read<void*>(descriptor, 0), std::size_t(i) * 0x38 + 0x18,
              read<std::uint32_t>(&state, 0x46C + std::size_t(i) * 0x1C));
    const auto saved = capture_step_fields(player, bindings);
    auto current = saved;
    current.recoil = read<float>(&state, 0x150);
    current.duck = read<float>(&state, 0x104);
    if (read<std::uint32_t>(&state, 0x238)) {
        const auto sample = (read<std::uint32_t>(&state, 0x234) % 6u) * 0x20;
        current.flags = read<std::uint32_t>(&state, 0x250 + sample);
        current.absolute_origin = read<Vector>(&state, 0x240 + sample);
    }
    const auto ticks = animation_step_count(&state, bindings.entity.interval);
    std::int32_t jump_tick = -1, land_tick = -1, pre_shot_tick = -1, duck_full_tick = -1,
                 duck_zero_tick = -1;
    auto shot_cutoff = INT32_MAX;
    bool interpolate_duck = current.duck != saved.duck;
    const auto duck_delta = (saved.duck - current.duck) / float(ticks);
    const float inverse = 1.0f / float(ticks);
    const Vector velocity_delta{(saved.velocity[0] - current.velocity[0]) * inverse,
                                (saved.velocity[1] - current.velocity[1]) * inverse,
                                (saved.velocity[2] - current.velocity[2]) * inverse};
    if (shot_tick != -1 && (flags & 4u)) {
        if (bindings.shot_angles_active)
            shot_cutoff = shot_tick;
        else
            pre_shot_tick = shot_tick;
    }
    bool physical_air = false;
    const auto event_tick = [&](float cycle, float rate) {
        const auto age = truncate((cycle / rate) / bindings.entity.interval + 0.5f);
        return add(span.last, subtract(1, age));
    };
    if (!(saved.flags & 1u)) {
        const auto cycle = read<float>(&state, 0x4F0), old_cycle = read<float>(&state, 0x384);
        if (!((cycle == old_cycle &&
               read<std::int32_t>(&state, 0x4DC) == read<std::int32_t>(&state, 0x370)) ||
              old_cycle <= cycle || !bindings.velocity.classify)) {
            if (bindings.velocity.classify(player, read<std::int32_t>(&state, 0x4DC)) == 0x3D9) {
                jump_tick = event_tick(cycle, read<float>(&state, 0x4EC));
                const char* name =
                    bindings.velocity.sequence_name
                        ? bindings.velocity.sequence_name(player, read<std::int32_t>(&state, 0x4DC))
                        : "";
                const auto comparison = bindings.velocity.compare_sequence_name(
                    name, bindings.velocity.jump_sequence_name);
                if (comparison == 0 && saved.duck >= 1.0f)
                    duck_full_tick = add(jump_tick, 1);
                else if (comparison != 0 && !(saved.duck > 0.0f))
                    duck_zero_tick = add(jump_tick, 1);
                interpolate_duck = false;
            }
            physical_air = jump_tick != -1 && owner.history.count != 0;
        }
    } else if (owner.history.count && !(owner.history.at(0).header.player_flags & 1u)) {
        if (read<float>(&state, 0x500) != 0.0f && read<float>(&state, 0x394) == 0.0f &&
            bindings.velocity.classify) {
            const auto activity =
                bindings.velocity.classify(player, read<std::int32_t>(&state, 0x4F8));
            if (activity == 0x3DC || activity == 0x3DD) {
                land_tick = event_tick(read<float>(&state, 0x50C), read<float>(&state, 0x508));
                physical_air = land_tick != -1 && owner.history.count != 0;
            }
        }
    } else
        physical_air = jump_tick != -1 && owner.history.count != 0;
    bool override_animation =
        read<std::uint8_t>(player, bindings.fields.animation_override_00a5bd68) == 0 &&
        read<float>(&state, 0x538) > 0.5f &&
        read<float>(&state, 0x538) == read<float>(&state, 0x3CC);
    const auto first = subtract(span.last, ticks);
    write(animation_state, 0x6C, float(first) * bindings.entity.interval);
    auto vertical = current.velocity[2];
    for (auto tick = add(first, 1); tick < span.last; tick = add(tick, 1)) {
        bindings.animation_time = float(tick) * bindings.entity.interval;
        if (physical_air) {
            if (!(current.flags & 1u)) {
                const auto half_gravity =
                    convar(bindings.velocity.cvars.gravity) * bindings.entity.interval * 0.5f;
                const auto half_velocity = vertical - half_gravity;
                vertical = half_velocity - half_gravity;
                current.absolute_origin[2] =
                    half_velocity * bindings.entity.interval + current.absolute_origin[2];
                current.velocity[2] = vertical;
            } else {
                vertical = 0.0f;
                current.velocity[2] = 0.0f;
            }
        }
        if (tick == land_tick) {
            current.flags |= 1u;
            auto* live = read<void*>(descriptor, 0);
            write(live, 0x144, std::uint32_t(0));
            write(live, 0x140, read<std::uint32_t>(&state, 0x508));
            land_tick = -1;
        }
        if (tick == subtract(jump_tick, 1))
            current.flags |= 1u;
        if (tick == jump_tick) {
            const auto previous_flags = current.flags;
            current.flags &= ~1u;
            auto* live = read<void*>(descriptor, 0);
            write(live, 0x10C, std::uint32_t(0));
            write(live, 0x108, read<std::uint32_t>(&state, 0x4EC));
            if (physical_air) {
                const auto impulse = convar(bindings.velocity.cvars.jump_impulse);
                vertical = previous_flags & 2u ? impulse : vertical + impulse;
                current.absolute_origin[2] =
                    vertical * bindings.entity.interval + current.absolute_origin[2];
                vertical -=
                    convar(bindings.velocity.cvars.gravity) * bindings.entity.interval * 0.5f;
                current.velocity[2] = vertical;
            }
        }
        if (tick >= shot_cutoff) {
            current.angles[0] = read<float>(&state, 0x1A8);
            current.lower_body_yaw = read<float>(&state, 0x118);
            current.angles[1] = read<float>(&state, 0x1AC);
        } else if (tick == shot_tick && tick == pre_shot_tick) {
            const auto old_pitch =
                read<float>(&state, 0x1C4) -
                convar(bindings.recoil_scale_convar) * read<float>(&state, 0x150);
            const auto pitch_delta = read<float>(&state, 0x1A8) - old_pitch;
            bool replace = !std::isnan(pitch_delta) && absolute(normalize(pitch_delta)) > 10.0f;
            if (!replace) {
                const auto yaw_delta = read<float>(&state, 0x1AC) - read<float>(&state, 0x1BC);
                replace = !std::isnan(yaw_delta) && absolute(normalize(yaw_delta)) > 10.0f;
            }
            if (replace) {
                current.angles[0] = old_pitch;
                current.angles[1] = read<float>(&state, 0x1BC);
            }
        }
        if (tick == duck_full_tick || tick == duck_zero_tick)
            current.duck = saved.duck;
        if (interpolate_duck)
            current.duck = clamp_unit(duck_delta + current.duck);
        current.velocity[0] = velocity_delta[0] + current.velocity[0];
        current.velocity[1] = velocity_delta[1] + current.velocity[1];
        vertical = velocity_delta[2] + vertical;
        current.velocity[2] = vertical;
        publish_step_fields(player, current, bindings);
        if (override_animation)
            write(player, bindings.fields.animation_override_00a5bd68, std::uint8_t(1));
        animation::update_animation_state(animation_state, current.angles,
                                          {bindings.animation_frame, bindings.animation_time,
                                           bindings.entity.interval, bindings.animation_reset_byte},
                                          bindings.animation);
        if (override_animation) {
            override_animation = false;
            write(player, bindings.fields.animation_override_00a5bd68, std::uint8_t(0));
        }
        result.speed_fraction = read<float>(animation_state, 0x11C);
        if (tick == shot_tick)
            current.recoil = saved.recoil;
        current.lower_body_yaw = read<float>(&state, 0x118);
    }
    publish_step_fields(player, saved, bindings);
    result.animation_publish_layer_weight = read<float>(read<void*>(descriptor, 0), 0x138);
}

struct MotionAxis {
    double start, linear = 0, quadratic = 0, cubic = 0;

    MotionAxis(float from, float to, float initial_speed, float final_speed, double duration)
        : start(from) {
        const double displacement = double(to) - from;
        double first = initial_speed * duration, last = final_speed * duration;
        // Opposite endpoint velocities can describe a reversal. Keep both
        // slopes when the integrated constant acceleration agrees with the
        // observed displacement to within one sixteenth of a map unit.
        const bool reversal = first * last < 0.0 &&
                              std::fabs(displacement - 0.5 * (first + last)) <= 0.0625;
        if (!reversal) {
            if (displacement == 0.0)
                return;
            // These tangent limits make the Hermite segment monotone.
            first = std::clamp(first / displacement, 0.0, 3.0) * displacement;
            last = std::clamp(last / displacement, 0.0, 3.0) * displacement;
        }
        linear = first;
        quadratic = 3.0 * displacement - 2.0 * first - last;
        cubic = first + last - 2.0 * displacement;
    }
    float position(double t) const noexcept {
        return float(start + t * (linear + t * (quadratic + t * cubic)));
    }
    float speed(double t, double duration) const noexcept {
        return float((linear + t * (2.0 * quadratic + 3.0 * t * cubic)) / duration);
    }
};

constexpr unsigned jump_layer = 4, land_layer = 5;
constexpr std::uint32_t on_ground = 1u;
struct LayerTransition {
    int step = 0;
    bool jump = false;
};
LayerTransition layer_transition(const AnimationLayerState& previous,
                                 const AnimationLayerState& received, unsigned index,
                                 void* player, SimulationTickSpan span,
                                 const HistoryResolverUpdateBindings& bindings) {
    const auto cycle = std::bit_cast<float>(received.words[6]);
    const auto rate = std::bit_cast<float>(received.words[5]);
    const auto sequence = std::bit_cast<std::int32_t>(received.words[1]);
    if (!bindings.velocity.classify || sequence <= 0 || !std::isfinite(cycle) ||
        cycle < 0.0f || cycle >= 1.0f || !std::isfinite(rate) || rate <= 0.0f)
        return {};
    const auto old_cycle = std::bit_cast<float>(previous.words[6]);
    const auto old_rate = std::bit_cast<float>(previous.words[5]);
    if (previous.words[1] == received.words[1] && std::isfinite(old_cycle) &&
        std::isfinite(old_rate) && old_rate > 0.0f && cycle >= old_cycle)
        return {};
    const auto activity = bindings.velocity.classify(player, sequence);
    if (index == jump_layer ? activity != 0x3D9 && activity != 0x3DA
                            : activity != 0x3DC && activity != 0x3DD)
        return {};
    const double age = double(cycle) / rate / bindings.entity.interval;
    if (!(age < double(span.count) + 0.5))
        return {};
    const auto ticks = std::max(1, int(std::floor(age + 0.5)));
    return {span.count + 1 - ticks, activity == 0x3D9};
}

struct FlightSegment {
    int from, until;
    double height, launch_speed, gravity;
    float position(double time) const noexcept {
        return float(height + time * (launch_speed - 0.5 * gravity * time));
    }
    float speed(double time) const noexcept {
        return float(launch_speed - gravity * time);
    }
};
std::optional<FlightSegment> fit_flight(int ticks, bool grounded_before, bool grounded_after,
                                      LayerTransition takeoff, LayerTransition landing,
                                      const StepFields& start, const StepFields& end,
                                      const HistoryResolverUpdateBindings& bindings) {
    const int from = grounded_before ? takeoff.step - 1 : 0;
    const int until = grounded_after ? landing.step : ticks;
    const auto gravity_object = bindings.velocity.cvars.gravity.get();
    if (from < 0 || until <= from || !gravity_object)
        return {};
    const double gravity = convar(gravity_object);
    const double interval = bindings.entity.interval;
    if (!std::isfinite(gravity) || gravity < 0.0)
        return {};
    const double duration = (until - from) * interval;
    const double displacement = double(end.absolute_origin[2]) - start.absolute_origin[2];
    const double launch = displacement / duration + 0.5 * gravity * duration;
    const double arrival = launch - gravity * duration;
    // Endpoint velocities are quantized and event cycles resolve to whole ticks.
    const double tolerance = std::max(8.0, 2.0 * gravity * interval);
    if ((!grounded_before && std::fabs(launch - start.velocity[2]) > tolerance) ||
        (!grounded_after && std::fabs(arrival - end.velocity[2]) > tolerance) ||
        (grounded_after && arrival > 0.0))
        return {};
    if (grounded_before && takeoff.jump) {
        const auto impulse_object = bindings.velocity.cvars.jump_impulse.get();
        if (!impulse_object)
            return {};
        const auto impulse = convar(impulse_object);
        if (!std::isfinite(impulse) || impulse <= 0.0f || launch <= 0.0 ||
            launch > impulse + tolerance)
            return {};
    } else if (grounded_before && std::fabs(launch) > tolerance) {
        return {};
    }
    return FlightSegment{from, until, start.absolute_origin[2], launch, gravity};
}

bool finite_vector(const Vector& value) noexcept {
    return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
}
Vector packet_origin(const PacketTimingSample& packet) noexcept {
    return {std::bit_cast<float>(packet.fields_04[0]), std::bit_cast<float>(packet.fields_04[1]),
            std::bit_cast<float>(packet.fields_04[2])};
}
void begin_replayed_layer(std::byte* layers, unsigned index,
                         const AnimationLayerState& received) noexcept {
    auto* layer = layers + index * 0x38;
    write(layer, 0x18, received.words[1]);
    write(layer, 0x28, received.words[5]);
    write(layer, 0x2C, std::uint32_t(0));
}

void rebuild_history_animation(client::PlayerState& owner, void* player, void* state,
                               SimulationTickSpan span, std::int32_t shot_tick, std::uint32_t flags,
                               HistoryResolverUpdateResult& result,
                               const HistoryResolverUpdateBindings& b, float previous_yaw) {
    const double interval = b.entity.interval;
    const auto ticks = span.count;
    if (!b.animation.update_state || ticks < 2 || ticks > 16 || !std::isfinite(interval) ||
        interval <= 0.0 || span.last <= span.first || std::int64_t(span.last) - span.first != ticks)
        return;
    const auto timeline = history_timeline(owner);
    if (timeline.packet_count < 2 || timeline.packet_count > timeline.packets.size())
        return;
    const auto& received = timeline.packets[timeline.packet_head % timeline.packets.size()];
    const auto& previous = timeline.packets[(timeline.packet_head + 1u) % timeline.packets.size()];
    if (previous.field_00 != span.first || received.field_00 != span.last)
        return;
    const auto first_time = float(span.first) * b.entity.interval;
    const auto last_update = read<float>(state, 0x6C);
    if (!std::isfinite(last_update) || std::fabs(last_update - first_time) > interval ||
        !(float(span.first + 1) * b.entity.interval > first_time))
        return;
    const auto& native = owner.native;
    const auto saved = capture_step_fields(player, b);
    auto start = saved;
    auto end = saved;
    start.absolute_origin = packet_origin(previous);
    end.absolute_origin = packet_origin(received);
    start.flags = previous.fields_04[4];
    end.flags = received.fields_04[4];
    start.velocity = read<Vector>(&native, 0x120);
    start.duck = read<float>(&native, 0x104);
    start.recoil = read<float>(&native, 0x150);
    start.lower_body_yaw = read<float>(&native, 0x184);
    start.angles[0] = read<float>(&native, 0x188);
    start.angles[1] = read<float>(&native, 0x114);
    if (!finite_vector(start.absolute_origin) || !finite_vector(end.absolute_origin) ||
        !finite_vector(start.velocity) || !finite_vector(end.velocity) ||
        !finite_vector(start.angles) || !finite_vector(end.angles) ||
        !std::isfinite(start.duck) || !std::isfinite(end.duck) || !std::isfinite(start.recoil) ||
        !std::isfinite(start.lower_body_yaw) || !std::isfinite(previous_yaw))
        return;
    double distance_squared = 0.0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto delta = double(end.absolute_origin[axis]) - start.absolute_origin[axis];
        distance_squared += delta * delta;
    }
    if (distance_squared > 4096.0 || ((saved.flags ^ end.flags) & on_ground))
        return;
    const auto* descriptor = static_cast<const std::byte*>(player) + b.entity.offsets.animation_layers;
    auto* layers = read<std::byte*>(descriptor, 0);
    const auto layer_count = read<std::int32_t>(descriptor, 0xC);
    if (!layers || layer_count <= int(land_layer) || layer_count > 13)
        return;
    const auto old_layers = read<std::array<AnimationLayerState, 13>>(&native, 0x2FC);
    const auto new_layers = read<std::array<AnimationLayerState, 13>>(&native, 0x468);
    const bool grounded_before = (start.flags & on_ground) != 0;
    const bool grounded_after = (end.flags & on_ground) != 0;
    const auto takeoff = layer_transition(old_layers[jump_layer], new_layers[jump_layer],
                                          jump_layer, player, span, b);
    const auto landing = layer_transition(old_layers[land_layer], new_layers[land_layer],
                                          land_layer, player, span, b);
    const bool complete_hop = grounded_before && grounded_after && takeoff.step > 0 &&
                              landing.step > takeoff.step;
    std::optional<FlightSegment> flight;
    if (!grounded_before || !grounded_after || complete_hop) {
        if ((!grounded_before && !grounded_after && landing.step > 0) ||
            (grounded_before && !takeoff.step) || (grounded_after && !landing.step))
            return;
        flight = fit_flight(ticks, grounded_before, grounded_after, takeoff, landing, start, end, b);
        if (!flight)
            return;
    }
    const double duration = ticks * interval;
    const MotionAxis x(start.absolute_origin[0], end.absolute_origin[0],
                       start.velocity[0], end.velocity[0], duration);
    const MotionAxis y(start.absolute_origin[1], end.absolute_origin[1],
                       start.velocity[1], end.velocity[1], duration);
    const MotionAxis ground_z(start.absolute_origin[2], end.absolute_origin[2], 0, 0, duration);
    const auto abs_velocity = b.fields.absolute_velocity_7a2d60c1;
    const auto saved_abs_velocity = abs_velocity ? read<Vector>(player, abs_velocity) : Vector{};
    const auto network_origin = b.entity.offsets.origin;
    const auto saved_network_origin = network_origin ? read<Vector>(player, network_origin) : Vector{};
    const auto selected_yaw = read<float>(state, 0x80);
    for (int i = 0; i < layer_count; ++i)
        std::memcpy(layers + i * 0x38 + 0x14, &old_layers[i], sizeof(AnimationLayerState));
    constexpr std::size_t animation_offsets[]{0x98, 0x19C, 0x190, 0x198, 0x180};
    constexpr std::size_t owner_offsets[]{0x3BC, 0x3C4, 0x3CC, 0x3D8, 0x458};
    for (unsigned i = 0; i < std::size(animation_offsets); ++i)
        write(state, animation_offsets[i], read<std::uint32_t>(&native, owner_offsets[i]));
    write(state, 0x80, previous_yaw);
    write(state, 0x6C, first_time);
    auto current = start;
    for (int step = 1; step < ticks; ++step) {
        const auto tick = span.first + step;
        const double t = double(step) / ticks;
        const bool airborne = flight && step > flight->from &&
                              (step < flight->until || !grounded_after);
        current.flags = (current.flags & ~on_ground) | (airborne ? 0u : on_ground);
        current.duck = clamp_unit(std::lerp(start.duck, end.duck, float(t)));
        current.absolute_origin = {x.position(t), y.position(t), ground_z.position(t)};
        current.velocity = {x.speed(t, duration), y.speed(t, duration), 0.0f};
        if (flight) {
            if (airborne) {
                const double air_time = (step - flight->from) * interval;
                current.absolute_origin[2] = flight->position(air_time);
                current.velocity[2] = flight->speed(air_time);
            } else {
                current.absolute_origin[2] = step <= flight->from ? start.absolute_origin[2]
                                                               : end.absolute_origin[2];
            }
            if (grounded_before && step == takeoff.step)
                begin_replayed_layer(layers, jump_layer, new_layers[jump_layer]);
            if (grounded_after && step == landing.step)
                begin_replayed_layer(layers, land_layer, new_layers[land_layer]);
        }
        if ((flags & 4u) && tick == shot_tick) {
            current.recoil = saved.recoil;
            current.lower_body_yaw = saved.lower_body_yaw;
            if (b.shot_angles_active)
                current.angles = saved.angles;
        }
        publish_step_fields(player, current, b);
        if (network_origin)
            write(player, network_origin, current.absolute_origin);
        if (abs_velocity)
            write(player, abs_velocity, current.velocity);
        b.animation_time = float(tick) * b.entity.interval;
        animation::update_animation_state(state, current.angles,
            {b.animation_frame, b.animation_time, b.entity.interval, b.animation_reset_byte},
            b.animation);
    }
    if (selected_yaw != previous_yaw)
        write(state, 0x80, selected_yaw);
    publish_step_fields(player, saved, b);
    if (network_origin)
        write(player, network_origin, saved_network_origin);
    if (abs_velocity)
        write(player, abs_velocity, saved_abs_velocity);
    result.speed_fraction = read<float>(state, 0x11C);
    result.animation_publish_layer_weight = read<float>(layers, 0x138);
    result.animation_rebuilt = true;
}

void step_history_animation(client::PlayerState& owner, void* player, void* state,
                            SimulationTickSpan span, std::int32_t shot_tick, std::uint32_t flags,
                            HistoryResolverUpdateResult& result,
                            const HistoryResolverUpdateBindings& bindings, float previous_yaw) {
    if (bindings.configuration.reworked())
        rebuild_history_animation(owner, player, state, span, shot_tick, flags, result, bindings,
                                  previous_yaw);
    else
        step_stock_animation(owner, player, state, span, shot_tick, flags, result, bindings);
}
} // namespace

HistoryResolverUpdateResult
update_history_resolver(client::PlayerState& owner, void* player, void* animation_state,
                        const HistoryAnimationSeed& seed, SimulationTickSpan span,
                        std::int32_t shot_tick, std::uint32_t& flags,
                        const HistoryResolverInput& input, HistoryResolverScratch& scratch,
                        const HistoryResolverUpdateBindings& bindings) {
    HistoryResolverUpdateResult result{read<float>(&owner.native, 0x394),
                                       input.initial_speed_fraction};
    const bool resolve = *bindings.configuration.resolver_enabled != 0;
    const bool rebuild = bindings.configuration.reworked();
    const auto angles = bindings.entity.offsets.eye_angles;
    const auto saved_yaw = read<float>(player, angles + 4);
    const auto saved_lower_body = read<float>(player, bindings.entity.offsets.field_a2a8247f);
    const auto previous_yaw = read<float>(animation_state, 0x80);
    if (resolve) {
        choose_resolver_yaw(owner, player, animation_state, span, shot_tick, flags, input, scratch,
                            bindings);
        const auto yaw = read<float>(player, angles + 4);
        if (saved_yaw != yaw)
            write(player, angles + 4, normalize(yaw));
    }
    if ((resolve || rebuild) && !(flags & 1u) && span.count > 1)
        step_history_animation(owner, player, animation_state, span, shot_tick, flags, result,
                               bindings, previous_yaw);
    if (resolve) {
        std::optional<std::int32_t> air_age;
        if (seed.active_air_layer && (!rebuild || (flags & 1u))) {
            const auto rate = read<float>(&owner.native, 0x4EC);
            const auto cycle = read<float>(&owner.native, 0x4F0);
            const auto interval = bindings.entity.interval;
            if (!rebuild)
                air_age = truncate((cycle / rate) / interval + 0.5f);
            else if (std::isfinite(rate) && rate > 0.0f && std::isfinite(interval) &&
                     interval > 0.0f && cycle >= 0.0f && cycle < 1.0f) {
                const double ticks = double(cycle) / rate / interval;
                if (ticks + 0.5 < double(INT32_MAX))
                    air_age = std::int32_t(ticks + 0.5);
            }
        }
        if (air_age) {
            const auto age = *air_age;
            const auto age_time = float(age) * bindings.entity.interval;
            const auto start = float(span.last) * bindings.entity.interval - age_time;
            if ((flags & 1u) && seed.previous_update_time < start) {
                write(animation_state, 0x108, std::uint16_t(0));
                write(player, bindings.fields.pose_parameters_1caaf0a8 + 0x18, std::uint32_t(0));
                write(animation_state, 0x110, std::uint32_t(0));
                write(animation_state, 0x6C, start);
            }
            write(animation_state, 0x110, age_time - bindings.entity.interval);
        }
        write(player, bindings.entity.offsets.field_a2a8247f, saved_lower_body);
        write(player, angles + 4, saved_yaw);
    }
    write(player, bindings.entity.offsets.velocity, seed.velocity);
    return result;
}
} // namespace skeet::lagcomp
