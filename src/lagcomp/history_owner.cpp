#include "skeet/lagcomp/history_owner.hpp"
#include "skeet/client/entity_observer_registry.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>
#include <cstring>
#include <new>

namespace skeet::lagcomp {
namespace {
template <class T> T read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
void* allocate(engine::JobMemory memory, std::size_t size) { //43387448
    if (!size)
        return nullptr;
    auto* raw = memory.allocate(memory.process_heap, 8, size + 0x20);
    if (!raw)
        return nullptr;
    const auto address = reinterpret_cast<std::uintptr_t>(raw);
    const auto header = (address + 15) & ~std::uintptr_t(15);
    const auto displacement = std::uint32_t(header - address);
    const auto total = std::uint32_t(size + 0x20);
    std::memcpy(reinterpret_cast<void*>(header + 8), &displacement, 4);
    std::memcpy(reinterpret_cast<void*>(header + 12), &total, 4);
    return reinterpret_cast<void*>(header + 16);
}
void release(engine::JobMemory memory, void* object) { //433837D0
    auto* bytes = static_cast<std::byte*>(object);
    std::uint32_t displacement;
    std::memcpy(&displacement, bytes - 8, 4);
    memory.free(memory.process_heap, 0, bytes - 16 - displacement);
}
HistoryAnimationPublishBindings
publication_bindings(const HistoryAnimationPublishBindings& source,
                     const HistoryRecordingConfiguration& configuration, const float& exponent) {
    return {source.entity,
            source.fields,
            source.clock,
            source.local_time_entity,
            source.now,
            source.receive_interval,
            source.reset_byte,
            configuration,
            source.player_table,
            source.player_table_offset,
            source.allow_zero_sequence,
            exponent,
            source.candidate_attempts,
            source.movement,
            source.power,
            source.arccosine,
            source.atan2};
}
HistoryRecordingBindings recording_bindings(const HistoryRecordingBindings& source,
                                            HistoryRecordingConfiguration& configuration,
                                            const HistoryAnimationPublishBindings& publication,
                                            const HistoryVelocityCaptureBindings& velocity) {
    return {source.players,    source.records,       source.entity,
            configuration,     source.activation,    source.targeting,
            source.prelude,    source.fields,        source.globals,
            source.hypotheses, source.yaw_side_fmod, source.bones,
            source.jobs,       source.environment,   velocity,
            source.resolver,   publication,          source.maximum_unlag_ms};
}
}

HistoryOwnerRuntime::HistoryOwnerRuntime(HistoryOwnerRuntime*& active,
                                         HistoryOwnerBindings bindings)
    : bindings_(bindings), active_(&active), configuration_(bindings.recording.configuration),
      physics_(bindings.recording.velocity.reconstruction),
      velocity_{bindings.recording.velocity.entity, physics_,
                bindings.recording.velocity.maximum_velocity_convar,
                bindings.recording.velocity.landing_limit_convar},
      publication_(
          publication_bindings(bindings.recording.publication, configuration_, movement_exponent_)),
      recording_(recording_bindings(bindings.recording, configuration_, publication_, velocity_)),
      runtime_(recording_) {
    auto& registries = bindings_.registries;
    registries.lifecycle.append(this); //4347E5EF, before publishing any table
    if (bindings_.scratch)
        receiver_.emplace(runtime_, *bindings_.scratch);
    else
        receiver_.emplace(runtime_);
    registries.entities.append(*receiver_); //4347E5F7, physical owner-local receiver
    registries.packets.append(&packet_endpoint_); //4347E5FF
    engine::register_frame_update(registries.frame_update, &frame_endpoint_); //4347E607
    registries.entities.publish(*receiver_); //4347E60E, after all four registrations
    packet_endpoint_.vtable = packet_; //4347E617
    frame_endpoint_.vtable = frame_; //4347E61E
    vtable = primary_; //4347E625

    initialize_boolean_config(registries.configuration.descriptors, bindings_.field_ed048f3d,
                              0xED048F3Du); //4347E631..4347E63F; stored value/reserved byte survive
    configuration_.target_hitboxes = nullptr;
    configuration_.recording_enabled = recording_.activation.recording_enabled;
    configuration_.resolver_enabled = &recording_.globals.transition_enabled;
    configuration_.prediction_available = 0;
    configuration_.engine = recording_.entity.engine; //captured4346A514, full+40
    first_activity_ = 0x3D3;
    second_activity_ = 0x3D4;

    //Complete433B2213->433C1360 for the decoded seven-byte literal. The
    //native valid allocation path owns eight bytes including the terminator.
    crouch_.begin = nullptr;
    crouch_.end = nullptr;
    crouch_.limit = nullptr;
    auto* text = static_cast<char*>(allocate(registries.lifecycle.memory, 8));
    crouch_.begin = text;
    crouch_.end = text;
    crouch_.limit = text + 8;
    std::memmove(text, "_crouch", 7);
    crouch_.end = text + 7;
    text[7] = 0;
    physics_.jump_sequence_name = crouch_.begin; //both actual43475AC0+4C consumers
    movement_exponent_ = std::bit_cast<float>(0x3EDC8172u); //full+5C, actual powf exponent
    registries.events.add({0x9D2A2EF7u, this, event}); //4347E6C6 ECX is the subscription hash
    // The source worker for4336A5B3 reads this same receiver runtime. Publish
    // it only after the complete constructor has established its owned data.
    bind_history_recording_runtime(runtime_);
}
HistoryOwnerRuntime::~HistoryOwnerRuntime() {
    //Complete4336B1E9. Signed native capacity comparison, then free through
    //the same allocation header. Its three string words are not cleared.
    const auto capacity =
        std::bit_cast<std::int32_t>(std::uint32_t(reinterpret_cast<std::uintptr_t>(crouch_.limit)) -
                                    std::uint32_t(reinterpret_cast<std::uintptr_t>(crouch_.begin)));
    if (capacity >= 2 && crouch_.begin)
        release(bindings_.registries.lifecycle.memory, crouch_.begin);
}
void HistoryOwnerRuntime::bind_configuration() {
    auto* descriptor = find_config(bindings_.registries.configuration.descriptors, 0x5B900854u);
    configuration_.target_hitboxes = &static_cast<IntegerConfig*>(descriptor)->value;
}
void HistoryOwnerRuntime::observe_packet(const void* packet_value) {
    if (!*configuration_.recording_enabled || !*configuration_.resolver_enabled ||
        !bindings_.local_player)
        return;
    //Valid native packet contract: the decoded zero-based index addresses a
    //player. No null-packet/failure substitute replaces the guarded body.
    const auto original = read<std::uint32_t>(packet_value, bindings_.fields.packet_player_index);
    const auto index = std::bit_cast<std::int32_t>(original + 1u);
    if (index > 64)
        return;
    if (!engine::invoke<void*>(recording_.entity.entity_list, 3, index))
        return;
    auto& player = *recording_.players.get(std::uint32_t(index));
    write(&player.native, 0x1C4, read<std::uint32_t>(packet_value, bindings_.fields.packet_timing));
    write(&player.native, 0x1BC,
          read<std::uint32_t>(packet_value, bindings_.fields.packet_timing + 4u));
}
void HistoryOwnerRuntime::on_event(events::GameEventReader& reader) {
    events::FieldValue value;
    value.integer = UINT32_MAX;
    if (reader.read(0x8361150Eu, events::FieldType::integer, value) || value.integer == UINT32_MAX)
        return;
    const auto player = engine::invoke<std::int32_t>(configuration_.engine, 9, value.integer);
    if (player != engine::invoke<std::int32_t>(configuration_.engine, 12))
        return;
    for (std::uint32_t index = 1; index <= 64; ++index) {
        auto& state = recording_.players.get(index)->native;
        state.probe_count = 0;
        state.resolver_status = 0;
    }
}

#if defined(_M_IX86) || defined(__i386__)
#define SKEET_HISTORY_MEMBER __thiscall
#define SKEET_HISTORY_FAST __fastcall
#else
#define SKEET_HISTORY_MEMBER
#define SKEET_HISTORY_FAST
#endif
void SKEET_HISTORY_MEMBER HistoryOwnerRuntime::destroy(void* object) {
    auto* owner = static_cast<HistoryOwnerRuntime*>(object);
    destroy_history_owner(owner, *owner->active_);
}
void SKEET_HISTORY_MEMBER HistoryOwnerRuntime::initialize(void* object) {
    static_cast<HistoryOwnerRuntime*>(object)->bind_configuration();
}
void SKEET_HISTORY_MEMBER HistoryOwnerRuntime::unchanged(void*) {} //433CCE8E
void SKEET_HISTORY_FAST HistoryOwnerRuntime::packet(void* endpoint, const void* value) {
    static_cast<PacketEndpoint*>(endpoint)->owner->observe_packet(value);
}
void SKEET_HISTORY_FAST HistoryOwnerRuntime::frame(void* endpoint, std::uint8_t) {
    static_cast<FrameEndpoint*>(static_cast<engine::FrameUpdateEndpoint*>(endpoint))
        ->owner->update_environment();
}
void HistoryOwnerRuntime::event(void* owner, events::GameEventReader& reader,
                                events::EventPlayerIndices&) {
    static_cast<HistoryOwnerRuntime*>(owner)->on_event(reader);
}
#undef SKEET_HISTORY_MEMBER
#undef SKEET_HISTORY_FAST

std::size_t history_owner_size() noexcept {
    return sizeof(HistoryOwnerRuntime);
}
HistoryOwnerRuntime& construct_history_owner(void* storage, HistoryOwnerRuntime*& active,
                                             HistoryOwnerBindings bindings) {
    return *::new (storage) HistoryOwnerRuntime(active, bindings);
}
void destroy_history_owner(void* storage, HistoryOwnerRuntime*& active) {
    active = nullptr;
    if (!storage)
        return;
    auto* owner = static_cast<HistoryOwnerRuntime*>(storage);
    const auto memory = owner->bindings_.registries.lifecycle.memory;
    owner->~HistoryOwnerRuntime();
    release(memory, storage);
}
} //namespace skeet::lagcomp
