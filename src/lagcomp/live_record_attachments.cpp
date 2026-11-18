#include "skeet/lagcomp/live_record_attachments.hpp"
#include "skeet/client/player_state.hpp"
#include <bit>
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T>
[[gnu::always_inline]] inline T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
}
void refresh_record_attachments(client::PlayerStateCache& players, void* target, LagRecord& record,
                                const EntityPreparationContext& preparation,
                                const LiveRecordAttachmentContext& attachments, bool projected_pose) {
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(target) + 8, 10);
    auto& cached = players.get(std::uint32_t(index))->native;
    const auto& offsets = preparation.entity.offsets;
    const auto old_432 = read<std::uint32_t>(target, offsets.field_432b1f5b);
    const auto old_0d = read<std::uint32_t>(target, offsets.field_0d560fac);
    const auto old_angles = read<std::array<std::uint32_t, 2>>(target, offsets.eye_angles);
    if (projected_pose) {
        write(target, offsets.eye_angles,
              std::array{record.header.eye_pitch, std::bit_cast<float>(record.header.player_history_1ac)});
    } else {
        prepare_entity_angles(players, target, record, true, preparation);
    }
    record.header.field_50 = read<float>(target, offsets.eye_angles + 4);
    auto* collision = engine::invoke<void*>(target, 3);
    auto* maximum = engine::invoke<Vector*>(collision, 2);
    const auto old_height = (*maximum)[2];
    (*maximum)[2] = record.header.collision_max_z;
    const auto old_duck = read<std::uint32_t>(target, offsets.duck_amount);
    write(target, offsets.duck_amount,
          projected_pose ? record.header.duck_amount : cached.cached_duck_amount);
    const auto old_6e = read<std::uint32_t>(target, attachments.field_6e9852d6);
    const auto old_2a = read<Vector>(target, attachments.field_2a671b42);
    write(target, attachments.field_2a671b42, record.header.origin);
    const auto old_4e = read<Vector>(target, attachments.field_4e66bb34);
    write(target, attachments.field_4e66bb34, record.header.origin);
    const auto old_flags = read<std::uint32_t>(target, attachments.field_83a6cf2b);
    write(target, attachments.field_83a6cf2b, old_flags & 0xFFFFF7FFu);
    if (record.header.flags & 4u)
        attachments.bones.refresh_hitbox_attachments(target, record.matrices, attachments.scratch);
    if (record.header.flags & 0x80u)
        for (auto& alternative : record.alternatives)
            attachments.bones.refresh_hitbox_attachments(target, alternative.matrices,
                                                         attachments.scratch);
    write(target, attachments.field_83a6cf2b, old_flags);
    write(target, attachments.field_4e66bb34, old_4e);
    write(target, attachments.field_2a671b42, old_2a);
    write(target, attachments.field_6e9852d6, old_6e);
    write(target, offsets.duck_amount, old_duck);
    (*maximum)[2] = old_height;
    write(target, offsets.eye_angles, old_angles);
    write(target, offsets.field_0d560fac, old_0d);
    write(target, offsets.field_432b1f5b, old_432);
}
void refresh_record_attachments(client::PlayerStateCache& players, void* target, LagRecord& record,
                                const EntityPreparationContext& preparation,
                                const RecordAttachmentContext& attachments,
                                const animation::BoneRuntime& bones, bool projected_pose) {
    const LiveRecordAttachmentContext live{bones,
                                           attachments.scratch,
                                           attachments.field_6e9852d6,
                                           attachments.field_2a671b42,
                                           attachments.field_4e66bb34,
                                           attachments.field_83a6cf2b};
    refresh_record_attachments(players, target, record, preparation, live, projected_pose);
}
} // namespace skeet::lagcomp
