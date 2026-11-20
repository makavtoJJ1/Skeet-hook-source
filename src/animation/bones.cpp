#include "skeet/animation/bones.hpp"
#include "skeet/engine/opaque_native_word.hpp"
#include <algorithm>

namespace skeet::animation {
Matrix3x4 bone_local_matrix(const RageVector& position, const BoneQuaternion& q) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const double dx = x, dy = y, dz = z, dw = w;
    return {{{float((1.0 - (dy + dy) * dy) - (dz + dz) * dz), (x + x) * y - (w + w) * z,
              (x + x) * z + (w + w) * y, position[0]},
             {float((dx + dx) * dy + (dw + dw) * dz), (1.0f - (x + x) * x) - (z + z) * z,
              (y + y) * z - (w + w) * x, position[1]},
             {float((dx + dx) * dz - (dw + dw) * dy), (w + w) * x + (y + y) * z,
              (1.0f - (x + x) * x) - (y + y) * y, position[2]}}};
}
namespace {
Matrix3x4 concatenate(const Matrix3x4& parent, const Matrix3x4& local) noexcept {
    Matrix3x4 result;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column)
            // Native skips unmasked output bones. If a descendant references
            // such a parent, observe the actual prior output bytes opaquely.
            result.rows[row][column] =
                ((engine::read_opaque_native_float(&parent.rows[row][2]) * local.rows[2][column] +
                  engine::read_opaque_native_float(&parent.rows[row][1]) * local.rows[1][column]) +
                 engine::read_opaque_native_float(&parent.rows[row][0]) * local.rows[0][column]) +
                (column == 3 ? engine::read_opaque_native_float(&parent.rows[row][3]) : 0.0f);
    return result;
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
void compose_bone_hierarchy(const BoneModel& model, const Matrix3x4& root,
                            std::span<const RageVector> positions,
                            std::span<const BoneQuaternion> rotations, std::int32_t starting_bone,
                            std::uint32_t mask, std::span<Matrix3x4> output) noexcept {
    const auto count = std::int32_t(model.parents.size());
    std::array<std::int32_t, maximum_bones> traversal;
    unsigned length = 0;
    if (starting_bone < -1 || starting_bone >= count)
        starting_bone = 0;
    if (starting_bone == -1) {
        length = unsigned(count);
        for (std::int32_t i = 0; i < count; ++i)
            traversal[unsigned(count - i - 1)] = i;
    } else {
        do {
            traversal[length++] = starting_bone;
            starting_bone = model.parents[unsigned(starting_bone)];
        } while (starting_bone != -1);
    }
    while (length) {
        const auto bone = unsigned(traversal[--length]);
        if (!(model.flags[bone] & mask))
            continue;
        const auto local = bone_local_matrix(positions[bone], rotations[bone]);
        const auto parent = model.parents[bone];
        output[bone] = concatenate(parent == -1 ? root : output[unsigned(parent)], local);
    }
}

bool build_bone_matrices(BoneEntityState& state, const BoneModel& model, const BoneRequest& request,
                         std::span<Matrix3x4> output, const BoneEngine& engine,
                         const RageTrigonometry& trig) {
    if (!model.renderable_model || model.model_index == -1 || !model.studio_header ||
        (model.virtual_model_required && !model.virtual_model))
        return false;
    const auto saved = state;
    state.interpolation_flags |= 2;
    state.animation_frame = 0;
    state.effects |= 8;
    state.setup_flags = request.options | 4;
    std::uint32_t owner_flags = 0;
    std::uintptr_t ik_owner = 0;
    if (state.owner_setup_flags) {
        owner_flags = *state.owner_setup_flags;
        *state.owner_setup_flags = state.setup_flags;
    }
    if (state.ik_owner_slot) {
        ik_owner = *state.ik_owner_slot;
        *state.ik_owner_slot = state.ik_owner;
    }
    state.ik_mask = request.options & 2 ? 0x300u : 0xffffffffu;
    if (state.layer_count > 12)
        state.last_layer_weight = 0.0f;
    state.angles[0] = state.angles[1] = request.angles;
    state.origins[0] = state.origins[1] = request.origin;
    state.entity_flags = (state.entity_flags & 0xffff07ffu) | 8;
    std::array<RageVector, maximum_bones> positions;
    std::array<BoneQuaternion, maximum_bones> rotations;
    engine.accumulate_pose(engine.context, state, model, positions, rotations, request.time,
                           request.mask);
    state.entity_flags &= ~8u;
    compose_bone_hierarchy(model, root_matrix(request.angles, request.origin, trig), positions,
                           rotations, request.starting_bone, request.mask, output);
    if (request.attachments && engine.update_attachments && (request.mask & 0xfff00u)) {
        const auto readable = state.readable_bones, writable = state.writable_bones;
        state.readable_bones = state.writable_bones = 0xfff00;
        engine.update_attachments(engine.context, state, output, request.mask);
        state.writable_bones = writable;
        state.readable_bones = readable;
    }
    if (saved.ik_owner_slot)
        *saved.ik_owner_slot = ik_owner;
    if (saved.owner_setup_flags)
        *saved.owner_setup_flags = owner_flags;
    state.entity_flags = saved.entity_flags;
    state.angles = saved.angles;
    state.origins = saved.origins;
    if (saved.layer_count > 12)
        state.last_layer_weight = saved.last_layer_weight;
    state.ik_mask = saved.ik_mask;
    state.setup_flags = saved.setup_flags;
    state.effects = saved.effects;
    state.animation_frame = saved.animation_frame;
    state.interpolation_flags = saved.interpolation_flags;
    return true;
}
std::int32_t select_hitbox_bone_matrices(const BoneModel& model, std::span<const Matrix3x4> bones,
                                         std::span<Matrix3x4> hitboxes) noexcept {
    if (!model.studio_header)
        return 0;
    const auto count = std::min({std::size_t(19), model.hitbox_bones.size(), hitboxes.size()});
    for (std::size_t i = 0; i < count; ++i)
        if (model.hitbox_bones[i] >= bones.size())
            return 0;
    for (std::size_t i = 0; i < count; ++i)
        hitboxes[i] = bones[model.hitbox_bones[i]];
    return std::int32_t(count);
}
std::int32_t build_resolver_hitbox_matrices(BoneEntityState& state, const BoneModel& model,
                                            const RageVector& origin, float yaw,
                                            std::span<Matrix3x4> hitboxes, const BoneEngine& engine,
                                            const RageTrigonometry& trig) {
    std::array<Matrix3x4, maximum_bones> bones;
    if (!build_bone_matrices(state, model, {{0.0f, yaw, 0.0f}, origin, 0.0f, 0x80300, -1, 0, false},
                             bones, engine, trig))
        return 0;
    return select_hitbox_bone_matrices(model, bones, hitboxes);
}
} // namespace skeet::animation
