#include "skeet/lagcomp/history_selection.hpp"
#include "skeet/lagcomp/live_record_attachments.hpp"
#include "skeet/lagcomp/record_validity.hpp"
#include "skeet/rage/targeting.hpp"
#include "skeet/movement/extrapolation.hpp"
#include "skeet/movement/extrapolation_model.hpp"
#include "skeet/client/command_orchestrator.hpp"
#include <bit>
#include <cmath>
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
std::int32_t subtract(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
float distance(const LagRecord& first, const LagRecord& second) noexcept {
    const auto& a = first.header.origin;
    const auto& b = second.header.origin;
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return (y * y + x * x) + z * z;
}
ExtrapolationPlan live_plan(const RecordRing& ring, std::uint32_t ordinal, std::uint32_t options,
                            float interval, const client::CommandBudgetBindings& budget) noexcept {
    ExtrapolationPlan result;
    if (!(options & 1u) || ordinal || ring.count < 2)
        return result;
    const auto& newest = ring.at(0);
    const auto& previous = ring.at(1);
    if (newest.header.flags & 0x200u)
        return result;
    if (newest.header.player_flags & 1u) {
        if (previous.header.player_flags & 1u)
            return result;
        const auto weight = read<float>(&newest, 0xFC), rate = read<float>(&newest, 0x104);
        if (weight > 0.0f && (std::bit_cast<std::uint32_t>(rate) & 0x7FFFFFFFu) &&
            time_to_tick(read<float>(&newest, 0x108) / rate, interval) > 1)
            return result;
    }
    const auto* first = &newest;
    const auto* second = &previous;
    if (!(distance(newest, previous) > 4096.0f)) {
        if (ring.count == 2 || (previous.header.flags & 0x200u))
            return result;
        result.inspected_third = true;
        const auto& third = ring.at(2);
        if (!(distance(previous, third) > 4096.0f))
            return result;
        const auto& a = newest.header.velocity;
        const auto& b = previous.header.velocity;
        if (std::sqrt(b[0] * b[0] + b[1] * b[1]) - std::sqrt(a[0] * a[0] + a[1] * a[1]) > 50.0f)
            return result;
        first = &previous;
        second = &third;
    }
    // The native433A13ED query is reached only after all preceding gates.
    const auto limit = client::query_command_budget(budget);
    const auto gap = subtract(first->header.simulation_tick, second->header.simulation_tick);
    result.ticks = limit < 1 ? limit : gap < 1 ? 1 : gap > limit ? limit : gap;
    return result;
}
}
bool select_target_history_record(
    client::CommandFrame& frame, void* player, std::int32_t requested_tick, std::uint32_t options,
    LagRecord& destination, std::uint32_t& output_flags, const HistorySelectionBindings& bindings,
    const animation::BoneRuntime& bones, const MovementExtrapolationLiveBindings& movement,
    const client::CommandBudgetBindings& budget, const LagRecord** selected_source,
    std::int32_t* predicted_ticks, RebuildUsage* usage) {
    if (usage)
        *usage = {};
    if (predicted_ticks)
        *predicted_ticks = 0;
    const auto& extrapolation = bindings.extrapolation;
    const auto& entity = extrapolation.preparation.entity;
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    auto& owner = *extrapolation.players.get(index);
    if (selected_source)
        *selected_source = nullptr;
    rage::TargetLagRecordHistory traversal{owner.native.cached_origin, owner.history.first,
                                           owner.history.count, owner.history.records};
    const auto choice = rage::prepare_target_record_copy(traversal, requested_tick, entity.interval,
                                                         options, destination, output_flags);
    if (!choice.source)
        return false;
    if (extrapolation.validate_record &&
        (!record_geometry_valid(*choice.source) ||
         record_precedes_discontinuity(owner.history, choice.source->header.simulation_tick)))
        return false;
    const bool rebuild_pose = bool(extrapolation.rebuild_pose);
    const auto plan = rebuild_pose ? ExtrapolationPlan{}
                                   : live_plan(owner.history, choice.ordinal, options, entity.interval, budget);
    const HistoryTickTiming timing{bindings.current_time,
                                   read<float>(&frame, 0x198),
                                   bindings.interpolation,
                                   entity.interval,
                                   bindings.maximum_unlag_ms,
                                   read<std::int32_t>(&frame, 0xD4),
                                   std::bit_cast<std::int32_t>(frame.field_18c)};
    const auto window = history_tick_window(timing);
    auto effective_tick = requested_tick;
    auto animation_rebuilt = owner.animation_rebuilt(choice.ordinal);
    if (const auto* older = history_interpolation_source(
            owner.history, choice.ordinal, *choice.source, requested_tick, plan.ticks, window)) {
        const bool use_endpoint = bindings.use_captured_endpoint_pose();
        const auto fraction = interpolate_history_record(
            destination, *choice.source, *older,
            float(requested_tick) * entity.interval,
            entity.interval, bindings.matrix_scratch, output_flags, bindings.rotation,
            use_endpoint);
        if (!(use_endpoint && fraction == 1.0f) &&
            (older->header.flags & 4u))
            animation_rebuilt = owner.animation_rebuilt(choice.ordinal + 1u);
        effective_tick = window.last;
    }
    const bool had_alternatives = (choice.source->header.flags & 0x80u) != 0;
    if (rebuild_pose && effective_tick > choice.source->header.simulation_tick) {
        const auto ticks = std::int64_t(effective_tick) - choice.source->header.simulation_tick;
        if (!(options & 1u) || choice.ordinal || ticks > maximum_pose_extrapolation_ticks ||
            ticks > client::query_command_budget(budget) ||
            extrapolate_lag_record(player, owner, destination, std::int32_t(ticks),
                                    extrapolation.movement, movement, true) != ticks)
            return false;
        output_flags |= 0x20u;
    } else if (!rebuild_pose && (options & 1u) && !choice.ordinal && owner.history.count > 1) {
        if (plan.ticks > 0) {
            const auto result = model_alternative_history_record(frame, player, owner, destination,
                                                                 plan.ticks, bindings.alternative);
            if (result < 0)
                return false;
            if (result > 0 || (owner.native.flags & 0x100u))
                output_flags |= 0x40u;
        } else {
            const auto newest_tick = owner.history.at(0).header.simulation_tick;
            const auto ticks = subtract(effective_tick, newest_tick);
            if (effective_tick > newest_tick && ticks > 0) {
                const auto result = extrapolate_lag_record(player, owner, destination, ticks,
                                                           extrapolation.movement, movement);
                if (result < 0)
                    return false;
                if (predicted_ticks && result > 0)
                    *predicted_ticks = result;
                if (result > 0)
                    output_flags |= 0x20u;
            }
        }
    }
    if (output_flags & 0x60u) {
        destination.header.field_00 = 0;
        build_extrapolated_record_matrices(player, destination, bones,
                                           extrapolation.attachments.scratch,
                                           rebuild_pose, entity.interval);
        if (rebuild_pose) {
            if (!(destination.header.flags & 4u))
                return false;
        } else if (had_alternatives && !(output_flags & 0x40u)) {
            const auto input = capture_record_hypothesis_input(player, owner, destination, entity);
            resolver_runtime::build_hypotheses(destination, input, player, bones);
        }
    }
    auto preparation = extrapolation.preparation;
    preparation.current_timing = &timing;
    refresh_record_attachments(extrapolation.players, player, destination, preparation,
                               extrapolation.attachments, bones, rebuild_pose && (output_flags & 0x20u));
    if (rebuild_pose && (output_flags & 0x20u)) {
        if (!record_geometry_valid(destination))
            return false;
        if (predicted_ticks)
            *predicted_ticks = subtract(destination.header.simulation_tick, choice.source->header.simulation_tick);
    }
    if (extrapolation.validate_record &&
        (!record_time_valid(destination, timing) || !record_geometry_valid(destination)))
        return false;
    if (selected_source && destination.header.field_00)
        *selected_source = choice.source;
    if (usage)
        *usage = {animation_rebuilt, rebuild_pose && bool(output_flags & 0x20u)};
    return true;
}
} //namespace skeet::lagcomp
