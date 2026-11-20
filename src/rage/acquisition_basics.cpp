#include "skeet/rage/acquisition.hpp"
#include <bit>
#include <cstring>

namespace skeet::rage {
namespace {
std::int32_t integer(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
}
std::uint32_t AcquisitionTargetSettings::record_limit() const noexcept {
    constexpr std::array<std::uint32_t, 5> limits{1, 1, 3, 2, 5}; //43474950
    return limits[words[0]];
}
std::uint8_t AcquisitionTargetSettings::option(std::size_t native_offset) const noexcept {
    return std::uint8_t(words[(native_offset - 0xBCU) >> 2] >> ((native_offset & 3U) * 8U));
}
void AcquisitionTargetSettings::option(std::size_t native_offset, std::uint8_t value) noexcept {
    auto& word = words[(native_offset - 0xBCU) >> 2];
    const auto shift = (native_offset & 3U) * 8U;
    word = (word & ~(0xFFU << shift)) | (std::uint32_t(value) << shift);
}
ScanPolicy AcquisitionTargetSettings::scan_policy() const noexcept {
    return {{words[1], words[2]},
            std::bit_cast<float>(words[3]),
            words[4],
            integer(words[5]),
            std::bit_cast<float>(words[6]),
            0,
            option(0xDC) != 0,
            option(0xDD) != 0,
            option(0xE1) != 0};
}
void AcquisitionTargetSettings::scan_policy(const ScanPolicy& policy) noexcept {
    words[1] = policy.hitboxes.regular;
    words[2] = policy.hitboxes.safe;
    words[3] = std::bit_cast<std::uint32_t>(policy.point_scale);
    words[4] = policy.multipoint_mask;
    words[5] = std::uint32_t(policy.minimum_damage);
    words[6] = std::bit_cast<std::uint32_t>(policy.minimum_chance);
    option(0xDC, std::uint8_t(policy.prefer_safe));
    option(0xDD, std::uint8_t(policy.prefer_body));
    option(0xE1, std::uint8_t(policy.additional_hypotheses));
}
bool pending_target_damage(const ShotFeedbackState* state, PendingTargetDamage& output) noexcept {
    if (!state || state->pending.empty())
        return false;
    output.entity_index = integer(state->pending.front().state.words[1]);
    output.damage = integer(state->pending.front().state.words[10]);
    return true;
}
std::int32_t scan_target_health(std::int32_t health, std::uint32_t entity_index,
                                const PendingTargetDamage* pending) noexcept {
    health = health < 1 ? 1 : health > 100 ? 100 : health;
    if (pending && std::uint32_t(pending->entity_index) == entity_index) {
        health = integer(std::uint32_t(health) - std::uint32_t(pending->damage));
        if (health <= 0)
            health = 1000;
    }
    return health;
}
void reset_scan_point(TargetPoint& point) noexcept {
    point.position = {0, 0, 0};
    point.reference_position = {0, 0, 0};
    point.damage = 0;
    point.hitbox = -1;
    point.hitgroup = -1;
    point.flags = 0;
    point.chance = 0;
    point.descriptor_weight = 0;
    point.priority = 0;
}
bool target_history_is_usable(const client::NativePlayerStateLayout& state,
                              const lagcomp::RecordRing& history, const client::CommandFrame& frame,
                              std::uint32_t entity_index, float interval) noexcept {
    const auto index = entity_index & 63U;
    const auto mask = 1U << (index & 31U);
    if (!(read<std::uint32_t>(&frame, 0x1EC + (index >> 5) * 4U) & mask))
        return true;
    const auto tick = lagcomp::time_to_tick(read<float>(&state, 0x110), interval);
    const auto receive = read<std::uint32_t>(&state, 0x168);
    if (tick >= integer(receive - 1U))
        return true;
    if (read<std::int32_t>(&state, 0x1D4) <= 1)
        return false;
    return !history.count || tick >= history.at(0).header.simulation_tick;
}
} // namespace skeet::rage
