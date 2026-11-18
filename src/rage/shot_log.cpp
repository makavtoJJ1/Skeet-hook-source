#include "skeet/rage/shot_log.hpp"
#include "skeet/rage/acquisition.hpp"
#include "skeet/rage/settings.hpp"
#include "skeet/rage/shot_feedback.hpp"
#include "skeet/misc/logs.hpp"
#include <bit>

namespace skeet::rage {
void log_shot_fire(const ShotLogSettings& settings, misc::NormalLog& output,
                   const client::CommandFrame& frame, const client::Command& command,
                   const TargetCandidate& candidate, const AcquisitionEvaluation& evaluation,
                   std::int32_t id, bool pending) {
    if (!settings.detailed.value)
        return;
    const auto& point = candidate.primary;
    const auto& record = evaluation.record.header;
    const auto policy = evaluation.settings.scan_policy();
    output.console_format(
        "[rage] FIRE id=%d cmd=%d target=%u hp=%d record_tick=%d selected_tick=%d cmd_tick=%d "
        "hitbox=%d group=%d point=(%.2f,%.2f,%.2f) damage=%d/min%d hc=%.2f%%/min%.2f%% "
        "spread=%.6f inaccuracy=%.6f safe=%u boosted=%u prefer_body=%u prefer_safe=%u "
        "point_rank=0x%X target_rank=0x%X scans=%u feedback=%s",
        id, frame.command_number_ac, candidate.entity_index, candidate.health,
        record.simulation_tick, evaluation.requested_tick, command.tick, point.hitbox,
        point.hitgroup, point.position[0], point.position[1], point.position[2], point.damage,
        policy.minimum_damage, point.chance * 100.0f, policy.minimum_chance * 100.0f,
        frame.spread_d0, frame.inaccuracy_c4, unsigned(bool(point.flags & 1u)),
        unsigned(bool(point.flags & 4u)), unsigned(candidate.prefer_body),
        unsigned(candidate.prefer_safe), point.priority, candidate.priority,
        evaluation.ballistic_evaluations, pending ? "pending" : "unavailable");
    output.console_format(
        "[rage] RECORD id=%d cmd=%d recv_tick=%d record_cmd_tick=%d origin=(%.2f,%.2f,%.2f) "
        "velocity=(%.2f,%.2f,%.2f) duck=%.3f eye_yaw=%.2f anim_yaw=%.2f "
        "resolver_mode=%u attempt=%u state=%u flags=0x%X extrapolated=%u extra_ticks=%d",
        id, frame.command_number_ac, record.receive_tick, record.command_tick,
        record.origin[0], record.origin[1], record.origin[2], record.velocity[0],
        record.velocity[1], record.velocity[2], record.duck_amount,
        std::bit_cast<float>(record.player_history_1ac), record.animation_yaw,
        unsigned(record.resolver_index), unsigned(record.resolver_mode),
        unsigned(record.resolver_state), record.flags, unsigned(evaluation.extrapolated),
        evaluation.extrapolation_ticks);
}

void log_shot_result(const ShotLogSettings& settings, misc::NormalLog& output,
                     const ShotRecordView& shot, bool hit, std::uint32_t reason,
                     std::int32_t target, std::int32_t group, std::int32_t damage) {
    if (!settings.detailed.value)
        return;
    const auto event = shot.script_shot();
    // Choke reconciliation can change words[3]; words[1] is the firing command.
    const auto command = std::bit_cast<std::int32_t>(shot.record.words[1]);
    const auto& record = shot.record.state.record.header;
    const auto point = shot.target_point();
    constexpr const char* reasons[]{"unconfirmed-pose-or-registration",
        "ray-model-mismatch(spread-or-obstruction)", "prediction-error", "death",
        "target-state-changed"};
    output.console_format(
        "[rage] %s id=%d cmd=%d target=%d actual_target=%d record_tick=%d selected_tick=%d "
        "aim_group=%d hit_group=%d expected_damage=%d damage=%d hc=%.2f%% "
        "point=(%.2f,%.2f,%.2f) resolver_mode=%u attempt=%u state=%u flags=0x%X reason=%s",
        hit ? "HIT" : reason == 5 ? "UNCONFIRMED" : "MISS", event.id, command, event.target, hit ? target : 0,
        record.simulation_tick, event.tick, event.hitgroup, hit ? group : 0,
        std::bit_cast<std::int32_t>(shot.record.state.words[10]), hit ? damage : 0,
        event.hit_chance * 100.0f, point[0], point[1], point[2],
        unsigned(record.resolver_index), unsigned(record.resolver_mode),
        unsigned(record.resolver_state), record.flags,
        hit ? "player_hurt" : reason == 5 ? "health-change-without-hurt-event"
                                         : reasons[reason < 5 ? reason : 0]);
}
}
