#include "skeet/rage/acquisition.hpp"
#include "skeet/lagcomp/record_validity.hpp"
#include "skeet/rage/preamble.hpp"
#include "skeet/rage/feedback_owner.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/movement/extrapolation.hpp"
#include <cstring>

namespace skeet::rage {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
bool convar_nonzero(const void* object) noexcept {
    for (;;) {
        const auto* parent = field<const void*>(object, 0x1C);
        if (parent == object)
            break;
        object = parent;
    }
    return field<std::uint32_t>(object, 0x30) !=
           std::uint32_t(reinterpret_cast<std::uintptr_t>(object));
}
}
void prepare_and_submit_target_records(PreambleState& state, PreambleResult& preamble,
                                       AcquisitionStorage& storage, void* player,
                                       client::PlayerState& cached,
                                       std::span<const std::int32_t> ticks,
                                       const AcquisitionTargetSettings& settings,
                                       std::uint32_t initial_flags,
                                       const AcquisitionRecordBindings& bindings) {
    auto& frame = *preamble.frame;
    auto* network = static_cast<std::byte*>(player) + 8;
    const auto start = state.evaluation_count;
    auto forecast = PoseForecastStatus::no_request;
    for (std::uint32_t age = 0; age < ticks.size(); ++age) {
        const auto ordinal = state.evaluation_count;
        if (ordinal >= storage.candidates.size() || ordinal - start >= settings.record_limit())
            break;
        auto& candidate = storage.candidates[ordinal];
        auto& slot = storage.evaluations[ordinal];
        candidate.flags = 0;
        candidate.entity_index = engine::invoke<std::uint32_t>(network, 10);
        const auto health = engine::invoke<std::int32_t>(player, 122);
        PendingTargetDamage pending;
        const bool has_pending =
            pending_target_damage(feedback_owner_state(bindings.feedback_owner), pending);
        candidate.health =
            scan_target_health(health, candidate.entity_index, has_pending ? &pending : nullptr);
        reset_scan_point(candidate.primary);
        candidate.priority = 0;
        slot.requested_tick = ticks[age];
        slot.body_disabler_reasons = 0;
        slot.ballistic_evaluations = 0;
        slot.extrapolated = false;
        slot.rebuild_usage = {};
        slot.forecast_status = forecast;
        slot.extrapolation_ticks = 0;
        slot.target_handle = *engine::invoke<const std::uint32_t*>(player, 2);
        slot.settings = settings;
        std::uint32_t output_flags = 0;
        bool usable = false;
        if (convar_nonzero(bindings.lag_compensation)) {
            const auto* current = bindings.players.get(candidate.entity_index);
            usable = target_history_is_usable(current->native, current->history, frame,
                                              candidate.entity_index, bindings.interval);
        }
        if (!usable) {
            if (!lagcomp::capture_extrapolated_record(frame, player, slot.record, output_flags,
                                                      bindings.extrapolation, bindings.bone_runtime,
                                                      bindings.live_movement, bindings.live_budget,
                                                      &slot.extrapolation_ticks, &slot.rebuild_usage))
                continue;
            slot.requested_tick = slot.record.header.simulation_tick;
            candidate.flags |= 4U;
            slot.source_record = reinterpret_cast<const lagcomp::LagRecord*>(
                std::uintptr_t(slot.record.header.field_00));
        } else {
            std::uint32_t options = (state.flags & 1U) ? 1U : 5U;
            if (state.flags & 8U)
                options |= 8U;
            const auto handle = bindings.remembered_target_handle == -1
                                    ? 0x1FFFU
                                    : std::uint32_t(bindings.remembered_target_handle) & 0xFFFFU;
            const bool forward_pose = bindings.history.extrapolation.rebuild_pose &&
                cached.history.count && slot.requested_tick > cached.history.at(0).header.simulation_tick;
            if (forward_pose)
                forecast = PoseForecastStatus::rejected;
            if (!forward_pose && !(frame.flags & 0x4000U) && candidate.entity_index != handle)
                options &= ~1U;
            if (!lagcomp::select_target_history_record(
                    frame, player, slot.requested_tick, options, slot.record, output_flags,
                    bindings.history, bindings.bone_runtime, bindings.live_movement,
                    bindings.live_budget, &slot.source_record, &slot.extrapolation_ticks,
                    &slot.rebuild_usage)) {
                slot.requested_tick = -1;
                continue;
            }
        }
        if (bindings.extrapolation.validate_record) {
            const lagcomp::HistoryTickTiming timing{
                bindings.history.current_time, field<float>(&frame, 0x198),
                bindings.history.interpolation, bindings.interval,
                bindings.history.maximum_unlag_ms, std::bit_cast<std::int32_t>(frame.field_d4),
                std::bit_cast<std::int32_t>(frame.field_18c)};
            if (!lagcomp::record_time_valid(slot.record, timing) ||
                !lagcomp::record_geometry_valid(slot.record) ||
                lagcomp::record_precedes_discontinuity(
                    bindings.players.get(candidate.entity_index)->history,
                    slot.record.header.simulation_tick))
                continue;
        }
        slot.extrapolated = slot.extrapolation_ticks > 0;
        auto scan = slot.settings.scan_policy();
        TargetRecordPolicy policy{{scan.hitboxes, slot.record.header.flags, 0, scan.prefer_body},
                                  initial_flags,
                                  scan.additional_hypotheses};
        if (!prepare_target_record_policy(policy, output_flags, age,
                                          std::uint8_t(bindings.body_disablers)))
            break;
        scan.hitboxes = policy.body.hitboxes;
        if (bindings.clear_safe_neck)
            scan.hitboxes.safe &= ~0x200U;
        scan.prefer_body = policy.body.context_ready;
        scan.additional_hypotheses = policy.delay_shot;
        slot.body_disabler_reasons = policy.body.disabler_reasons;
        slot.settings.words[1] = scan.hitboxes.regular;
        slot.settings.words[2] = scan.hitboxes.safe;
        if (output_flags & 0x40U)
            slot.settings.option(0xDD, 1);
        if (!scan.additional_hypotheses)
            slot.settings.option(0xE1, 0);
        candidate.prefer_body = scan.prefer_body;
        candidate.prefer_safe = scan.prefer_safe;
        candidate.weapon_priority_enabled = (slot.record.header.player_flags & 1U) != 0;
        candidate.target_state_flags = slot.record.header.flags;
        submit_acquisition_scan(storage, state, bindings.scanner, player, ordinal,
                                policy.evaluation_flags, bindings.settings_menu);
        if (slot.rebuild_usage.extrapolated_pose)
            forecast = PoseForecastStatus::submitted;
        slot.forecast_status = forecast;
        ++state.evaluation_count;
    }
}
} // namespace skeet::rage
