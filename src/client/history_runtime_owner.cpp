#include "skeet/client/history_runtime_owner.hpp"
#include "skeet/client/history_runtime_factories.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/player_runtime_owner.hpp"
#include "skeet/client/source_terminal_cleanup.hpp"
#include "skeet/animation/runtime_owner.hpp"
#include "skeet/engine/build_history_bindings.hpp"
#include "skeet/engine/decoded_offset.hpp"
#include "skeet/engine/netvar_initialization.hpp"
#include "skeet/engine/scene_runtime_owner.hpp"
#include "skeet/engine/trace.hpp"
#include "skeet/lagcomp/native_history_boundaries.hpp"
#include "skeet/script/source_runtime_owner.hpp"
#include <array>
#include <bit>
#include <stdexcept>

namespace skeet::client {
namespace {
struct DerivedOffset {
    std::uint32_t pointer, key, salt;
};
// Native4348D65D..4348D9E6 derived encodings and the two collision-history
// keys sharing word0. All other fields use the verified receive/
// datamap recipes, by exact identity.
constexpr std::array derived{DerivedOffset{0x43467004u, 0x4346A6B4u, 0x1599541Fu},
                             DerivedOffset{0x43469064u, 0x4346A484u, 0x17978090u},
                             DerivedOffset{0x43468E7Cu, 0x434674A8u, 0x54783141u},
                             DerivedOffset{0x43467F4Cu, 0x4346A8B0u, 0x3D52A1A7u},
                             DerivedOffset{0x43467D78u, 0x43467B80u, 0x1CAAF0A8u},
                             DerivedOffset{0x4346757Cu, 0x4346707Cu, 0x043FA575u},
                             DerivedOffset{0x43468BF8u, 0x43467B8Cu, 0xD070684Cu},
                             DerivedOffset{0x43467634u, 0x43468F2Cu, 0xBB78DC43u},
                             DerivedOffset{0x43467EBCu, 0x43467038u, 0x432B1F5Bu},
                             DerivedOffset{0x43467A88u, 0x43467160u, 0x0D560FACu}};
std::uint32_t offset(const engine::NativeOffsetGlobals& globals, std::uint32_t pointer) {
    for (const auto& row : engine::build_receive_offset_recipes())
        if (row.pointer_identity == pointer)
            return std::uint32_t(std::ptrdiff_t(engine::DecodedOffset(
                globals, pointer, row.xor_key_identity, row.initial_xor_key, 0)));
    for (const auto& row : engine::build_data_map_offset_recipes())
        if (row.pointer_identity == pointer)
            return std::uint32_t(std::ptrdiff_t(engine::DecodedOffset(
                globals, pointer, row.xor_key_identity, row.initial_xor_key, 0)));
    for (const auto& row : derived)
        if (row.pointer == pointer)
            return std::uint32_t(
                std::ptrdiff_t(engine::DecodedOffset(globals, pointer, row.key, row.salt, 0)));
    throw std::logic_error("History field identity has no recovered offset recipe");
}
engine::OwnerRegistryBinding registry(ClientRuntimeStorage& storage, std::uint32_t address) {
    return {storage.owner_registries().at(address), storage.memory()};
}
const std::uint32_t& bits(const std::int32_t& value) noexcept {
    // Corresponding signed/unsigned types may alias the same integer object.
    return reinterpret_cast<const std::uint32_t&>(value);
}
ClientRuntimeStorage& checked(ClientRuntimeStorage& storage, const EngineRuntimeBindings& engine,
                              ConfigurationRuntimeOwner& config, PlayerRuntimeOwner& players,
                              engine::SceneQueryRuntimeOwner& scene, animation::RuntimeOwner& bones,
                              script::SourceScriptRuntimeOwner& scripts) {
    auto& data = storage.data();
    if (&engine.variables.data() != &data || &config.storage() != &storage ||
        &players.storage() != &storage || &scene.storage() != &storage ||
        &scene.players() != &players || &bones.storage() != &storage ||
        &bones.players() != &players.cache() || &scripts.storage() != &storage)
        throw std::invalid_argument(
            "History must share the canonical engine, configuration, player, scene and script owners");
    if (engine.modules.at(u"client.dll") != data.native_cell<void*>(0x4346A55Cu) ||
        engine.interfaces.core().engine_client != data.native_cell<void*>(0x4346A514u) ||
        engine.interfaces.core().entity_list != data.native_cell<void*>(0x4346A890u))
        throw std::invalid_argument(
            "History requires the initialized canonical engine interface bundle");
    auto* const globals = data.native_cell<std::byte*>(0x43467B04u);
    if (!globals || reinterpret_cast<std::uintptr_t>(globals) % 4 ||
        data.native_cell<void*>(0x4346A338u) != globals + 0x20 ||
        data.native_cell<void*>(0x434678F8u) != globals + 4 ||
        data.native_cell<void*>(0x43467EB8u) != globals + 0x10 ||
        data.native_cell<void*>(0x43469300u) != globals + 0x18 ||
        !data.native_cell<float*>(0x4346A334u) || !data.native_cell<std::int32_t*>(0x43475A60u) ||
        !storage.offsets().words || storage.offsets().globals.pointers.empty())
        throw std::invalid_argument(
            "History requires live clock aliases and initialized entity offsets");
    for (const auto cell : {0x434688C8u, 0x43467E44u, 0x4346A7F0u, 0x434693F0u})
        if (!data.native_cell<void*>(cell))
            throw std::invalid_argument("History requires the recovered engine animation entries");
    for (const auto cell :
         {0x43467A8Cu, 0x4346A568u, 0x43467A54u, 0x43467990u, 0x43467124u, 0x4346A820u, 0x43467968u,
          0x43467E28u, 0x43468C2Cu, 0x43467FA0u, 0x4346794Cu, 0x4346A850u})
        if (!engine.variables.at(cell).get())
            throw std::invalid_argument("History ConVar acquisition is incomplete");
    return storage;
}
rage::TargetOffsets compose_target_offsets(const engine::NativeOffsetGlobals& g) {
    return {{offset(g, 0x43468E78u), offset(g, 0x4346A5DCu), offset(g, 0x43468BF8u),
             offset(g, 0x43467DD0u)},
            offset(g, 0x434688B0u),
            offset(g, 0x434678ACu),
            offset(g, 0x43468BD4u),
            offset(g, 0x434688D0u),
            offset(g, 0x434670CCu),
            offset(g, 0x434679F8u),
            offset(g, 0x43467A74u),
            offset(g, 0x4346908Cu)};
}
lagcomp::HistoryVelocityFields velocity_fields(const engine::NativeOffsetGlobals& g) {
    return {offset(g, 0x434670CCu), offset(g, 0x434679F8u), offset(g, 0x43468BF8u),
            offset(g, 0x43467230u), offset(g, 0x43469198u), offset(g, 0x434685CCu),
            offset(g, 0x434688D0u), offset(g, 0x43468EE4u), offset(g, 0x43468C40u),
            offset(g, 0x43467A74u)};
}
lagcomp::EntityStateRuntime compose_entity_runtime(ClientRuntimeStorage& storage) {
    auto& data = storage.data();
    return {native_history_entity_fields(storage.offsets().globals),
            data.native_cell<void*>(0x4346A514u),
            data.native_cell<void*>(0x4346A890u),
            *data.native_cell<float*>(0x4346A338u),
            *reinterpret_cast<float*>(data.native_cell<std::byte*>(0x43467B04u) + 0x20),
            *data.native_cell<std::int32_t*>(0x434678F8u),
            data.native_cell<std::int32_t*>(0x43475A60u)[1],
            data.native_cell<const std::int32_t*>(0x43468F9Cu),
            data.native_cell<const std::int32_t*>(0x434670A8u),
            data.native_cell<lagcomp::SetAbsoluteVector>(0x434688C8u),
            data.native_cell<lagcomp::SetAbsoluteVector>(0x43467E44u),
            data.native_cell<lagcomp::SetCollisionBounds>(0x4346A7F0u),
            data.native_cell<std::uint8_t*>(0x434683D0u)};
}
void trace(void* raw, const resolver_runtime::EnvironmentTraceRequest& source,
           BallisticTrace& result) {
    auto& storage = *static_cast<ClientRuntimeStorage*>(raw);
    engine::TraceRay ray;
    ray.start = {source.ray.ray.start[0], source.ray.ray.start[1], source.ray.ray.start[2], 0};
    ray.delta = {source.ray.ray.delta[0], source.ray.ray.delta[1], source.ray.ray.delta[2], 0};
    ray.start_offset = {source.ray.start_offset[0], source.ray.start_offset[1],
                        source.ray.start_offset[2], 0};
    ray.extents = {source.ray.extents[0], source.ray.extents[1], source.ray.extents[2], 0};
    ray.is_ray = source.ray.is_ray;
    ray.swept = source.ray.swept;
    auto* const engine = storage.data().native_cell<void*>(0x43468304u);
    if (source.trace_type == 0) {
        engine::TraceRejectEntitiesFilter filter;
        engine::invoke<void>(engine, 5, &ray, source.mask, &filter, &result);
    } else if (source.trace_type == 1) {
        engine::TraceWorldOnlyFilter filter;
        engine::invoke<void>(engine, 5, &ray, source.mask, &filter, &result);
    } else
        throw std::invalid_argument("History environment has an unproved trace-filter type");
}
std::uint32_t point_contents(void* raw, const RageVector& point, std::uint32_t mask) {
    auto& data = static_cast<ClientRuntimeStorage*>(raw)->data();
    //43348ECB/4340E0CB call the world-only contents virtual+4 with exactly
    //point and mask; the separate general contents entry takes a third arg.
    return engine::invoke<std::uint32_t>(data.native_cell<void*>(0x43468304u), 1, &point, mask);
}
void view_angles(void* raw, RageVector& angles) {
    auto& data = static_cast<ClientRuntimeStorage*>(raw)->data();
    engine::invoke<void>(data.native_cell<void*>(0x4346A514u), 18, &angles);
}
HistoryRuntimeImports require_imports(HistoryRuntimeImports imports) {
    const auto& n = imports.numeric;
    if (!imports.thread_pool || !n.fmod || !n.environment_atan2 || !n.atan2 || !n.power ||
        !n.arccosine || !n.contains_sequence_name)
        throw std::invalid_argument("History requires its actual thread-pool and numeric imports");
    return imports;
}
}

lagcomp::EntityNetvars native_history_entity_fields(const engine::NativeOffsetGlobals& g) {
    if (offset(g, 0x43467EBCu) < sizeof(void*) || offset(g, 0x43467A88u) < sizeof(void*))
        throw std::logic_error(
            "History collision offsets were not initialized before feature construction");
    //433FD690,43389C5D,4331E453 and4341D38E operand identities.
    return {offset(g, 0x43468EA0u), offset(g, 0x43469198u), offset(g, 0x43467230u),
            offset(g, 0x43468FECu), offset(g, 0x434670CCu), offset(g, 0x434688D0u),
            offset(g, 0x4346A8E4u), offset(g, 0x43467DD4u), offset(g, 0x434679F8u),
            offset(g, 0x43467A74u), offset(g, 0x43467B44u), offset(g, 0x43467F4Cu),
            offset(g, 0x4346A660u), offset(g, 0x43467D78u), offset(g, 0x43469064u),
            offset(g, 0x43468E7Cu), offset(g, 0x4346A808u), offset(g, 0x434675CCu),
            offset(g, 0x434688B4u), offset(g, 0x43467634u), offset(g, 0x43467EBCu),
            offset(g, 0x43467A88u), offset(g, 0x43469344u)};
}

struct HistoryRuntimeOwner::Data {
    ClientRuntimeStorage& storage;
    PlayerRuntimeOwner& players;
    const HistoryRuntimeImports imports;
    const engine::BuildHistoryBindings functions;
    const lagcomp::HistoryNumericBindings numeric;
    const lagcomp::EntityStateRuntime entity;
    const rage::TargetOffsets targets;
    engine::NativeConVarReference game_type, enemies;
    ShotFeedbackState feedback;
    ui::SettingsMenuValues menu;
    InputBitmap keys;
    lagcomp::HistoryRecordingConfiguration configuration;
    lagcomp::HistoryJobDispatcher jobs;
    resolver_runtime::EnvironmentEngine environment;
    lagcomp::HistoryEnvironmentEngine history_environment;
    animation::AnimationUpdateEngine animation;
    std::uint32_t seed_update_flags = 0;
    lagcomp::HistoryVelocityReconstructionBindings physics;
    lagcomp::HistoryVelocityCaptureBindings velocity;
    lagcomp::HistoryResolverUpdateBindings resolver;
    const float seed_exponent = std::bit_cast<float>(0x3EDC8172u);
    lagcomp::HistoryAnimationPublishBindings publication;
    lagcomp::HistoryRecordingBindings recording;
    BooleanConfig field_ed048f3d;
    HistoryRuntimeFactoryBindings factory;
    SourceTerminalCleanupBindings& terminal;

    Data(ClientRuntimeStorage& source, const EngineRuntimeBindings& engine,
         ConfigurationRuntimeOwner& config, PlayerRuntimeOwner& player,
         engine::SceneQueryRuntimeOwner& scene, animation::RuntimeOwner& bones,
         script::SourceScriptRuntimeOwner& scripts, lagcomp::HistoryRecordingScratch* scratch,
         HistoryRuntimeImports services)
        : storage(checked(source, engine, config, player, scene, bones, scripts)), players(player),
          imports(require_imports(services)),
          functions(engine::initialize_build_history_bindings(storage.data())),
          numeric(imports.numeric), entity(compose_entity_runtime(storage)),
          targets(compose_target_offsets(storage.offsets().globals)),
          game_type(engine.variables.at(0x43467A8Cu)), enemies(engine.variables.at(0x4346A568u)),
          feedback(scripts.bridge()), menu(config.settings_menu().values()),
          keys(storage.input_runtime().bitmap()),
          configuration{nullptr,
                        &config.rage_settings().global.enabled.value,
                        &config.rage_settings().global.anti_aim_correction.value,
                        0,
                        entity.engine,
                        config.rage_settings().global.experimental(rage::ExperimentalRageOption::animation),
                        {&config.rage_settings().global.jitter_fix.value,
                         &config.rage_settings().global.experimental_rage.value},
                        config.rage_settings().global.experimental(rage::ExperimentalRageOption::resolver)},
          jobs(imports.thread_pool, storage.memory()), environment{&storage,
                                                                   trace,
                                                                   point_contents,
                                                                   view_angles,
                                                                   scene.queries().ballistics(),
                                                                   bones.trigonometry(),
                                                                   entity.transform_override,
                                                                   config.rage_settings().global.experimental(rage::ExperimentalRageOption::resolver)},
          history_environment{environment, entity.offsets.origin, numeric.environment_atan2},
          animation(lagcomp::native_history_animation_engine(
              storage.data().native_cell<void*>(0x434693F0u))),
          physics{player.cache(),
                  scene.queries().movement(),
                  entity.entity_list,
                  entity.interval,
                  velocity_fields(storage.offsets().globals),
                  {engine.variables.at(0x43467A54u), engine.variables.at(0x43467990u),
                   engine.variables.at(0x43467124u), engine.variables.at(0x4346A820u),
                   engine.variables.at(0x43467968u), engine.variables.at(0x43467E28u)},
                  functions.activity,
                  functions.sequence_name,
                  numeric.contains_sequence_name,
                  "_crouch",
                  numeric.atan2,
                  bones.trigonometry().sine,
                  bones.trigonometry().cosine,
                  entity.transform_override,
                  seed_update_flags},
          velocity{entity, physics, engine.variables.at(0x43468C2Cu),
                   engine.variables.at(0x43467FA0u)},
          resolver{entity,
                   configuration,
                   player.adjustments().settings_base(),
                   storage.data().native_cell<std::int32_t>(0x4346DB1Cu),
                   storage.data().native_cell<void* const*>(0x4346A3A4u),
                   *storage.data().native_cell<float*>(0x4346A334u),
                   *storage.data().native_cell<float*>(0x43467EB8u),
                   entity.frame,
                   menu.hold_aim,
                   storage.data().native_cell<std::uint8_t*>(0x43467B24u),
                   storage.data().native_cell<RageVector>(0x43475BD4u),
                   feedback.pending,
                   resolver_fields(storage.offsets().globals),
                   engine.variables.at(0x4346794Cu),
                   physics,
                   animation,
                   environment},
          publication{entity,
                      publication_fields(storage.offsets().globals),
                      entity.engine,
                      resolver.local_time_entity,
                      &resolver.current_time,
                      entity.receive_interval,
                      resolver.animation_reset_byte,
                      configuration,
                      player.adjustments().settings_base(),
                      0,
                      storage.data().native_cell<std::int32_t>(0x4347C9ECu),
                      seed_exponent,
                      &storage.data().native_cell<std::uint8_t>(0x434745E0u),
                      lagcomp::native_history_movement_engine(functions.sequence_displacement),
                      numeric.power,
                      numeric.arccosine,
                      numeric.atan2},
          recording{player.cache(),
                    player.records(),
                    entity,
                    configuration,
                    {&storage.data().native_cell<std::uint8_t>(0x434693ECu),
                     &storage.data().native_cell<void**>(0x4346A3A4u),
                     offset(storage.offsets().globals, 0x43468378u),
                     &storage.data().native_cell<legit::LifecycleOwner*>(0x4347588Cu),
                     configuration.recording_enabled},
                    {game_type, enemies, targets},
                    prelude_fields(storage.offsets().globals),
                    recording_fields(storage.offsets().globals),
                    {*storage.data().native_cell<std::int32_t*>(0x43469300u),
                     *storage.data().native_cell<std::int32_t*>(0x43475A60u), resolver.current_time,
                     resolver.animation_time,
                     *reinterpret_cast<float*>(storage.data().native_cell<std::byte*>(0x43467B04u) +
                                               0x14),
                     entity.frame, menu, menu.hold_aim, *configuration.resolver_enabled,
                     resolver.animation_reset_byte,
                     storage.data().native_cell<RageVector>(0x43475BD4u), player.adjustments()},
                    {game_type, resolver.selected_entity,
                     config.rage_settings().global.force_safe_point.value.binding, keys,
                     bits(config.rage_settings().global.avoid_unsafe_hitboxes.value),
                     config.rage_settings().global.prefer_body_aim.value,
                     bits(config.rage_settings().global.body_aim_disablers.value)},
                    numeric.fmod,
                    bones.bones(),
                    jobs,
                    history_environment,
                    velocity,
                    resolver,
                    publication,
                    menu.maximum_unlag},
          factory{storage.data_owner(),
                  registry(storage, 0x43475B78u),
                  storage.entity_observers(),
                  {storage.data().native_cell<void*>(0x4346A514u),
                   storage.data().native_cell<void*>(0x4346A890u),
                   storage.data().native_cell<void*>(0x43475A50u),
                   storage.data().native_cell<void*>(0x434683E0u),
                   storage.data().native_cell<std::uint32_t*>(0x43469060u),
                   offset(storage.offsets().globals, 0x4346A808u)},
                  {{registry(storage, 0x43475B78u), storage.entity_observers(),
                    registry(storage, 0x4347584Cu), registry(storage, 0x43475AD8u),
                    storage.configuration(), storage.event_registry()},
                   recording,
                   scratch,
                   field_ed048f3d,
                   storage.data().native_cell<void*>(0x43475808u),
                   engine.variables.at(0x4346A850u),
                   feedback,
                   entity.command_tick,
                   owner_fields(storage.offsets().globals)},
                  storage.data().native_cell<lagcomp::HistoryOwnerRuntime*>(0x43475AC0u)},
          terminal(scripts.source_terminal_cleanup(player)) {}

    static lagcomp::HistoryResolverFields resolver_fields(const engine::NativeOffsetGlobals& g) {
        return {offset(g, 0x43468354u), offset(g, 0x434670E0u), offset(g, 0x434679CCu),
                offset(g, 0x43467D78u), offset(g, 0x43467E8Cu), offset(g, 0x43468240u),
                offset(g, 0x43468D6Cu), offset(g, 0x434670A0u)};
    }
    static lagcomp::HistoryAnimationPublishFields
    publication_fields(const engine::NativeOffsetGlobals& g) {
        return {offset(g, 0x43468DBCu), offset(g, 0x43467D78u), offset(g, 0x4346A8E4u),
                offset(g, 0x43467E8Cu), offset(g, 0x434670CCu), offset(g, 0x4346A594u),
                offset(g, 0x434688D0u), offset(g, 0x43467230u), offset(g, 0x43468D6Cu),
                offset(g, 0x4346A808u)};
    }
    static lagcomp::HistoryPreludeOffsets prelude_fields(const engine::NativeOffsetGlobals& g) {
        return {offset(g, 0x43468E78u), offset(g, 0x4346A5DCu), offset(g, 0x43467004u),
                offset(g, 0x4346908Cu)};
    }
    static lagcomp::HistoryRecordingFields recording_fields(const engine::NativeOffsetGlobals& g) {
        return {offset(g, 0x43468B50u), offset(g, 0x4346757Cu), offset(g, 0x4346934Cu),
                offset(g, 0x43467200u), offset(g, 0x43468DBCu), offset(g, 0x4346A6C0u)};
    }
    static lagcomp::HistoryOwnerFields owner_fields(const engine::NativeOffsetGlobals& g) {
        return {offset(g, 0x43467EC0u), offset(g, 0x43468FACu), offset(g, 0x43468E78u)};
    }
};

HistoryRuntimeOwner::HistoryRuntimeOwner(ClientRuntimeStorage& storage,
                                         const EngineRuntimeBindings& engine,
                                         ConfigurationRuntimeOwner& config,
                                         PlayerRuntimeOwner& players,
                                         engine::SceneQueryRuntimeOwner& scene,
                                         animation::RuntimeOwner& bones,
                                         script::SourceScriptRuntimeOwner& scripts)
    : HistoryRuntimeOwner(storage, engine, config, players, scene, bones, scripts,
                          native_history_runtime_imports()) {}
HistoryRuntimeOwner::HistoryRuntimeOwner(
    ClientRuntimeStorage& storage, const EngineRuntimeBindings& engine,
    ConfigurationRuntimeOwner& config, PlayerRuntimeOwner& players,
    engine::SceneQueryRuntimeOwner& scene, animation::RuntimeOwner& bones,
    script::SourceScriptRuntimeOwner& scripts, HistoryRuntimeImports imports)
    : data_(std::make_unique<Data>(storage, engine, config, players, scene, bones, scripts, nullptr,
                                   imports)) {}
HistoryRuntimeOwner::HistoryRuntimeOwner(
    ClientRuntimeStorage& storage, const EngineRuntimeBindings& engine,
    ConfigurationRuntimeOwner& config, PlayerRuntimeOwner& players,
    engine::SceneQueryRuntimeOwner& scene, animation::RuntimeOwner& bones,
    script::SourceScriptRuntimeOwner& scripts, lagcomp::HistoryRecordingScratch& scratch)
    : HistoryRuntimeOwner(storage, engine, config, players, scene, bones, scripts, scratch,
                          native_history_runtime_imports()) {}
HistoryRuntimeOwner::HistoryRuntimeOwner(
    ClientRuntimeStorage& storage, const EngineRuntimeBindings& engine,
    ConfigurationRuntimeOwner& config, PlayerRuntimeOwner& players,
    engine::SceneQueryRuntimeOwner& scene, animation::RuntimeOwner& bones,
    script::SourceScriptRuntimeOwner& scripts, lagcomp::HistoryRecordingScratch& scratch,
    HistoryRuntimeImports imports)
    : data_(std::make_unique<Data>(storage, engine, config, players, scene, bones, scripts,
                                   &scratch, imports)) {}
HistoryRuntimeImports native_history_runtime_imports() {
    return {engine::native_thread_pool(), lagcomp::native_history_numeric_bindings()};
}
HistoryRuntimeOwner::~HistoryRuntimeOwner() = default;
ClientRuntimeStorage& HistoryRuntimeOwner::storage() const noexcept {
    return data_->storage;
}
PlayerRuntimeOwner& HistoryRuntimeOwner::players() const noexcept {
    return data_->players;
}
const lagcomp::EntityStateRuntime& HistoryRuntimeOwner::entity_runtime() const noexcept {
    return data_->entity;
}
const rage::TargetOffsets& HistoryRuntimeOwner::target_offsets() const noexcept {
    return data_->targets;
}
engine::NativeConVarReference HistoryRuntimeOwner::game_type() const noexcept {
    return data_->game_type;
}
engine::NativeConVarReference HistoryRuntimeOwner::teammates_are_enemies() const noexcept {
    return data_->enemies;
}
ShotFeedbackState& HistoryRuntimeOwner::feedback_state() noexcept {
    return data_->feedback;
}
const lagcomp::HistoryRecordingBindings& HistoryRuntimeOwner::recording_bindings() const noexcept {
    return data_->recording;
}
HistoryRuntimeFactoryBindings& HistoryRuntimeOwner::factory_bindings() noexcept {
    return data_->factory;
}
SourceTerminalCleanupBindings& HistoryRuntimeOwner::source_terminal_cleanup() noexcept {
    return data_->terminal;
}
void HistoryRuntimeOwner::bind_initializers(NativeStartupSources& sources) {
    bind_history_runtime_factories(sources, data_->factory);
}
} //namespace skeet::client
