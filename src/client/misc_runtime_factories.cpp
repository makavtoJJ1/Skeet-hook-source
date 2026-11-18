#include "skeet/client/misc_runtime_factories.hpp"
#include "skeet/client/native_startup.hpp"
#include "skeet/engine/owner_allocation.hpp"
#include "skeet/engine/native_data_image.hpp"
#include "skeet/engine/scene_queries.hpp"
#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>

namespace skeet::client {
namespace {
using Bindings = MiscRuntimeFactoryBindings;

void* allocate(engine::JobMemory memory, std::size_t native_size, std::size_t source_size) {
    auto* const storage =
        engine::allocate_owner_storage(memory, std::max(native_size, source_size));
    // The original allocation-failure edge reaches unresolved INT1; source allocation failure terminates this factory with bad_alloc.
    if (!storage)
        throw std::bad_alloc{};
    return storage;
}
void initialize_low_fps(void* owner) { //434903AD ->4347E6F2
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.low_fps;
    (void)misc::initialize_low_fps_owner(b.lifecycle, b.commands, b.hud, b.configuration,
                                         c.settings, c.rage, c.engine, c.scripts, c.input, c.layout,
                                         c.life_state_offset, c.active);
}
void initialize_quick_switch(void* owner) { //43481DC0 ->4347DC0A
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.quick_switch;
    auto* const storage = allocate(b.lifecycle.memory, 0x44, misc::quick_switch_owner_size());
    (void)misc::construct_quick_switch(storage, b.lifecycle, b.commands, b.recharge, b.prediction,
                                       b.configuration, c.settings, c.runtime);
}
void initialize_grenades(void* owner) { //43481EDA ->4348675E
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.grenades;
    const misc::GrenadeOwnerRegistries registries{b.lifecycle, b.commands, b.empty_ticks, b.hud,
                                                  b.entities,  b.updates,  b.recharge};
    //43481EF3 publishes this invocation's complete owner, including null.
    (void)misc::create_grenade_owner(registries, b.configuration, c.settings, c.visuals, c.engine);
}
void initialize_no_fall_damage(void* owner) { //4348401C ->4348DCB4
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.no_fall_damage;
    auto* const storage = allocate(b.lifecycle.memory, 0x40, misc::no_fall_damage_owner_size());
    (void)misc::construct_no_fall_damage(storage, b.lifecycle, b.commands, b.hud, b.configuration,
                                         c.settings, c.engine);
}
void initialize_automatic_weapons(void* owner) { //4348E51F ->4347D991
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.automatic_weapons;
    auto* const storage = allocate(b.lifecycle.memory, 0x64, misc::automatic_weapons_owner_size());
    (void)misc::construct_automatic_weapons(storage, b.lifecycle, b.commands, b.configuration,
                                            c.settings, c.state, c.engine);
}
void initialize_knife(void* owner) { //43483FF8 ->434802E2
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.knife;
    auto* const storage = allocate(b.lifecycle.memory, 0x34, misc::knife_owner_size());
    (void)misc::construct_knife(storage, b.lifecycle, b.commands, b.configuration, c.settings,
                                c.engine, c.scheduler_flags);
}
void initialize_zeus(void* owner) { //43490389 ->43485D4C
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.zeus;
    auto* const storage = allocate(b.lifecycle.memory, 0x24, misc::zeus_owner_size());
    (void)misc::construct_zeus(storage, b.lifecycle, b.commands, b.configuration, c.settings,
                               c.state, c.engine);
}
void initialize_clan_tag(void* owner) { //4348DA01 ->4347E092
    auto& b = *static_cast<Bindings*>(owner);
    auto& c = b.clan_tag;
    auto* const storage = allocate(b.lifecycle.memory, 0x30, misc::clan_tag_owner_size());
    misc::construct_clan_tag_owner(storage, b.lifecycle, b.entities, b.messages, b.configuration,
                                   c.settings, c.runtime, c.environment);
}
void initialize_rebuy(void* owner) { //4347E2E3 ->4348217D
    (void)misc::create_rebuy_fix_owner(static_cast<Bindings*>(owner)->rebuy.engine);
}

bool same_memory(engine::JobMemory first, engine::JobMemory second) noexcept {
    return first.allocate == second.allocate && first.free == second.free &&
           first.process_heap == second.process_heap;
}
bool same_input(InputBitmap first, InputBitmap second) noexcept {
    return first.words.data() == second.words.data() && first.words.size() == second.words.size() &&
           first.bit_count == second.bit_count;
}
void require_bindings(NativeStartupSources& sources, const Bindings& b) {
    const auto& image = sources.data();
    if (!b.lifecycle.memory.allocate || !b.lifecycle.memory.free ||
        !b.lifecycle.memory.process_heap)
        throw std::invalid_argument("Misc factories require the canonical process heap");
    struct RegistryAlias {
        const engine::OwnerRegistryBinding* binding;
        std::uint32_t address;
    };
    const std::array aliases{RegistryAlias{&b.lifecycle, 0x43475B78u},
                             RegistryAlias{&b.hud, 0x43475A9Cu},
                             RegistryAlias{&b.updates, 0x43475988u},
                             RegistryAlias{&b.frames, 0x43475AD8u},
                             RegistryAlias{&b.messages, 0x43475B94u},
                             RegistryAlias{&b.rebuy.engine.owners, 0x43475B78u},
                             RegistryAlias{&b.rebuy.engine.frame_updates, 0x43475AD8u}};
    for (const auto& alias : aliases)
        if (&alias.binding->registry != &image.native_cell<engine::OwnerRegistry>(alias.address) ||
            !same_memory(alias.binding->memory, b.lifecycle.memory))
            throw std::invalid_argument(
                "Misc factories must share their native registry cells and heap");
    if (&b.configuration.descriptors != &image.native_cell<ConfigLink>(0x434798BCu) ||
        &b.configuration.hotkeys != &image.native_cell<HotkeyLink>(0x434798C4u) ||
        &b.rebuy.engine.configuration != &b.configuration)
        throw std::invalid_argument("Misc factories require the canonical configuration registry");
    if (&b.commands.image() != &image || &b.empty_ticks.image() != &image ||
        &b.entities.image() != &image || &b.recharge.image() != &image ||
        &b.prediction.image() != &image || &b.rebuy.engine.command_owners != &b.commands ||
        &b.commands.native_storage() != &image.native_cell<engine::OwnerRegistry>(0x434757A0u) ||
        &b.empty_ticks.native_storage() != &image.native_cell<engine::OwnerRegistry>(0x43475978u) ||
        &b.entities.native_storage() != &image.native_cell<engine::OwnerRegistry>(0x43475BA8u) ||
        &b.recharge.native_storage() != &image.native_cell<engine::OwnerRegistry>(0x43475784u) ||
        &b.prediction.native_storage() != &image.native_cell<engine::OwnerRegistry>(0x43475AB0u))
        throw std::invalid_argument(
            "Misc factories require the canonical command and observer registries");
    if (&b.knife.scheduler_flags != &b.commands.services().scheduler_flags ||
        !same_input(b.low_fps.input, b.commands.services().input))
        throw std::invalid_argument(
            "Misc factories must share the live command flags and input words");
    const auto& g = b.grenades.engine;
    const auto& grenade_visuals = b.grenades.visuals;
    if (&g.active != &image.native_cell<void*>(0x43475AACu) ||
        &g.input != &image.native_cell<void*>(0x4346A894u) ||
        &g.live.engine_client != &image.native_cell<void*>(0x4346A514u) ||
        &g.view_angles.engine_client != &g.live.engine_client ||
        &g.live.local_player != &image.native_cell<void*>(0x43475808u) ||
        &g.live.game_rules_slot != &image.native_cell<void*>(0x4346A3A4u) ||
        &g.live.command_buttons != &image.native_cell<std::uint32_t>(0x434759C0u) ||
        &g.release_choke_enabled != &image.native_cell<std::uint8_t>(0x43467560u) ||
        &g.observer_suppressed != &image.native_cell<std::uint8_t>(0x434684E0u) ||
        &g.ribbons_suppressed != &image.native_cell<std::uint8_t>(0x43468E58u) ||
        &g.ribbons_color != &grenade_visuals.entity_color.value ||
        &g.live.grenade_entities_enabled != &grenade_visuals.entities.value ||
        &g.live.queries.grenades().bindings().entity_list != &image.native_cell<void*>(0x4346A890u))
        throw std::invalid_argument(
            "Misc grenade factory requires its canonical owner and live engine cells");
}
}

void bind_misc_runtime_factories(NativeStartupSources& sources,
                                 MiscRuntimeFactoryBindings& bindings) {
    require_bindings(sources, bindings);
    const std::array calls{
        NativeInitializerCall{0x434903ADu, &bindings, initialize_low_fps},
        NativeInitializerCall{0x43481DC0u, &bindings, initialize_quick_switch},
        NativeInitializerCall{0x43481EDAu, &bindings, initialize_grenades},
        NativeInitializerCall{0x4348401Cu, &bindings, initialize_no_fall_damage},
        NativeInitializerCall{0x4348E51Fu, &bindings, initialize_automatic_weapons},
        NativeInitializerCall{0x43483FF8u, &bindings, initialize_knife},
        NativeInitializerCall{0x43490389u, &bindings, initialize_zeus},
        NativeInitializerCall{0x4348DA01u, &bindings, initialize_clan_tag},
        NativeInitializerCall{0x4347E2E3u, &bindings, initialize_rebuy}};
    for (const auto& call : calls)
        sources.bind_initializer(call);
}
} //namespace skeet::client
