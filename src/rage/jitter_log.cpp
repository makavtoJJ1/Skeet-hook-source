#include "skeet/rage/jitter_log.hpp"
#include "skeet/rage/acquisition.hpp"
#include "skeet/rage/shot_feedback.hpp"
#include "skeet/misc/logs.hpp"
#include <bit>

namespace skeet::rage {
void log_jitter_fire(client::PlayerState& player, misc::NormalLog& output,
                     const TargetCandidate& candidate, const AcquisitionEvaluation& evaluation,
                     std::int32_t id, std::int32_t command, bool pending) {
    const auto& record = evaluation.record.header;
    const auto& point = candidate.primary;
    const auto policy = evaluation.settings.scan_policy();
    const auto* evidence = player.jitter.find(record);
    const bool applied = evidence && evidence->applied_to(record);
    const auto* decision = !evidence ? "record-evidence-unavailable"
                           : evidence->applied() && !applied ? "record-pose-updated"
                           : resolver_runtime::jitter_decision_name(evidence->decision);
    const auto estimate = evidence ? evidence->estimate : resolver_runtime::JitterPoseEstimate{};
    output.console_status(applied,
        "[jitter] FIRE id=%d cmd=%d target=%u record=%d aim_tick=%d hitbox=%d group=%d "
        "point=(%.1f,%.1f,%.1f) damage=%d/min%d hc=%.1f%%/min%.1f%% safe=%u "
        "prefer_body=%u prefer_safe=%u point_rank=0x%X target_rank=0x%X "
        "eye=%.1f feet=%.1f anchor=%.1f pose_error=%.1f margin=%.1f mode=%u "
        "attempt=%u correction=%s feedback=%s",
        id, command, candidate.entity_index, record.simulation_tick, evaluation.requested_tick,
        point.hitbox, point.hitgroup, point.position[0], point.position[1], point.position[2],
        point.damage, policy.minimum_damage, point.chance * 100, policy.minimum_chance * 100,
        unsigned(bool(point.flags & 1u)), unsigned(candidate.prefer_body),
        unsigned(candidate.prefer_safe), point.priority, candidate.priority,
        std::bit_cast<float>(record.player_history_1ac), record.animation_yaw, estimate.anchor,
        estimate.error, estimate.margin, unsigned(record.resolver_index), unsigned(record.resolver_mode),
        decision,
        pending ? "pending" : "unavailable");
    if (pending)
        player.jitter.shots[player.jitter.next_shot++ % player.jitter.shots.size()] = {
            id, command, evaluation.requested_tick, true, applied};
}
namespace {
resolver_runtime::JitterLoggedShot* find_shot(client::PlayerStateCache& players,
                                              const ShotRecordView& shot) noexcept {
    const auto event = shot.script_shot();
    if (event.target < 1 || event.target > 64)
        return nullptr;
    auto* player = players.find(std::uint32_t(event.target));
    if (!player)
        return nullptr;
    for (auto& entry : player->jitter.shots)
        // words[3] is moved to the send command by choke reconciliation.
        // words[1] retains the original firing command used by FIRE.
        if (entry.id == event.id &&
            entry.command == std::bit_cast<std::int32_t>(shot.record.words[1]) &&
            entry.tick == event.tick)
            return &entry;
    return nullptr;
}
}
bool jitter_shot_used(client::PlayerStateCache& players, const ShotRecordView& shot) noexcept {
    const auto* entry = find_shot(players, shot);
    return entry && entry->resolved;
}
void log_jitter_result(client::PlayerStateCache& players, misc::NormalLog& output,
                       const ShotRecordView& shot, bool hit, std::uint32_t reason,
                       std::int32_t target, std::int32_t group, std::int32_t damage) {
    auto* tracked = find_shot(players, shot);
    if (!tracked || !tracked->pending)
        return;
    tracked->pending = false;
    const auto event = shot.script_shot();
    if (hit) {
        output.console_status(tracked->resolved && target == event.target && damage > 0,
                              "[jitter] HIT id=%d cmd=%d target=%d actual_target=%d "
                              "aim_group=%d hit_group=%d damage=%d",
                              event.id, tracked->command, event.target, target, event.hitgroup,
                              group, damage);
    } else {
        // A ray intersecting the reconstructed model is not proof of the
        // server pose. Keep unknown misses distinct from measured spread.
        constexpr const char* reasons[]{"unconfirmed-pose-or-registration",
                                        "ray-model-mismatch(spread-or-obstruction)",
                                        "prediction-error", "death", "target-state-changed"};
        output.console_status(false, "[jitter] %s id=%d cmd=%d target=%d aim_group=%d reason=%s",
                              reason == 5 ? "UNCONFIRMED" : "MISS", event.id, tracked->command,
                              event.target, event.hitgroup,
                              reason == 5 ? "health-change-without-hurt-event"
                                          : reasons[reason < 5 ? reason : 0]);
    }
}
}
