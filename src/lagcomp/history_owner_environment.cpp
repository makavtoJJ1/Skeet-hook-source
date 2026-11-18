#include "skeet/lagcomp/history_owner.hpp"
#include "skeet/resolver_runtime/environment_entry.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>
#include <cstring>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
//Complete43407DE2. Its COMISS policy admits a NaN decoded value and rejects
//either signed zero; the same required ConVar object is read at this gate.
bool environment_enabled(const void* object) noexcept {
    for (;;) {
        const auto* parent = read<const void*>(object, 0x1C);
        if (parent == object)
            break;
        object = parent;
    }
    const auto encoded = read<std::uint32_t>(object, 0x2C);
    return std::bit_cast<float>(encoded ^
                                std::uint32_t(reinterpret_cast<std::uintptr_t>(object))) != 0.0f;
}
}
void HistoryOwnerRuntime::update_environment() {
    const auto& bindings = recording_;
    if (!engine::invoke<bool>(configuration_.engine, 26))
        return;
    if (!environment_enabled(bindings_.environment_convar))
        return;
    if (!bindings_.local_player)
        return;
    if (read<std::uint8_t>(bindings_.local_player, bindings_.fields.local_life_state))
        return;
    if (bindings.hypotheses.selected_handle == -1)
        return;
    auto* target =
        engine::invoke<void*>(bindings.entity.entity_list, 4, bindings.hypotheses.selected_handle);
    if (!target)
        return;
    if (engine::invoke<bool>(static_cast<std::byte*>(target) + 8, 9))
        return;
    if (read<std::uint32_t>(target, bindings.fields.effects_38ca5228) & 8u)
        return;
    if (read<std::uint8_t>(target, bindings.fields.field_043fa575) !=
        read<std::uint8_t>(target, bindings.fields.field_334b460c))
        return;
    //433279A4 reloads the selected handle after target/networkable calls.
    const auto index = std::uint32_t(bindings.hypotheses.selected_handle) & 0xFFFFu;
    if (resolver_runtime::has_pending_environment_shot(bindings_.feedback.pending, index))
        return;
    if (!bindings.globals.adjustments.settings_base()[std::size_t(index) * 0x20 + 6])
        return;
    if (!index)
        return;
    auto& owner = *bindings.players.get(index);
    const resolver_runtime::EnvironmentLiveUpdateInput input{
        target,
        bindings.bones,
        bindings.resolver.local_time_entity,
        bindings.resolver.fields.local_time_9f8e7f71,
        bindings.globals.current_time,
        bindings_.command_tick,
        bindings.entity.previous_command,
        bindings.entity.current_command,
        bindings.globals.local_origin,
        bindings.environment.origin_offset,
        bindings.configuration.resolver_reworked()};
    //The common live implementation retains the canonical owner/ring. Its
    //world probe precedes the fresh studio/set/hitbox0/3 reads.
    resolver_runtime::update_environment_entry(owner, input, bindings.resolver.environment);
}
} //namespace skeet::lagcomp
