#include "skeet/lagcomp/animation_capture.hpp"
#include "skeet/client/player_state.hpp"
#include "skeet/animation/layer_state.hpp"
#include "skeet/resolver.hpp"
#include <bit>
#include <cmath>
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
template <class T> void write(void* object, std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
}
float normalize(float value) noexcept {
    if (std::isnan(value))
        return 0.0f;
    if (std::fabs(value) > 180.0f) {
        const auto turns = round_half_away_from_zero(std::fabs(value / 360.0f)) * 360.0f;
        value = value < 0.0f ? value + turns : value - turns;
    }
    return value;
}
}
HistoryAnimationSeed prepare_history_animation(client::PlayerState& owner, void* player,
                                               void* animation_state, std::int32_t simulation_ticks,
                                               std::uint32_t update_flags,
                                               const EntityStateRuntime& entity,
                                               std::uint32_t flags_offset, bool strict_bounds) {
    const auto reference = owner.native.reference_yaw;
    const auto stationary = read<float>(&owner.native, 0x118);
    const auto elapsed = float(simulation_ticks) * entity.interval;
    const auto target = normalize(reference);
    const AnimationYawMotion motion{
        {read<float>(animation_state, 0xA4), read<float>(animation_state, 0xF8),
         read<float>(animation_state, 0xFC), read<float>(animation_state, 0x11C),
         read<float>(animation_state, 0x334), read<float>(animation_state, 0x338)},
        read<float>(animation_state, 0xEC),
        read<float>(animation_state, 0xF0)};
    owner.native.capture_field_180 = std::bit_cast<std::uint32_t>(advance_animation_yaw(
        motion, target, stationary, elapsed, std::bit_cast<float>(owner.native.capture_field_180),
        strict_bounds));
    if ((update_flags & 1u) && !owner.history.count &&
        (read<std::uint8_t>(player, flags_offset) & 1u)) {
        write(animation_state, 0x108, std::uint8_t(1));
        write(animation_state, 0x109, std::uint8_t(0));
    }
    HistoryAnimationSeed seed;
    seed.previous_update_time = read<float>(animation_state, 0x6C);
    seed.velocity = read<Vector>(player, entity.offsets.velocity);
    seed.active_air_layer =
        !(read<std::uint8_t>(player, flags_offset) & 1u) &&
        animation::layer_is_active(reinterpret_cast<const std::byte*>(&owner.native) + 0x4D8);
    return seed;
}
} // namespace skeet::lagcomp
