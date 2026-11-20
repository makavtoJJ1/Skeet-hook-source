#include "skeet/rage/initialization.hpp"
#include "skeet/rage/acquisition.hpp"
#include "skeet/rage/preamble.hpp"
#include "skeet/esp/hitbox_highlight.hpp"
#include <cstring>

namespace skeet::rage {
namespace {
void initialize_prefix(const RageResetBindings& bindings) noexcept {
    bindings.field_174 = -1;
    bindings.highlight.target_handle = -1;
    bindings.state.spread.generation = UINT32_MAX;
    bindings.shot_index = 0;
    bindings.highlight.entries = {{{0, 0}, {3, 0}, {9, 0}, {10, 0}}};
    bindings.state.evaluation_count = 0;
}
void initialize_storage(const RageResetBindings& bindings) noexcept {
    for (std::size_t index = 0; index < bindings.acquisition.evaluations.size(); ++index) {
        auto& evaluation = bindings.acquisition.evaluations[index];
        //43387448 allocates this separate80-record block with HEAP_ZERO_MEMORY.
        // Keep its native payload and the existing typed candidate view equal.
        std::memset(&evaluation.native, 0, sizeof(evaluation.native));
        evaluation.record.header.field_00 = 0;
        evaluation.extrapolated = false;
        evaluation.rebuild_usage = {};
        evaluation.forecast_status = PoseForecastStatus::no_request;
        evaluation.extrapolation_ticks = 0;
        evaluation.target_handle = UINT32_MAX;
        restore_evaluation_state(evaluation.native, bindings.acquisition.candidates[index],
                                 evaluation);
    }
    bindings.state.frame_first = 0;
    bindings.acquisition.queued.count = 0;
    bindings.acquisition.queued.inline_storage = true;
    bindings.state.frame_count = 0;
    for (auto& frame : bindings.state.frames)
        client::initialize_command_frame(frame);
}
}
void initialize_rage_state(const RageResetBindings& bindings) noexcept {
    initialize_prefix(bindings);
    initialize_storage(bindings);
}
void initialize_rage_state(const RageResetBindings& bindings, engine::JobMemory memory) {
    initialize_prefix(bindings);
    bindings.acquisition.evaluations.allocate(memory);
    initialize_storage(bindings);
}
}
