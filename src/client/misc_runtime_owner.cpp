#include "skeet/client/misc_runtime_owner.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/engine/hook_descriptor_storage.hpp"
#include "skeet/engine/scene_runtime_owner.hpp"
#include "skeet/client/player_runtime_owner.hpp"
#include "skeet/animation/runtime_owner.hpp"
#include "skeet/engine/netvar_initialization.hpp"
#include "skeet/animation/native_trigonometry.hpp"
#include "skeet/misc/runtime.hpp"
#include "skeet/misc/knife_aim.hpp"
#include "skeet/misc/knife_melee.hpp"
#include "skeet/movement/extrapolation.hpp"
#include "skeet/client/command_orchestrator.hpp"
#include "skeet/rage/runtime_owner.hpp"
#include <type_traits>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace skeet::client {
namespace {
std::ptrdiff_t offset(ClientRuntimeStorage& storage, std::uint32_t identity) {
    const auto& globals = storage.offsets().globals;
    for (const auto& recipe : engine::build_receive_offset_recipes())
        if (recipe.pointer_identity == identity)
            return engine::DecodedOffset(globals, identity, recipe.xor_key_identity,
                                         recipe.initial_xor_key, 0);
    for (const auto& recipe : engine::build_data_map_offset_recipes())
        if (recipe.pointer_identity == identity)
            return engine::DecodedOffset(globals, identity, recipe.xor_key_identity,
                                         recipe.initial_xor_key, 0);
    throw std::invalid_argument("Misc binding has no proven receive/datamap offset recipe");
}
std::uint32_t offset32(ClientRuntimeStorage& storage, std::uint32_t identity) {
    return static_cast<std::uint32_t>(offset(storage, identity));
}
template <class T> T& pointed(engine::NativeDataImage& image, std::uint32_t identity) {
    auto* const pointer = image.native_cell<T*>(identity);
    if (!pointer)
        throw std::invalid_argument("Misc runtime requires an initialized live engine alias");
    return *pointer;
}
void* variable(const EngineRuntimeBindings& engine, std::uint32_t identity) {
    auto* const object = engine.variables.at(identity).get();
    if (!object)
        throw std::invalid_argument("Misc runtime requires its genuine acquired ConVar");
    return object;
}
engine::OwnerRegistryBinding registry(ClientRuntimeStorage& storage, std::uint32_t identity) {
    return {storage.owner_registries().at(identity), storage.memory()};
}
misc::RuntimeInterfaces& initialize_interfaces(misc::RuntimeInterfaces& result,
                                               ClientRuntimeStorage& storage,
                                               const EngineRuntimeBindings& engine) {
    auto& data = storage.data();
    const auto core = engine.interfaces.core();
    if (core.engine_client != data.native_cell<void*>(0x4346A514u) ||
        core.entity_list != data.native_cell<void*>(0x4346A890u) ||
        core.cvar != data.native_cell<void*>(0x4346A87Cu) || &engine.variables.data() != &data)
        throw std::invalid_argument(
            "Misc runtime must share the canonical engine interfaces and variables");
    result.engine = core.engine_client;
    result.entity_list = core.entity_list;
    result.cvar = core.cvar;
    result.allocator = data.native_cell<void*>(0x43467204u);
    result.game_rules = data.native_cell<void**>(0x4346A3A4u);
    result.player_resource = data.native_cell<void**>(0x43468FF0u);
    result.max_clients = &pointed<std::int32_t>(data, 0x43469300u);
    result.name_cvar = variable(engine, 0x43467A5Cu);
    result.drawing_suspended = &data.native_cell<std::uint8_t>(0x43468E58u);
    result.log_hud_slot = &data.native_cell<misc::ColoredLogRing*>(0x434758CCu);
    if (!result.engine || !result.entity_list || !result.cvar || !result.allocator ||
        !result.game_rules || !result.player_resource)
        throw std::invalid_argument(
            "Misc runtime requires the genuine engine objects and live global slots");
    return result;
}
misc::RuntimeOffsets runtime_offsets(ClientRuntimeStorage& storage) {
    return {offset32(storage, 0x434688B0u), offset32(storage, 0x43468378u),
            offset32(storage, 0x43467C54u), offset32(storage, 0x4346926Cu)};
}
misc::QuickSwitchOffsets quick_offsets(ClientRuntimeStorage& storage) {
    return {offset(storage, 0x43467A74u), offset(storage, 0x4346908Cu),
            offset(storage, 0x43467BE4u), offset(storage, 0x43467A6Cu),
            offset(storage, 0x43467AA4u), offset(storage, 0x4346A6DCu),
            offset(storage, 0x4346A60Cu)};
}
misc::AutomaticWeaponsOffsets automatic_offsets(ClientRuntimeStorage& storage) {
    return {offset(storage, 0x434688B0u), offset(storage, 0x4346A4F4u),
            offset(storage, 0x4346A6C4u), offset(storage, 0x4346834Cu),
            offset(storage, 0x4346799Cu), offset(storage, 0x4346A6DCu),
            offset(storage, 0x4346908Cu), offset(storage, 0x43467200u)};
}
misc::RebuyFixOffsets rebuy_offsets(ClientRuntimeStorage& storage) {
    return {offset32(storage, 0x434688B0u), offset32(storage, 0x4346A3B4u),
            offset32(storage, 0x43468D34u), offset32(storage, 0x434685D4u),
            offset32(storage, 0x43467908u), offset32(storage, 0x43468C04u)};
}
void require_shared(ClientRuntimeStorage& storage, ConfigurationRuntimeOwner& configuration,
                    engine::HookDescriptorStorage& descriptors,
                    const MiscRuntimeSharedBindings& shared, const MiscRuntimeImports& imports) {
    auto& data = storage.data();
    if (&configuration.storage() != &storage || &descriptors.image() != &data ||
        &shared.scene.storage() != &storage || &shared.animation.storage() != &storage ||
        &shared.scene.players().cache() != &shared.animation.players() ||
        &shared.scene.queries().offsets() != &storage.offsets().globals ||
        &shared.live_movement.capture.interval != &pointed<float>(data, 0x4346A338u) ||
        &shared.live_movement.environment != shared.live_movement.capture.environment ||
        &shared.ribbons.entity_list != &data.native_cell<void*>(0x4346A890u))
        throw std::invalid_argument(
            "Misc owners require the same canonical configuration, scene, movement and player contexts");
    std::visit(
        [&](const auto& source) {
            using Source = std::remove_cvref_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, rage::RuntimeOwner*>) {
                if (!source || &source->storage() != &storage)
                    throw std::invalid_argument(
                        "Production misc knife requires the canonical RAGE runtime owner");
            } else if (&source.targeting.player_adjustments !=
                           &shared.scene.players().adjustments() ||
                       &source.records.players != &shared.scene.players().cache() ||
                       &source.extrapolation.players != &shared.scene.players().cache())
                throw std::invalid_argument(
                    "Standalone misc knife views must share their canonical player state");
        },
        shared.knife.source);
    if (!imports.random_integer || !imports.performance_clock.counter ||
        !imports.performance_clock.frequency || !imports.damage_math.trigonometry.sine ||
        !imports.damage_math.trigonometry.cosine || !imports.damage_math.exponential ||
        !imports.arc_tangent2)
        throw std::invalid_argument(
            "Misc owners require their actual imported RNG, performance clock and SSE math");
    if (!data.native_cell<misc::SetClanTag>(0x43467550u) ||
        !data.native_cell<const misc::AutomaticWeaponsBombList*>(0x434688C0u))
        throw std::invalid_argument(
            "Misc owners require the verified clan-tag and planted-bomb bindings");
}
ClientRuntimeStorage& validated_storage(ClientRuntimeStorage& storage,
                                        const EngineRuntimeBindings& engine,
                                        ConfigurationRuntimeOwner& configuration,
                                        engine::HookDescriptorStorage& descriptors,
                                        const MiscRuntimeSharedBindings& shared,
                                        const MiscRuntimeImports& imports) {
    auto& data = storage.data();
    const auto* globals = data.native_cell<const std::byte*>(0x43467B04u);
    const auto* state = data.native_cell<const std::byte*>(0x43468E44u);
    if (!globals || !state || &engine.variables.data() != &data ||
        reinterpret_cast<const std::byte*>(data.native_cell<float*>(0x4346A338u)) !=
            globals + 0x20 ||
        reinterpret_cast<const std::byte*>(data.native_cell<float*>(0x43467EB8u)) !=
            globals + 0x10 ||
        reinterpret_cast<const std::byte*>(data.native_cell<float*>(0x4346A334u)) != globals ||
        reinterpret_cast<const std::byte*>(data.native_cell<std::int32_t*>(0x43469300u)) !=
            globals + 0x18 ||
        reinterpret_cast<const std::byte*>(data.native_cell<std::int32_t*>(0x43468D4Cu)) !=
            globals + 0x1C ||
        reinterpret_cast<const std::byte*>(data.native_cell<std::int32_t*>(0x434678F8u)) !=
            globals + 4 ||
        reinterpret_cast<const std::byte*>(data.native_cell<std::int32_t*>(0x43475B2Cu)) !=
            state + 0x4D24 ||
        reinterpret_cast<const std::byte*>(data.native_cell<std::int32_t*>(0x434757DCu)) !=
            state + 0x4D28 ||
        reinterpret_cast<const std::byte*>(data.native_cell<std::int32_t*>(0x43475A60u)) !=
            state + 0x164)
        throw std::invalid_argument(
            "Misc owner requires the initialized canonical client-state/global aliases");
    require_shared(storage, configuration, descriptors, shared, imports);
    for (const auto identity : {0x43467C3Cu, 0x4346A4FCu, 0x4346A7ECu, 0x434685C8u, 0x4346794Cu,
                                0x43467A8Cu, 0x4346A568u})
        (void)variable(engine, identity);
    return storage;
}
}

MiscRuntimeImports native_misc_runtime_imports(const EngineRuntimeBindings& engine) {
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
    const auto module = static_cast<HMODULE>(engine.modules.at(u"vstdlib.dll"));
    const auto random = module ? GetProcAddress(module, "RandomInt") : nullptr;
    if (!random)
        throw std::runtime_error("Misc runtime requires the loaded vstdlib RandomInt export");
    return {reinterpret_cast<misc::AutomaticWeaponsRandomInteger>(random),
            misc::native_low_fps_clock(),
            {animation::native_sse_trigonometry(), animation::native_sse_exponential},
            animation::native_sse_atan2};
#else
    (void)engine;
    throw std::logic_error("Native misc imports require Windows x86");
#endif
}

struct MiscRuntimeOwner::Data {
    ClientRuntimeStorage& storage;
    ConfigurationRuntimeOwner& configuration;
    MiscRuntimeSharedBindings shared;
    MiscRuntimeImports imports;
    ChannelHookState channel;
    misc::AutoAcceptPatchState patch;
    misc::RuntimeInterfaces interfaces;
    misc::Runtime miscellaneous;
    misc::QuickSwitchRuntime quick;
    misc::AutomaticWeaponsState automatic{};
    misc::ZeusState zeus{};
    misc::LowFpsRuntime* performance = nullptr;
    std::uint32_t life_state_offset;
    ScriptCommandEnvironment command_environment;
    misc::ClanTagClock clan_clock;
    misc::AutomaticWeaponsBindings automatic_bindings;
    misc::KnifeAimBindings aim;
    struct KnifeSource final : misc::KnifeCommandSource {
        Data& data;
        explicit KnifeSource(Data& owner) noexcept : data(owner) {}
        void process(const misc::KnifeConfig&, CommandFrame&, Command&, void*, std::uint8_t&,
                     std::uint32_t&) override;
    } knife;
    misc::ZeusBindings zeus_bindings;
    MiscRuntimeFactoryBindings factories;

    Data(ClientRuntimeStorage& source, const EngineRuntimeBindings& engine,
         ConfigurationRuntimeOwner& settings, engine::HookDescriptorStorage& descriptors,
         MiscRuntimeSharedBindings dependencies, MiscRuntimeImports external)
        : storage(source), configuration(settings), shared(dependencies), imports(external),
          channel(source.data().native_cell<void**>(0x43475B0Cu),
                  source.data().native_cell<void**>(0x43475B14u),
                  source.data().native_cell<void*>(0x43475B08u)),
          patch{descriptors.at(misc::auto_accept_hook_descriptor)},
          interfaces(source.data().native_cell<void*>(0x43475808u)),
          miscellaneous(initialize_interfaces(interfaces, source, engine), patch, channel,
                        runtime_offsets(source)),
          quick{nullptr,
                {interfaces.entity_list, interfaces.game_rules,
                 source.data().native_cell<EntityCommandOwner*>(0x43475898u),
                 &pointed<std::int32_t>(source.data(), 0x43475B2Cu),
                 &pointed<std::int32_t>(source.data(), 0x434757DCu), quick_offsets(source)},
                misc::QuickSwitchState{},
                prediction::NativePredictionObserverReceiver{}},
          life_state_offset(offset32(source, 0x43468E78u)),
          command_environment{&settings.anti_untrusted().enabled.value, interfaces.game_rules,
                              offset(source, 0x43468378u)},
          clan_clock{source.data().native_cell<lagcomp::TickTimingSamples>(0x43478C70u),
                     (&pointed<std::int32_t>(source.data(), 0x43475A60u))[1],
                     source.data().native_cell<const std::int32_t*>(0x43468F9Cu),
                     source.data().native_cell<const std::int32_t*>(0x434670A8u),
                     pointed<float>(source.data(), 0x4346A338u)},
          automatic_bindings{
              &source.data().native_cell<const misc::AutomaticWeaponsBombList*>(0x434688C0u),
              &pointed<float>(source.data(), 0x43467EB8u), automatic_offsets(source),
              imports.random_integer},
          aim{source.data().native_cell<void*>(0x4346A894u), offset(source, 0x43468B50u),
              engine::DecodedOffset(source.offsets().globals, 0x4346757Cu, 0x4346707Cu, 0x043FA575u,
                                    0),
              offset(source, 0x4346934Cu), imports.arc_tangent2},
          knife(*this), zeus_bindings{shared.scene.queries().ballistics(),
                                      source.data().native_cell<void* const*>(0x43475B90u),
                                      offset(source, 0x4346908Cu), offset(source, 0x43467DD0u)},
          factories{
              source.configuration(),
              registry(source, 0x43475B78u),
              registry(source, 0x43475A9Cu),
              registry(source, 0x43475988u),
              registry(source, 0x43475AD8u),
              registry(source, 0x43475B94u),
              source.command_phases(),
              source.empty_ticks(),
              source.entity_observers(),
              source.recharge_observers(),
              source.prediction_observers(),
              {settings.rage_settings().performance,
               settings.rage_settings().global,
               {pointed<std::int32_t>(source.data(), 0x434678F8u),
                pointed<float>(source.data(), 0x4346A338u),
                source.data().native_cell<void*>(0x4346D560u),
                source.data().native_cell<void*>(0x4346D55Cu), imports.performance_clock},
               shared.scripts,
               source.command_phases().services().input,
               shared.indicators,
               life_state_offset,
               performance},
              {settings.miscellaneous().quick_switch, quick},
              {settings.miscellaneous().grenades,
               settings.other_esp_settings().grenades,
               {{shared.scene.queries(),
                 shared.live_movement.capture,
                 shared.live_movement.environment,
                 shared.scene.players().adjustments(),
                 shared.scene.players().cache(),
                 source.data().native_cell<void*>(0x4346A514u),
                 source.data().native_cell<void*>(0x43475808u),
                 source.data().native_cell<void*>(0x4346A3A4u),
                 pointed<std::int32_t>(source.data(), 0x43469300u),
                 pointed<std::int32_t>(source.data(), 0x43468D4Cu),
                 pointed<std::int32_t>(source.data(), 0x434757DCu),
                 pointed<std::int32_t>(source.data(), 0x43475B2Cu),
                 pointed<float>(source.data(), 0x43467EB8u),
                 pointed<float>(source.data(), 0x4346A338u),
                 reinterpret_cast<float*>(source.data().native_cell<void*>(0x43467B04u))[9],
                 settings.other_esp_settings().grenades.entities.value,
                 shared.projection,
                 shared.renderer,
                 shared.explosive_texture,
                 shared.fire_texture,
                 imports.damage_math,
                 imports.arc_tangent2,
                 source.data().native_cell<std::uint8_t*>(0x434683D0u),
                 source.data().native_cell<std::uint32_t>(0x434759C0u),
                 {engine.variables.at(0x43467C3Cu), engine.variables.at(0x4346A4FCu),
                  engine.variables.at(0x4346A7ECu), engine.variables.at(0x434685C8u),
                  engine.variables.at(0x4346794Cu), engine.variables.at(0x43467A8Cu),
                  engine.variables.at(0x4346A568u)}},
                source.data().native_cell<void*>(0x4346A894u),
                (&pointed<std::int32_t>(source.data(), 0x43475A60u))[1],
                source.data().native_cell<std::uint8_t>(0x43467560u),
                source.data().native_cell<std::uint8_t>(0x434684E0u),
                {source.data().native_cell<void*>(0x4346A514u),
                 source.data().native_cell<void*>(0x43469264u), command_environment},
                shared.release_movement,
                shared.movement_feedback,
                shared.ribbons,
                source.data().native_cell<std::uint8_t>(0x43468E58u),
                settings.other_esp_settings().grenades.entity_color.value,
                source.data().native_cell<void*>(0x43475AACu)}},
              {settings.miscellaneous().no_fall_damage,
               {{offset(source, 0x43468BD4u), offset(source, 0x434688D0u),
                 offset(source, 0x4346A8E4u)},
                interfaces.engine,
                variable(engine, 0x43467A54u),
                variable(engine, 0x43467990u),
                variable(engine, 0x434674E4u),
                pointed<float>(source.data(), 0x4346A338u),
                shared.scene.queries().movement(),
                shared.live_movement.capture,
                shared.live_movement.environment,
                shared.projection}},
              {settings.miscellaneous().automatic_weapons, automatic, automatic_bindings},
              {settings.miscellaneous().knife, knife,
               source.command_phases().services().scheduler_flags},
              {settings.miscellaneous().zeus, zeus, zeus_bindings},
              {settings.miscellaneous().clan_tag, miscellaneous.clan_tag, command_environment},
              {{source.configuration(), settings.miscellaneous().rebuy, miscellaneous,
                source.event_registry(), registry(source, 0x43475B78u), source.command_phases(),
                registry(source, 0x43475AD8u), variable(engine, 0x434670E8u),
                &pointed<float>(source.data(), 0x4346A334u),
                &source.data().native_cell<double>(0x4347A500u),
                &source.data().native_cell<double>(0x4347B118u), rebuy_offsets(source)}}} {
        require_shared(source, settings, descriptors, shared, imports);
        miscellaneous.quick_switch = &quick;
        // Auto-accept's descriptor lives inside its separate complete owner.
        // MiscellaneousMenu binds that registered value before its callback.
        miscellaneous.upgrade_tablet_enabled =
            &source.data().native_cell<std::uint8_t>(0x43478C6Cu);
        miscellaneous.clan_tag.set_tag = source.data().native_cell<misc::SetClanTag>(0x43467550u);
        miscellaneous.clan_tag.clan_id_cvar = variable(engine, 0x43467128u);
        miscellaneous.clan_tag.clock = &clan_clock;
        auto& ping = miscellaneous.ping_spike;
        ping.enabled = &settings.ping_spike().enabled.value;
        ping.hotkey = &settings.ping_spike().key.value;
        ping.keys = source.input_runtime().bitmap();
        ping.milliseconds = &settings.ping_spike().milliseconds.value;
        ping.maximum_unlag_milliseconds = &source.data().native_cell<std::int32_t>(0x43468230u);
        ping.latency_4347a508 = &source.data().native_cell<double>(0x4347A508u);
        ping.latency_4347b120 = &source.data().native_cell<double>(0x4347B120u);
        ping.interval_per_tick = &pointed<float>(source.data(), 0x4346A338u);
    }
};

void MiscRuntimeOwner::Data::KnifeSource::process(const misc::KnifeConfig& settings,
                                                  CommandFrame& frame, Command& command,
                                                  void* player, std::uint8_t& packet,
                                                  std::uint32_t& flags) {
    const auto* enabled = &data.configuration.rage_settings().global.enabled.value;
    if (!misc::knife_command_enabled(settings, frame, command, enabled))
        return;
    const auto invoke = [&](const rage::RuntimeFrameBindings& views) {
        auto& storage = data.storage;
        auto& image = storage.data();
        const misc::KnifeMeleeBindings melee{image.native_cell<void*>(0x43468304u),
                                             image.native_cell<void* const*>(0x434758B4u),
                                             views.targeting,
                                             offset(storage, 0x434670CCu),
                                             offset(storage, 0x43467908u),
                                             offset(storage, 0x43467A50u),
                                             &pointed<float>(image, 0x43467EB8u),
                                             data.imports.damage_math.trigonometry,
                                             data.imports.arc_tangent2};
        const misc::KnifeRuntimeBindings bindings{enabled,
                                                  data.interfaces.max_clients,
                                                  views.targeting,
                                                  offset(storage, 0x43467908u),
                                                  views.candidates,
                                                  views.records,
                                                  views.extrapolation,
                                                  melee,
                                                  data.aim,
                                                  data.shared.animation.bones(),
                                                  data.shared.live_movement,
                                                  data.shared.budget};
        misc::process_enabled_knife_command(settings, frame, command, player, packet, flags,
                                            bindings);
    };
    std::visit(
        [&](const auto& source) {
            using Source = std::remove_cvref_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, rage::RuntimeOwner*>)
                source->with_frame(frame, player, command, invoke);
            else
                invoke({source.targeting, source.candidates, source.records, source.extrapolation});
        },
        data.shared.knife.source);
}

MiscRuntimeOwner::MiscRuntimeOwner(ClientRuntimeStorage& storage,
                                   const EngineRuntimeBindings& engine,
                                   ConfigurationRuntimeOwner& configuration,
                                   engine::HookDescriptorStorage& descriptors,
                                   MiscRuntimeSharedBindings shared)
    : MiscRuntimeOwner(storage, engine, configuration, descriptors, shared,
                       native_misc_runtime_imports(engine)) {}
ClientRuntimeStorage& MiscRuntimeOwner::require_available(ClientRuntimeStorage& storage) {
    if (storage.active_misc_owner_)
        throw std::logic_error("The source Misc feature state already has a process owner");
    return storage;
}
MiscRuntimeOwner::MiscRuntimeOwner(ClientRuntimeStorage& storage,
                                   const EngineRuntimeBindings& engine,
                                   ConfigurationRuntimeOwner& configuration,
                                   engine::HookDescriptorStorage& descriptors,
                                   MiscRuntimeSharedBindings shared, MiscRuntimeImports imports)
    : data_(std::make_unique<Data>(validated_storage(require_available(storage), engine,
                                                     configuration, descriptors, shared, imports),
                                   engine, configuration, descriptors, shared, imports)) {
    storage.active_misc_owner_ = this;
}
MiscRuntimeOwner::~MiscRuntimeOwner() {
    data_->storage.active_misc_owner_ = nullptr;
}
ClientRuntimeStorage& MiscRuntimeOwner::storage() const noexcept {
    return data_->storage;
}
misc::Runtime& MiscRuntimeOwner::runtime() noexcept {
    return data_->miscellaneous;
}
ChannelHookState& MiscRuntimeOwner::channel() noexcept {
    return data_->channel;
}
misc::AutoAcceptPatchState& MiscRuntimeOwner::auto_accept_patch() noexcept {
    return data_->patch;
}
misc::QuickSwitchRuntime& MiscRuntimeOwner::quick_switch() noexcept {
    return data_->quick;
}
misc::LowFpsRuntime* MiscRuntimeOwner::low_fps() const noexcept {
    return data_->performance;
}
MiscRuntimeFactoryBindings& MiscRuntimeOwner::factory_bindings() noexcept {
    return data_->factories;
}
void MiscRuntimeOwner::bind_initializers(NativeStartupSources& sources) {
    bind_misc_runtime_factories(sources, data_->factories);
}
} // namespace skeet::client
