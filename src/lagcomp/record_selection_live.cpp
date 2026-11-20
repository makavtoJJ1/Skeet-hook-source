#include "skeet/lagcomp/record_selection_live.hpp"
#include "skeet/client/player_state.hpp"
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
}
RecordSelectionLiveContext live_record_selection_context(const RecordSelectionContext& source,
                                                         const animation::BoneRuntime& bones) {
    const auto& old = source.attachments;
    return {source.players,
            source.preparation,
            source.timing,
            source.rotation,
            source.interpolation_scratch,
            {bones, old.scratch, old.field_6e9852d6, old.field_2a671b42, old.field_4e66bb34,
             old.field_83a6cf2b},
            source.current_bone_counter};
}
bool select_and_apply_history_record_live(EntityRecordScope& scope,
                                          const RecordSelectionLiveContext& context,
                                          std::int32_t requested, std::uint32_t unused_options) {
    (void)unused_options; // Native never reads its third stack argument.
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(scope.player) + 8, 10);
    auto& cached = *context.players.get(std::uint32_t(index));
    scope.flags = 0;
    const auto& runtime = context.preparation.entity;
    const auto choice = choose_history_record(cached.history, cached.native.cached_origin,
                                              requested, runtime.interval);
    if (!choice.record)
        return false;
    if (scope.snapshot.state.capture_frame != runtime.frame)
        runtime.capture_snapshot(scope.snapshot, scope.player);
    std::memcpy(&scope.record, choice.record, sizeof scope.record);
    const auto interpolated = interpolate_selected_record(
        scope.record, cached.history, choice, requested, history_tick_window(context.timing),
        runtime.interval, context.rotation, context.interpolation_scratch);
    if (interpolated.used)
        scope.flags |= 0x10u;
    const auto& bones = context.attachments.bones;
    const EntityHitboxModelOffsets model{bones.fields.studio_wrapper_32e4daf1,
                                         bones.fields.hitbox_set_c2b6fb10};
    runtime.apply_record_live_model(scope.player, scope.record, model,
                                    context.current_bone_counter);
    // Entry clears flags; interpolation only sets10. Thus the native60 rebuild
    // arm is unreachable here, regardless of the unused caller options.
    auto preparation = context.preparation;
    preparation.current_timing = &context.timing;
    refresh_record_attachments(context.players, scope.player, scope.record, preparation,
                               context.attachments);
    if (scope.record.header.flags & 4u) {
        const auto& offsets = runtime.offsets;
        auto* cache = read<Matrix3x4*>(scope.player, offsets.bone_cache);
        const int count = read<int>(scope.player, offsets.bone_cache + 0xc);
        if (cache && count > 0)
            bones.scatter_hitbox_bone_matrices(scope.player, scope.record.matrices,
                                               {cache, static_cast<std::size_t>(count)});
        write(scope.player, offsets.bone_accessor + 8, 0xFFF00u);
        write(scope.player, offsets.bone_accessor + 12, 0xFFF00u);
        if (context.current_bone_counter)
            write(scope.player, offsets.recent_bone_counter, *context.current_bone_counter);
    }
    return true;
}
bool select_and_apply_history_record(EntityRecordScope& scope,
                                     const RecordSelectionContext& context, std::int32_t requested,
                                     std::uint32_t unused_options,
                                     const animation::BoneRuntime& bones) {
    const auto live = live_record_selection_context(context, bones);
    return select_and_apply_history_record_live(scope, live, requested, unused_options);
}
} // namespace skeet::lagcomp
