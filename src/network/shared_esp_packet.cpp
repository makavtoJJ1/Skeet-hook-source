#include "skeet/network/shared_esp.hpp"
#include <bit>
#include <cstring>

namespace skeet::network {
namespace {
float coordinate(std::uint32_t value) noexcept {
    const auto whole = (value & 1u) ? ((value >> 3) & 0x3fffu) + 1u : 0u;
    const auto fraction = (value & 2u) ? (value >> 17) & 31u : 0u;
    const auto result = float(whole) + float(fraction) * 0.03125f;
    return (value & 4u) ? -result : result;
}

std::uint16_t encrypt_tail(std::uint16_t left, std::uint16_t right,
                           std::uint16_t previous_left, std::uint16_t previous_right,
                           std::uint32_t key) noexcept {
    left = static_cast<std::uint16_t>((left ^ previous_left) + key);
    right = static_cast<std::uint16_t>((right ^ previous_right) + std::rotr(key, 1));
    auto round_key = std::rotr(key, 2);
    for (unsigned round = 0; round < 15; ++round) {
        left = static_cast<std::uint16_t>(
            std::rotl(static_cast<std::uint16_t>(right ^ left), right & 15u) + round_key);
        round_key = std::rotr(round_key, 1);
        right = static_cast<std::uint16_t>(
            std::rotl(static_cast<std::uint16_t>(right ^ left), left & 15u) + round_key);
        round_key = std::rotr(round_key, 1);
    }
    return left;
}
}

bool decode_shared_esp_packet(SharedEspPacket packet,
                             const client::RejectedPlayerUpdateOwner& owner,
                             std::uint32_t sender_account, std::uint32_t sender_user_id,
                             SharedEspFields& decoded) noexcept {
    if (packet.sender_index < 1 || packet.sender_index > 64)
        return false;
    client::transform_shared_esp_packet(client::SharedEspPacketCipher::rc4_drop128, owner,
                                       packet.bytes);
    std::array<std::uint16_t, 10> words{};
    std::memcpy(words.data(), packet.bytes.data(), packet.bytes.size());
    const auto encrypted_tail = words[9];
    std::uint16_t previous_left = 0, previous_right = 0;
    for (std::size_t index = 1; index < 9; index += 2) {
        const auto saved_left = words[index], saved_right = words[index + 1];
        auto left = saved_left, right = saved_right;
        auto round_key = std::rotl(owner.mixing_key, 1);
        for (unsigned round = 0; round < 15; ++round) {
            right = static_cast<std::uint16_t>(
                std::rotr(static_cast<std::uint16_t>(right - round_key), left & 15u) ^ left);
            round_key = std::rotl(round_key, 1);
            left = static_cast<std::uint16_t>(
                std::rotr(static_cast<std::uint16_t>(left - round_key), right & 15u) ^ right);
            round_key = std::rotl(round_key, 1);
        }
        words[index] = static_cast<std::uint16_t>(
            (left - std::rotl(round_key, 1)) ^ previous_left);
        words[index + 1] = static_cast<std::uint16_t>((right - round_key) ^ previous_right);
        previous_left = saved_left;
        previous_right = saved_right;
    }
    std::array<std::uint32_t, 5> fields{};
    std::memcpy(fields.data(), words.data(), packet.bytes.size());
    fields[0] ^= sender_account;
    fields[1] ^= sender_user_id;
    fields[2] ^= std::uint32_t(packet.sender_index);
    if (fields[0] >> 16 != owner.marker)
        return false;
    const auto entity_index = (fields[2] & 0x7fu) + 1u;
    if (entity_index > 64)
        return false;

    // The wire omits the final encrypted right half. Its plaintext is the
    // selected player index; the left half contains only two Z fraction bits.
    // Recover those four possibilities exactly, without decrypting the sender
    // index as if it were two more bytes received from the network.
    unsigned matches = 0;
    std::uint32_t tail = 0;
    for (std::uint16_t candidate = 0; candidate < 4; ++candidate) {
        if (encrypt_tail(candidate, static_cast<std::uint16_t>(entity_index), previous_left,
                         previous_right, owner.mixing_key) == encrypted_tail) {
            tail = candidate;
            ++matches;
        }
    }
    if (matches != 1)
        return false;
    fields[4] = (fields[4] & 0xffffu) | (tail << 16);
    const auto x = (fields[2] >> 16) | ((fields[3] & 63u) << 16);
    const auto y = (fields[3] >> 6) & 0x3fffffu;
    const auto z = (fields[3] >> 28) | ((fields[4] & 0x3ffffu) << 4);
    decoded = {static_cast<std::uint16_t>(fields[0] >> 16),
               std::bit_cast<std::int32_t>(fields[1]), entity_index,
               (fields[2] >> 7) & 0x1ffu, {coordinate(x), coordinate(y), coordinate(z)}};
    return true;
}
} // namespace skeet::network
