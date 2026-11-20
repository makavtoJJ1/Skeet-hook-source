#include "skeet/animation/bone_runtime.hpp"
#include "skeet/engine/studio_hitbox_set.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace skeet::animation {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void write(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
template <class T> T* field(void* object, std::ptrdiff_t offset) noexcept {
    return reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}
const void* studio_wrapper(const void* player, std::ptrdiff_t offset) noexcept {
    const auto* wrapper = read<const void*>(player, offset);
    return wrapper && read<const void*>(wrapper, 0) ? wrapper : nullptr;
}
const std::byte* hitbox_set(const void* player, const void* wrapper,
                            std::ptrdiff_t offset) noexcept {
    const auto* header = read<const std::byte*>(wrapper, 0);
    return engine::studio_hitbox_set(header, read<std::int32_t>(player, offset));
}
BoneModel hierarchy_model(const void* wrapper) noexcept {
    const auto* header = read<const void*>(wrapper, 0);
    const auto count = std::size_t(read<std::int32_t>(header, 0x9C));
    return {true,
            0,
            true,
            false,
            false,
            {read<const std::uint32_t*>(wrapper, 0x30), count},
            {read<const std::int32_t*>(wrapper, 0x44), count},
            {}};
}
Matrix3x4 root_matrix(const RageVector& angles, const RageVector& origin,
                      const RageTrigonometry& trig) {
    constexpr float radians = 0.01745329238474369f;
    const float sy = trig.sine(angles[1] * radians), cy = trig.cosine(angles[1] * radians);
    const float sp = trig.sine(angles[0] * radians), cp = trig.cosine(angles[0] * radians);
    const float sr = trig.sine(angles[2] * radians), cr = trig.cosine(angles[2] * radians);
    const float crcy = cr * cy, crsy = cr * sy, srcy = sr * cy, srsy = sr * sy;
    return {{{cp * cy, srcy * sp - crsy, crcy * sp + srsy, origin[0]},
             {cp * sy, srsy * sp + crcy, crsy * sp - srcy, origin[1]},
             {-sp, sr * cp, cr * cp, origin[2]}}};
}
}

BoneModel BoneModelContext::model() const noexcept {
    return {true,
            0,
            header != nullptr,
            virtual_model_required,
            virtual_model,
            flags,
            parents,
            {hitbox_bones.data(), std::size_t(std::max(0, hitbox_count))}};
}

BoneModelContext BoneRuntime::load_model(const void* player) const {
    BoneModelContext result;
    result.wrapper = studio_wrapper(player, fields.studio_wrapper_32e4daf1);
    if (!result.wrapper)
        return result;
    result.header = read<const void*>(result.wrapper, 0);
    const auto count = std::size_t(read<std::int32_t>(result.header, 0x9C));
    result.virtual_model_required = read<std::int32_t>(result.header, 0x150) != 0;
    result.virtual_model = read<const void*>(result.wrapper, 4) != nullptr;
    result.flags = {read<const std::uint32_t*>(result.wrapper, 0x30), count};
    result.parents = {read<const std::int32_t*>(result.wrapper, 0x44), count};
    const auto* set = hitbox_set(player, result.wrapper, fields.hitbox_set_c2b6fb10);
    if (!set)
        return result;
    result.hitbox_count = std::min(19, read<std::int32_t>(set, 4));
    if (result.hitbox_count <= 0)
        return result;
    const auto* descriptors = set + read<std::int32_t>(set, 8);
    for (std::int32_t i = 0; i < result.hitbox_count; ++i)
        result.hitbox_bones[std::size_t(i)] =
            read<std::uint32_t>(descriptors, std::ptrdiff_t(i) * 0x44);
    return result;
}

std::span<float, 24> BoneRuntime::pose_parameters(void* player) const noexcept {
    return std::span<float, 24>(field<float>(player, fields.pose_parameters), 24);
}

namespace {
std::span<Matrix3x4> matrix_output(std::span<Matrix3x4> output, std::size_t) noexcept {
    return output;
}
std::span<Matrix3x4> matrix_output(Matrix3x4* output, std::size_t count) noexcept {
    return {output, count};
}
template <class Output>
bool build_live_bones(const BoneRuntime& runtime, void* player, const BoneRequest& request,
                      Output output, std::int32_t* count) {
    const auto& fields = runtime.fields;
    const auto entity_list = runtime.entity_list;
    const auto& trigonometry = runtime.trigonometry;
    if (count)
        *count = 0;
    if (!engine::invoke<const void*>(static_cast<std::byte*>(player) + 4, 8))
        return false;
    if (read<std::int32_t>(player, fields.model_index_a25d8271) == -1)
        return false;
    // The wrapper is retained through the transaction, but its live header is
    // read again for the count and hierarchy after genuine engine boundaries.
    const auto* wrapper = read<const void*>(player, fields.studio_wrapper_32e4daf1);
    if (!wrapper)
        return false;
    const auto* header = read<const void*>(wrapper, 0);
    if (!header)
        return false;
    if (read<std::int32_t>(header, 0x150) && !read<const void*>(wrapper, 4))
        return false;

    auto* interpolation = field<std::uint8_t>(player, fields.interpolation_flags_57b2d535);
    const auto saved_interpolation = *interpolation;
    *interpolation = saved_interpolation | 2u;
    auto* animation_frame = field<std::uint32_t>(player, fields.animation_frame_ae5b982e);
    const auto saved_animation_frame = *animation_frame;
    *animation_frame = 0;
    auto* effects = field<std::uint32_t>(player, fields.effects_38ca5228);
    const auto saved_effects = *effects;
    *effects = saved_effects | 8u;
    auto* setup_flags = field<std::uint32_t>(player, fields.setup_flags_bb78dc43);
    const auto saved_setup_flags = *setup_flags;
    *setup_flags = request.options | 4u;

    std::uint32_t* related_setup_flags = nullptr;
    std::uint32_t saved_related_setup_flags = 0;
    void** ik_owner_slot = nullptr;
    void* saved_ik_owner = nullptr;
    const auto owner_handle = read<std::uint32_t>(player, fields.owner_handle_cd812654);
    if (owner_handle != UINT32_MAX) {
        auto* owner = engine::invoke<void*>(entity_list, 4, owner_handle);
        if (owner) {
            const auto related_handle = read<std::uint32_t>(owner, fields.related_handle_ca2b5ffb);
            if (related_handle != UINT32_MAX) {
                if (auto* related = engine::invoke<void*>(entity_list, 4, related_handle)) {
                    related_setup_flags =
                        field<std::uint32_t>(related, fields.setup_flags_bb78dc43);
                    saved_related_setup_flags = *related_setup_flags;
                    *related_setup_flags = read<std::uint32_t>(player, fields.setup_flags_bb78dc43);
                }
            }
            if (auto* animation = read<void*>(player, fields.animation_state_3d52a1a7)) {
                ik_owner_slot = field<void*>(animation, 0x5C);
                // The optional state may be a native caller's still-opaque
                // automatic copy. Save/restore the actual pointer bits.
                saved_ik_owner = engine::read_opaque_native_pointer(ik_owner_slot);
                *ik_owner_slot = owner;
            }
        }
    }

    if (count)
        *count = read<std::int32_t>(read<const void*>(wrapper, 0), 0x9C);
    auto* ik_mask = field<std::uint32_t>(player, fields.ik_mask_01d0bacc);
    const auto saved_ik_mask = *ik_mask;
    *ik_mask = (request.options & 2u) ? 0x300u : UINT32_MAX;
    std::uint32_t* layer_weight = nullptr;
    std::uint32_t saved_layer_weight = 0;
    if (read<std::int32_t>(player, fields.animation_layers_b538bba5 + 0xC) > 12) {
        layer_weight =
            field<std::uint32_t>(read<void*>(player, fields.animation_layers_b538bba5), 0x2C0);
        saved_layer_weight = *layer_weight;
        *layer_weight = 0;
    }
    auto* angles_first = field<RageVector>(player, fields.angles_3b1f6ecc);
    const auto saved_angles_first = *angles_first;
    *angles_first = request.angles;
    auto* angles_second = field<RageVector>(player, fields.angles_f8205eba);
    const auto saved_angles_second = *angles_second;
    *angles_second = request.angles;
    auto* origin_first = field<RageVector>(player, fields.origin_2a671b42);
    const auto saved_origin_first = *origin_first;
    *origin_first = request.origin;
    auto* origin_second = field<RageVector>(player, fields.origin_4e66bb34);
    const auto saved_origin_second = *origin_second;
    *origin_second = request.origin;
    auto* entity_flags = field<std::uint32_t>(player, fields.entity_flags_83a6cf2b);
    const auto saved_entity_flags = *entity_flags;
    *entity_flags = saved_entity_flags & 0xFFFF07FFu;
    *entity_flags |= 8u;

    std::array<RageVector, maximum_bones> positions;
    std::array<BoneQuaternion, maximum_bones> rotations;
    engine::invoke<void>(player, 0x338 / 4, wrapper, positions.data(), rotations.data(),
                         request.time, request.mask);
    auto* current_flags = field<std::uint32_t>(player, fields.entity_flags_83a6cf2b);
    *current_flags &= ~8u;
    const auto root = root_matrix(request.angles, request.origin, trigonometry);
    const auto model = hierarchy_model(wrapper);
    const auto matrices = matrix_output(output, model.parents.size());
    compose_bone_hierarchy(model, root, positions, rotations, request.starting_bone, request.mask,
                           matrices);
    if (request.attachments)
        runtime.update_bone_attachments(player, matrices, request.mask);

    // Restore the exact saved destinations in native order; genuine engine
    // changes to other fields, including layer count and bone accessors, remain.
    *entity_flags = saved_entity_flags;
    *origin_second = saved_origin_second;
    *origin_first = saved_origin_first;
    *angles_second = saved_angles_second;
    *angles_first = saved_angles_first;
    if (layer_weight)
        *layer_weight = saved_layer_weight;
    *ik_mask = saved_ik_mask;
    if (ik_owner_slot)
        *ik_owner_slot = saved_ik_owner;
    if (related_setup_flags)
        *related_setup_flags = saved_related_setup_flags;
    *setup_flags = saved_setup_flags;
    *effects = saved_effects;
    *animation_frame = saved_animation_frame;
    *interpolation = saved_interpolation;
    return true;
}
}
bool BoneRuntime::build_bone_matrices(void* player, const BoneRequest& request,
                                      std::span<Matrix3x4> output, std::int32_t* count) const {
    return build_live_bones(*this, player, request, output, count);
}
bool BoneRuntime::build_bone_matrices(void* player, const BoneRequest& request, Matrix3x4* output,
                                      std::int32_t* count) const {
    return build_live_bones(*this, player, request, output, count);
}

std::int32_t BoneRuntime::select_hitbox_bone_matrices(const void* player,
                                                      std::span<const Matrix3x4> bones,
                                                      std::span<Matrix3x4> hitboxes) const {
    const auto* wrapper = studio_wrapper(player, fields.studio_wrapper_32e4daf1);
    if (!wrapper)
        return 0;
    const auto* set = hitbox_set(player, wrapper, fields.hitbox_set_c2b6fb10);
    if (!set)
        return 0;
    const auto count = std::min(read<std::int32_t>(set, 4),
        static_cast<int>(std::min<std::size_t>(19, hitboxes.size())));
    if (count <= 0)
        return 0;
    std::array<std::uint32_t, 19> mapping;
    const auto relative = read<std::int32_t>(set, 8);
    for (std::int32_t i = 0; i < count; ++i) {
        mapping[i] = read<std::uint32_t>(set, std::ptrdiff_t(relative) + std::ptrdiff_t(i) * 0x44);
        if (mapping[i] >= bones.size())
            return 0;
    }
    for (int i = 0; i < count; ++i)
        hitboxes[i] = bones[mapping[i]];
    return count;
}

void BoneRuntime::scatter_hitbox_bone_matrices(const void* player,
                                               std::span<const Matrix3x4> hitboxes,
                                               std::span<Matrix3x4> bones) const {
    const auto* wrapper = studio_wrapper(player, fields.studio_wrapper_32e4daf1);
    if (!wrapper)
        return;
    const auto* set = hitbox_set(player, wrapper, fields.hitbox_set_c2b6fb10);
    if (!set)
        return;
    const auto count = std::min(read<std::int32_t>(set, 4),
        static_cast<int>(std::min<std::size_t>(19, hitboxes.size())));
    for (std::int32_t i = 0; i < count; ++i) {
        const auto relative = read<std::int32_t>(set, 8);
        const auto bone =
            read<std::uint32_t>(set, std::ptrdiff_t(relative) + std::ptrdiff_t(i) * 0x44);
        if (bone < bones.size())
            bones[bone] = hitboxes[std::size_t(i)];
    }
}

void BoneRuntime::update_bone_attachments(void* player, std::span<Matrix3x4> bones,
                                          std::uint32_t mask) const {
    if (!update_attachments || !(mask & 0xFFF00u))
        return;
    auto* accessor = static_cast<std::byte*>(player) + fields.bone_accessor_54783141;
    const auto readable = read<std::uint32_t>(accessor, 8);
    write(accessor, 8, 0xFFF00u);
    const auto writable = read<std::uint32_t>(accessor, 0xC);
    write(accessor, 0xC, 0xFFF00u);
    update_attachments(player, bones.data(), mask);
    write(accessor, 0xC, writable);
    write(accessor, 8, readable);
}

std::int32_t BoneRuntime::refresh_hitbox_attachments(void* player,
                                                     std::span<Matrix3x4, 19> hitboxes,
                                                     std::span<Matrix3x4, 256> scratch) const {
    scatter_hitbox_bone_matrices(player, hitboxes, scratch);
    update_bone_attachments(player, scratch, 0x300u);
    return select_hitbox_bone_matrices(player, scratch, hitboxes);
}

std::int32_t BoneRuntime::build_resolver_hitbox_matrices(void* player, float yaw,
                                                         std::span<Matrix3x4> hitboxes) const {
    const auto origin = read<RageVector>(player, fields.bone_origin_a9ee6062);
    std::array<Matrix3x4, maximum_bones> bones;
    const BoneRequest request{{0.0f, yaw, 0.0f}, origin, 0.0f, 0x80300u, -1, 0, false};
    if (!build_bone_matrices(player, request, bones))
        return 0;
    return select_hitbox_bone_matrices(player, bones, hitboxes);
}
} // namespace skeet::animation
