#include "skeet/rage/shot_feedback.hpp"
#include "skeet/rage/jitter_log.hpp"
#include "skeet/rage/shot_log.hpp"
#include "skeet/rage/feedback_entity_owner.hpp"
#include "skeet/rage/spread_miss_log.hpp"
#include "skeet/misc/logs.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <xmmintrin.h>

namespace skeet {
template <class Values>
bool shot_record_expired_body(const ShotRecordView&, const client::PlayerState&,
                              ShotFeedbackRuntime&, Values&);
namespace {
struct SnapshotFeedbackValues {
    const ShotFeedbackRuntime& runtime;
    bool enabled(const ShotFeedbackState& state) const noexcept {
        return state.events.enabled;
    }
    float now() const noexcept {
        return runtime.now;
    }
    float interval() const noexcept {
        return runtime.interval;
    }
    float receive_interval() const noexcept {
        return runtime.receive_interval;
    }
    float lerp() const noexcept {
        return maximum_number(runtime.interpolation,
                              runtime.interpolation_ratio / runtime.update_rate);
    }
    float correction_minimum() const noexcept {
        return runtime.correction_minimum;
    }
    float correction_extra() const noexcept {
        return runtime.correction_extra;
    }
    std::int32_t maximum_unlag_milliseconds() const noexcept {
        return runtime.maximum_unlag_milliseconds;
    }
    bool ray_tests_disabled() const noexcept {
        return runtime.ray_tests_disabled;
    }
    bool reworked() const noexcept {
        return bool(runtime.resolver_rework);
    }
};
static_assert(offsetof(client::NativePlayerStateLayout, resolver_slots) == 0x10);
static_assert(offsetof(players::Record, adjustments) +
                  offsetof(players::Adjustments, correction_active) ==
              0xa);
float real(std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}
std::int32_t integer(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}
std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}
RageVector vector_words(const std::uint32_t* words) noexcept {
    return {real(words[0]), real(words[1]), real(words[2])};
}
RageVector difference(const RageVector& a, const RageVector& b) noexcept {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
float length(const RageVector& v) noexcept {
    return std::sqrt((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
}
RageVector normalized(const RageVector& delta) noexcept {
    const float inverse = 1.0f / (length(delta) + std::numeric_limits<float>::epsilon());
    return {inverse * delta[0], inverse * delta[1], inverse * delta[2]};
}
float distance_squared_yxz(const RageVector& v) noexcept {
    return (v[1] * v[1] + v[0] * v[0]) + v[2] * v[2];
}
float distance_squared_zyx(const RageVector& v) noexcept {
    return (v[2] * v[2] + v[1] * v[1]) + v[0] * v[0];
}
template <class T>
T target_field(const client::PlayerState& target, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, reinterpret_cast<const std::byte*>(&target.native) + offset, sizeof value);
    return value;
}
template <class T>
void target_field(client::PlayerState& target, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(reinterpret_cast<std::byte*>(&target.native) + offset, &value, sizeof value);
}
client::PlayerState& target_state(ShotFeedbackRuntime& runtime, std::int32_t index) {
    if (index < 1 || index > 64)
        throw std::out_of_range("Native pending shot requires target index 1..64");
    return *runtime.players.get(std::uint32_t(index));
}
void notify_miss(ShotFeedbackState& state, ShotFeedbackRuntime& runtime,
                 const ShotRecordView& shot, std::uint32_t reason) {
    const auto event = shot.script_shot();
    rage::log_shot_result(runtime.spread_log_settings, runtime.normal_log, shot, false, reason);
    rage::log_jitter_result(runtime.players, runtime.normal_log, shot, false, reason);
    script::dispatch_aim_miss(state.scripts, event, reason);
    state.notices.push_back({ShotFeedbackNoticeKind::miss, event, reason, 0, 0, 0});
}
void notify_hit(ShotFeedbackState& state, ShotFeedbackRuntime& runtime,
                const ShotRecordView& shot, const events::ShotEvent& hurt) {
    const auto event = shot.script_shot();
    const auto target = integer(hurt.payload[0]), group = integer(hurt.payload[1]),
               damage = integer(hurt.payload[2]);
    rage::log_shot_result(runtime.spread_log_settings, runtime.normal_log, shot, true, 0,
                         target, group, damage);
    rage::log_jitter_result(runtime.players, runtime.normal_log, shot, true, 0, target, group, damage);
    script::dispatch_aim_hit(state.scripts, event, target, group, damage);
    state.notices.push_back({ShotFeedbackNoticeKind::hit, event, 0, target, group, damage});
}
void erase_shot(ShotFeedbackState& state, std::size_t index) {
    for (std::size_t next = index + 1; next < state.pending.size(); ++next)
        lagcomp::assign_evaluation(state.pending[next - 1], state.pending[next]);
    state.pending.pop_back();
}
lagcomp::LagRecord& history_record(client::PlayerState& target, std::uint32_t age) {
    return *target.history.records[(target.history.first + age) & 31U];
}
const lagcomp::LagRecord& history_record(const client::PlayerState& target, std::uint32_t age) {
    return *target.history.records[(target.history.first + age) & 31U];
}
unsigned attempt_slot(std::uint8_t mode, std::uint8_t attempt) noexcept {
    return mode ? unsigned(attempt) % 3U : 0U;
}
std::ptrdiff_t attempt_offset(std::uint8_t mode, std::uint8_t attempt) noexcept {
    return 0x1c + std::uint32_t(mode) * 0x24 + attempt_slot(mode, attempt) * 8;
}
lagcomp::PacketTimingSample packet_sample(const client::PlayerState& target,
                                          std::uint32_t index) noexcept {
    return target_field<lagcomp::PacketTimingSample>(target, 0x23c + index * 0x20);
}
void confirm_target_hit(client::PlayerState& target, ShotFeedbackRecord& record) {
    for (std::uint32_t i = 0; i < target.history.count; ++i) {
        auto& current = history_record(target, i);
        if (current.header.simulation_tick <= ShotRecordView{record}.record_tick()) {
            current.header.flags |= 1U;
            break;
        }
    }
    const auto mode = record.state.record.header.resolver_index;
    const auto attempt = record.state.record.header.resolver_mode;
    target_field(target, attempt_offset(mode, attempt), 0.0f);
    target.native.resolver_slots[mode].attempt = attempt;
    target_field(target, 0xa4, std::uint8_t(target_field<std::uint8_t>(target, 0xa4) + 1U));
    target_field(target, 0x1a0, 0.0f);
    target_field(target, 0x108, bits(record.state.record.header.eye_pitch));
}
Matrix3x4 hitbox_rotation(const RageVector& angles, const RageTrigonometry& trig) {
    constexpr float radians = 0.01745329238474369f;
    const float sy = trig.sine(angles[1] * radians), cy = trig.cosine(angles[1] * radians);
    const float sp = trig.sine(angles[0] * radians), cp = trig.cosine(angles[0] * radians);
    const float sr = trig.sine(angles[2] * radians), cr = trig.cosine(angles[2] * radians);
    const float crcy = cr * cy, crsy = cr * sy, srcy = sr * cy, srsy = sr * sy;
    return {{{cp * cy, srcy * sp - crsy, crcy * sp + srsy, 0.0f},
             {cp * sy, srsy * sp + crcy, crsy * sp - srcy, 0.0f},
             {-sp, sr * cp, cr * cp, 0.0f}}};
}
Matrix3x4 compose(const Matrix3x4& matrix, const Matrix3x4& rotation) {
    Matrix3x4 result;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            const auto& m = matrix.rows[row];
            const float value =
                (m[2] * rotation.rows[2][column] + m[1] * rotation.rows[1][column]) +
                m[0] * rotation.rows[0][column];
            result.rows[row][column] = value + (column == 3 ? m[3] : 0.0f);
        }
    return result;
}
// Complete nearest-point part of 4338590E, retaining its signed endpoint result.
float segment_distance(const RageVector& point, const RageVector& first, const RageVector& second,
                       RageVector& nearest) {
    const auto delta = difference(second, first), offset = difference(point, first);
    const float extent = length(delta),
                inverse = 1.0f / (extent + std::numeric_limits<float>::epsilon());
    const RageVector unit{delta[0] * inverse, delta[1] * inverse, delta[2] * inverse};
    const float projection = (unit[1] * offset[1] + unit[0] * offset[0]) + unit[2] * offset[2];
    if (projection < 0.0f) {
        nearest = first;
        return -length(offset);
    }
    if (projection > extent) {
        nearest = second;
        return -length(difference(point, second));
    }
    nearest = {unit[0] * projection + first[0], unit[1] * projection + first[1],
               unit[2] * projection + first[2]};
    return length(difference(point, nearest));
}
template <class Values>
void validate_shot_record(ShotFeedbackRecord& record, ShotFeedbackRuntime& runtime,
                          Values& values) {
    ShotRecordView shot{record};
    auto& target = target_state(runtime, shot.target());
    if (shot.record_tick() <= 0)
        return;
    if (shot_record_expired_body(shot, target, runtime, values))
        shot.flags() |= 2U;
    if (shot.target_handle() != 0xffffffffU) {
        const auto entity = runtime.engine.entity_by_handle(shot.target_handle());
        if (entity.exists) {
            const float lerp = values.lerp();
            float correct = maximum_number(values.correction_minimum(), lerp) +
                            (values.correction_extra() + 0.0f);
            correct = _mm_cvtss_f32(
                _mm_min_ss(_mm_max_ss(_mm_set_ss(correct), _mm_setzero_ps()),
                           _mm_set_ss(float(values.maximum_unlag_milliseconds()) * 0.001f)));
            const float interval = values.interval();
            const float rounded =
                float(lagcomp::time_to_tick(entity.simulation_time, interval)) * interval;
            const float recorded = float(shot.record_tick()) * interval;
            if (std::fabs(correct - (rounded - recorded)) > 0.2f)
                shot.flags() |= 4U;
        }
    }
    if (target.history.count >= 2) {
        const lagcomp::LagRecord* previous = nullptr;
        for (std::uint32_t age = 0; age < target.history.count; ++age) {
            const auto& current = history_record(target, age);
            if (previous && distance_squared_yxz(difference(current.header.origin,
                                                            previous->header.origin)) > 4096.0f) {
                shot.flags() |= 8U;
                break;
            }
            if (current.header.simulation_tick <= shot.record_tick()) {
                if (current.header.simulation_tick < shot.record_tick())
                    shot.flags() |= 0x10U;
                break;
            }
            previous = &current;
        }
    }
}
}

std::int32_t ShotRecordView::command() const noexcept {
    return integer(record.words[3]);
}
float ShotRecordView::time() const noexcept {
    return real(record.words[4]);
}
std::int32_t ShotRecordView::shot_tick() const noexcept {
    return integer(record.words[6]);
}
std::int32_t ShotRecordView::target_life_id() const noexcept {
    return integer(record.words[8]);
}
RageVector ShotRecordView::origin() const noexcept {
    return vector_words(record.words.data() + 9);
}
std::int32_t ShotRecordView::model() const noexcept {
    return integer(record.words[12]);
}
std::int32_t ShotRecordView::hitbox_set() const noexcept {
    return integer(record.words[13]);
}
std::int32_t ShotRecordView::target() const noexcept {
    return integer(record.state.words[1]);
}
std::int32_t ShotRecordView::target_health() const noexcept {
    return integer(record.state.words[2]);
}
std::int32_t ShotRecordView::record_tick() const noexcept {
    return integer(record.state.words[45]);
}
RageVector ShotRecordView::target_point() const noexcept {
    return vector_words(record.state.words.data() + 3);
}
script::AimShot ShotRecordView::script_shot() const noexcept {
    return {integer(record.words[5]), target(), integer(record.state.words[12]),
            real(record.state.words[14]), record_tick()};
}

float quantize_shot_coordinate(float value) noexcept {
    const auto whole = _mm_cvtt_ss2si(_mm_set_ss(std::fabs(value)));
    const auto scaled = _mm_cvtt_ss2si(_mm_set_ss(value * 32.0f));
    const auto sign = scaled < 0 ? 0xffffffffU : 0U;
    const auto fraction = ((std::uint32_t(scaled) ^ sign) - sign) & 31U;
    float result = 0.0f;
    if (whole || fraction) {
        result = float(double(fraction) * 0.03125 + double(whole));
        if (value <= -0.03125f)
            result = std::bit_cast<float>(bits(result) ^ 0x80000000U);
    }
    return result;
}
RageVector quantize_shot_origin(const RageVector& value) noexcept {
    return {quantize_shot_coordinate(value[0]), quantize_shot_coordinate(value[1]),
            quantize_shot_coordinate(value[2])};
}

template <class Values>
bool shot_record_expired_body(const ShotRecordView& shot, const client::PlayerState& target,
                              ShotFeedbackRuntime& runtime, Values& values) {
    const auto engine_time = runtime.engine.engine_time();
    const auto maximum_unlag = values.maximum_unlag_milliseconds();
    const auto received = lagcomp::receive_time_to_tick(engine_time, values.receive_interval());
    const float threshold = float(received) * values.interval() - float(maximum_unlag) * 0.001f;
    const auto lower = _mm_cvtt_ss2si(_mm_set_ss(threshold));
    for (std::uint32_t age = 0; age < target.history.count; ++age) {
        const auto tick = history_record(target, age).header.simulation_tick;
        if (tick > shot.record_tick())
            continue;
        const float time = float(tick) * values.interval();
        if (!(float(lower) > time))
            return false;
        const float newest_time =
            float(history_record(target, 0).header.simulation_tick) * values.interval();
        const auto entity = runtime.engine.entity_by_index(shot.target());
        return newest_time > time || (entity.exists && entity.field_d5fd2e2c > time);
    }
    return false;
}

template <class Values>
void classify_shot_ray_body(ShotFeedbackState& state, ShotFeedbackRecord& record,
                            const RageVector* origin, const RageVector* direction,
                            ShotFeedbackRuntime& runtime, Values& values) {
    ShotRecordView shot{record};
    std::uint32_t reason = 0;
    const bool jitter = rage::jitter_shot_used(runtime.players, shot);
    if (shot.flags() & 0x20u) {
        rage::log_shot_result(runtime.spread_log_settings, runtime.normal_log, shot, false, 5);
        rage::log_jitter_result(runtime.players, runtime.normal_log, shot, false, 5);
    }
    if ((values.reworked() || jitter) && (shot.flags() & 0x20u)) {
        return;
    }
    const auto finite = [](const RageVector& value) {
        return std::all_of(value.begin(), value.end(),
                           [](float axis) { return std::isfinite(axis); });
    };
    if ((values.reworked() || jitter) &&
        ((origin && !finite(*origin)) ||
         (direction && (!finite(*direction) || distance_squared_zyx(*direction) < 1e-8f)))) {
        origin = nullptr;
        direction = nullptr;
    }
    bool verified_intersection = false;
    if (origin && std::sqrt(distance_squared_zyx(difference(*origin, shot.origin()))) > 0.5f)
        reason = 2;
    else if (origin && direction && !values.ray_tests_disabled()) {
        BallisticQuery query;
        query.allow_penetration = 1;
        query.weapon_id = std::uint16_t(record.words[27]);
        query.weapon_padding = std::uint16_t(record.words[27] >> 16);
        query.penetration = real(record.words[28]);
        query.range = real(record.words[29]);
        query.range_modifier = real(record.words[30]);
        query.armor_ratio = real(record.words[31]);
        query.base_damage = real(record.words[32]);
        query.reserved = record.words[33];
        const RageVector end{(*direction)[0] * query.range + (*origin)[0],
                             (*direction)[1] * query.range + (*origin)[1],
                             (*direction)[2] * query.range + (*origin)[2]};
        const BallisticFilter filter{nullptr,    0,    0x600400bU,
                                     0x600400bU, 1.0f, BallisticFilterKind::reject_players};
        trace_ballistic_bullet(runtime.ballistics, 0, shot.origin(), end, filter, query);
        const RageRay ray{*origin, difference(query.trace.end, *origin)};
        const auto hitboxes = runtime.engine.hitboxes(shot.model(), shot.hitbox_set());
        const auto count = jitter && !(record.state.record.header.flags & 4u)
                               ? 0u
                               : std::min<std::size_t>(19, hitboxes.size());
        reason = jitter && !count ? 0 : 1;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& box = hitboxes[index];
            auto matrix = record.state.record.matrices[index];
            if (!(box.radius > 0.0f))
                matrix = compose(matrix, hitbox_rotation(box.rotation, runtime.trigonometry));
            RageTrace trace;
            if (runtime.engine.clip_hitbox(ray, box, matrix, trace) >= 0) {
                reason = 0;
                verified_intersection = true;
                break;
            }
        }
        if (reason == 1)
            rage::log_spread_miss(runtime.spread_log_settings, runtime.normal_log);
    }
    if (!(shot.flags() & 0x20U)) {
        notify_miss(state, runtime, shot, reason);
        shot.flags() |= 0x20U;
    }
    if (reason)
        return;
    // Missing impact data is an unknown miss, not evidence against a pose.
    if ((values.reworked() || jitter) &&
        (!verified_intersection || !(record.state.record.header.flags & 4u) ||
         (shot.flags() & 0x1Eu)))
        return;
    auto& target = target_state(runtime, shot.target());
    runtime.player_adjustments.records[std::size_t(shot.target())].adjustments.correction_active =
        1;
    target_field(target, 0xa8, std::uint16_t(target_field<std::uint16_t>(target, 0xa8) + 1U));
    target_field(target, 0x1a0, values.now());
    target_field(target, 0xa4, std::uint8_t(0));
    const auto mode = record.state.record.header.resolver_index;
    const auto attempt = record.state.record.header.resolver_mode;
    const auto slot = attempt_offset(mode, attempt);
    target_field(target, slot, values.now());
    target_field(target, slot + 4, target_field<std::uint32_t>(target, slot + 4) + 1U);
    if (!(shot.flags() & 0x40U)) {
        const resolver_runtime::AttemptShot attempt_shot{
            record.state.record, shot.flags(), shot.shot_flags(), std::uint32_t(shot.target()),
            real(record.words[29])};
        const auto next = resolver_runtime::choose_next_attempt(
            {target_field<float>(target, 0x88), target_field<float>(target, 0x1a0)}, attempt_shot,
            values.now(), values.enabled(state), origin, direction, runtime.attempt_engine);
        target.native.resolver_slots[mode].attempt = next;
        if (next != attempt)
            target.native.resolver_status = 0;
    }
    if (shot.shot_flags() & 0x10U)
        target_field(target, 0xd0, values.now());
    if (record.state.record.header.resolver_state == 1) {
        target_field(target, 0xb8, target_field<std::uint32_t>(target, 0xb8) + 1U);
        target_field(target, 0xb4, values.now());
    }
    if (record.state.record.header.resolver_state == 2)
        target_field(target, 0xcc, values.now());
    if ((shot.shot_flags() & 5U) == 4U && mode) {
        for (std::uint32_t age = 0; age < target.history.count; ++age) {
            auto& current = history_record(target, age);
            if (current.header.resolver_index == mode && current.header.resolver_mode == attempt)
                current.header.flags |= 0x40U;
            if (current.header.flags & 0x200U)
                break;
        }
    }
}

template <class Values>
void reconcile_shot_feedback_body(ShotFeedbackState& state, ShotFeedbackRuntime& runtime,
                                  Values& values) {
    auto finish = [&] {
        state.events.feedback.clear();
        state.recent_position_count = 0;
        state.events.has_pending_shots = !state.pending.empty();
    };
    const auto enabled = values.enabled(state);
    state.events.enabled = enabled;
    if (!enabled) {
        state.fire_snapshots.clear();
        finish();
        return;
    }
    while (!state.pending.empty()) {
        ShotRecordView shot{state.pending.front()};
        if (std::fabs(shot.time() - values.now()) < 0.5f)
            break;
        classify_shot_ray_body(state, state.pending.front(), nullptr, nullptr, runtime, values);
        erase_shot(state, 0);
    }
    if (state.pending.empty()) {
        state.fire_snapshots.clear();
        finish();
        return;
    }
    const auto command = runtime.engine.current_command();
    std::size_t index = 0;
    while (index < state.pending.size()) {
        auto& record = state.pending[index];
        ShotRecordView shot{record};
        if (command < shot.command())
            break;
        if (!(shot.flags() & 0x100U)) {
            // The life-id check occurs after timing capture and before validity.
            auto& target = target_state(runtime, shot.target());
            shot.flags() |= 0x100U;
            record.words[16] = std::uint32_t(
                lagcomp::time_to_tick(target_field<float>(target, 0x110), values.interval()));
            record.words[17] = target_field<std::uint32_t>(target, 0x1ac);
            for (unsigned axis = 0; axis < 3; ++axis)
                record.words[18 + axis] = bits(target.native.cached_origin[axis]);
            const auto local_index = runtime.engine.local_player_index();
            if (local_index < shot.target()) {
                auto& local = target_state(runtime, runtime.engine.local_player_index());
                const auto target_head = target_field<std::uint32_t>(target, 0x234);
                const auto local_head = target_field<std::uint32_t>(local, 0x234);
                if (target.native.field_238 >= 2 && local.native.field_238 &&
                    packet_sample(target, target_head % 6U).field_18 ==
                        packet_sample(local, local_head % 6U).field_18) {
                    const auto sample = packet_sample(target, (target_head + 1U) % 6U);
                    record.words[16] = std::uint32_t(sample.field_00);
                    record.words[17] = sample.field_1c;
                    for (unsigned axis = 0; axis < 3; ++axis)
                        record.words[18 + axis] = sample.fields_04[axis];
                }
            }
            const auto local = runtime.engine.local_player();
            if (!local.hidden && shot.target_handle() != 0xffffffffU &&
                shot.target_life_id() != -1) {
                const auto entity = runtime.engine.entity_by_handle(shot.target_handle());
                if (entity.exists && entity.life_id != -1 &&
                    entity.life_id > shot.target_life_id()) {
                    if (!(shot.flags() & 0x20U)) {
                        notify_miss(state, runtime, shot, 4);
                        shot.flags() |= 0x20U;
                    }
                    erase_shot(state, index);
                    continue;
                }
            }
            validate_shot_record(record, runtime, values);
        }
        ++index;
    }
    if (state.pending.empty()) {
        state.fire_snapshots.clear();
        finish();
        return;
    }
    const auto& queue = state.events.feedback;
    for (std::size_t event_index = 0; event_index < queue.size();) {
        const auto& event = queue[event_index];
        if (event.kind == events::ShotEventKind::death) {
            const auto victim = integer(event.payload[0]);
            if (victim == runtime.engine.local_player_index()) {
                for (auto& record : state.pending) {
                    ShotRecordView shot{record};
                    if (!(shot.flags() & 0x20U)) {
                        notify_miss(state, runtime, shot, 3);
                        shot.flags() |= 0x20U;
                    }
                }
                state.pending.clear();
                state.fire_snapshots.clear();
                finish();
                return;
            }
            for (auto& record : state.pending) {
                ShotRecordView shot{record};
                if (shot.target() == victim && !(shot.flags() & 0x20U)) {
                    notify_miss(state, runtime, shot, 3);
                    shot.flags() |= 0x20U;
                }
            }
            ++event_index;
            continue;
        }
        if (event.kind != events::ShotEventKind::fire) {
            ++event_index;
            continue;
        }
        ShotFireSnapshot snapshot;
        const bool has_snapshot = !state.fire_snapshots.empty();
        if (has_snapshot) {
            snapshot = state.fire_snapshots.front();
            state.fire_snapshots.pop_front();
        }
        ++event_index;
        if (state.pending.empty())
            continue;
        auto& record = state.pending.front();
        ShotRecordView shot{record};
        if (event.payload[0] != shot.fire_weapon())
            continue;
        const events::ShotEvent* first_impact = nullptr;
        const events::ShotEvent* last_impact = nullptr;
        const events::ShotEvent* hurt = nullptr;
        while (event_index < queue.size()) {
            const auto& following = queue[event_index];
            if (following.kind == events::ShotEventKind::impact) {
                if (!first_impact)
                    first_impact = &following;
                last_impact = &following;
            } else if (following.kind == events::ShotEventKind::hurt) {
                if (following.payload[3] == shot.hurt_weapon())
                    hurt = &following;
            } else
                break;
            ++event_index;
        }
        RageVector origin = shot.origin(), direction{};
        const RageVector* origin_argument = &origin;
        const RageVector* direction_argument = nullptr;
        if (has_snapshot) {
            const auto quantized = quantize_shot_origin(origin);
            if (length(difference(quantized, snapshot.origin)) > 0.5f)
                origin = snapshot.origin;
            direction = snapshot.direction;
            direction_argument = &direction;
        }
        if (first_impact && last_impact) {
            const auto first = vector_words(first_impact->payload.data());
            const auto last = vector_words(last_impact->payload.data());
            if (first_impact != last_impact) {
                direction = normalized(difference(last, first));
                const RageVector backward{first[0] - direction[0] * 1000.0f,
                                          first[1] - direction[1] * 1000.0f,
                                          first[2] - direction[2] * 1000.0f};
                RageVector nearest;
                if (segment_distance(origin, first, backward, nearest) > 0.0f)
                    origin = nearest;
            } else
                direction = normalized(difference(first, origin));
            direction_argument = &direction;
        }
        if (hurt) {
            notify_hit(state, runtime, shot, *hurt);
            if (!(shot.flags() & 0x20U) && runtime.engine.entity_by_index(shot.target()).exists)
                confirm_target_hit(target_state(runtime, shot.target()), record);
        } else if (!(shot.flags() & 0x20U))
            classify_shot_ray_body(state, record, origin_argument, direction_argument, runtime,
                                   values);
        erase_shot(state, 0);
    }
    if (state.pending.empty())
        state.fire_snapshots.clear();
    else if (state.recent_position_count == 1) {
        ShotFeedbackRecord* candidate = nullptr;
        unsigned count = 0;
        for (auto& record : state.pending)
            if (ShotRecordView{record}.command() == runtime.engine.current_command()) {
                if (++count == 2)
                    break;
                candidate = &record;
            }
        if (count == 1 && candidate) {
            ShotRecordView shot{*candidate};
            if (!(shot.flags() & 0x20U) &&
                distance_squared_yxz(difference(state.recent_positions[1], shot.target_point())) <
                    3600.0f) {
                const auto entity = runtime.engine.entity_by_index(shot.target());
                if (entity.exists && entity.health < shot.target_health()) {
                    const auto local = runtime.engine.local_player();
                    if (local.weapon_handle != 0xffffffffU) {
                        const auto weapon = runtime.engine.entity_by_handle(local.weapon_handle);
                        if (weapon.exists &&
                            lagcomp::time_to_tick(weapon.simulation_time, values.interval()) >=
                                shot.shot_tick() &&
                            lagcomp::time_to_tick(entity.last_shot_time, values.interval()) ==
                                shot.shot_tick()) {
                            confirm_target_hit(target_state(runtime, shot.target()), *candidate);
                            shot.flags() |= 0x20U;
                        }
                    }
                }
            }
        }
    }
    finish();
}

bool shot_record_expired(const ShotRecordView& shot, const client::PlayerState& target,
                         ShotFeedbackRuntime& runtime) {
    SnapshotFeedbackValues values{runtime};
    return shot_record_expired_body(shot, target, runtime, values);
}
bool shot_record_expired(const ShotRecordView& shot, const client::PlayerState& target,
                         ShotFeedbackRuntime& runtime, const rage::FeedbackEntityBindings& values) {
    return shot_record_expired_body(shot, target, runtime, values);
}
void classify_shot_ray(ShotFeedbackState& state, ShotFeedbackRecord& record,
                       const RageVector* origin, const RageVector* direction,
                       ShotFeedbackRuntime& runtime) {
    SnapshotFeedbackValues values{runtime};
    classify_shot_ray_body(state, record, origin, direction, runtime, values);
}
void classify_shot_ray(ShotFeedbackState& state, ShotFeedbackRecord& record,
                       const RageVector* origin, const RageVector* direction,
                       ShotFeedbackRuntime& runtime, const rage::FeedbackEntityBindings& values) {
    classify_shot_ray_body(state, record, origin, direction, runtime, values);
}
void reconcile_shot_feedback(ShotFeedbackState& state, ShotFeedbackRuntime& runtime) {
    SnapshotFeedbackValues values{runtime};
    reconcile_shot_feedback_body(state, runtime, values);
}
void reconcile_shot_feedback(ShotFeedbackState& state, ShotFeedbackRuntime& runtime,
                             const rage::FeedbackEntityBindings& values) {
    reconcile_shot_feedback_body(state, runtime, values);
}
} // namespace skeet
