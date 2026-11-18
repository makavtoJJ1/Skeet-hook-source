#include "skeet/lagcomp/history_selection.hpp"
#include "skeet/lagcomp/record_validity.hpp"
#include "skeet/rage/targeting.hpp"
#include "skeet/movement/extrapolation.hpp"
#include <bit>
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
std::int32_t subtract(std::int32_t first, std::int32_t second) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(first) - std::uint32_t(second));
}
}
bool select_target_history_record(client::CommandFrame& frame, void* player,
                                  std::int32_t requested_tick, std::uint32_t options,
                                  LagRecord& destination, std::uint32_t& output_flags,
                                  const HistorySelectionBindings& bindings,
                                  const LagRecord** selected_source) {
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
    const auto plan =
        plan_history_extrapolation(owner.history, choice.ordinal, options, entity.interval,
                                   extrapolation.prediction_limit.value());
    const HistoryTickTiming timing{bindings.current_time,
                                   read<float>(&frame, 0x198),
                                   bindings.interpolation,
                                   entity.interval,
                                   bindings.maximum_unlag_ms,
                                   read<std::int32_t>(&frame, 0xD4),
                                   std::bit_cast<std::int32_t>(frame.field_18c)};
    const auto window = history_tick_window(timing);
    auto effective_tick = requested_tick;
    if (const auto* older = history_interpolation_source(
            owner.history, choice.ordinal, *choice.source, requested_tick, plan.ticks, window)) {
        //C977 rewrites the later extrapolation tick, while SP2C still contains
        // the original requested time consumed by the field interpolation.
        interpolate_history_record(
            destination, *choice.source, *older,
            float(requested_tick) * entity.interval,
            entity.interval, bindings.matrix_scratch, output_flags, bindings.rotation,
            bindings.use_captured_endpoint_pose());
        effective_tick = window.last;
    }
    const bool had_alternatives = (choice.source->header.flags & 0x80u) != 0;
    if ((options & 1u) && !choice.ordinal && owner.history.count > 1) {
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
                                                           extrapolation.movement);
                if (result < 0)
                    return false;
                if (result > 0)
                    output_flags |= 0x20u;
            }
        }
    }
    if (output_flags & 0x60u) {
        destination.header.field_00 = 0;
        build_extrapolated_record_matrices(destination, extrapolation.bones.decoded(),
                                           extrapolation.attachments.scratch);
        if (had_alternatives && !(output_flags & 0x40u)) {
            const auto input = capture_record_hypothesis_input(player, owner, destination, entity);
            const auto origin = read<Vector>(player, entity.offsets.origin);
            resolver_runtime::HypothesisBuildContext context{
                extrapolation.bones.decoded().entity,
                extrapolation.bones.decoded().model,
                origin,
                extrapolation.bones.decoded().pose_parameters,
                extrapolation.bones.decoded().engine,
                extrapolation.bones.decoded().trigonometry};
            resolver_runtime::build_hypotheses(destination, input, context);
        }
    }
    auto preparation = extrapolation.preparation;
    preparation.current_timing = &timing;
    refresh_record_attachments(extrapolation.players, player, destination, preparation,
                               extrapolation.attachments);
    if (extrapolation.validate_record &&
        (!record_time_valid(destination, timing) || !record_geometry_valid(destination)))
        return false;
    if (selected_source && destination.header.field_00)
        *selected_source = choice.source;
    return true;
}
} // namespace skeet::lagcomp
