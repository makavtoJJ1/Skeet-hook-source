#include "skeet/rage/targeting_acquired.hpp"
#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/preamble.hpp"

namespace skeet::rage {
const AcquisitionEvaluation& acquisition_evaluation_for(const AcquisitionStorage& storage,
                                                        const TargetCandidate& candidate) noexcept {
    const auto slot = &candidate - storage.candidates.data();
    return storage.evaluations[std::size_t(slot)];
}
TargetShotChoice select_acquired_target_for_shot(TargetBatch& batch, AcquisitionStorage& storage,
                                                 client::CommandFrame& frame, PreambleState& state,
                                                 const FinalTargetContextBindings& bindings) {
    auto& context_flags = state.flags;
    if (!batch.count)
        return {};
    const bool experimental = bindings.validates_records() || bindings.precise_damage() ||
                              bindings.full_seed_hitchance();
    if (!experimental && (context_flags & 0x10U) && frame.weapon_id_d8 == 64 && (frame.flags & 0x40U)) {
        batch.candidates[0]->primary.chance = 1.0f;
        return {batch.candidates[0], 0};
    }
    for (std::size_t ordinal = 0; ordinal != batch.count; ++ordinal) {
        auto& candidate = *batch.candidates[ordinal];
        float chance = 1.0f;
        if (experimental || (context_flags & 0x10U)) {
            const auto& evaluation = acquisition_evaluation_for(storage, candidate);
            const auto evaluate = [&] {
                return evaluate_final_target(frame, state, candidate, evaluation, context_flags,
                                             bindings, chance);
            };
            if (!evaluate()) {
                if (!(frame.flags & 0x400U) || !(candidate.flags & 8U))
                    continue;
                candidate.flags &= ~8U;
                candidate.primary = candidate.alternate_first;
                if (!evaluate()) {
                    if (!(candidate.flags & 0x10U))
                        continue;
                    candidate.flags &= ~0x10U;
                    candidate.primary = candidate.alternate_second;
                    if (!evaluate())
                        continue;
                }
            }
        }
        // Stack2C starts at +0.0 and stack90 at0.5; neither is rewritten
        // between the complete main entry and the selection loop.
        const float effective = (candidate.primary.flags & 0x20U) ? chance * 0.5f : chance;
        if (!(0.0f >= effective)) {
            candidate.primary.chance = chance;
            return {&candidate, ordinal};
        }
    }
    return {};
}
} // namespace skeet::rage
