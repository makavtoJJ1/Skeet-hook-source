#include "skeet/effects/runtime_owner.hpp"
#include "skeet/effects/bullet_impact_command_owner.hpp"
#include "skeet/effects/camera_runtime_owner.hpp"
#include "skeet/effects/smoke_owner.hpp"
#include "skeet/effects/scope_owner.hpp"
#include "skeet/effects/model_hooks.hpp"
#include "skeet/effects/model_render_sources.hpp"
#include "skeet/client/combat_runtime_factories.hpp"
#include "skeet/client/native_startup.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/client/visual_runtime_owner.hpp"
#include "skeet/client/player_runtime_owner.hpp"
#include "skeet/client/history_runtime_owner.hpp"
#include "skeet/engine/build_material_bindings.hpp"
#include "skeet/engine/build_esp_bindings.hpp"
#include "skeet/engine/scene_runtime_owner.hpp"
#include "skeet/engine/decoded_offset.hpp"
#include "skeet/engine/netvar_initialization.hpp"
#include "skeet/engine/hook_descriptor_storage.hpp"
#include "skeet/animation/runtime_owner.hpp"
#include "skeet/lagcomp/history_runtime.hpp"
#include "skeet/rage/spread_storage.hpp"
#include "skeet/rage/shot_diagnostics.hpp"
#include <stdexcept>

namespace skeet::effects {
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::invalid_argument(message);
}
client::ClientRuntimeStorage& checked(client::ClientRuntimeStorage& storage,
                                      const client::EngineRuntimeBindings& engine,
                                      client::ConfigurationRuntimeOwner& configuration,
                                      const engine::BuildMaterialBindings& imports,
                                      void* key_values) {
    auto& data = storage.data();
    require(&engine.variables.data() == &data && &configuration.storage() == &storage &&
                &imports.data == &data && key_values,
            "Effects must borrow one canonical engine, configuration, visual and material owner");
    require(imports.create_variable &&
                imports.create_variable == data.native_cell<CreateMaterialVariable>(0x43467A68u) &&
                imports.localization &&
                imports.localization == data.native_cell<void*>(0x4346A31Cu),
            "Effects require genuine acquired material and localization bindings");
    require(engine.modules.at(u"materialsystem.dll") == data.native_cell<void*>(0x4346A600u),
            "Effects material interface must belong to the published module bundle");
    const auto manifest = engine::build_interface_manifest();
    for (const auto cell : {0x4346A514u, 0x4346A890u, 0x4346A4E0u, 0x43467014u, 0x43467118u,
                            0x4346A44Cu, 0x4346A894u, 0x43469264u, 0x4346712Cu}) {
        // CInput is an independently proven inline object, not a factory row.
        if (cell == 0x4346A894u) {
            require(data.native_cell<void*>(cell) != nullptr,
                    "Effects require the acquired genuine CInput object");
            continue;
        }
        bool found = false;
        for (std::size_t index = 0; index < manifest.size(); ++index) {
            if (manifest[index].destination_cell != cell)
                continue;
            auto* object = data.native_cell<void*>(cell);
            require(object && object == engine.interfaces.objects[index],
                    "Effects interface differs from its acquired canonical engine object");
            found = true;
            break;
        }
        require(found, "Effects interface is absent from the fixed-build acquisition manifest");
    }
    require(storage.offsets().words && !storage.offsets().globals.pointers.empty() &&
                data.native_cell<const std::int32_t*>(0x43469300u) &&
                data.native_cell<void*>(0x434684C4u),
            "Effects require initialized offsets, the live client limit and genuine CSun class");
    require(engine.variables.at(0x4346A534u).get() != nullptr,
            "Effects view requires the acquired viewmodel_recoil ConVar");
    return storage;
}
void* key_values(client::ClientRuntimeStorage& storage, client::VisualRuntimeOwner& visual) {
    require(&visual.storage() == &storage, "Effects require the same canonical visual owner");
    return visual.key_values_system();
}
std::uint32_t offset(const engine::NativeOffsetGlobals& globals, std::uint32_t salt) {
    // This is the entity origin, not CTEFireBullets::m_vecOrigin, which
    // happens to carry the same XOR salt.
    if (salt == 0x2A671B42u)
        return std::uint32_t(
            std::ptrdiff_t(engine::DecodedOffset(globals, 0x43468D30u, 0x43468ED4u, salt, 0)));
    for (const auto& row : engine::build_receive_offset_recipes())
        if (row.initial_xor_key == salt)
            return std::uint32_t(std::ptrdiff_t(engine::DecodedOffset(
                globals, row.pointer_identity, row.xor_key_identity, salt, 0)));
    for (const auto& row : engine::build_data_map_offset_recipes())
        if (row.initial_xor_key == salt)
            return std::uint32_t(std::ptrdiff_t(engine::DecodedOffset(
                globals, row.pointer_identity, row.xor_key_identity, salt, 0)));
    if (salt == 0x3B1F6ECCu)
        return std::uint32_t(
            std::ptrdiff_t(engine::DecodedOffset(globals, 0x43468368u, 0x43467FACu, salt, 0)));
    if (salt == 0x54783141u)
        return std::uint32_t(
            std::ptrdiff_t(engine::DecodedOffset(globals, 0x43468E7Cu, 0x434674A8u, salt, 0)));
    if (salt == 0xD070684Cu)
        return std::uint32_t(
            std::ptrdiff_t(engine::DecodedOffset(globals, 0x43468BF8u, 0x43467B8Cu, salt, 0)));
    throw std::logic_error("Effects field has no recovered native offset recipe");
}
WorldNetvars world_fields(const engine::NativeOffsetGlobals& globals) {
    return {offset(globals, 0x37482375u), offset(globals, 0xDC91E6CFu),
            offset(globals, 0x60816092u)};
}
ViewOffsets view_fields(const engine::NativeOffsetGlobals& globals) {
    return {offset(globals, 0x9D920E35u), offset(globals, 0xCACAEECCu),
            offset(globals, 0xCD812654u), offset(globals, 0xFBE8C5EAu),
            offset(globals, 0x35B5B2ECu), offset(globals, 0x5AE32976u),
            offset(globals, 0x1BAE1168u)};
}
EntityRenderOffsets entity_fields(const engine::NativeOffsetGlobals& globals) {
    return {
        offset(globals, 0x73DBBB49u), offset(globals, 0x38CA5228u), offset(globals, 0x9C14F5A7u),
        offset(globals, 0x1B5F47D5u), offset(globals, 0xFC2723F7u), offset(globals, 0x8481EA04u),
        offset(globals, 0x258BF82Bu), offset(globals, 0xA9EE6062u), offset(globals, 0x2A671B42u),
        offset(globals, 0x3B1F6ECCu), offset(globals, 0xC865F10Eu)};
}
engine::OwnerRegistryBinding registry(client::ClientRuntimeStorage& storage,
                                      std::uint32_t identity) {
    return {storage.owner_registries().at(identity), storage.memory()};
}
float& frame_time(engine::NativeDataImage& data) {
    auto* globals = data.native_cell<std::byte*>(0x43467B04u);
    require(globals != nullptr, "Effects require the canonical gpGlobals frame-time alias");
    return *reinterpret_cast<float*>(globals + 0x14);
}
}
struct EffectsRuntimeOwner::Data {
    client::ClientRuntimeStorage& storage;
    const client::EngineRuntimeBindings& engine;
    client::ConfigurationRuntimeOwner& configuration;
    ModelMaterialInterfaces materials;
    ModelRenderRuntime models;
    WorldInterfaces world_interfaces;
    WorldRuntime world;
    ViewBindings view_interfaces;
    ViewRuntime view;
    SmokeRuntimeBindings smoke_engine;
    SmokeOwnerBindings smoke;
    ScopeOwnerBindings scope;
    ModelShotState shot;
    rage::ShotModelBoneStorage shot_bones;
    struct Command {
        const lagcomp::EntityNetvars animation_fields;
        EntityRenderBindings entities;
        const rage::ShotgunPattern shotgun;
        BulletImpactCommandBindings bullet;
        EffectsCommandOwner owner;
        Command(Data& data, CameraRuntimeOwner& camera, client::PlayerRuntimeOwner& players,
                engine::SceneQueryRuntimeOwner& scene, client::CrtRuntimeOwner& crt,
                const engine::BuildEspBindings& functions)
            : animation_fields(
                  client::native_history_entity_fields(data.storage.offsets().globals)),
              entities{data.storage.data().native_cell<void*>(0x4346A890u),
                       data.storage.data().native_cell<void*>(0x43467DC0u),
                       data.engine.variables.at(0x4346A67Cu),
                       data.storage.data().native_cell<InvalidateEntityPhysics>(0x43468C10u),
                       entity_fields(data.storage.offsets().globals),
                       animation_fields},
              shotgun(functions.shotgun_pattern()),
              bullet{data.storage.data().native_cell<void**>(0x4346A3A4u),
                     std::ptrdiff_t(offset(data.storage.offsets().globals, 0xD720AEA6u)),
                     std::ptrdiff_t(offset(data.storage.offsets().globals, 0x5C9F0FBBu)),
                     data.storage.data().native_cell<void*>(0x43468260u),
                     data.engine.variables.at(0x43468520u),
                     scene.queries().ballistics(),
                     data.storage.data().native_cell<void* const*>(0x43475B90u),
                     crt.spread().tables(),
                     crt.random(),
                     shotgun,
                     crt.trigonometry()},
              owner{camera.runtime(), data.world, data.models, players.cache(), entities, bullet} {}
    };
    std::unique_ptr<Command> command;
    struct Primary {
        Data& data;
        ModelClassification classification;
        ModelEntityBindings entities;
        const lagcomp::RotationMath rotations;
        ModelShadowTimingSource timing;
        LocalTransparencyInterfaces transparency;
        LocalTransparencyNetvars transparency_fields;
        ModelOcclusionInterfaces occlusion;
        ModelRenderLifecycleState lifecycle_state{};
        ModelRenderLifecycleBindings lifecycle_bindings;
        ModelRenderLifecycle lifecycle;
        WorldCallbackState world_state;
        WorldCallbackBindings world_bindings;
        WorldCallbacks updates;
        ModelRenderHookInputs draw;
        ModelRenderHooks hooks;
        client::EffectsRuntimeFactoryBindings factory;

        Primary(Data& source, CameraRuntimeOwner& camera, client::HistoryRuntimeOwner& history,
                animation::RuntimeOwner& bones, client::CrtRuntimeOwner& crt)
            : data(source), classification{ModelRenderableTables(source.storage.data()),
                                           source.storage.data().native_cell<void*>(0x4346A890u),
                                           offset(source.storage.offsets().globals, 0x38CA5228u),
                                           offset(source.storage.offsets().globals, 0x4E36DFD7u)},
              entities{source.storage.data().native_cell<void*>(0x4346A514u),
                       source.storage.data().native_cell<void*>(0x4346A890u),
                       nullptr,
                       offset(source.storage.offsets().globals, 0x9D920E35u),
                       offset(source.storage.offsets().globals, 0x7501CB91u),
                       offset(source.storage.offsets().globals, 0x2F4E97C4u),
                       offset(source.storage.offsets().globals, 0x3B704B41u),
                       offset(source.storage.offsets().globals, 0x8B5586F6u),
                       offset(source.storage.offsets().globals, 0x430C8993u),
                       offset(source.storage.offsets().globals, 0x49D93E62u) - 0x18u,
                       0,
                       false},
              rotations{crt.trigonometry().sine, crt.trigonometry().cosine,
                        history.recording_bindings().publication.arccosine,
                        history.recording_bindings().publication.atan2},
              timing(source.storage.data(), source.storage.offsets().globals),
              transparency{source.storage.data().native_cell<void*>(0x4346A894u),
                           source.storage.data().native_cell<void*>(0x4346A890u),
                           source.storage.data().native_cell<void*>(0x4346A3A0u)},
              transparency_fields{offset(source.storage.offsets().globals, 0xAF649DACu),
                                  offset(source.storage.offsets().globals, 0xCACAEECCu),
                                  offset(source.storage.offsets().globals, 0xC07D1150u),
                                  offset(source.storage.offsets().globals, 0xCD812654u)},
              occlusion{source.storage.data().native_cell<void*>(0x4346A890u),
                        source.storage.data().native_cell<void*>(0x4346A614u),
                        source.storage.data().native_cell<void*>(0x43469330u), nullptr, UINT32_MAX},
              lifecycle_bindings{source.storage.data().native_cell<ModelHookDescriptor>(
                                     studio_model_hook_descriptor),
                                 source.storage.data().native_cell<ModelHookDescriptor>(
                                     model_occlusion_hook_descriptor),
                                 camera.interfaces(),
                                 source.storage.data().native_cell<ObserverProxy>(0x43476A34u),
                                 source.storage.data().native_cell<void*>(0x43468A4Cu),
                                 source.storage.data().native_cell<void*>(0x4346A3A0u),
                                 source.storage.data().native_cell<void*>(0x43467204u),
                                 source.storage.data().native_cell<std::uint32_t>(0x4346D674u)},
              lifecycle(source.models, source.configuration.colored_model_settings(), source.world,
                        lifecycle_state, lifecycle_bindings),
              world_bindings{
                  lifecycle_bindings.first_hook,
                  lifecycle_bindings.second_hook,
                  source.storage.data().native_cell<std::uint8_t>(0x43467AECu),
                  source.engine.variables.at(0x4346A39Cu),
                  source.engine.variables.at(0x43467080u),
                  source.storage.data().native_cell<void*>(0x43467204u),
                  source.storage.data().native_cell<void*>(0x4346A3A0u),
                  source.engine.variables.at(0x43468DFCu),
                  camera.interfaces(),
                  camera.runtime().offsets,
                  source.storage.data().native_cell<std::uint32_t>(0x4346D674u),
                  source.storage.data().native_cell<const std::uint32_t*>(0x43469060u),
                  source.shot,
                  source.storage.data().native_cell<engine::MemberFunction<void>>(0x43468320u),
                  {{std::ptrdiff_t(offset(source.storage.offsets().globals, 0x54783141u)),
                    std::ptrdiff_t(offset(source.storage.offsets().globals, 0x2B554AD0u))},
                   offset(source.storage.offsets().globals, 0x38CA5228u),
                   offset(source.storage.offsets().globals, 0xFC3297F7u),
                   offset(source.storage.offsets().globals, 0xCA2B5FFBu)},
                  source.storage.input_runtime().bitmap(),
                  transparency,
                  transparency_fields,
                  source.storage.data().native_cell<std::uint8_t>(0x4346A65Cu),
                  frame_time(source.storage.data())},
              updates{source.world,    source.view, source.models,
                      lifecycle_state, world_state, world_bindings},
              draw{classification,
                   entities,
                   crt.trigonometry(),
                   rotations,
                   history.players().adjustments(),
                   ModelShadowSource(history.players().cache(), timing),
                   bones.bones(),
                   offset(source.storage.offsets().globals, 0xD070684Cu),
                   offset(source.storage.offsets().globals, 0xCDE290D2u),
                   offset(source.storage.offsets().globals, 0x9C14F5A7u),
                   offset(source.storage.offsets().globals, 0x8D1BD8D0u),
                   source.storage.data().native_cell<void*>(0x43475808u),
                   source.storage.data().native_cell<std::uint8_t>(0x4346A65Cu),
                   history.game_type(),
                   history.teammates_are_enemies(),
                   source.engine.variables.at(0x43467A78u),
                   {source.storage.data().native_cell<anti_aim::CommandOwner*>(0x43475940u),
                    source.storage.data().native_cell<const std::int32_t*>(0x43475B2Cu),
                    source.storage.data().native_cell<float>(0x43475890u),
                    source.storage.data().native_cell<RageVector>(0x43476414u),
                    source.storage.data().native_cell<RageVector>(0x4347A4F0u),
                    source.storage.data().native_cell<float>(0x43472534u)},
                   source.storage.data().native_cell<std::uint32_t>(0x4346D674u),
                   source.shot,
                   occlusion},
              hooks(source.models, source.world, draw, lifecycle_bindings.first_hook,
                    lifecycle_bindings.second_hook),
              factory{{registry(source.storage, 0x43475B78u), source.storage.command_phases(),
                       registry(source.storage, 0x43475A9Cu), source.storage.entity_observers(),
                       registry(source.storage, 0x43475988u), registry(source.storage, 0x43475AD8u),
                       source.storage.empty_ticks()},
                      {source.command->owner, lifecycle, hooks, updates,
                       source.storage.event_registry(),
                       source.storage.data().native_cell<EffectsCommandOwner*>(0x43475A1Cu),
                       source.storage.data(), classification}} {}
        static void initialize(void* context) {
            auto& self = *static_cast<Primary*>(context);
            client::create_effects_runtime_owner(self.factory, self.data.storage.configuration());
        }
    };
    std::unique_ptr<Primary> primary;
    Data(client::ClientRuntimeStorage& source, const client::EngineRuntimeBindings& bindings,
         client::ConfigurationRuntimeOwner& settings, const engine::BuildMaterialBindings& imports,
         void* key_values)
        : storage(checked(source, bindings, settings, imports, key_values)), engine(bindings),
          configuration(settings), materials(storage.data(), key_values),
          models(materials, settings.colored_model_settings()), world_interfaces(storage.data()),
          world(settings.effects_settings(), world_interfaces,
                world_fields(storage.offsets().globals),
                storage.data().native_cell<std::uint8_t>(0x43468E58u)),
          view_interfaces(storage.data()), view(settings.effects_settings(), view_interfaces,
                                                view_fields(storage.offsets().globals)),
          smoke_engine{storage.data().native_cell<void*>(0x4346A514u),
                       storage.data().native_cell<void*>(0x4346A890u),
                       storage.data().native_cell<void*>(0x434679C0u)},
          smoke{world, smoke_engine, storage.data().native_cell<WorldRuntime*>(0x4347585Cu)},
          scope{view, settings.effects_settings()}, shot(storage.data()),
          shot_bones(storage.data(), storage.memory()) {}
};
EffectsRuntimeOwner::EffectsRuntimeOwner(client::ClientRuntimeStorage& storage,
                                         const client::EngineRuntimeBindings& engine,
                                         client::ConfigurationRuntimeOwner& configuration,
                                         client::VisualRuntimeOwner& visual,
                                         const engine::BuildMaterialBindings& imports)
    : EffectsRuntimeOwner(storage, engine, configuration, imports, key_values(storage, visual)) {}
EffectsRuntimeOwner::EffectsRuntimeOwner(client::ClientRuntimeStorage& storage,
                                         const client::EngineRuntimeBindings& engine,
                                         client::ConfigurationRuntimeOwner& configuration,
                                         const engine::BuildMaterialBindings& imports,
                                         void* key_values)
    : data_(std::make_unique<Data>(storage, engine, configuration, imports, key_values)) {}
EffectsRuntimeOwner::~EffectsRuntimeOwner() = default;
client::ClientRuntimeStorage& EffectsRuntimeOwner::storage() const noexcept {
    return data_->storage;
}
ModelRenderRuntime& EffectsRuntimeOwner::models() noexcept {
    return data_->models;
}
WorldRuntime& EffectsRuntimeOwner::world() noexcept {
    return data_->world;
}
ViewRuntime& EffectsRuntimeOwner::view() noexcept {
    return data_->view;
}
SmokeOwnerBindings& EffectsRuntimeOwner::smoke_bindings() noexcept {
    return data_->smoke;
}
ScopeOwnerBindings& EffectsRuntimeOwner::scope_bindings() noexcept {
    return data_->scope;
}
ModelShotState& EffectsRuntimeOwner::shot_state() noexcept {
    return data_->shot;
}
rage::ShotModelBoneStorage& EffectsRuntimeOwner::shot_bones() noexcept {
    return data_->shot_bones;
}
void EffectsRuntimeOwner::compose_command(CameraRuntimeOwner& camera,
                                          client::PlayerRuntimeOwner& players,
                                          engine::SceneQueryRuntimeOwner& scene,
                                          client::CrtRuntimeOwner& crt,
                                          const engine::BuildEspBindings& functions) {
    if (data_->command)
        throw std::logic_error("The canonical effects command was already composed");
    auto& storage = data_->storage;
    auto& image = storage.data();
    require(&camera.storage() == &storage && &players.storage() == &storage &&
                &scene.storage() == &storage && &scene.players() == &players &&
                &crt.storage() == &storage && &functions.data == &image &&
                &crt.spread().image() == &image,
            "Effects command must share its actual camera, player, scene, CRT and engine owners");
    require(&camera.runtime().force_dead == &data_->world.settings.third_person_dead.value &&
                &camera.runtime().alive_hotkey == &data_->world.settings.third_person_key.value,
            "Effects command and camera require the same canonical third-person settings");
    require(crt.random().seed && crt.random().uniform && crt.trigonometry().sine &&
                crt.trigonometry().cosine && crt.spread().tables().standard.data() &&
                crt.spread().tables().shotgun_patterns.data(),
            "Effects command requires its genuinely initialized CRT spread and numeric services");
    require(
        image.native_cell<void*>(0x43467DC0u) && image.native_cell<void*>(0x43475B90u) &&
            image.native_cell<InvalidateEntityPhysics>(0x43468C10u) &&
            image.native_cell<engine::ShotgunPatternEntry>(0x43467DDCu) &&
            data_->engine.variables.at(0x4346A67Cu).get() &&
            data_->engine.variables.at(0x43468520u).get(),
        "Effects command requires the recovered ragdoll, trace, physics, shotgun and ConVar bindings");
    data_->command =
        std::make_unique<Data::Command>(*data_, camera, players, scene, crt, functions);
}
EffectsCommandOwner& EffectsRuntimeOwner::command() {
    if (!data_->command)
        throw std::logic_error("Actual effects command dependencies have not been composed");
    return data_->command->owner;
}
void EffectsRuntimeOwner::compose_primary(CameraRuntimeOwner& camera,
                                          client::HistoryRuntimeOwner& history,
                                          animation::RuntimeOwner& bones,
                                          client::CrtRuntimeOwner& crt) {
    if (data_->primary)
        throw std::logic_error("The canonical primary effects owner was already composed");
    if (!data_->command)
        throw std::logic_error("Primary effects require the actual command owner first");
    auto& storage = data_->storage;
    auto& image = storage.data();
    const auto& recording = history.recording_bindings();
    require(
        &camera.storage() == &storage && &history.storage() == &storage &&
            &bones.storage() == &storage && &crt.storage() == &storage &&
            &camera.runtime() == &data_->command->owner.camera &&
            &history.players().cache() == &data_->command->owner.players &&
            &recording.players == &history.players().cache() &&
            &bones.players() == &recording.players && &recording.bones == &bones.bones(),
        "Primary effects must share their actual command, camera, player cache, history, animation and CRT owners");
    require(&crt.descriptor_storage().image() == &image &&
                static_cast<void*>(&crt.descriptor_storage().at(studio_model_hook_descriptor)) ==
                    &image.native_cell<ModelHookDescriptor>(studio_model_hook_descriptor) &&
                static_cast<void*>(&crt.descriptor_storage().at(model_occlusion_hook_descriptor)) ==
                    &image.native_cell<ModelHookDescriptor>(model_occlusion_hook_descriptor),
            "Effects hooks require the canonical source descriptor storage");
    require(recording.publication.arccosine && recording.publication.atan2 &&
                crt.trigonometry().sine && crt.trigonometry().cosine,
            "Effects history interpolation requires its genuine CRT numeric imports");
    const auto manifest = engine::build_interface_manifest();
    for (const auto cell : {0x43468A4Cu, 0x4346A3A0u, 0x4346A614u, 0x43469330u}) {
        bool found = false;
        for (std::size_t index = 0; index < manifest.size(); ++index) {
            if (manifest[index].destination_cell != cell)
                continue;
            auto* object = image.native_cell<void*>(cell);
            require(object && object == data_->engine.interfaces.objects[index],
                    "Primary effects interface differs from the acquired canonical engine object");
            found = true;
            break;
        }
        require(found, "Primary effects interface is absent from the acquisition manifest");
    }
    require(image.native_cell<void*>(0x43467204u) && image.native_cell<void*>(0x43475B2Cu) &&
                image.native_cell<void*>(0x43467EB8u) && image.native_cell<void*>(0x4346A338u),
            "Primary effects require their actual allocator and live timing aliases");
    for (const auto cell : {0x4346A39Cu, 0x43467080u, 0x43468DFCu, 0x43467A78u, 0x43467A8Cu,
                            0x4346A568u, 0x4346747Cu, 0x4346A748u, 0x4346A330u, 0x4346A850u})
        require(data_->engine.variables.at(cell).get() != nullptr,
                "Primary effects require their acquired world, camera and history ConVars");
    data_->primary = std::make_unique<Data::Primary>(*data_, camera, history, bones, crt);
}
client::EffectsRuntimeFactoryBindings& EffectsRuntimeOwner::factory_bindings() {
    if (!data_->primary)
        throw std::logic_error("Primary effects dependencies have not been composed");
    return data_->primary->factory;
}
ModelRenderHooks& EffectsRuntimeOwner::model_hooks() {
    if (!data_->primary)
        throw std::logic_error("Primary effects draw hooks have not been composed");
    return data_->primary->hooks;
}
ModelRenderLifecycle& EffectsRuntimeOwner::lifecycle() {
    return factory_bindings().owner.lifecycle;
}
WorldCallbacks& EffectsRuntimeOwner::updates() {
    return factory_bindings().owner.updates;
}
void EffectsRuntimeOwner::bind_initializers(client::NativeStartupSources& sources) {
    (void)factory_bindings();
    require(&sources.data() == &data_->storage.data(),
            "Effects startup must use its canonical source data image");
    sources.bind_initializer(
        {client::effects_runtime_factory, data_->primary.get(), &Data::Primary::initialize});
}
} //namespace skeet::effects
