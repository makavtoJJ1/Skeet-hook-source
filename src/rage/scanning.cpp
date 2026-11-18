#include "scanning_detail.hpp"
#include "skeet/rage/acquisition.hpp"
#include "skeet/anti_aim/targets.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/rage_safe_points.hpp"
#include "../rage_safe_point_detail.hpp"
#include "../rage_math.hpp"
#include <cmath>
#include <immintrin.h>

namespace skeet::rage {
namespace {
std::vector<RageAimPoint> descriptors(const ScanContext& context, const ScanPolicy& policy,
                                      const lagcomp::LagRecord& record,
                                      const scan_detail::Model& local,
                                      const scan_detail::Geometry& geometry,
                                      const ScanHypothesisMatrices* additional) {
    constexpr unsigned order[]{3, 4, 0, 6, 8, 7, 10, 9, 17, 15, 18, 16, 11, 12};
    const RagePointGeneration settings{context.origin,
                                       policy.hitboxes.regular,
                                       policy.multipoint_mask,
                                       policy.point_scale,
                                       context.spread,
                                       context.accuracy_percent,
                                       (context.configuration_flags & 4U) != 0};
    std::vector<RageAimPoint> result;
    for (const auto index : order) {
        const auto regular =
            generate_rage_regular_hitbox_points(settings, index, local.hitboxes, geometry.primary);
        result.insert(result.end(), regular.begin(), regular.end());
        const auto& hitbox = geometry.primary[index];
        const auto group = index == 11 || index == 12
                               ? 0x200U
                               : 1U << (std::uint32_t(local.hitboxes[index].hitgroup) & 31U);
        if (!(policy.hitboxes.safe & group) || !(hitbox.capsule.radius > 0.0f))
            continue;
        const auto center = rage_point_detail::midpoint(hitbox.capsule);
        std::array<Matrix3x4, 9> matrices;
        std::size_t count = 0;
        if (additional) {
            for (const auto& yaw : *additional)
                for (const auto& hypothesis : yaw)
                    matrices[count++] = hypothesis[index];
        } else {
            for (const auto& hypothesis : record.alternatives)
                matrices[count++] = hypothesis.matrices[index];
        }
        std::vector<RageAimPoint> safe;
        if (index != 0) {
            safe = generate_rage_body_safe_points(settings, center, record.header.origin, index,
                                                  local.hitboxes[index], {matrices.data(), count});
        } else if (additional) {
            //433FE9C0 returns before head/body occlusion when the optional
            //nine-set pointer is present. Bounds still consume all nine sets.
            auto bounds = rage_point_detail::build_safe_point_bounds(
                context.origin, center, local.hitboxes[0], {matrices.data(), count});
            safe = rage_point_detail::finish_safe_points(std::move(bounds.points), settings,
                                                         record.header.origin, 0,
                                                         local.hitboxes[0].hitgroup);
        } else {
            RageHeadOcclusion occlusion{local.hitboxes[6],
                                        {record.alternatives[0].matrices[6],
                                         record.alternatives[1].matrices[6],
                                         record.alternatives[2].matrices[6]}};
            safe = generate_rage_head_safe_points(settings, center, record.header.origin,
                                                  local.hitboxes[0],
                                                  std::span<const Matrix3x4, 3>(matrices.data(), 3),
                                                  &occlusion, context.trigonometry);
        }
        result.insert(result.end(), safe.begin(), safe.end());
    }
    return result;
}
float safe_multiplier(const ScanContext& context, const ScanPolicy& policy, std::int32_t health,
                      BallisticEntity target, const BallisticQuery& bullet,
                      const scan_detail::ProjectedPoint& point) {
    if (context.weapon_id == 31)
        return 0.0f;
    const auto delta = rage_point_detail::subtract(point.position, context.origin);
    const auto distance =
        std::sqrt((delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2]);
    auto damage = float(std::pow(double(bullet.range_modifier), double(distance * 0.002f)) *
                        double(bullet.base_damage));
    const auto info = context.ballistics.entity_info(target);
    if (hitgroup_armored(info.armor, info.helmet, point.hitgroup))
        damage = (bullet.armor_ratio * 0.5f) * damage;
    const auto integer = _mm_cvttss_si32(_mm_set_ss(damage));
    const auto shots = rage_detail::shots_to_kill(health, integer);
    const auto required =
        maximum_number(float(policy.minimum_damage), float(health) / float(shots));
    return damage == 0.0f ? rage_detail::hitgroup_multipliers[std::uint32_t(point.hitgroup) & 15U]
                          : required / damage;
}
}
namespace scan_detail {
DamageRayResult damage_ray(const ScanContext& context, BallisticEntity target,
                            const Geometry& geometry, const RageVector& direction,
                            const BallisticQuery& bullet, bool safe) {
    const auto model = capture_damage_model(context.ballistics, target, *context.damage_context, bullet);
    anti_aim::TargetTraceFilter native_filter(context.trace_filter_vtable, context.trace_entities,
                                              0x600400bU, 0x600400bU, 1.0f);
    const auto filter = native_filter.binding();
    auto result = trace_damage_ray(context.ballistics, context.source, context.origin, direction,
                                   geometry.primary, context.box_clipper, filter, bullet, model);
    if (safe && result.health_damage > 0) {
        for (const auto& pose : geometry.alternatives) {
            const auto trial = trace_damage_ray(context.ballistics, context.source, context.origin,
                                                 direction, pose, context.box_clipper, filter,
                                                 bullet, model);
            result.health_damage = std::min(result.health_damage, trial.health_damage);
            if (result.health_damage <= 0)
                break;
        }
    }
    // Proposal bound only: the final stage traces each pellet separately.
    // Do not let a one-pellet scan eliminate an otherwise viable shotgun shot.
    if (bullet.reserved > 1 && bullet.reserved <= 16)
        result.health_damage *= float(bullet.reserved);
    return result;
}
RageSpreadBasis spread_basis(const RageVector& axis) noexcept {
    const auto basis = rage_point_detail::basis_from_axis(axis);
    return {axis, basis.side, basis.upper};
}
RageHitchance normal_chance(const ScanContext& context, void* player, const Geometry& geometry,
                            const RageSpreadBasis& basis, float range, std::int32_t hitgroup,
                            float minimum) {
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(player) + 8, 10);
    (void)context.players.get(std::uint32_t(index));
    return evaluate_rage_hitchance(
        {context.origin, basis, range, geometry.primary, context.record_count, hitgroup, minimum},
        context.samples, context.box_clipper);
}
ProjectedPoint project(const ScanContext& context, const Geometry& geometry,
                       const RageVector& point, float range) {
    ProjectedPoint result;
    const auto delta = rage_point_detail::subtract(point, context.origin);
    result.direction = rage_point_detail::normalize(
        delta, std::sqrt((delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2]));
    if (precise_damage(context)) {
        const auto hit = intersect_damage_ray(context.origin, result.direction, range,
                                              geometry.primary, context.box_clipper);
        result.position = hit.position;
        result.hitbox = hit.hitbox;
        result.hitgroup = hit.hitgroup;
        result.found = hit.found;
        return result;
    }
    const auto scale = range / (rage_point_detail::length(result.direction) + 0x1p-23f);
    const RageVector extended{result.direction[0] * scale, result.direction[1] * scale,
                              result.direction[2] * scale};
    const auto intersection =
        intersect_scan_hitboxes({context.origin, extended}, geometry.primary, context.box_clipper);
    if (!intersection.found)
        return result;
    result.found = true;
    result.hitbox = std::int32_t(intersection.hitbox);
    result.hitgroup = geometry.primary[intersection.hitbox].hitgroup;
    for (std::size_t axis = 0; axis != 3; ++axis)
        result.position[axis] = extended[axis] * intersection.fraction + context.origin[axis];
    return result;
}
float minimum_multiplier(const ScanPolicy& policy, std::int32_t health, std::int32_t damage,
                         std::int32_t hitgroup, std::int32_t weapon_id) noexcept {
    if (weapon_id == 31)
        return 0.0f;
    const auto multiplier = rage_detail::hitgroup_multipliers[std::uint32_t(hitgroup) & 15U];
    const auto base = float(damage) / multiplier;
    const auto required =
        maximum_number(float(policy.minimum_damage),
                       float(health) / float(rage_detail::shots_to_kill(health, damage)));
    return base == 0.0f ? multiplier : required / base;
}
}
void scan_candidate(ScanContext& context, ScanRequest& request) {
    auto& evaluation = request.evaluation;
    auto& candidate = evaluation.candidate;
    const auto& policy = evaluation.policy;
    const auto local = scan_detail::model(request.target_player, context.model_offsets);
    scan_detail::Geometry geometry;
    geometry.primary = prepare_rage_scan_hitboxes(local.hitboxes, evaluation.record.matrices,
                                                  context.trigonometry);
    if (!geometry.primary.empty() && (evaluation.record.header.flags & 0x80U))
        for (std::size_t i = 0; i != 3; ++i)
            geometry.alternatives[i] = prepare_rage_scan_hitboxes(
                local.hitboxes, evaluation.record.alternatives[i].matrices, context.trigonometry);
    animation::BoneModel bone_model{};
    bone_model.studio_header = local.header != nullptr;
    bone_model.hitbox_bones = local.bones;
    const ScanHypothesisContext hypothesis_context{context.players, context.preparation,
                                                   context.hypothesis_offsets, bone_model,
                                                   context.update_attachments};
    ScanHypothesisMatrices additional;
    std::array<Matrix3x4, 256> attachment_scratch;
    const bool extra = build_optional_scan_hypotheses(
        hypothesis_context, request.target_player, candidate.entity_index, evaluation.record,
        evaluation.source_record, policy.additional_hypotheses, policy.hitboxes.safe, additional,
        attachment_scratch);
    // Valid scanner models provide the nineteen records referenced by the
    //native generation traversal, even when only a subset is enabled.
    const auto points = descriptors(context, policy, evaluation.record, local, geometry,
                                    extra ? &additional : nullptr);
    const auto target = request.target_entity;
    const RagePointRequirements requirements{candidate.health, policy.minimum_damage,
                                             policy.minimum_chance, policy.prefer_body};
    RagePointAdmissionState admission{{{}, 0},
                                      (policy.body_disabler_reasons & 6U) ? 0xffefffffU
                                                                          : 0xffffffffU,
                                      context.configuration_flags,
                                      policy.body_disabler_reasons};
    std::vector<TargetPoint> accepted;
    for (const auto& descriptor : points) {
        TargetPoint point{};
        point.hitbox = point.hitgroup = -1;
        auto bullet = prepare_target_bullet(context.weapon, context.weapon_item_offset,
                                            context.allow_penetration);
        const auto projected =
            scan_detail::project(context, geometry, descriptor.position, bullet.range);
        const auto flags = descriptor.flags | request.descriptor_flags;
        if (projected.found && projected.hitgroup == descriptor.hitgroup) {
            bool safe = true;
            if ((flags & 1U) && !scan_detail::precise_damage(context)) {
                const auto minimum =
                    safe_multiplier(context, policy, candidate.health, target, bullet, projected);
                RageVector direction;
                for (std::size_t axis = 0; axis != 3; ++axis)
                    direction[axis] = bullet.range * projected.direction[axis];
                safe = rage_point_safe({context.origin, direction}, projected.hitgroup, minimum,
                                       geometry.views(), context.box_clipper);
            }
            if (safe) {
                anti_aim::TargetTraceFilter native_filter(
                    context.trace_filter_vtable, context.trace_entities, 0x600400bU, 0x600400bU,
                    float(policy.minimum_damage) /
                        rage_detail::hitgroup_multipliers[std::uint32_t(projected.hitgroup) & 15U]);
                const auto filter = native_filter.binding();
                DamageRayResult exact;
                if (scan_detail::precise_damage(context)) {
                    exact = scan_detail::damage_ray(context, target, geometry, projected.direction,
                                                     bullet, (flags & 1U) != 0);
                    bullet.damage = exact.health_damage;
                    bullet.flags = exact.flags;
                } else {
                    trace_ballistic_bullet(context.ballistics, context.source, context.origin,
                                           projected.position, filter, bullet);
                }
                if (scan_detail::precise_damage(context)
                        ? std::isfinite(bullet.damage) && bullet.damage > 0
                        : _mm_cvttss_si32(_mm_set_ss(bullet.damage)) > 0) {
                    const auto info = context.ballistics.entity_info(target);
                    point.position = descriptor.position;
                    point.damage = scan_detail::precise_damage(context)
                                       ? damage_lower_bound(bullet.damage)
                                       : final_bullet_damage(bullet.damage, bullet.armor_ratio,
                                                       std::bit_cast<std::int32_t>(bullet.reserved),
                                                       bullet.weapon_id, projected.hitgroup,
                                                       info.armor, info.helmet);
                    point.hitbox = projected.hitbox;
                    point.hitgroup = projected.hitgroup;
                    point.flags = flags;
                    point.chance = 1.0f;
                    if (bullet.flags & 2U) {
                        const auto index = candidate.entity_index & 63U;
                        context.target_mask[index >> 5] |= 1U << (index & 31U);
                    }
                    ++evaluation.ballistic_evaluations;
                    if ((context.configuration_flags & 0x10U) &&
                        point.damage >= policy.minimum_damage) {
                        const auto minimum =
                            scan_detail::minimum_multiplier(policy, candidate.health, point.damage,
                                                            point.hitgroup, context.weapon_id);
                        const auto basis = scan_detail::spread_basis(projected.direction);
                        const auto chance =
                            (flags & 1U)
                                ? evaluate_scan_safe_hitchance({context.origin, basis, bullet.range,
                                                                point.hitgroup, minimum,
                                                                geometry.views()},
                                                               context.samples, context.box_clipper)
                                : scan_detail::normal_chance(context, request.target_player,
                                                             geometry, basis, bullet.range,
                                                             point.hitgroup, minimum);
                        //433604A7 writes evaluation+38. The temporary point's
                        //chance remains1 until selection/refinement copies it.
                        candidate.primary.chance = chance.fraction;
                    }
                    point.priority = rage_point_priority(
                        {point.damage, point.hitgroup, point.flags, point.chance, 0}, requirements);
                }
            }
        }
        RagePointScore score{point.damage, point.hitgroup, point.flags, point.chance,
                             point.priority};
        admission.evaluation_flags = context.configuration_flags;
        if (admit_rage_point(score, requirements, admission, descriptor.position,
                             descriptor.flags)) {
            point.reference_position = admission.reference.position;
            point.reference_damage = admission.reference.damage;
            point.descriptor_weight = descriptor.scale;
            accepted.push_back(point);
        }
        context.configuration_flags = admission.evaluation_flags;
    }
    candidate.prefer_body = policy.prefer_body;
    candidate.prefer_safe = policy.prefer_safe;
    candidate.target_state_flags = evaluation.record.header.flags;
    candidate.weapon_priority_enabled = (evaluation.record.header.player_flags & 1U) != 0;
    select_target_points(candidate, accepted, admission.priority_mask);
    if (candidate.primary.damage && (candidate.primary.flags & 2U)) {
        scan_detail::refine(context, policy, candidate.health, request.target_player, target,
                            geometry, candidate.primary);
        if (candidate.flags & 8U) {
            if (candidate.alternate_first.flags & 2U)
                scan_detail::refine(context, policy, candidate.health, request.target_player,
                                    target, geometry, candidate.alternate_first);
            if ((candidate.flags & 0x10U) && (candidate.alternate_second.flags & 2U))
                scan_detail::refine(context, policy, candidate.health, request.target_player,
                                    target, geometry, candidate.alternate_second);
        }
    }
    if (!candidate.primary.damage)
        return;
    if (scan_detail::teammate_blocks(context, candidate.primary.position)) {
        reset_scan_point(candidate.primary);
        return;
    }
    TargetBatch batch;
    batch.candidates[0] = &candidate;
    batch.count = 1;
    recalculate_target_priorities(batch, context.targeting);
    std::atomic_ref(request.completed[request.ordinal >> 5])
        .fetch_or(1U << (request.ordinal & 31U), std::memory_order_seq_cst);
}
}
