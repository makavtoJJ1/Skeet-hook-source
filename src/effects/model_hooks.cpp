#include "skeet/effects/model_hooks.hpp"
#include "skeet/effects/bullet_impact_command_owner.hpp"
#include "skeet/anti_aim/command_owner.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>

namespace skeet::effects {
namespace {
EffectsCommandOwner* const* active_material_owner = nullptr;
template <class T> T field(const void* object, std::ptrdiff_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
std::int32_t convar_integer(const void* variable) {
    for (;;) {
        const auto* parent = field<const void*>(variable, 0x1C);
        if (parent == variable)
            break;
        variable = parent;
    }
    return std::bit_cast<std::int32_t>(
        field<std::uint32_t>(variable, 0x30) ^
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(variable)));
}
ModelRenderHooks& hooks(void* receiver) {
    auto* table = *static_cast<void***>(receiver);
    auto* descriptor = static_cast<ModelHookDescriptor*>(table[-2]);
    auto* owner = static_cast<ModelRenderRuntime*>(descriptor->owner);
    return *owner->hooks;
}
void* active_material(std::size_t index) {
    if (!active_material_owner)
        throw std::logic_error("Canonical model material owner slot is not bound");
    auto* owner = *active_material_owner;
    if (!owner)
        return nullptr;
    return owner->models.prepare_material(&owner->models.materials[index]);
}
}
void bind_model_material_owner(EffectsCommandOwner* const& active) noexcept {
    active_material_owner = &active;
}
void* player_model_material() {
    return active_material(0);
}
void* hands_model_material() {
    return active_material(5);
}
void* weapon_viewmodel_material() {
    return active_material(6);
}
std::span<const ModelMaterialProviderEntry, 3> model_material_provider_entries() {
    static const std::array<ModelMaterialProviderEntry, 3> entries{
        {{0x43336B64, &player_model_material},
         {0x434019FB, &hands_model_material},
         {0x43417FC3, &weapon_viewmodel_material}}};
    return entries;
}
std::span<const ModelHookReplacement, 5> studio_model_hook_replacements() {
    static const std::array<ModelHookReplacement, 5> entries{
        {{29, 0x433CCB15, reinterpret_cast<void*>(&studio_model_draw_hook)},
         {30, 0x43401A33, reinterpret_cast<void*>(&studio_prop_hook)},
         {47, 0x433B0A83, reinterpret_cast<void*>(&model_prop50_hook)},
         {49, 0x433C930C, reinterpret_cast<void*>(&model_prop48_hook)},
         {48, 0x4337D6DC, reinterpret_cast<void*>(&model_prop60_hook)}}};
    return entries;
}
std::span<const ModelHookReplacement, 1> model_occlusion_hook_replacements() {
    static const std::array<ModelHookReplacement, 1> entries{
        {{16, 0x4336C334, reinterpret_cast<void*>(&model_unoccluded_hook)}}};
    return entries;
}
FakeModelPose capture_fake_model_pose(const ModelFakePoseBindings& bindings) {
    FakeModelPose pose{};
    const auto* record = bindings.record;
    // The original 434685B8 gate is Body yaw, now owned by the source AA config.
    // Reading the old BSS cell here leaves the fake pose permanently disabled.
    if (!record || !record->global.enabled.value || !record->global.body_yaw.value)
        return pose;
    const auto count = record->state.value_1b0;
    if (count == 0)
        return pose;
    if (count > 256 || count > record->finisher.matrices_520.size() ||
        &record->precalculation.common != static_cast<const anti_aim::State*>(&record->state) ||
        &record->finisher.common != &record->state)
        throw std::logic_error(
            "Source fake model pose requires its canonical AA state and bounded matrix storage");
    pose.available = true;
    pose.capture_tick = record->state.value_1b4;
    if (!bindings.current_tick)
        throw std::logic_error("Fake model pose requires its live outgoing-command alias");
    pose.current_tick = *bindings.current_tick;
    pose.blend = bindings.blend;
    pose.observed_yaw = record->state.reference_yaw_258;
    pose.reference_yaw = record->precalculation.feet_yaw_194;
    pose.observed_angles = bindings.observed_angles;
    pose.reference_angles = bindings.reference_angles;
    pose.transform_angles = record->finisher.absolute_angles_1b8;
    pose.rotation_radians = bindings.rotation_radians;
    pose.bones = record->finisher.matrices_520.first(count);
    return pose;
}
ModelRenderHooks::ModelRenderHooks(ModelRenderRuntime& models, WorldRuntime& world,
                                   const ModelRenderHookInputs& inputs, ModelHookDescriptor& studio,
                                   ModelHookDescriptor& occlusion)
    : models_(models), world_(world), inputs_(inputs), studio_(studio), occlusion_(occlusion) {}
ModelRenderHooks::~ModelRenderHooks() {
    if (models_.hooks == this)
        models_.hooks = nullptr;
}
void ModelRenderHooks::bind_replacements() {
    if (!studio_.replacement_table || !occlusion_.replacement_table)
        throw std::logic_error(
            "Actual model hook descriptor tables must be prepared before lifecycle initialization");
    bind_studio_replacements();
    bind_occlusion_replacements();
}
void ModelRenderHooks::bind_studio_replacements() {
    if (!studio_.replacement_table)
        throw std::logic_error("Studio descriptor has not been prepared");
    models_.hooks = this;
    for (const auto& entry : studio_model_hook_replacements())
        studio_.replacement_table[entry.slot] = entry.source_entry;
}
void ModelRenderHooks::bind_occlusion_replacements() {
    if (!occlusion_.replacement_table)
        throw std::logic_error("Occlusion descriptor has not been prepared");
    models_.hooks = this;
    for (const auto& entry : model_occlusion_hook_replacements())
        occlusion_.replacement_table[entry.slot] = entry.source_entry;
}
void ModelRenderHooks::draw(void* studio, const ModelDrawArguments& args) {
    auto entities = inputs_.entities;
    entities.local_player = inputs_.local_player;
    const bool effects =
        world_.suspended == 0 && field<std::int32_t>(studio, 0x260) != 2 && !(args.flags & 0x204);
    bool history = false;
    FakeModelPose fake{};
    if (effects) {
        entities.game_type = convar_integer(inputs_.game_type);
        entities.teammates_are_enemies = convar_integer(inputs_.teammates_are_enemies) != 0;
        if (models_.settings.shadow.value)
            history = convar_integer(inputs_.shadow_history) != 0;
        if (models_.settings.local_fake.enabled.value)
            fake = capture_fake_model_pose(inputs_.fake_pose);
    }
    const ModelDrawFrame frame{inputs_.classification,
                               entities,
                               inputs_.trigonometry,
                               inputs_.rotation_math,
                               inputs_.local_player_flag,
                               inputs_.immunity,
                               inputs_.ragdoll_owner,
                               inputs_.attachment_owner,
                               world_.suspended != 0,
                               inputs_.player_effects != 0,
                               inputs_.player_adjustments,
                               history,
                               fake.available,
                               fake.available ? &fake : nullptr,
                               inputs_.shadows,
                               inputs_.bone_runtime,
                               inputs_.shot_entity_handle,
                               &inputs_.shot,
                               world_.settings.wall_transparency.value};
    models_.draw_model(studio, args, frame);
}
void ModelRenderHooks::unoccluded(void* list, void* view) {
    auto bindings = inputs_.occlusion;
    bindings.player_renderable_vtable = inputs_.classification.renderable_vtables[0];
    bindings.shot_entity_handle = inputs_.shot_entity_handle;
    models_.build_unoccluded_list(list, view, bindings);
}
void ModelRenderHooks::prop50(void* receiver, std::uintptr_t first, std::int32_t count,
                              void* records, std::uintptr_t last) {
    models_.draw_prop_records_50(receiver, first, count, records, last, world_.settings,
                                 world_.suspended != 0);
}
void ModelRenderHooks::prop48(void* receiver, std::uintptr_t first, std::int32_t count,
                              void* records, std::int32_t stride, std::uintptr_t last) {
    models_.draw_prop_records_48(receiver, first, count, records, stride, last, world_.settings,
                                 world_.suspended != 0);
}
void ModelRenderHooks::prop60(void* receiver, std::uintptr_t first, std::int32_t count,
                              ModelPropBatch* batches, std::int32_t stride, std::uint32_t flags) {
    models_.draw_prop_batches_60(receiver, first, count, batches, stride, flags,
                                 inputs_.classification.renderable_vtables[3], world_.settings,
                                 world_.suspended != 0);
}
void ModelRenderHooks::prop_studio(void* receiver, std::uintptr_t first, std::uintptr_t second,
                                   std::uintptr_t third) {
    models_.draw_prop_studio(receiver, first, second, third, world_.settings,
                             world_.suspended != 0);
}
#if defined(_M_IX86) || defined(__i386__)
#define SKEET_MODEL_HOOK_ABI __fastcall
#else
#define SKEET_MODEL_HOOK_ABI
#endif
void SKEET_MODEL_HOOK_ABI studio_model_draw_hook(void* receiver, void*, void* result, void* info,
                                                 void* bones, void* flex, void* delayed,
                                                 void* origin, std::uint32_t flags) {
    hooks(receiver).draw(receiver, {result, info, bones, flex, delayed, origin, flags});
}
void SKEET_MODEL_HOOK_ABI studio_prop_hook(void* receiver, void*, std::uintptr_t a,
                                           std::uintptr_t b, std::uintptr_t c) {
    hooks(receiver).prop_studio(receiver, a, b, c);
}
void SKEET_MODEL_HOOK_ABI model_prop50_hook(void* receiver, void*, std::uintptr_t a, std::int32_t n,
                                            void* p, std::uintptr_t z) {
    hooks(receiver).prop50(receiver, a, n, p, z);
}
void SKEET_MODEL_HOOK_ABI model_prop48_hook(void* receiver, void*, std::uintptr_t a, std::int32_t n,
                                            void* p, std::int32_t stride, std::uintptr_t z) {
    hooks(receiver).prop48(receiver, a, n, p, stride, z);
}
void SKEET_MODEL_HOOK_ABI model_prop60_hook(void* receiver, void*, std::uintptr_t a, std::int32_t n,
                                            ModelPropBatch* p, std::int32_t stride,
                                            std::uint32_t flags) {
    hooks(receiver).prop60(receiver, a, n, p, stride, flags);
}
void SKEET_MODEL_HOOK_ABI model_unoccluded_hook(void* receiver, void*, void* view) {
    hooks(receiver).unoccluded(receiver, view);
}
#undef SKEET_MODEL_HOOK_ABI
} // namespace skeet::effects
