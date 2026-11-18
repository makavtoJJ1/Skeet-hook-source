#include "skeet/client/rejected_player_update.hpp"
#include "skeet/misc/runtime.hpp"
#include <bit>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#include <utility>

namespace skeet::client {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T result;
    std::memcpy(&result, static_cast<const std::byte*>(object) + offset, sizeof result);
    return result;
}
template <class T> void write(void* object, std::ptrdiff_t offset, T value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
std::int32_t integer(float value) noexcept {
    return _mm_cvttss_si32(_mm_set_ss(value));
}
float absolute(float value) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) & 0x7fffffffu);
}
std::int32_t signed_bits(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}
std::int32_t convar_integer(const void* object) noexcept {
    for (;;) {
        const auto parent = field<const void*>(object, 0x1c);
        if (parent == object)
            break;
        object = parent;
    }
    return signed_bits(field<std::uint32_t>(object, 0x30) ^
                       std::uint32_t(reinterpret_cast<std::uintptr_t>(object)));
}
std::uint32_t timestamp_mix() noexcept {
    const auto value = static_cast<std::uint32_t>(__rdtsc());
    return std::rotr(value, 3) ^ value;
}
std::uint32_t coordinate(float value) noexcept {
    const auto whole = integer(absolute(value));
    const auto scaled = integer(value * 32.0f);
    const auto magnitude = scaled < 0 ? 0u - std::uint32_t(scaled) : std::uint32_t(scaled);
    auto fraction = magnitude & 31u;
    std::uint32_t result = whole != 0 ? 1u : 0u;
    if (fraction)
        result |= 2u;
    if (-0.03125f >= value)
        result |= 4u;
    if (whole != 0)
        result |= ((std::uint32_t(whole) - 1u) & 0x3fffu) << 3;
    else
        result |= (timestamp_mix() * 0x8208u) & 0x1fff8u;
    if (!fraction)
        fraction = timestamp_mix();
    return result | (fraction << 17);
}

// Five chained 16-bit pairs start at packet+2. The fifth pair's right word
// aliases the saved player index at packet+20 in the native stack frame.
// Only the 20 wire bytes are published; the scan cursor remains an entity index.
void mix_packet(std::array<std::byte, 20>& packet, std::int32_t selected,
                std::uint32_t key) noexcept {
    std::array<std::uint16_t, 11> words{};
    std::memcpy(words.data(), packet.data(), packet.size());
    words[10] = static_cast<std::uint16_t>(selected);
    const auto rotated = std::rotr(key, 1);
    std::uint16_t previous_left = 0, previous_right = 0;
    for (std::size_t index = 1; index < 10; index += 2) {
        auto left = static_cast<std::uint16_t>((words[index] ^ previous_left) + key);
        auto right = static_cast<std::uint16_t>((words[index + 1] ^ previous_right) + rotated);
        auto round_key = std::rotr(rotated, 1);
        for (std::uint32_t round = 0; round < 15; ++round) {
            left = static_cast<std::uint16_t>(
                std::rotl(static_cast<std::uint16_t>(right ^ left), right & 15u) + round_key);
            round_key = std::rotr(round_key, 1);
            right = static_cast<std::uint16_t>(
                std::rotl(static_cast<std::uint16_t>(right ^ left), left & 15u) + round_key);
            round_key = std::rotr(round_key, 1);
        }
        words[index] = left;
        words[index + 1] = right;
        previous_left = left;
        previous_right = right;
    }
    std::memcpy(packet.data(), words.data(), packet.size());
}

RageVector shared_origin(PlayerState& state, const void* player,
                         const RejectedPlayerUpdateContext& context) noexcept {
    if (state.history.count) {
        const auto tick =
            lagcomp::time_to_tick(field<float>(&state.native, 0x110), context.tick_interval);
        const auto previous =
            signed_bits(std::uint32_t(field<std::int32_t>(&state.native, 0x168)) - 1u);
        const auto* newest = state.history.records[state.history.first & 31u];
        if (tick >= previous || tick >= newest->header.simulation_tick)
            return newest->header.origin;
    }
    return field<RageVector>(player, context.offsets.origin_a9ee6062);
}
std::uint32_t shared_yaw(PlayerState& state, const RejectedPlayerUpdateContext& context) noexcept {
    std::uint32_t result = 0x1ffu;
    if (field<std::int32_t>(&state.native, 0xc) == -1 || (state.native.flags & 0x80u))
        return result;
    state.native.flags |= 0x80u;
    float rule_time = 0.0f;
    if (context.game_rules && *context.game_rules)
        rule_time = field<float>(*context.game_rules, context.offsets.rule_time_9f8e7f71);
    if (field<float>(&state.native, 4) == rule_time) {
        const auto encoded =
            std::uint32_t(integer(field<float>(&state.native, 8) * 182.04444885253906f)) & 0xffffu;
        result = std::uint32_t(integer(static_cast<float>(encoded) * 0.0054931640625f));
    }
    return result;
}

void publish(RejectedPlayerUpdateOwner& owner, void* selected, std::int32_t index,
             const RejectedPlayerUpdateContext& context) {
    enter_shared_esp_update(context);
    auto& cache = *context.cache.get(std::uint32_t(index));
    const auto origin = shared_origin(cache, selected, context);
    const auto yaw = shared_yaw(cache, context);
    const auto z = coordinate(origin[2]);
    const auto y = coordinate(origin[1]);
    const auto x = coordinate(origin[0]);
    const auto clock = engine::invoke<float>(owner.engine_client, 14);
    const auto clock_interval = context.engine_tick_interval;
    const auto random = timestamp_mix() * 0x1041u;
    std::array<std::uint32_t, 5> words{};
    words[0] = (random & 0xffffu) | (std::uint32_t(owner.marker) << 16);
    words[1] = std::uint32_t(lagcomp::receive_time_to_tick(clock, clock_interval));
    words[2] = (((x << 9) | (yaw & 0x1ffu)) << 7) | ((std::uint32_t(index) - 1u) & 0x7fu);
    words[3] = (((z << 22) | (y & 0x3fffffu)) << 6) | ((x >> 16) & 0x3fu);
    words[4] = (z >> 4) & 0x3ffffu;
    const auto local_index = engine::invoke<std::int32_t>(owner.engine_client, 12);
    misc::PlayerInfo info;
    if (engine::invoke<bool>(owner.engine_client, 8, local_index, &info)) {
        words[0] ^= field<std::uint32_t>(info.bytes.data(), 8);
        words[1] ^= field<std::uint32_t>(info.bytes.data(), 0x90);
        words[2] ^= std::uint32_t(local_index);
    }
    std::array<std::byte, 20> packet{};
    std::memcpy(packet.data(), words.data(), packet.size());
    mix_packet(packet, index, owner.mixing_key);
    transform_shared_esp_packet(context.packet_cipher, owner, packet);
    std::memcpy(words.data(), packet.data(), packet.size());
    auto* channel = engine::invoke<void*>(owner.engine_client, 78);
    if (channel) {
        owner.message.fill(std::byte{});
        auto* message = owner.message.data();
        owner.construct_message(message);
        write<std::uint32_t>(message, 0x20, 0);
        write<std::uint32_t>(message, 0x34, field<std::uint32_t>(message, 0x34) | 0x3eu);
        write(message, 0x18, words[0]);
        write(message, 0x1c, words[1]);
        write(message, 0x28, words[2]);
        write(message, 0x24, words[3]);
        write(message, 0x2c, words[4]);
        engine::invoke<bool>(channel, 40, message, false, false);
        owner.destroy_message(message);
    }
    leave_shared_esp_update(context);
    owner.last_player = index;
    owner.last_update = context.current_time;
}
} // namespace

void transform_shared_esp_packet(SharedEspPacketCipher cipher,
                                 const RejectedPlayerUpdateOwner& owner,
                                 std::span<std::byte> packet) noexcept {
    if (cipher == SharedEspPacketCipher::legacy_xor) {
        auto seed = field<std::uint32_t>(packet.data(), 0);
        for (std::size_t index = 4; index < 20; ++index) {
            seed = seed * 0x41c64e6du + 0x3039u;
            packet[index] ^= std::byte(static_cast<std::uint8_t>(seed >> 16));
        }
        return;
    }
    std::array<std::uint8_t, 256> state{};
    for (std::size_t index = 0; index < 256; ++index)
        state[index] = static_cast<std::uint8_t>(index);
    std::uint8_t j = 0;
    for (std::size_t index = 0; index < 256; ++index) {
        j = static_cast<std::uint8_t>(j + state[index] + owner.packet_key[index & 15]);
        std::swap(state[index], state[j]);
    }
    std::uint8_t i = 0;
    j = 0;
    for (std::uint32_t skipped = 0; skipped < 128; ++skipped) {
        ++i;
        j = static_cast<std::uint8_t>(j + state[i]);
        std::swap(state[i], state[j]);
    }
    for (auto& byte : packet) {
        ++i;
        j = static_cast<std::uint8_t>(j + state[i]);
        std::swap(state[i], state[j]);
        byte ^= std::byte(state[static_cast<std::uint8_t>(state[i] + state[j])]);
    }
}

void update_rejected_player(void* player, const RejectedPlayerUpdateContext& context) {
    auto* owner = context.owner;
    if (!owner || !context.settings.enabled.value ||
        !hotkey_active(context.settings.activation.value.binding, context.input))
        return;
    if (0.0625f > absolute(context.current_time - owner->last_update))
        return;
    if (convar_integer(owner->game_type) == 6)
        return;
    if (!owner->construct_message || !owner->destroy_message || !owner->construct_receiver ||
        !owner->destroy_receiver)
        return;
    if (!convar_integer(owner->send_gate_a) && !convar_integer(owner->send_gate_b))
        return;
    if (!engine::invoke<void*>(owner->engine_client, 78) ||
        engine::invoke<bool>(owner->engine_client, 197) ||
        engine::invoke<bool>(owner->engine_client, 224) || *context.blocked)
        return;
    if (context.game_rules && *context.game_rules &&
        field<std::uint8_t>(*context.game_rules, context.offsets.rule_d720aea6))
        return;
    auto index = signed_bits(std::uint32_t(owner->last_player) + 1u);
    for (std::int32_t iteration = 1; iteration <= context.maximum_clients; ++iteration) {
        if (index > context.maximum_clients)
            index = 1;
        auto* candidate = engine::invoke<void*>(context.entity_list, 3, index);
        if (candidate && engine::invoke<bool>(candidate, 158) &&
            !field<std::uint8_t>(candidate, context.offsets.rejected_d070684c) &&
            !engine::invoke<bool>(static_cast<std::byte*>(candidate) + 8, 9) &&
            !field<std::uint8_t>(candidate, context.offsets.rejected_9d920e35) &&
            engine::invoke<std::int32_t>(candidate, 122) > 0 &&
            !context.players.records[std::size_t(index)].adjustments.whitelist &&
            (context.settings.other_team.value ||
             rage::target_is_enemy(context.targeting.capture(player), candidate))) {
            publish(*owner, candidate, index, context);
            return;
        }
        index = signed_bits(std::uint32_t(index) + 1u);
    }
}

void enter_shared_esp_update(const RejectedPlayerUpdateContext& context) noexcept {
    context.packet_cipher = SharedEspPacketCipher::rc4_drop128;
}
void leave_shared_esp_update(const RejectedPlayerUpdateContext& context) noexcept {
    context.packet_cipher = SharedEspPacketCipher::legacy_xor;
}

} // namespace skeet::client
