#include "skeet/network/shared_esp.hpp"
#include "skeet/engine/offset_storage.hpp"
#include "skeet/esp/player_runtime.hpp"
#include "skeet/misc/runtime.hpp"
#include "skeet/movement/steering_math.hpp"
#include "skeet/script/cvar_api.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace skeet::network {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}
std::int32_t integer(float value) noexcept {
    return _mm_cvttss_si32(_mm_set_ss(value));
}
void* networkable(void* player) noexcept {
    return static_cast<std::byte*>(player) + 8;
}
std::int32_t index(void* player) {
    return engine::invoke<std::int32_t>(networkable(player), 10);
}
bool dormant(void* player) {
    return engine::invoke<bool>(networkable(player), 9);
}

bool alive(const SharedEspRuntime& runtime, void* player) { //433F1E5F
    if (!player)
        return false;
    if (!dormant(player))
        return read<std::uint8_t>(player, runtime.offsets.life_state) == 0;
    if (!runtime.player_resource || !*runtime.player_resource)
        return false;
    const auto resource = *runtime.player_resource;
    const auto player_index = index(player);
    if (!read<std::uint8_t>(resource, runtime.offsets.connected + player_index))
        return false;
    return read<std::uint8_t>(resource, runtime.offsets.alive + player_index) != 0;
}
bool enemy(const SharedEspRuntime& runtime, void* local, void* player) { //433E4E4E
    if (!local || !player || local == player)
        return false;
    if (script::cvar_int(runtime.game_type.get()) == 6) {
        const auto squad = read<std::int32_t>(local, runtime.offsets.survival_team);
        return squad == -1 || squad != read<std::int32_t>(player, runtime.offsets.survival_team);
    }
    if (!runtime.player_resource || !*runtime.player_resource)
        return false;
    const auto resource = *runtime.player_resource;
    const auto first = index(local), second = index(player);
    if (!read<std::uint8_t>(resource, runtime.offsets.connected + first) ||
        !read<std::uint8_t>(resource, runtime.offsets.connected + second))
        return false;
    const auto other_team =
        read<std::int32_t>(resource, runtime.offsets.resource_team + second * 4);
    const auto local_team = read<std::int32_t>(resource, runtime.offsets.resource_team + first * 4);
    if (local_team == 2) {
        if (other_team == 3)
            return true;
    } else if (local_team == 3) {
        if (other_team == 2)
            return true;
    } else
        return false;
    return script::cvar_int(runtime.teammates_are_enemies.get()) != 0;
}
void geometry(esp::PlayerGeometry& output, void* entity, esp::Vector3 origin,
              const SharedEspOffsets& offsets) {
    //4341AF4A retains pose reads after the first two engine bounds calls.
    output.origin = origin;
    output.center = origin;
    const auto& minimum = engine::invoke<const esp::Vector3&>(entity, 76);
    const auto& maximum = engine::invoke<const esp::Vector3&>(entity, 77);
    output.center.z += (minimum.z + maximum.z) * 0.5f;
    output.eye_height = read<float>(entity, offsets.pose_parameters + 4) * 18.0f + 58.0f;
    output.pose_adjustment = read<float>(entity, offsets.pose_parameters + 0x30) * 6.0f;
    const auto& second_maximum = engine::invoke<const esp::Vector3&>(entity, 77);
    output.lower = {output.center.x, output.center.y, output.center.z - second_maximum.z * 0.6f};
    output.upper = {output.center.x, output.center.y,
                    (output.origin.z + output.eye_height) - output.pose_adjustment};
}
bool valid_origin(esp::Vector3 origin) noexcept {
    for (const auto value : {origin.x, origin.y, origin.z})
        if (!std::isfinite(value) || !(value > -16384.0f) || !(value < 16384.0f))
            return false;
    return true;
}
float rule_time(const SharedEspRuntime& runtime) noexcept { //433FC87C
    return runtime.game_rules && *runtime.game_rules
               ? read<float>(*runtime.game_rules, runtime.offsets.rule_time)
               : 0.0f;
}
}
SharedEspOffsets shared_esp_offsets(const engine::NativeOffsetStorage& storage) {
    const auto decode = [&](std::uint32_t pointer, std::uint32_t key, std::uint32_t mask) {
        return static_cast<std::ptrdiff_t>(*storage.globals.pointers.at(pointer) ^
                                           storage.globals.keys.at(key) ^ mask);
    };
    return {decode(0x43468e78, 0x434684e4, 0x9d920e35), decode(0x43467cb8, 0x4346a814, 0x9571d5f5),
            decode(0x4346a724, 0x4346857c, 0xd266b537), decode(0x434684f0, 0x4346a558, 0xc1c49a34),
            decode(0x434678ac, 0x43467b7c, 0x430c8993), decode(0x43467d78, 0x43467b80, 0x1caaf0a8),
            decode(0x43468d6c, 0x43468428, 0x9f8e7f71)};
}

bool receive_shared_esp(SharedEspRuntime& runtime, void* message) { //433D5BCF
    auto* owner = runtime.owner;
    if (!owner)
        return false;
    auto* local = runtime.local_player;
    if (!local || !owner->construct_message || !owner->destroy_message ||
        !owner->construct_receiver || !owner->destroy_receiver)
        return false;
    if (engine::invoke<bool>(runtime.engine, 82) || engine::invoke<bool>(runtime.engine, 93))
        return false;
    if (script::cvar_int(const_cast<void*>(owner->game_type)) == 6)
        return false;
    if (!script::cvar_int(const_cast<void*>(owner->send_gate_a)) &&
        !script::cvar_int(const_cast<void*>(owner->send_gate_b)))
        return false;
    const auto flags = read<std::uint32_t>(message, 0x34);
    if ((flags & 0x10u) && read<std::uint32_t>(read<const void*>(message, 0x18), 0x10) != 0)
        return false;
    if (!(flags & 0x40u) || read<std::uint32_t>(message, 0x20) != 0 || (flags & 0x185u) != 0x185u)
        return false;
    const auto raw_sender = read<std::uint32_t>(message, 8);
    if (raw_sender >= 64u)
        return false;
    const auto sender = static_cast<std::int32_t>(raw_sender + 1u);
    if (sender == engine::invoke<std::int32_t>(owner->engine_client, 12))
        return true;

    const std::array<std::uint32_t, 5> wire{
        read<std::uint32_t>(message, 0x10), read<std::uint32_t>(message, 0x14),
        read<std::uint32_t>(message, 0x28), read<std::uint32_t>(message, 0x24),
        read<std::uint32_t>(message, 0x2c)};
    SharedEspPacket packet;
    std::memcpy(packet.bytes.data(), wire.data(), packet.bytes.size());
    packet.sender_index = sender;
    misc::PlayerInfo info{};
    SharedEspFields decoded{};
    if (!engine::invoke<bool>(owner->engine_client, 8, sender, &info) ||
        !decode_shared_esp_packet(packet, *owner, read<std::uint32_t>(info.bytes.data(), 8),
                                 read<std::uint32_t>(info.bytes.data(), 0x90), decoded)) {
        dispatch_voice_message(runtime, message);
        return false;
    }
    if (!runtime.receive_enabled)
        return false;
    if (runtime.permission_gate &&
        !runtime.adjustments.records[std::uint32_t(sender)].adjustments.allow_shared_esp)
        return true;
    if (decoded.entity_index < 1 || decoded.entity_index > 64 || !valid_origin(decoded.origin))
        return true;
    const auto now = engine::invoke<float>(owner->engine_client, 14);
    const auto current_tick = lagcomp::receive_time_to_tick(now, runtime.engine_interval);
    if (decoded.tick > current_tick)
        return true;
    const auto second = integer(1.0f / runtime.tick_interval + 0.5f);
    const auto oldest =
        std::bit_cast<std::int32_t>(std::uint32_t(current_tick) - std::uint32_t(second));
    if (decoded.tick < oldest || (decoded.yaw > 360 && decoded.yaw < 511))
        return true;
    auto* player = engine::invoke<void*>(runtime.entity_list, 3, decoded.entity_index);
    auto* cached = runtime.cache.get(decoded.entity_index);
    if (!player || !engine::invoke<bool>(player, 158) || !alive(runtime, player) ||
        !enemy(runtime, local, player))
        return true;
    if (dormant(player)) {
        if (decoded.tick <= cached->geometry.last_live_tick)
            return true;
        geometry(cached->geometry, player, decoded.origin, runtime.offsets);
        cached->geometry.alpha = 1.0f;
        cached->geometry.last_live_tick = decoded.tick;
    } else {
        if (decoded.yaw == 511 || decoded.tick <= cached->native.shared_yaw_tick)
            return true;
        cached->native.shared_rule_time = rule_time(runtime);
        cached->native.shared_yaw = normalize_movement_angle(float(decoded.yaw));
        cached->native.shared_yaw_tick = current_tick;
        cached->native.flags = (cached->native.flags & ~0x40u) | 0x80u;
    }
    return true;
}
} // namespace skeet::network
