#include "skeet/animation/fix_runtime_bindings.hpp"
#include "skeet/engine/decoded_offset.hpp"
#include "skeet/animation/runtime_owner.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/movement/global_config.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/effects/player_opacity.hpp"
#include "skeet/rage/settings.hpp"
#include "skeet/anti_aim/config.hpp"
#include "skeet/esp/effects_config.hpp"
#include <stdexcept>
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace skeet::animation {
namespace {
std::uint32_t offset(const engine::NativeOffsetGlobals& globals, std::uint32_t pointer,
                     std::uint32_t key, std::uint32_t salt) {
    return std::uint32_t(std::ptrdiff_t(engine::DecodedOffset(globals, pointer, key, salt, 0)));
}
}
FixAnimationFields native_fix_animation_fields(const engine::NativeOffsetGlobals& globals) {
    // Exact operands in4337DE0B,433E80F7,4335B798,433C7FC5 and433CE396.
    // Previous-time1599541F is the derived simulation+4 slot, distinct from
    // the replicatedD3D4230D slot. The eye-angle consumer applies its own+4.
    return {offset(globals, 0x43468E78u, 0x434684E4u, 0x9D920E35u),
            offset(globals, 0x43469198u, 0x4346A538u, 0xD5FD2E2Cu),
            offset(globals, 0x43467004u, 0x4346A6B4u, 0x1599541Fu),
            offset(globals, 0x43467230u, 0x43467100u, 0xD3D4230Du),
            offset(globals, 0x43468FECu, 0x4346A3C4u, 0xA2A8247Fu),
            offset(globals, 0x43468DBCu, 0x434689C8u, 0x8481EA04u),
            offset(globals, 0x4346A5DCu, 0x43468430u, 0xCDE290D2u),
            offset(globals, 0x43468BD4u, 0x4346A678u, 0x7501CB91u),
            offset(globals, 0x4346A6CCu, 0x43468EF8u, 0x2F4E97C4u),
            offset(globals, 0x43468F84u, 0x43467E64u, 0x3B704B41u),
            offset(globals, 0x43468A34u, 0x43468C00u, 0xC246315Eu),
            offset(globals, 0x43467BA4u, 0x43468E1Cu, 0x74019D87u),
            offset(globals, 0x43468F88u, 0x4346A6D8u, 0x6E9852D6u),
            offset(globals, 0x43467A60u, 0x43468BA4u, 0xC1B66EE8u),
            offset(globals, 0x4346A724u, 0x4346857Cu, 0xD266B537u)};
}
FixAnimationEntityFields
native_fix_animation_entity_fields(const engine::NativeOffsetGlobals& globals) {
    return {offset(globals, 0x434688B4u, 0x4346A7D0u, 0x0471E13Cu),
            offset(globals, 0x43467EBCu, 0x43467038u, 0x432B1F5Bu),
            offset(globals, 0x43467A88u, 0x43467160u, 0x0D560FACu),
            offset(globals, 0x434688D0u, 0x43468C24u, 0xE2FBE695u),
            offset(globals, 0x43467D78u, 0x43467B80u, 0x1CAAF0A8u)};
}
AnimationCeiling native_animation_ceiling() {
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
    static const auto entry = [] {
        const auto module = GetModuleHandleW(L"msvcrt.dll");
        if (!module)
            throw std::runtime_error("Animation backup growth requires the loaded msvcrt.dll");
        const auto function = reinterpret_cast<AnimationCeiling>(GetProcAddress(module, "ceil"));
        if (!function)
            throw std::runtime_error("Loaded msvcrt.dll lacks the native ceil export");
        return function;
    }();
    return entry;
#else
    throw std::logic_error("Native animation ceiling requires Windows x86");
#endif
}
namespace {
client::ClientRuntimeStorage& checked(RuntimeOwner& bones,
                                      client::ConfigurationRuntimeOwner& settings,
                                      const effects::PlayerOpacityBindings& opacity) {
    auto& storage = bones.storage();
    auto& data = storage.data();
    if (&settings.storage() != &storage ||
        &settings.settings_menu() != &storage.settings_menu_storage())
        throw std::invalid_argument(
            "Fix animation owners must share the canonical storage, settings and hook list");
    auto* const globals = data.native_cell<std::byte*>(0x43467B04u);
    if (!globals || data.native_cell<void*>(0x434678F8u) != globals + 4 ||
        data.native_cell<void*>(0x43467EB8u) != globals + 0x10 ||
        data.native_cell<void*>(0x4346A338u) != globals + 0x20 ||
        !data.native_cell<std::int32_t*>(0x43475B2Cu) || !data.native_cell<void*>(0x43467204u) ||
        !data.native_cell<double (*)()>(0x43468C84u) || !data.native_cell<double*>(0x4346A7E4u) ||
        !data.native_cell<void**>(0x43468FF0u) ||
        !data.native_cell<engine::MemberFunction<void, std::int32_t>>(0x43468C10u))
        throw std::invalid_argument(
            "Fix animation requires published globals, client-state, tier0 and entity animation bindings");
    if (!data.native_cell<FixAnimationBackupNode**>(0x4346D6E4u) ||
        !data.native_cell<std::uint32_t>(0x4346D6E8u))
        throw std::invalid_argument("Fix animation requires its initialized canonical backup map");
    if (opacity.input != data.native_cell<void*>(0x4346A894u) ||
        &opacity.settings != &settings.colored_model_settings() ||
        opacity.entities.engine != data.native_cell<void*>(0x4346A514u) ||
        opacity.entities.entity_list != data.native_cell<void*>(0x4346A890u) ||
        &opacity.entities.local_player != &data.native_cell<void*>(0x43475808u) ||
        &opacity.active_effects != &data.native_cell<effects::EffectsCommandOwner*>(0x43475A1Cu) ||
        &opacity.smoke_registry != &data.native_cell<void*>(0x434679C0u) ||
        &opacity.smoke_clock != &data.native_cell<const std::int32_t*>(0x43468D4Cu) ||
        &opacity.interval != reinterpret_cast<float*>(globals + 0x20) ||
        &opacity.camera_position != &data.native_cell<RageVector>(0x43479CCCu) ||
        &opacity.suspended != &data.native_cell<std::uint8_t>(0x43468E58u) ||
        opacity.player_adjustments.settings_base() != &data.native_cell<std::uint8_t>(0x4347BE08u))
        throw std::invalid_argument(
            "Fix animation opacity must borrow its actual canonical renderer and player storage");
    return storage;
}
FixAnimationGlobals globals(client::ClientRuntimeStorage& storage,
                            client::ConfigurationRuntimeOwner& settings) {
    auto& data = storage.data();
    auto* const timing = data.native_cell<std::byte*>(0x43467B04u);
    return {settings.settings_menu().fix_animations,
            settings.rage_settings().global.enabled.value,
            settings.anti_aim_global().enabled.value,
            settings.movement().infinite_duck.value,
            settings.effects_settings().third_person_alive.value,
            data.native_cell<std::uint32_t>(0x434759C0u),
            *data.native_cell<std::int32_t*>(0x434678F8u),
            *reinterpret_cast<std::int32_t*>(timing + 4),
            *reinterpret_cast<std::int32_t*>(timing + 0x38),
            *reinterpret_cast<std::int32_t*>(timing + 0x3C),
            *data.native_cell<float*>(0x4346A338u),
            *reinterpret_cast<float*>(timing + 0x20),
            *data.native_cell<float*>(0x43467EB8u),
            *data.native_cell<std::int32_t*>(0x43475B2Cu),
            data.native_cell<client::EntityCommandOwner*>(0x43475898u),
            data.native_cell<void*>(0x43475808u),
            data.native_cell<RageVector>(0x43476414u),
            data.native_cell<RageVector>(0x4347BD30u),
            data.native_cell<std::uint8_t>(0x43475BD1u),
            data.native_cell<double*>(0x4346A7E4u),
            data.native_cell<double (*)()>(0x43468C84u),
            data.native_cell<FixAnimationBackupNode**>(0x4346D6E4u),
            data.native_cell<std::uint32_t>(0x4346D6E8u),
            data.native_cell<std::uint32_t>(0x4346D6ECu),
            data.native_cell<std::int32_t>(0x4347C9ECu)};
}
const void* interpolation(const engine::NativeDataImage& data) {
    const auto address = data.native_cell<std::uint32_t>(0x4346A850u) ^ 0x478720C2u;
    if (!address)
        throw std::invalid_argument("Fix animation requires the discovered cl_interpolate ConVar");
    return reinterpret_cast<const void*>(std::uintptr_t(address));
}
}
FixAnimationsBindings
native_fix_animations_bindings(RuntimeOwner& bones, client::ConfigurationRuntimeOwner& settings,
                               const client::PlayerDataUpdateOffsets& updates,
                               const effects::PlayerOpacityBindings& opacity) {
    auto& storage = checked(bones, settings, opacity);
    auto& data = storage.data();
    return {bones.bones(),
            {data.native_cell<void*>(0x4346A514u), data.native_cell<void*>(0x4346A890u),
             native_fix_animation_entity_fields(storage.offsets().globals)},
            bones.players(),
            native_fix_animation_fields(storage.offsets().globals),
            globals(storage, settings),
            data.native_cell<void*>(0x4346A894u),
            interpolation(data),
            data.native_cell<void**>(0x43468FF0u),
            &data.native_cell<std::byte>(0x4347BE00u),
            storage.data_update_observers(),
            updates,
            storage.owner_registries().at(0x43475988u),
            data.native_cell<engine::MemberFunction<void, std::int32_t>>(0x43468C10u),
            data.native_cell<void*>(0x43467204u),
            storage.memory(),
            data.native_cell<AnimationLayerProxy>(0x43476974u),
            opacity,
            settings.rage_settings().global.experimental(rage::ExperimentalRageOption::animation)};
}
FixAnimationBackupBindings
native_fix_animation_backup_bindings(RuntimeOwner& bones, const FixAnimationsBindings& bindings) {
    auto& data = bones.storage().data();
    if (&bindings.bones != &bones.bones() || &bindings.players != &bones.players() ||
        &bindings.globals.backup_buckets !=
            &data.native_cell<FixAnimationBackupNode**>(0x4346D6E4u))
        throw std::invalid_argument(
            "Fix backup runtime must share its actual source bone owner and backup map");
    return {data.native_cell<FixAnimationBackupNode**>(0x4346D6E4u),
            data.native_cell<std::uint32_t>(0x4346D6E8u),
            data.native_cell<std::uint32_t>(0x4346D6ECu),
            data.native_cell<FixAnimationBackupGrowth>(0x4346D6F0u),
            bindings.globals.backup_mode,
            bindings.globals.prediction_frame,
            bones.bones(),
            bindings.entity,
            bindings.fields.life_state_9d920e35,
            bones.storage().memory(),
            native_animation_ceiling()};
}
} //namespace skeet::animation
