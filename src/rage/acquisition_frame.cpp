#include "skeet/rage/acquisition_frame.hpp"
#include <bit>
#include <cstring>

namespace skeet::rage {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
lagcomp::HistoryTickTiming refresh_timing(lagcomp::HistoryTickTiming timing,
                                          const client::CommandFrame& frame, float clock) noexcept {
    timing.current_time = clock;
    timing.latency_198 = field<float>(&frame, 0x198);
    timing.tick_d4 = field<std::int32_t>(&frame, 0xD4);
    timing.tick_18c = std::bit_cast<std::int32_t>(frame.field_18c);
    return timing;
}
lagcomp::HistoryTickTiming preparation_timing(const lagcomp::EntityPreparationContext& seed,
                                              const RecordCandidateBindings& candidates,
                                              const client::CommandFrame& frame,
                                              float clock) noexcept {
    const lagcomp::HistoryTickTiming frame_timing{
        clock, 0, candidates.interpolation, candidates.interval, candidates.maximum_unlag_ms, 0, 0};
    return refresh_timing(seed.current_timing ? *seed.current_timing : frame_timing, frame, clock);
}
lagcomp::EntityPreparationContext refresh_preparation(const lagcomp::EntityPreparationContext& seed,
                                                      const lagcomp::HistoryTickTiming& timing,
                                                      float clock) noexcept {
    auto result = seed;
    result.global_frame = std::bit_cast<std::uint32_t>(clock);
    // A null original timing state selects native mean-latency clock logic.
    result.current_timing = seed.current_timing ? &timing : nullptr;
    return result;
}
RecordCandidateBindings refresh_candidates(RecordCandidateBindings seed, float clock) noexcept {
    seed.current_time = clock;
    return seed;
}
TargetBindings refresh_targeting(const TargetBindings& seed,
                                 const players::State& adjustments) noexcept {
    return {seed.local_player,          seed.entity_list, seed.game_type,
            seed.teammates_are_enemies, adjustments,      seed.offsets};
}
BodyAimProbeContext refresh_body(const BodyAimProbeContext& seed, PreambleState& state,
                                 const client::CommandFrame& frame,
                                 const GlobalSettings& settings) noexcept {
    //43353190 reads the current4346A3E0 descriptor before the probe query.
    return {seed.collision,     seed.ballistics,
            seed.source,        reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address)),
            seed.offsets,       state.flags,
            state.weapon_flags, settings.automatic_penetration.value,
            seed.trace_flag,    seed.trace_filter_vtable,
            seed.trace_entities};
}
ScanContext refresh_scanner(const ScanContext& seed, PreambleState& state,
                            const client::CommandFrame& frame,
                            const TargetPreparationBindings& preparation,
                            const lagcomp::EntityPreparationContext& entity,
                            const std::array<RageSpreadSample, 32>& samples) {
    return {seed.ballistics,
            preparation.targeting,
            seed.source,
            reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address)),
            seed.weapon_item_offset,
            preparation.global.automatic_penetration.value,
            frame.weapon_id_d8,
            frame.origin_dc,
            std::bit_cast<float>(frame.field_160),
            field<std::int32_t>(&frame, 0x19C),
            field<std::int32_t>(&frame, 0x1D0),
            preparation.weapon_settings.minimum_hit_chance.value,
            state.flags,
            state.previous_targets,
            samples,
            seed.model_offsets,
            seed.trigonometry,
            seed.box_clipper,
            seed.players,
            entity,
            seed.hypothesis_offsets,
            seed.update_attachments,
            seed.teammate_convars,
            seed.trace_filter_vtable,
            seed.trace_entities,
            seed.damage_context};
}
ScanContext refresh_projected_scanner(const ScanContext& seed, const client::CommandFrame& frame,
                                      const lagcomp::EntityPreparationContext& preparation) {
    return {seed.ballistics,
            seed.targeting,
            seed.source,
            reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address)),
            seed.weapon_item_offset,
            seed.allow_penetration,
            frame.weapon_id_d8,
            frame.origin_dc,
            std::bit_cast<float>(frame.field_160),
            frame.pellet_count_19c,
            std::bit_cast<std::int32_t>(frame.field_1d0),
            seed.accuracy_percent,
            seed.configuration_flags,
            seed.target_mask,
            seed.samples,
            seed.model_offsets,
            seed.trigonometry,
            seed.box_clipper,
            seed.players,
            preparation,
            seed.hypothesis_offsets,
            seed.update_attachments,
            seed.teammate_convars,
            seed.trace_filter_vtable,
            seed.trace_entities,
            seed.damage_context};
}
BodyAimProbeContext refresh_projected_body(const BodyAimProbeContext& seed,
                                           const ScanContext& scanner,
                                           const client::CommandFrame& frame) noexcept {
    return {seed.collision,     seed.ballistics,
            seed.source,        reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address)),
            seed.offsets,       scanner.configuration_flags,
            seed.weapon_flags,  scanner.allow_penetration,
            seed.trace_flag,    seed.trace_filter_vtable,
            seed.trace_entities};
}
}
AcquisitionBranchFrameBindings::AcquisitionBranchFrameBindings(
    PreambleState& state, const client::CommandFrame& frame, float clock, std::int32_t handle,
    std::uint32_t scheduler_flags, const std::array<RageSpreadSample, 32>& samples,
    const TargetPreparationBindings& preparation_seed, const BodyAimProbeContext& body_seed,
    const TargetPredictionBindings& prediction_seed, const RecordCandidateBindings& candidates_seed,
    const AcquisitionRecordBindings& records_seed, const players::State& adjustments,
    const ui::SettingsMenuValues& settings_menu)
    : targeting(refresh_targeting(preparation_seed.targeting, adjustments)),
      prediction_timing(refresh_timing(prediction_seed.record_selection.timing, frame, clock)),
      scanner_timing(
          preparation_timing(records_seed.scanner.preparation, candidates_seed, frame, clock)),
      extrapolation_timing(preparation_timing(records_seed.extrapolation.preparation,
                                              candidates_seed, frame, clock)),
      history_timing(preparation_timing(records_seed.history.extrapolation.preparation,
                                        candidates_seed, frame, clock)),
      prediction_preparation(refresh_preparation(prediction_seed.record_selection.preparation,
                                                 prediction_timing, clock)),
      scanner_preparation(
          refresh_preparation(records_seed.scanner.preparation, scanner_timing, clock)),
      extrapolation_preparation(
          refresh_preparation(records_seed.extrapolation.preparation, extrapolation_timing, clock)),
      history_preparation(refresh_preparation(records_seed.history.extrapolation.preparation,
                                              history_timing, clock)),
      prediction_records{prediction_seed.record_selection.players,
                         prediction_preparation,
                         prediction_timing,
                         prediction_seed.record_selection.rotation,
                         prediction_seed.record_selection.interpolation_scratch,
                         prediction_seed.record_selection.attachments,
                         prediction_seed.record_selection.current_bone_counter},
      extrapolation{
          records_seed.extrapolation.players,     extrapolation_preparation,
          records_seed.extrapolation.attachments, records_seed.extrapolation.bones,
          records_seed.extrapolation.movement,    records_seed.extrapolation.prediction_limit,
          records_seed.extrapolation.validate_record, records_seed.extrapolation.rebuild_pose},
      history_extrapolation{records_seed.history.extrapolation.players,
                            history_preparation,
                            records_seed.history.extrapolation.attachments,
                            records_seed.history.extrapolation.bones,
                            records_seed.history.extrapolation.movement,
                            records_seed.history.extrapolation.prediction_limit,
                            records_seed.history.extrapolation.validate_record,
                            records_seed.history.extrapolation.rebuild_pose},
      history{history_extrapolation,
              records_seed.history.alternative,
              records_seed.history.rotation,
              records_seed.history.matrix_scratch,
              clock,
              records_seed.history.interpolation,
              records_seed.history.maximum_unlag_ms,
              records_seed.history.jitter_fix,
              records_seed.history.animation_rebuild,
              records_seed.history.resolver_corrections},
      preparation{targeting,
                  preparation_seed.global,
                  preparation_seed.weapon_settings,
                  preparation_seed.input,
                  state,
                  handle,
                  preparation_seed.accuracy_convar,
                  preparation_seed.offsets},
      record_candidates(refresh_candidates(candidates_seed, clock)),
      scanner(refresh_scanner(records_seed.scanner, state, frame, preparation, scanner_preparation,
                              samples)),
      records{records_seed.players,
              records_seed.lag_compensation,
              records_seed.interval,
              handle,
              preparation.global.body_aim_disablers.value,
              records_seed.feedback_owner,
              history,
              extrapolation,
              scanner,
              settings_menu,
              records_seed.bone_runtime,
              records_seed.live_movement,
              records_seed.live_budget,
              records_seed.clear_safe_neck},
      body_probe(refresh_body(body_seed, state, frame, preparation.global)),
      prediction{body_probe,
                 prediction_records,
                 prediction_seed.engine_trace,
                 prediction_seed.local_maximum_speed,
                 scheduler_flags,
                 prediction_seed.command} {}

AcquisitionFrameBindings::AcquisitionFrameBindings(
    PreambleState& state, const PreambleResult& preamble, float clock, std::int32_t handle,
    std::uint32_t scheduler_flags, const SingleTargetBindings& single_seed,
    const MultipleTargetBindings& multiple_seed, const ui::SettingsMenuValues& settings_menu)
    : samples(hitchance_spread_samples(state.spread)),
      single_frame(state, *preamble.frame, clock, handle, scheduler_flags, samples,
                   single_seed.preparation, single_seed.prediction.trace, single_seed.prediction,
                   single_seed.record_candidates, single_seed.records, single_seed.adjustments,
                   settings_menu),
      multiple_frame(state, *preamble.frame, clock, handle, scheduler_flags, samples,
                     multiple_seed.preparation, multiple_seed.body_probe, multiple_seed.prediction,
                     multiple_seed.record_candidates, multiple_seed.records,
                     multiple_seed.adjustments, settings_menu),
      single{single_frame.preparation, single_frame.prediction, single_frame.record_candidates,
             single_frame.records,     single_seed.adjustments, single_seed.maximum_clients,
             single_seed.jobs},
      multiple{multiple_frame.preparation,    multiple_frame.body_probe,
               multiple_frame.prediction,     multiple_frame.record_candidates,
               multiple_frame.records,        multiple_seed.adjustments,
               multiple_seed.maximum_clients, multiple_seed.jobs} {}
TargetFrameBindings::TargetFrameBindings(const TargetPredictionBindings& prediction_seed,
                                         ScanContext& scanner_seed,
                                         const client::CommandFrame& frame, float clock,
                                         std::uint32_t scheduler_flags)
    : prediction_timing(refresh_timing(prediction_seed.record_selection.timing, frame, clock)),
      scanner_timing(refresh_timing(scanner_seed.preparation.current_timing
                                        ? *scanner_seed.preparation.current_timing
                                        : prediction_seed.record_selection.timing,
                                    frame, clock)),
      prediction_preparation(refresh_preparation(prediction_seed.record_selection.preparation,
                                                 prediction_timing, clock)),
      scanner_preparation(refresh_preparation(scanner_seed.preparation, scanner_timing, clock)),
      prediction_records{prediction_seed.record_selection.players,
                         prediction_preparation,
                         prediction_timing,
                         prediction_seed.record_selection.rotation,
                         prediction_seed.record_selection.interpolation_scratch,
                         prediction_seed.record_selection.attachments,
                         prediction_seed.record_selection.current_bone_counter},
      body_probe(refresh_projected_body(prediction_seed.trace, scanner_seed, frame)),
      prediction{body_probe,
                 prediction_records,
                 prediction_seed.engine_trace,
                 prediction_seed.local_maximum_speed,
                 scheduler_flags,
                 prediction_seed.command},
      scanner(refresh_projected_scanner(scanner_seed, frame, scanner_preparation)) {}
} // namespace skeet::rage
