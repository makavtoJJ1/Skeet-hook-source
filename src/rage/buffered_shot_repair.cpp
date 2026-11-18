#include "skeet/rage/buffered_shot_repair.hpp"
#include "skeet/rage/acquisition_frame.hpp"
#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/targeting_acquired.hpp"
#include "skeet/rage/runtime_owner.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>

namespace skeet::rage {
namespace {
struct ReconstructionFlag {
    PreambleState& state;
    explicit ReconstructionFlag(PreambleState& owner) : state(owner) {
        state.flags |= 1U;
    }
    ~ReconstructionFlag() {
        state.flags &= ~1U;
    }
};
void initialize_historical_bullet(BallisticQuery& query, const client::CommandBullet& source,
                                  bool penetrate) noexcept {
    // Complete433D0015. Seven raw words are copied after433FCFD4's reset;
    // damage itself remains zero until a concrete ballistic trace starts.
    query.allow_penetration = std::uint8_t(penetrate);
    reset_ballistic_query(query);
    query.weapon_id = source.weapon_id;
    query.weapon_padding = source.padding;
    query.penetration = source.penetration;
    query.range = source.range;
    query.range_modifier = source.range_modifier;
    query.armor_ratio = source.armor_ratio;
    query.base_damage = source.damage;
    query.reserved = source.pellet_count;
}
void clear_mask(std::array<std::uint32_t, 2>& mask, std::uint32_t entity) noexcept {
    const auto index = entity & 63U;
    mask[index >> 5] &= ~(1U << (index & 31U));
}
void group_completed(PreambleState& state, AcquisitionStorage& storage,
                     BufferedShotRepairScratch& scratch) {
    std::array<std::int8_t, 64> groups;
    groups.fill(-1);
    auto& batch = scratch.candidates;
    for (std::uint32_t ordinal = 0; ordinal < 80; ++ordinal) {
        if (!(state.completed[ordinal >> 5] & (1U << (ordinal & 31U))))
            continue;
        auto& candidate = storage.candidates[ordinal];
        const auto entity = candidate.entity_index & 63U;
        if (!(state.flags & 1U))
            state.target_mask[entity >> 5] |= 1U << (entity & 31U);
        const auto group = groups[entity];
        if (group == -1) {
            if (batch.count >= 16 || !scratch.inline_storage)
                continue;
            groups[entity] = std::int8_t(batch.count);
            batch.candidates[batch.count++] = &candidate;
        } else if (candidate.priority > batch.candidates[std::uint32_t(group)]->priority)
            batch.candidates[std::uint32_t(group)] = &candidate;
    }
}
void acquire_repair_target(PreambleState& state, PreambleResult& preamble,
                           AcquisitionStorage& storage, void* target, client::PlayerState& cached,
                           std::uint32_t index, const players::State& adjustments,
                           AcquisitionBranchFrameBindings& current, const client::Command& command,
                           bool body_probe) {
    auto& frame = *preamble.frame;
    AcquisitionTargetSettings settings{preamble.scan_template};
    prepare_acquisition_target_settings(frame, target, cached, index,
                                        adjustments.records[index].adjustments, settings,
                                        current.preparation);
    auto trace = current.body_probe;
    trace.context_flags = state.flags;
    trace.weapon_flags = state.weapon_flags;
    trace.weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
    if (body_probe) {
        if (!(state.flags & 9U))
            clear_mask(state.previous_targets, index);
        probe_body_aim(frame, settings, target, trace);
    }
    const TargetPredictionBindings prediction{trace,
                                              current.prediction.record_selection,
                                              current.prediction.engine_trace,
                                              current.prediction.local_maximum_speed,
                                              current.prediction.scheduler_flags,
                                              &command};
    refine_acquisition_minimum_damage(frame, target, cached, settings, prediction,
                                      current.records.bone_runtime);
    auto ticks = acquisition_target_ticks(frame, target, cached, settings, current.preparation,
                                          current.record_candidates);
    auto records = current.records;
    records.clear_safe_neck = true;
    prepare_and_submit_target_records(state, preamble, storage, target, cached, ticks.values,
                                      settings, ticks.descriptor_flags, records);
}
}
const lagcomp::EvaluationState* repair_buffered_shot(client::CommandFrame& frame, void* local,
                                                     client::Command& command,
                                                     const ShotDiagnosticEntry& diagnostic,
                                                     BufferedShotRepairScratch& scratch,
                                                     const BufferedShotRepairBindings& bindings) {
    if (const auto* source = std::get_if<RuntimeOwner*>(&bindings.source))
        return (*source)->repair_buffered(frame, local, command, diagnostic, scratch);
    auto* owner = std::get<CommandOwner*>(bindings.source);
    if (!owner || (frame.flags & 0x20040U))
        return nullptr;
    auto& state = owner->state;
    auto& storage = owner->acquisition;
    ReconstructionFlag active(state);
    PreambleResult preamble;
    preamble.frame = &frame;
    preamble.command = &command;
    preamble.scan_template = prepare_rage_scan_template(state, frame, owner->preamble);
    preamble.point_scale_is_automatic =
        owner->preamble.weapon_settings.multipoint_scale.value == 24;
    const SpreadParameters parameters{std::uint32_t(frame.weapon_id_d8),
                                      std::int32_t(frame.field_b0),
                                      frame.spread_d0,
                                      frame.inaccuracy_c4,
                                      frame.weapon_field_100,
                                      frame.pellet_count_19c,
                                      owner->preamble.shotgun_patterns,
                                      frame.weapon_kind_124};
    prepare_spread_cache(state.spread, parameters, frame.adjusted_spread_160,
                         std::uint32_t(frame.command_number_ac), (state.flags & 0x10U) != 0,
                         owner->preamble.spread_tables, owner->preamble.random,
                         owner->preamble.pattern, owner->preamble.trigonometry);
    initialize_historical_bullet(scratch.historical_bullet, diagnostic.bullet,
                                 owner->preamble.settings.automatic_penetration.value != 0);
    storage.queued.clear();
    state.completed.fill(0);
    state.evaluation_count = 0;
    AcquisitionFrameBindings current(state, preamble, owner->preamble.prediction_time,
                                     owner->preamble.yaw_target.handle,
                                     owner->preamble.scheduler_flags, owner->single_target,
                                     owner->multiple_targets, owner->settings_menu);
    const bool single = owner->preamble.weapon_settings.target_selection.value == 1;
    bool selected = true;
    if (single) {
        if (!(state.flags & 9U)) {
            state.target_mask.fill(0);
            state.previous_targets.fill(0);
        }
        auto& branch = current.single_frame;
        void* target = select_single_target(
            {branch.preparation.targeting, current.single.maximum_clients, frame.origin_dc,
             frame.direction_1a8, std::bit_cast<float>(preamble.scan_template[7])});
        selected = target != nullptr;
        if (target) {
            const auto index =
                engine::invoke<std::uint32_t>(static_cast<std::byte*>(target) + 8, 10);
            auto& cached = *branch.records.players.get(index);
            acquire_repair_target(state, preamble, storage, target, cached, index,
                                  current.single.adjustments, branch, command, false);
            finish_target_acquisition(state, storage, current.single.jobs, branch.records.players,
                                      current.single.maximum_clients, branch.records.settings_menu);
        }
    } else {
        auto& branch = current.multiple_frame;
        const TargetView view{frame.origin_dc, frame.direction_1a8,
                              branch.preparation.weapon_settings.maximum_fov.value,
                              std::bit_cast<float>(preamble.scan_template[7])};
        for (std::int32_t entity = 1; entity <= current.multiple.maximum_clients; ++entity) {
            auto& cached = *branch.records.players.get(std::uint32_t(entity));
            auto* target =
                engine::invoke<void*>(branch.preparation.targeting.entity_list, 3, entity);
            if (!(state.flags & 9U))
                clear_mask(state.target_mask, std::uint32_t(entity));
            if (!target_is_eligible(branch.preparation.targeting, target) ||
                !target_passes_scan_filter(branch.preparation.targeting, target, view))
                continue;
            const auto index =
                engine::invoke<std::uint32_t>(static_cast<std::byte*>(target) + 8, 10);
            acquire_repair_target(state, preamble, storage, target, cached, index,
                                  current.multiple.adjustments, branch, command, true);
        }
        finish_target_acquisition(state, storage, current.multiple.jobs, branch.records.players,
                                  current.multiple.maximum_clients, branch.records.settings_menu);
    }
    if (selected)
        group_completed(state, storage, scratch);
    auto& scanner = single ? current.single_frame.scanner : current.multiple_frame.scanner;
    recalculate_target_priorities(scratch.candidates, scanner.targeting);
    if (scratch.candidates.count > 1)
        sort_target_candidates(scratch.candidates);
    const auto final = owner->final_target.with_scan(scanner);
    auto choice = select_acquired_target_for_shot(scratch.candidates, storage, frame, state, final);
    for (std::uint32_t ordinal = 0; ordinal < state.evaluation_count; ++ordinal)
        synchronize_evaluation_state(storage.candidates[ordinal], storage.evaluations[ordinal]);
    if (!choice.candidate)
        return nullptr;
    auto& evaluation =
        storage.evaluations[std::size_t(choice.candidate - storage.candidates.data())];
    (void)local; // Native EDX is overwritten at433C1599 before its first read.
    return &synchronize_evaluation_state(*choice.candidate, evaluation);
}
} // namespace skeet::rage
