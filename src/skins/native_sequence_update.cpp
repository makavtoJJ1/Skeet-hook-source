#include "skeet/skins/native_sequence_update.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/misc/viewmodel_lag_owner.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <xmmintrin.h>

namespace skeet::skins {
namespace {
constexpr std::uint32_t invalid = 0xFFFFFFFFu;
template <class T> T read(const void* object, std::ptrdiff_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
void* at(void* object, std::ptrdiff_t offset) {
    return static_cast<std::byte*>(object) + offset;
}
std::uint32_t name_hash(const char* text) {
    std::uint32_t result = 0x811C9DC5;
    for (; *text; ++text)
        result = (result ^ std::uint8_t(*text)) * 0x1000193u;
    return result ^ 0x92CA5DF1;
}
void receive(engine::RecvVarProxy proxy, void* entity, std::ptrdiff_t offset, std::int32_t value) {
    //433E593A/433E5Fxx: only the two intermediate variant words are unspecified.
    std::array<std::uint32_t, 10> data;
    data[0] = data[1] = 0;
    data[2] = std::bit_cast<std::uint32_t>(value);
    data[5] = 1;
    data[6] = data[7] = data[8] = data[9] = 0;
    proxy(reinterpret_cast<const engine::RecvProxyData*>(data.data()), entity, at(entity, offset));
}
struct AnimationContext {
    void* animation;
    std::array<std::uint32_t, 6> unspecified;
    void* entity;
};
#if UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(AnimationContext, entity) == 0x1C);
#endif
}

std::ptrdiff_t NativeSequenceRuntime::offset(std::uint32_t pointer, std::uint32_t key,
                                             std::uint32_t mask) const {
    const auto& globals = bindings_.offsets.globals;
    return std::bit_cast<std::int32_t>(*globals.pointers.at(pointer) ^ globals.keys.at(key) ^ mask);
}
std::int32_t NativeSequenceRuntime::team(const void* player) const {
    return read<std::int32_t>(player, offset(0x434688B0, 0x4346A518, 0x8B5586F6));
}
bool NativeSequenceRuntime::enabled(const void* player) const {
    return (team(player) == 3 ? settings_.knife_ct.value : settings_.knife_t.value) != 0;
}
std::int32_t NativeSequenceRuntime::selection(const void* player) const {
    return team(player) == 3 ? settings_.knife_model_ct.value : settings_.knife_model_t.value;
}
bool NativeSequenceRuntime::same_owner(const void* entity) const {
    const auto high = read<std::uint32_t>(entity, offset(0x43468390, 0x434683E4, 0xA1493792));
    const auto low = read<std::uint32_t>(entity, offset(0x43469248, 0x4346798C, 0x3C1D8AA6));
    const auto index = engine::invoke<std::int32_t>(bindings_.engine_client, 12);
    std::array<std::byte, 0x158> information;
    write(information.data(), 8, std::uint32_t(0));
    write(information.data(), 12, std::uint32_t(0));
    if (!engine::invoke<bool>(bindings_.engine_client, 8, index, information.data()))
        return false;
    const auto current_low = read<std::uint32_t>(information.data(), 8);
    const auto current_high = read<std::uint32_t>(information.data(), 12);
    return (current_low || current_high) && low == current_low && high == current_high;
}
std::uint32_t NativeSequenceRuntime::class_id(void* entity) const {
    return engine::invoke<std::uint32_t>(at(entity, 8), 2);
}
void* NativeSequenceRuntime::resolve(std::uint32_t handle) const {
    return engine::invoke<void*>(bindings_.entity_list, 4, handle);
}
std::int32_t NativeSequenceRuntime::sequence_count(void* header) const {
    if (!read<std::int32_t>(header, 0x150))
        return read<std::int32_t>(header, 0xBC);
    auto* model = engine::invoke<void*>(bindings_.model_info, 26, header); //433FD399/433CCB8F
    return read<std::int32_t>(model, 0x14);
}
void* NativeSequenceRuntime::sequence_description(void* header, std::int32_t index) const {
    if (read<std::int32_t>(header, 0x150)) {
        if (auto* model = engine::invoke<void*>(bindings_.model_info, 26, header)) { //43417239
            const auto* sequences = read<const std::byte*>(model, 8);
            const auto group = read<std::int32_t>(sequences, std::ptrdiff_t(index) * 16 + 8);
            const auto* groups = read<const std::byte*>(model, 0x58);
            auto* group_header = engine::invoke<void*>(
                bindings_.model_info, 24, read<void*>(groups, std::ptrdiff_t(group) * 0x90));
            index = read<std::int32_t>(sequences, std::ptrdiff_t(index) * 16 + 12);
            header = group_header;
        }
    }
    if (index < 0 || index >= read<std::int32_t>(header, 0xBC))
        index = 0; //433E9F0C
    return at(header, std::ptrdiff_t(index) * 0xD4 + read<std::int32_t>(header, 0xC0));
}
std::int32_t NativeSequenceRuntime::weighted_sequence(void* entity, std::uint32_t seed) const {
    if (!bindings_.animation)
        return -1;
    auto* random_seed = bindings_.random_seed;
    std::int32_t previous_seed = 0;
    if (random_seed) {
        previous_seed = *random_seed;
        *random_seed = std::bit_cast<std::int32_t>(seed);
    }
    auto** prediction_player = bindings_.prediction_player;
    auto* local = bindings_.receive.local_player;
    void* previous_player = nullptr;
    if (prediction_player && local) {
        previous_player = *prediction_player;
        *prediction_player = local;
    }
    AnimationContext context;
    context.animation = bindings_.animation;
    context.entity = entity;
    const auto target =
        read<engine::MemberFunction<std::int32_t, std::int32_t>>(context.animation, 0x5C);
    const auto result = target(&context, 213);
    if (prediction_player && local)
        *prediction_player = previous_player;
    if (random_seed)
        *random_seed = previous_seed;
    return result;
}
std::int32_t NativeSequenceRuntime::inspection_sequence(void* entity) const {
    if (!bindings_.animation)
        return -1;
    AnimationContext context;
    context.animation = bindings_.animation;
    context.entity = entity;
    //433C0554 leaves these two words on the target's stack; it does not pass
    //the context as an ECX receiver, unlike the weighted+5C engine target.
    using Lookup = std::int32_t(__cdecl*)(AnimationContext*, const char*);
    return read<Lookup>(context.animation, 0x64)(&context, state_.inspection_sequence.data());
}

void NativeSequenceRuntime::remap(void* viewmodel, std::int32_t& model, std::int32_t& sequence) {
    auto* local = bindings_.receive.local_player;
    if (!local || engine::invoke<bool>(bindings_.engine_client, 82) ||
        engine::invoke<bool>(bindings_.engine_client, 93) || !enabled(local) || !state_.knife_class)
        return;
    auto* weapon_field =
        static_cast<std::byte*>(viewmodel) + offset(0x43468A98, 0x43467E78, 0xA116369A);
    const auto weapon_handle = read<std::uint32_t>(weapon_field, 0);
    if (weapon_handle == invalid)
        return;
    auto* weapon = resolve(weapon_handle);
    if (!weapon || !same_owner(weapon))
        return;
    const auto weapon_class = class_id(weapon);
    const bool knife = weapon_class == state_.knife_class;
    std::int32_t selected_model = -1;
    local = bindings_.receive.local_player;
    if (knife && local) {
        const auto index = std::uint32_t(selection(local));
        if (index >= state_.models.size())
            return;
        selected_model = state_.models[index].entity_model;
        if (selected_model == -1)
            return;
        const auto previous =
            model == -1 ? read<std::int16_t>(viewmodel, offset(0x43468978, 0x43468F7C, 0x47877DEB))
                        : model;
        if (previous != selected_model)
            model = selected_model;
    }
    const auto command = engine::invoke<std::uint32_t>(bindings_.engine_client, 205);
    //433E561F captures the live owner after the engine command query.
    const auto& history =
        misc::viewmodel_lag_owner_state(bindings_.viewmodel_owner)->snapshots[command % 150];
    if (history.command != command ||
        history.weapon_handle != read<std::uint32_t>(weapon_field, 0) ||
        history.animation_token_08 !=
            read<std::int32_t>(viewmodel, offset(0x43468DA0, 0x4346A6BC, 0x695FF325)))
        return;
    if (history.animation_token_0c ==
            read<std::int32_t>(viewmodel, offset(0x43467B4C, 0x43468414, 0xAEF72A04)) &&
        (!knife || history.selected_model == selected_model)) {
        sequence = history.sequence;
        return;
    }
    local = bindings_.receive.local_player;
    if (!local || read<std::uint8_t>(local, offset(0x43468E78, 0x434684E4, 0x9D920E35)))
        return;
    if (read<std::uint8_t>(local, offset(0x43467EC4, 0x43467AF8, 0x53E7C7D2))) {
        auto mapped = weighted_sequence(viewmodel, history.random_seed);
        if (mapped == -1)
            mapped = inspection_sequence(viewmodel);
        if (mapped != -1)
            sequence = mapped;
        return;
    }
    if (!knife || sequence < 0)
        return;
    const auto index = std::uint32_t(selection(bindings_.receive.local_player));
    const auto& target = state_.models[index];
    if (target.entity_model == state_.original_model || state_.original_model == -1)
        return;
    auto* source_model = engine::invoke<void*>(bindings_.model_info, 1, state_.original_model);
    if (!source_model)
        return;
    auto* source = engine::invoke<void*>(bindings_.model_info, 32, source_model);
    if (!source || sequence >= sequence_count(source))
        return;
    auto* description = sequence_description(source, sequence);
    const auto* name = static_cast<const char*>(description) + read<std::int32_t>(description, 4);
    if (!*name)
        name = nullptr;
    const auto activity = read<std::int32_t>(description, 0x10);
    auto* destination_model = engine::invoke<void*>(bindings_.model_info, 1, target.entity_model);
    if (!destination_model)
        return;
    auto* destination = engine::invoke<void*>(bindings_.model_info, 32, destination_model);
    if (!destination)
        return;
    const auto count = sequence_count(destination);
    if (name) {
        std::array<char, 64> copied{};
        ::strcpy_s(copied.data(), copied.size(), name);
        const auto variants = knife_models()[index].native_word_08;
        if (variants > 1 && name_hash(name) == 0xD4A1BEA9u) {
            const auto time = read<float>(weapon, offset(0x43467A50, 0x4346A458, 0xF71CE331));
            const auto ticks = time / *bindings_.interval + 0.5f;
            bindings_.seed_random(_mm_cvttss_si32(_mm_set_ss(ticks)));
            copied[7] =
                char(std::uint8_t(bindings_.random_integer(1, std::int32_t(variants))) + 0x30);
        }
        for (std::int32_t i = 0; i < count; ++i) {
            auto* candidate = sequence_description(destination, i);
            const auto* candidate_name =
                static_cast<const char*>(candidate) + read<std::int32_t>(candidate, 4);
            if (::_stricmp(candidate_name, copied.data()) == 0) {
                sequence = i;
                return;
            }
        }
    }
    if (activity != -1)
        for (std::int32_t i = 0; i < count; ++i)
            if (read<std::int32_t>(sequence_description(destination, i), 0x10) == activity) {
                sequence = i;
                return;
            }
}

void NativeSequenceRuntime::process(void* player, std::uintptr_t incoming) {
    // Keep each native vector's original end until its replay loop finishes.
    const auto end = state_.viewmodels.end();
    for (auto pending = state_.viewmodels.begin(); pending != end; ++pending) {
        if (pending->handle == invalid)
            continue;
        auto* entity = resolve(pending->handle);
        if (!entity)
            continue;
        std::int32_t model = pending->model, sequence = pending->sequence;
        remap(entity, model, sequence);
        if (model != -1)
            receive(bindings_.receive.model, entity, offset(0x43468978, 0x43468F7C, 0x47877DEB),
                    model);
        if (sequence != -1)
            receive(bindings_.receive.sequence, entity, offset(0x43467ABC, 0x4346A56C, 0xA25D8271),
                    sequence);
    }
    state_.viewmodels.clear();
    auto* local = bindings_.receive.local_player;
    if (player && !read<std::uint8_t>(player, offset(0x43468E78, 0x434684E4, 0x9D920E35)) &&
        local && !engine::invoke<bool>(bindings_.engine_client, 82) &&
        !engine::invoke<bool>(bindings_.engine_client, 93) && enabled(local)) {
        const auto* snapshot = reinterpret_cast<const std::byte*>(incoming);
        const auto count = read<std::int32_t>(snapshot, 0x4044);
        auto* resolver =
            state_.inventory_resolver; //captured by4348DE49, after descriptor registration
        for (std::int32_t i = 0; i < count; ++i) {
            const auto* entry = snapshot + 0x44 + std::ptrdiff_t(i) * 8;
            auto* networkable = engine::invoke<void*>(resolver, 0, read<std::uint32_t>(entry, 0));
            if (!networkable)
                continue;
            auto* unknown = engine::invoke<void*>(networkable, 0);
            if (!unknown || !engine::invoke<void*>(unknown, 7))
                continue;
            const auto entity_class = engine::invoke<std::uint32_t>(networkable, 2);
            if (!entity_class)
                continue;
            auto* entity = static_cast<std::byte*>(networkable) - 8;
            if (entity_class == state_.knife_class) {
                const auto tag = read<std::uint32_t>(entry, 4);
                if (!same_owner(entity))
                    continue;
                const auto index = std::uint32_t(selection(player));
                if (index >= state_.models.size())
                    continue;
                if (!tag) {
                    state_.original_model =
                        read<std::int16_t>(entity, offset(0x43468978, 0x43468F7C, 0x47877DEB));
                    state_.original_definition =
                        read<std::uint16_t>(entity, offset(0x4346908C, 0x43468BA0, 0x5C9F0FBB));
                }
                const auto& model = state_.models[index];
                write(entity, offset(0x4346908C, 0x43468BA0, 0x5C9F0FBB),
                      knife_models()[index].definition);
                write(entity, offset(0x43469370, 0x43468E74, 0x1D7092B7), model.entity_model);
                write(entity, offset(0x434679E4, 0x43467DE4, 0xE7BAED36), model.view_model);
                write(entity, offset(0x43467474, 0x43468904, 0x26F3C118), model.world_model);
                write(entity, offset(0x43468940, 0x4346A818, 0xADBAA629), std::int32_t(3));
                if (model.entity_model == -1 ||
                    model.entity_model ==
                        read<std::int16_t>(entity, offset(0x43468978, 0x43468F7C, 0x47877DEB)))
                    continue;
                engine::invoke<void>(bindings_.model_cache, 33);
                const auto selected_model = model.entity_model;
                engine::invoke<void>(bindings_.model_cache, 33);
                engine::invoke<void>(entity, 75, selected_model);
                engine::invoke<void>(bindings_.model_cache, 34);
                engine::invoke<void>(bindings_.model_cache, 34);
                if (tag)
                    refresh_.schedule_full_update(refresh_.current_time() + 0.25f);
            } else if (entity_class == state_.weapon_world_model_class) {
                local = bindings_.receive.local_player;
                if (!local)
                    continue;
                const auto index = std::uint32_t(selection(local));
                if (index >= state_.models.size())
                    continue;
                const auto owner =
                    read<std::uint32_t>(entity, offset(0x43468C9C, 0x43469280, 0x4E36DFD7));
                if (owner == invalid ||
                    std::uint32_t(std::uint16_t(owner)) !=
                        engine::invoke<std::uint32_t>(bindings_.engine_client, 12))
                    continue;
                const auto handle =
                    read<std::uint32_t>(entity, offset(0x4346922C, 0x434691D8, 0xA4CECBE2));
                if (handle == invalid)
                    continue;
                auto* weapon = resolve(handle);
                if (!weapon)
                    continue;
                const auto weapon_class = class_id(weapon);
                if (weapon_class != state_.knife_class || !same_owner(weapon))
                    continue;
                const auto model = state_.models[index].view_model;
                if (model == -1 ||
                    model ==
                        read<std::int16_t>(entity, offset(0x43468978, 0x43468F7C, 0x47877DEB)) ||
                    !bindings_.viewmodel_model)
                    continue;
                receive(bindings_.viewmodel_model, entity,
                        offset(0x43468978, 0x43468F7C, 0x47877DEB), model);
                write(entity, offset(0x43468978, 0x43468F7C, 0x47877DEB), std::int16_t(model));
            }
        }
    }
    const auto handles_end = state_.weapon_handles.end();
    for (auto pending = state_.weapon_handles.begin(); pending != handles_end; ++pending) {
        if (pending->handle == invalid)
            continue;
        if (auto* entity = resolve(pending->handle))
            receive(bindings_.receive.weapon_handle, entity,
                    offset(0x4346922C, 0x434691D8, 0xA4CECBE2),
                    std::bit_cast<std::int32_t>(pending->value));
    }
    state_.weapon_handles.clear();
}
} //namespace skeet::skins
