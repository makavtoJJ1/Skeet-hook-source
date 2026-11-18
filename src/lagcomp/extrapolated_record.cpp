#include "skeet/lagcomp/extrapolated_record.hpp"
#include "skeet/lagcomp/packet_capture.hpp"
#include "skeet/lagcomp/live_record_attachments.hpp"
#include "skeet/lagcomp/live_timeline.hpp"
#include "skeet/lagcomp/receive_timing.hpp"
#include "skeet/lagcomp/record_validity.hpp"
#include "skeet/client/command_orchestrator.hpp"
#include "skeet/movement/extrapolation.hpp"
#include "skeet/movement/extrapolation_model.hpp"
#include "skeet/animation/record_pose_scope.hpp"
#include "skeet/resolver_runtime/build_validation.hpp"
#include <bit>
#include <cmath>
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T>
[[gnu::always_inline]] inline T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
std::int32_t decrement(std::int32_t value) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(value) - 1u);
}
struct RecordCaptureResult {
    client::PlayerState& owner;
    HistoryTickTiming timing;
    std::int32_t changed;
    std::int32_t ticks;
};
template <bool Live>
RecordCaptureResult capture_extrapolation_source(client::CommandFrame& frame, void* player,
                                                 LagRecord& destination,
                                                 const ExtrapolatedRecordBindings& bindings) {
    const auto index = engine::invoke<std::uint32_t>(static_cast<std::byte*>(player) + 8, 10);
    auto& owner = *bindings.players.get(index);
    const auto timeline = history_timeline(owner);
    const auto& entity = bindings.preparation.entity;
    bool capture = timeline.record_count == 0;
    if (!capture) {
        const auto tick = time_to_tick(timeline.simulation_time, entity.interval);
        capture = tick < decrement(timeline.last_update_tick) &&
                  tick < timeline.at(0).header.simulation_tick;
    }
    std::int32_t changed = 0;
    if (capture) {
        if constexpr (Live)
            entity.capture_record(destination, player, bindings.players);
        else
            entity.capture_record(destination, player, owner.capture_state());
        changed = 1;
    } else
        std::memcpy(&destination, &timeline.at(0), sizeof destination);
    HistoryTickTiming current{};
    current.tick_d4 = read<std::int32_t>(&frame, 0xD4);
    current.tick_18c = std::bit_cast<std::int32_t>(frame.field_18c);
    return {owner, current, changed, 0};
}
MissingTickTiming missing_tick_timing(const RecordCaptureResult& captured, void* player,
                                      const ExtrapolatedRecordBindings& bindings,
                                      std::int32_t limit) noexcept {
    const auto& entity = bindings.preparation.entity;
    return {captured.timing.tick_d4,
            captured.timing.tick_18c,
            entity.interval,
            entity.receive_interval,
            limit,
            entity.engine,
            player};
}
RecordCaptureResult prepare_extrapolated_record(client::CommandFrame& frame, void* player,
                                                LagRecord& destination,
                                                const ExtrapolatedRecordBindings& bindings) {
    auto captured = capture_extrapolation_source<false>(frame, player, destination, bindings);
    captured.ticks = estimate_missing_ticks(
        history_timeline(captured.owner),
        missing_tick_timing(captured, player, bindings, bindings.prediction_limit.value()));
    return captured;
}
RecordCaptureResult prepare_extrapolated_record(client::CommandFrame& frame, void* player,
                                                LagRecord& destination,
                                                const ExtrapolatedRecordBindings& bindings,
                                                const client::CommandBudgetBindings& budget) {
    auto captured = capture_extrapolation_source<true>(frame, player, destination, bindings);
    if (bindings.rebuild_pose) {
        if (captured.changed)
            return captured;
        const auto& entity = bindings.preparation.entity;
        const auto receive_tick = sample_receive_tick(entity.engine, entity.receive_interval);
        const auto elapsed = std::int64_t(receive_tick) - destination.header.receive_tick;
        const double ticks = double(elapsed) * entity.receive_interval / entity.interval;
        if (!std::isfinite(ticks) || ticks < 0.0 || ticks >= maximum_pose_extrapolation_ticks + 0.5) {
            captured.changed = -1;
            return captured;
        }
        captured.ticks = std::int32_t(std::round(ticks));
        if (captured.ticks > 0 && captured.ticks > client::query_command_budget(budget)) {
            captured.ticks = 0;
            captured.changed = -1;
        }
        return captured;
    }
    captured.ticks =
        estimate_missing_ticks(history_timeline(captured.owner),
                               missing_tick_timing(captured, player, bindings, 0), budget);
    return captured;
}
}

void build_extrapolated_record_matrices(LagRecord& record,
                                        resolver_runtime::HypothesisBuildContext& context,
                                        std::span<Matrix3x4, 256> scratch) {
    record.header.flags &= ~4U;
    const animation::BoneRequest request{{0.0f, record.header.animation_yaw, 0.0f},
                                         record.header.origin,
                                         0.0f,
                                         0x300u,
                                         -1,
                                         0,
                                         false};
    if (animation::build_bone_matrices(context.entity, context.model, request, scratch,
                                       context.engine, context.trigonometry)) {
        animation::select_hitbox_bone_matrices(context.model, scratch, record.matrices);
        record.header.flags = (record.header.flags & ~0x20u) | 4u;
    }
}

void build_extrapolated_record_matrices(void* player, LagRecord& record,
                                        const animation::BoneRuntime& bones,
                                        std::span<Matrix3x4, 256> scratch, bool strict, float interval) {
    record.header.flags &= ~4U;
    if (strict) {
        const auto count = animation::build_captured_hitbox_matrices(
            player, record, bones, scratch, record.matrices, 0x300u,
            float(record.header.simulation_tick) * interval);
        if (resolver_runtime::complete_hitbox_matrices(count, record.matrices))
            record.header.flags = (record.header.flags & ~0x20u) | 4u;
        return;
    }
    const animation::BoneRequest request{{0.0f, record.header.animation_yaw, 0.0f},
                                         record.header.origin,
                                         0.0f,
                                         0x300u,
                                         -1,
                                         0,
                                         false};
    if (bones.build_bone_matrices(player, request, scratch)) {
        bones.select_hitbox_bone_matrices(player, scratch, record.matrices);
        record.header.flags = (record.header.flags & ~0x20u) | 4u;
    }
}

resolver_runtime::HypothesisInput
capture_record_hypothesis_input(void* player, const client::PlayerState& owner,
                                const LagRecord& record,
                                const EntityStateRuntime& entity, bool use_record_yaw) noexcept {
    const auto* animation = read<const void*>(player, entity.offsets.animation_state);
    if (!animation && use_record_yaw)
        return {};
    const auto velocity = read<Vector>(player, entity.offsets.velocity);
    resolver_runtime::HypothesisInput result;
    result.speed_fraction = animation ? read<float>(animation, 0x11C) : 0.0f;
    result.feet_yaw = read<float>(animation, 0x84);
    result.movement_yaw = read<float>(animation, 0x88);
    result.movement_yaw_target = read<float>(animation, 0x8C);
    result.duck_amount = read<float>(animation, 0xA4);
    result.running_fraction = read<float>(animation, 0xF8);
    result.ducking_fraction = read<float>(animation, 0xFC);
    result.minimum_yaw = read<float>(animation, 0x334);
    result.maximum_yaw = read<float>(animation, 0x338);
    result.eye_yaw = use_record_yaw ? std::bit_cast<float>(record.header.player_history_1ac)
                                    : owner.native.reference_yaw;
    result.animation_fraction = read<float>(&owner.native, 0x538);
    result.velocity_x = velocity[0];
    result.velocity_y = velocity[1];
    result.velocity_z = velocity[2];
    result.lower_body_yaw = read<float>(player, entity.offsets.field_a2a8247f);
    result.interval = entity.interval;
    result.record_flags = record.header.flags;
    result.moving = read<std::uint8_t>(animation, 0x108);
    return result;
}

bool capture_extrapolated_record(client::CommandFrame& frame, void* player, LagRecord& destination,
                                 std::uint32_t& output_flags,
                                 const ExtrapolatedRecordBindings& bindings) {
    auto captured = prepare_extrapolated_record(frame, player, destination, bindings);
    if (captured.ticks > 0)
        captured.changed = extrapolate_lag_record(player, captured.owner, destination,
                                                  captured.ticks, bindings.movement);
    auto& owner = captured.owner;
    const auto& entity = bindings.preparation.entity;
    if (captured.changed < 0)
        return false;
    if (captured.changed > 0) {
        output_flags |= 0x20u;
        build_extrapolated_record_matrices(destination, bindings.bones.decoded(),
                                           bindings.attachments.scratch);
        if (!(output_flags & 0x40u)) {
            const auto input = capture_record_hypothesis_input(player, owner, destination, entity);
            const auto origin = read<Vector>(player, entity.offsets.origin);
            resolver_runtime::HypothesisBuildContext context{
                bindings.bones.decoded().entity,
                bindings.bones.decoded().model,
                origin,
                bindings.bones.decoded().pose_parameters,
                bindings.bones.decoded().engine,
                bindings.bones.decoded().trigonometry};
            resolver_runtime::build_hypotheses(destination, input, context);
        }
    }
    auto preparation = bindings.preparation;
    preparation.current_timing = &captured.timing;
    refresh_record_attachments(bindings.players, player, destination, preparation,
                               bindings.attachments);
    return true;
}

namespace {
bool finish_live_record_capture(const RecordCaptureResult& captured, void* player,
                                LagRecord& destination, std::uint32_t& output_flags,
                                const ExtrapolatedRecordBindings& bindings,
                                const animation::BoneRuntime& bones) {
    if (captured.changed < 0)
        return false;
    const bool rebuild_pose = bool(bindings.rebuild_pose);
    if (captured.changed > 0) {
        if (!rebuild_pose || captured.ticks > 0)
            output_flags |= 0x20u;
        if (rebuild_pose)
            destination.header.field_00 = 0;
        build_extrapolated_record_matrices(player, destination, bones,
                                           bindings.attachments.scratch, rebuild_pose,
                                           bindings.preparation.entity.interval);
        if (rebuild_pose) {
            if (!(destination.header.flags & 4u))
                return false;
        } else if (!(output_flags & 0x40u)) {
            const auto input = capture_record_hypothesis_input(
                player, captured.owner, destination, bindings.preparation.entity);
            resolver_runtime::build_hypotheses(destination, input, player, bones);
        }
    }
    auto preparation = bindings.preparation;
    preparation.current_timing = &captured.timing;
    refresh_record_attachments(bindings.players, player, destination, preparation,
                               bindings.attachments, bones, rebuild_pose && captured.changed > 0);
    return !rebuild_pose || captured.changed == 0 || record_geometry_valid(destination);
}
}

bool capture_extrapolated_record(client::CommandFrame& frame, void* player, LagRecord& destination,
                                 std::uint32_t& output_flags,
                                 const ExtrapolatedRecordBindings& bindings,
                                 const animation::BoneRuntime& bones,
                                 const MovementExtrapolationLiveBindings& movement,
                                 const client::CommandBudgetBindings& budget,
                                 std::int32_t* predicted_ticks) {
    if (predicted_ticks)
        *predicted_ticks = 0;
    auto captured = prepare_extrapolated_record(frame, player, destination, bindings, budget);
    const auto source_tick = destination.header.simulation_tick;
    if (captured.ticks > 0)
        captured.changed = extrapolate_lag_record(player, captured.owner, destination,
                                                  captured.ticks, bindings.movement, movement,
                                                  bool(bindings.rebuild_pose));
    if (!finish_live_record_capture(captured, player, destination, output_flags, bindings, bones))
        return false;
    if (predicted_ticks && captured.changed > 0)
        *predicted_ticks = destination.header.simulation_tick - source_tick;
    return true;
}

} // namespace skeet::lagcomp
