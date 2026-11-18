#include "skeet/skins/sequence_owner.hpp"
#include "skeet/skins/knife_precache.hpp"
#include "skeet/skins/native_sequence_update.hpp"
#include "skeet/engine/interface_call.hpp"
#include <array>
#include <cstring>
#include <new>

namespace skeet::skins {
namespace {
#if defined(_M_IX86) || defined(__i386__)
#define SEQUENCE_MEMBER __thiscall
#else
#define SEQUENCE_MEMBER
#endif
using OwnerEntry = void(SEQUENCE_MEMBER*)(void*);
struct SequenceOwner;
void SEQUENCE_MEMBER destroy(void*);
void SEQUENCE_MEMBER unchanged(void*) {} //433CCE8E
void SEQUENCE_MEMBER reset(void*);
void SEQUENCE_MEMBER reset_forwarder(void*);
void SEQUENCE_MEMBER precache(void*);
const std::array<OwnerEntry, 9> owner_table{destroy,         unchanged, unchanged,
                                            unchanged,       unchanged, reset,
                                            reset_forwarder, precache,  unchanged};
void register_integer(ConfigLink& registry, IntegerConfig& value, std::uint32_t hash) {
    value.hash = hash;
    value.serialized_size = 4;
    value.type = ConfigType::integer;
    append_config_link(registry, value);
}
void capture_proxy(void* client, std::uint32_t klass, std::uint32_t table, std::uint32_t property,
                   engine::RecvVarProxy& destination) {
    if (auto* found = engine::find_receive_property(client, klass, table, property))
        destination = found->proxy;
}
struct SequenceOwner {
    const OwnerEntry* vtable = nullptr;
    SequenceEntityObserverReceiver endpoint;
    SequenceOwnerBindings bindings;
    engine::JobMemory memory;
    SequenceOwner(engine::OwnerRegistryBinding owners, client::EntityObserverRegistry& updates,
                  ConfigRegistries& registry, SequenceOwnerBindings input)
        : bindings(input), memory(owners.memory) {
        owners.append(this); //433676A0, before the native vtables/configuration
        updates.append(endpoint, bindings.update); //4348DDD4 ->4333BF32, table still null
        vtable = owner_table.data(); //4348DDDC, primary4346FF18
        updates.publish(endpoint); //4348DDE4, observer4346DC68
        auto& setting = bindings.settings;
        initialize_boolean_config(registry.descriptors, setting.knife_t, 0xD347BC87);
        initialize_boolean_config(registry.descriptors, setting.knife_ct, 0xD4F2A320);
        register_integer(registry.descriptors, setting.knife_model_t, 0xE0BDA969);
        register_integer(registry.descriptors, setting.knife_model_ct, 0xB3C4B6CA);
        auto& state = bindings.state;
        state.inventory_resolver = bindings.entity_list;
        state.weapon_world_model_class = bindings.weapon_world_model_class;
        state.knife_class = bindings.knife_class;
        state.original_model = state.original_definition = -1;
        state.inspection_sequence = "lookat01";
        state.models.fill({0, 0, 0});
        state.default_models.fill({0, 0, 0});
        state.viewmodels.clear();
        state.weapon_handles.clear();
        bindings.receive.previous_handle = 0xFFFFFFFF;
        bindings.receive.previous_sequence = -1;
        bindings.viewmodel_model = nullptr;
        bindings.receive.bindings.model = nullptr;
        bindings.receive.bindings.sequence = nullptr;
        // These three lookups are unchanged original code4348E026..E06C.
        capture_proxy(bindings.base_client, 0xAEEB25F3, 0x6B34DA8F, 0x0BA094FB,
                      bindings.viewmodel_model);
        capture_proxy(bindings.base_client, 0xDD75A186, 0xB05C42F2, 0x0BA094FB,
                      bindings.receive.bindings.model);
        capture_proxy(bindings.base_client, 0xDD75A186, 0xB05C42F2, 0xA5BF8ECD,
                      bindings.receive.bindings.sequence);
        bindings.death_listener.bind(bindings.events); //single canonical433ABC62 route
    }
};
void SEQUENCE_MEMBER destroy(void* object) { //43369BFF ->433AFDB4 ->433837D0
    auto* owner = static_cast<SequenceOwner*>(object);
    if (!owner)
        return;
    unbind_sequence_receive_runtime(owner->bindings.receive);
    std::vector<PendingWeaponHandle>{}.swap(owner->bindings.state.weapon_handles);
    std::vector<PendingViewModel>{}.swap(owner->bindings.state.viewmodels);
    const auto memory = owner->memory;
    owner->~SequenceOwner();
    auto* bytes = static_cast<std::byte*>(object);
    std::uint32_t displacement;
    std::memcpy(&displacement, bytes - 8, 4);
    memory.free(memory.process_heap, 0, bytes - 16 - displacement);
}
void SEQUENCE_MEMBER reset(void* object) {
    static_cast<SequenceOwner*>(object)->bindings.receive.reset();
}
void SEQUENCE_MEMBER reset_forwarder(void* object) {
    static_cast<SequenceOwner*>(object)->vtable[5](object);
}
void SEQUENCE_MEMBER precache(void* object) {
    auto& b = static_cast<SequenceOwner*>(object)->bindings;
    refresh_knife_precache(b.state, b.string_tables);
}
#undef SEQUENCE_MEMBER
}
std::size_t sequence_owner_size() noexcept {
    return sizeof(SequenceOwner);
}
void construct_sequence_owner(void* storage, engine::OwnerRegistryBinding owners,
                              client::EntityObserverRegistry& updates, ConfigRegistries& registry,
                              SequenceOwnerBindings bindings) {
    // Native43481F83 publishes only after4348DDB6 returns.
    auto* owner = ::new (storage) SequenceOwner(owners, updates, registry, bindings);
    bind_sequence_receive_runtime(owner->bindings.receive);
}
void* initialize_sequence_owner(engine::OwnerRegistryBinding owners,
                                client::EntityObserverRegistry& updates, ConfigRegistries& registry,
                                SequenceOwnerBindings bindings) {
    const auto size = sizeof(SequenceOwner);
    auto* allocation = owners.memory.allocate(owners.memory.process_heap, 8, size + 0x20);
    if (!allocation) {
        unbind_sequence_receive_runtime(bindings.receive);
        return nullptr;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(allocation);
    const auto header = (base + 15) & ~std::uintptr_t(15);
    const auto displacement = std::uint32_t(header - base);
    const auto allocation_size = std::uint32_t(size + 0x20);
    std::memcpy(reinterpret_cast<void*>(header + 8), &displacement, 4);
    std::memcpy(reinterpret_cast<void*>(header + 12), &allocation_size, 4);
    auto* storage = reinterpret_cast<void*>(header + 16);
    construct_sequence_owner(storage, owners, updates, registry, bindings);
    return storage;
}
} //namespace skeet::skins
