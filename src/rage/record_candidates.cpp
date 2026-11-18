#include "skeet/rage/record_candidates.hpp"
#include "skeet/rage/acquisition.hpp"
#include "skeet/client/command_orchestrator.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/lagcomp/record_validity.hpp"
#include "skeet/movement/extrapolation_model.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <xmmintrin.h>

namespace skeet::rage {
namespace {
std::int32_t integer(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return integer(std::uint32_t(a) + std::uint32_t(b));
}
std::int32_t sub(std::int32_t a, std::int32_t b) noexcept {
    return integer(std::uint32_t(a) - std::uint32_t(b));
}
std::int32_t absolute(std::int32_t value) noexcept {
    return value < 0 ? sub(0, value) : value;
}
template <class T> T field(const void* object, std::size_t offset) noexcept {
    T out;
    std::memcpy(&out, static_cast<const std::byte*>(object) + offset, sizeof out);
    return out;
}
std::int32_t convar_integer(const void* object) noexcept {
    for (;;) {
        const auto parent = field<const void*>(object, 0x1c);
        if (parent == object)
            break;
        object = parent;
    }
    return integer(field<std::uint32_t>(object, 0x30) ^
                   std::uint32_t(reinterpret_cast<std::uintptr_t>(object)));
}
std::int32_t rounded(float value) noexcept {
    return _mm_cvtss_si32(_mm_set_ss(value));
}
float converted(std::int32_t value) noexcept {
    return _mm_cvtss_f32(_mm_cvtsi32_ss(_mm_setzero_ps(), value));
}
std::int32_t ceiling_tick(float value) noexcept {
    const auto result = rounded(value);
    return add(result, value > converted(result) ? 1 : 0);
}
std::int32_t floor_tick(float value) noexcept {
    const auto result = rounded(value);
    return sub(result, converted(result) > value ? 1 : 0);
}
std::int32_t cap_gap(std::int32_t value, std::int32_t limit) noexcept {
    if (limit < 1)
        return limit;
    return std::clamp(value, 1, limit);
}
lagcomp::LagTimeline timeline_view(const client::PlayerState& player) noexcept {
    lagcomp::LagTimeline out{};
    out.simulation_time = field<float>(&player.native, 0x110);
    out.last_update_tick = field<std::int32_t>(&player.native, 0x168);
    out.packet_head = field<std::uint32_t>(&player.native, 0x234);
    out.packet_count = field<std::uint32_t>(&player.native, 0x238);
    for (std::size_t i = 0; i < 6; ++i)
        out.packets[i] = field<lagcomp::PacketTimingSample>(&player.native, 0x23c + i * 0x20);
    out.record_head = player.history.first;
    out.record_count = player.history.count;
    for (std::size_t i = 0; i < 32; ++i)
        out.records[i] = player.history.records[i];
    return out;
}
struct Window {
    float low, high;
    std::int32_t corrected, upper;
};
lagcomp::HistoryTickTiming candidate_timing(const client::CommandFrame& frame,
                                            const RecordCandidateBindings& bindings) noexcept {
    return {bindings.current_time, field<float>(&frame, 0x198), bindings.interpolation,
            bindings.interval, bindings.maximum_unlag_ms, field<std::int32_t>(&frame, 0xd4),
            integer(frame.field_18c)};
}
Window candidate_window(const client::CommandFrame& frame,
                        const RecordCandidateBindings& bindings) noexcept {
    const float maximum = converted(bindings.maximum_unlag_ms) * 0.001F;
    const float delay =
        minimum_number(field<float>(&frame, 0x198) + bindings.interpolation, maximum);
    const float low = ((bindings.current_time - delay) - 0.2F) / bindings.interval;
    const float high = ((bindings.current_time - delay) + 0.2F) / bindings.interval;
    return {low, high, lagcomp::history_tick_window(candidate_timing(frame, bindings)).corrected,
            add(field<std::int32_t>(&frame, 0x144), integer(frame.field_18c))};
}
std::int32_t extrapolated_tick(const client::CommandFrame& frame, void* target,
                               const client::PlayerState& player,
                               const RecordCandidateBindings& bindings, const Window& window) {
    const auto& ring = player.history;
    const auto& newest = ring.at(0).header;
    const auto now = add(field<std::int32_t>(&frame, 0xd4), integer(frame.field_18c));
    const auto arrival = lagcomp::receive_time_to_tick(
        engine::invoke<float>(bindings.engine_client, 14), bindings.receive_interval);
    auto receive_position = field<std::int32_t>(&player.native, 0x168);
    const auto arrival_gap = sub(arrival, receive_position);
    if (absolute(sub(now, newest.simulation_tick)) > 127)
        return -1;
    const auto timeline = timeline_view(player);
    const auto upper = std::min(window.upper, floor_tick(window.high));
    const auto lower = ceiling_tick(window.low);
    const auto simulation = lagcomp::time_to_tick(timeline.simulation_time, bindings.interval);
    bool simple = ring.count < 2 || timeline.packet_count < 2;
    if (!simple && simulation < sub(timeline.last_update_tick, 1) &&
        simulation < newest.simulation_tick)
        simple = true;
    if (!simple && lagcomp::packet_timing_discontinuity(timeline))
        simple = true;
    if (!simple && (newest.flags & 0x200U))
        simple = true;
    if (!simple) {
        const auto first = timeline.packet_head % 6U, next = (timeline.packet_head + 1U) % 6U;
        if (sub(timeline.packets[first].field_18, timeline.packets[next].field_18) < 4 &&
            sub(newest.receive_tick, ring.at(1).header.receive_tick) > 7)
            simple = true;
    }
    std::array<std::int32_t, 10> pattern{};
    std::size_t count = 0;
    if (simple)
        pattern[count++] = 2;
    else {
        const lagcomp::LagRecordHeader* previous = nullptr;
        for (std::uint32_t age = 1; age < ring.count; ++age) {
            const auto& record = ring.at(age).header;
            if (previous && !(previous->flags & 8U)) {
                pattern[count++] = cap_gap(
                    sub(previous->simulation_tick, record.simulation_tick),
                    (bindings.live_budget ? client::query_command_budget(*bindings.live_budget)
                                          : bindings.prediction_limit.value()));
                if (count == 10)
                    break;
            }
            if (record.flags & 0x200U)
                break;
            previous = &record;
        }
        const auto newest_gap =
            cap_gap(sub(newest.simulation_tick, ring.at(1).header.simulation_tick),
                    (bindings.live_budget ? client::query_command_budget(*bindings.live_budget)
                                          : bindings.prediction_limit.value()));
        const auto received = lagcomp::receive_time_to_tick(
            engine::invoke<float>(bindings.engine_client, 14), bindings.receive_interval);
        const auto receive_gap =
            cap_gap(add(sub(received, newest.receive_tick), 1),
                    (bindings.live_budget ? client::query_command_budget(*bindings.live_budget)
                                          : bindings.prediction_limit.value()));
        if (count < 10)
            pattern[count++] = std::max(newest_gap, receive_gap);
    }
    std::uint32_t ordinal = 0;
    auto result = newest.simulation_tick;
    while (result < window.corrected || result < lower || result > upper) {
        const auto step = pattern[ordinal % count];
        receive_position = add(receive_position, step);
        const auto proposed = add(result, step);
        if (receive_position > now)
            break;
        if (receive_position == now) {
            const auto index =
                engine::invoke<std::int32_t>(static_cast<std::byte*>(target) + 8, 10);
            if (engine::invoke<std::int32_t>(bindings.engine_client, 12) < index)
                break;
        }
        if (upper <= proposed)
            break;
        ++ordinal;
        result = proposed;
    }
    result = std::min(result, upper);
    if (newest.simulation_tick < result && arrival_gap > 2)
        return -1;
    return result;
}
void append_masked_ticks(const lagcomp::RecordRing& ring, std::uint32_t mask, std::uint32_t maximum,
                         std::int32_t preferred, std::vector<std::int32_t>& output) {
    const auto emit = [&](std::uint32_t age) {
        output.push_back(ring.at(age).header.simulation_tick);
        mask &= ~(1U << (age & 31U));
    };
    if (maximum > 3) {
        const auto original_count = std::popcount(mask);
        if (std::uint32_t(original_count) <= maximum) {
            for (std::uint32_t i = 0; i < ring.count; ++i)
                if (mask & (1U << (i & 31U)))
                    emit(i);
            return;
        }
        for (std::uint32_t i = 0; i < ring.count; ++i)
            if (mask & (1U << (i & 31U))) {
                emit(i);
                if (output.size() > 1)
                    break;
            }
        if (!mask || output.size() >= maximum)
            return;
        for (std::uint32_t i = ring.count; i > 0; --i)
            if (mask & (1U << ((i - 1U) & 31U))) {
                emit(i - 1U);
                if (output.size() > 3)
                    break;
            }
        if (!mask || output.size() >= maximum)
            return;
        for (std::uint32_t i = 0; i < ring.count; ++i)
            if ((mask & (1U << (i & 31U))) && (ring.at(i).header.flags & 8U)) {
                emit(i);
                break;
            }
        if (!mask || output.size() >= maximum)
            return;
        for (std::uint32_t i = 0; i < ring.count; ++i)
            if ((mask & (1U << (i & 31U))) && (ring.at(i).header.flags & 0x20U)) {
                emit(i);
                break;
            }
        if (!mask || output.size() >= maximum)
            return;
        std::uint32_t ordinal = 0;
        for (std::uint32_t i = 0; i < ring.count; ++i)
            if (mask & (1U << (i & 31U))) {
                if (++ordinal >= std::uint32_t(original_count >> 1)) {
                    emit(i);
                    return;
                }
            }
        return;
    }
    if (preferred != 0 && maximum == 1)
        for (std::uint32_t i = 0; i < ring.count; ++i)
            if ((mask & (1U << (i & 31U))) && ring.at(i).header.simulation_tick == preferred) {
                emit(i);
                return;
            }
    // The first eligible tick is emitted even for maximum==0.
    for (std::uint32_t i = 0; i < ring.count; ++i)
        if (mask & (1U << (i & 31U))) {
            emit(i);
            break;
        }
    if (!mask || output.size() >= maximum)
        return;
    for (std::uint32_t i = ring.count - 1U; i > 0; --i)
        if (mask & (1U << (i & 31U))) {
            emit(i);
            break;
        }
    if (!mask || output.size() >= maximum)
        return;
    for (std::uint32_t i = 0; i < ring.count; ++i)
        if ((mask & (1U << (i & 31U))) && (ring.at(i).header.flags & 0x20U)) {
            emit(i);
            return;
        }
}
}
void append_target_record_ticks(const client::CommandFrame& frame, void* target,
                                client::PlayerState& player, std::uint32_t maximum,
                                const RecordCandidateBindings& bindings,
                                std::vector<std::int32_t>& output) {
    const auto& ring = player.history;
    if (!ring.count)
        return;
    const auto entity_index = engine::invoke<std::int32_t>(static_cast<std::byte*>(target) + 8, 10);
    if (convar_integer(bindings.lag_compensation) == 0 ||
        !target_history_is_usable(player.native, ring, frame, std::uint32_t(entity_index),
                                  bindings.interval)) {
        output.push_back(ring.at(0).header.simulation_tick);
        return;
    }
    const auto window = candidate_window(frame, bindings);
    const auto raw_first = add(ceiling_tick(window.low), 1);
    const auto first = std::max(window.corrected, raw_first);
    const auto last = std::min(window.upper, sub(floor_tick(window.high), 1));
    const bool validate = bool(bindings.validate_record);
    const auto timing = candidate_timing(frame, bindings);
    std::uint32_t mask = 0;
    bool extrapolate = false;
    auto previous = player.native.cached_origin;
    for (std::uint32_t age = 0; age < ring.count; ++age) {
        const auto& sample = ring.at(age);
        const auto& record = sample.header;
        if (validate && lagcomp::record_precedes_discontinuity(ring, record.simulation_tick))
            break;
        const auto& origin = record.origin;
        const float dx = origin[0] - previous[0], dy = origin[1] - previous[1],
                    dz = origin[2] - previous[2];
        if ((dy * dy + dx * dx) + dz * dz > 4096.0F) {
            if (!validate && !mask)
                mask = 1;
            break;
        }
        if (record.simulation_tick <= last) {
            if (record.simulation_tick < first) {
                if (age == 0 && raw_first <= record.simulation_tick)
                    extrapolate = true;
                break;
            }
            if (!validate || (lagcomp::record_time_valid(sample, timing) &&
                              lagcomp::record_geometry_valid(sample)))
                mask |= 1U << (age & 31U);
        }
        previous = origin;
    }
    if (!mask) {
        if (!extrapolate || ring.count < 2 || (ring.at(0).header.flags & 0x200U))
            return;
        if (bindings.rebuild_pose) {
            const auto ticks = std::int64_t(first) - ring.at(0).header.simulation_tick;
            if (ticks > 0 && ticks <= maximum_pose_extrapolation_ticks && first <= last)
                output.push_back(first);
            return;
        }
        const auto tick = extrapolated_tick(frame, target, player, bindings, window);
        if (tick != -1)
            output.push_back(tick);
        return;
    }
    if (frame.weapon_kind_124 == 0 || frame.weapon_id_d8 == 0x1f) {
        const lagcomp::LagRecordHeader* chosen = &ring.at(0).header;
        float best = std::numeric_limits<float>::max();
        for (std::uint32_t age = 0; age < ring.count; ++age)
            if (mask & (1U << (age & 31U))) {
                const auto& record = ring.at(age).header;
                const float dx = record.origin[0] - frame.origin_dc[0],
                            dy = record.origin[1] - frame.origin_dc[1];
                const float dz =
                    (record.collision_max_z * 0.5F + record.origin[2]) - frame.origin_dc[2];
                const float distance = (dy * dy + dx * dx) + dz * dz;
                if (distance < best) {
                    best = distance;
                    chosen = &record;
                }
            }
        output.push_back(chosen->simulation_tick);
        return;
    }
    append_masked_ticks(ring, mask, maximum, field<std::int32_t>(&player.native, 0x194), output);
}
std::vector<std::int32_t> collect_target_record_ticks(const client::CommandFrame& frame,
                                                      void* target, client::PlayerState& player,
                                                      std::uint32_t maximum,
                                                      const RecordCandidateBindings& bindings) {
    std::vector<std::int32_t> output;
    append_target_record_ticks(frame, target, player, maximum, bindings, output);
    return output;
}
} // namespace skeet::rage
