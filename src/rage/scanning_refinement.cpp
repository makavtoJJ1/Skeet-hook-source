#include "scanning_detail.hpp"
#include "skeet/anti_aim/targets.hpp"
#include "../rage_point_math.hpp"
#include "../rage_math.hpp"
#include <immintrin.h>

namespace skeet::rage::scan_detail {
void refine(const ScanContext& context, const ScanPolicy& policy, std::int32_t health, void* player,
            BallisticEntity target, const Geometry& geometry, TargetPoint& point) {
    const auto original = point.position;
    const auto reference = point.reference_position;
    const auto damage = point.damage;
    const auto shots = rage_detail::shots_to_kill(health, damage);
    if (precise_damage(context)) {
        const auto bullet = prepare_target_bullet(context.weapon, context.weapon_item_offset,
                                                   context.allow_penetration);
        // Damage is discontinuous at cover and hitgroup boundaries. Evaluate
        // bounded candidates instead of interpolating damage or assuming monotonicity.
        for (unsigned step = 0; step < 8; ++step) {
            const float fraction = float(step) / 8.0f;
            RageVector trial;
            for (unsigned axis = 0; axis < 3; ++axis)
                trial[axis] = reference[axis] + (original[axis] - reference[axis]) * fraction;
            const auto projection = project(context, geometry, trial, bullet.range);
            if (!projection.found || projection.hitgroup != point.hitgroup)
                continue;
            const auto evaluated = damage_ray(context, target, geometry, projection.direction,
                                               bullet, (point.flags & 1U) != 0);
            const auto trial_damage = damage_lower_bound(evaluated.health_damage);
            if (trial_damage <= 0 || trial_damage < policy.minimum_damage ||
                rage_detail::shots_to_kill(health, trial_damage) > shots)
                continue;
            point.position = trial;
            point.damage = trial_damage;
            point.hitbox = evaluated.hit.hitbox;
            point.hitgroup = evaluated.hit.hitgroup;
            point.descriptor_weight *= fraction;
            break;
        }
        // The final stage counts the complete seed domain with actual damage.
        // A 32-ray geometric estimate must not veto that damage-aware decision.
        point.chance = 1.0f;
        return;
    }
    float fraction = 0.5f;
    auto reference_damage = point.reference_damage;
    bool interpolate = rage_detail::shots_to_kill(health, reference_damage) == shots;
    if (!interpolate) {
        float lower = 0.0f, upper = 1.0f;
        reference_damage = 0;
        auto bullet = prepare_target_bullet(context.weapon, context.weapon_item_offset,
                                            context.allow_penetration);
        const auto difference = rage_point_detail::subtract(original, reference);
        for (unsigned iteration = 0; iteration != 4; ++iteration) {
            const auto midpoint = (upper + lower) * 0.5f;
            const RageVector candidate{difference[0] * midpoint + reference[0],
                                       difference[1] * midpoint + reference[1],
                                       difference[2] * midpoint + reference[2]};
            const auto hit = project(context, geometry, candidate, bullet.range);
            bool keep = false;
            if (hit.found &&
                rage_detail::hitgroup_multipliers[std::uint32_t(hit.hitgroup) & 15U] >=
                    rage_detail::hitgroup_multipliers[std::uint32_t(point.hitgroup) & 15U]) {
                anti_aim::TargetTraceFilter native_filter(context.trace_filter_vtable,
                                                          context.trace_entities, 0x600400bU,
                                                          0x600400bU, 1.0f);
                const auto filter = native_filter.binding();
                trace_ballistic_bullet(context.ballistics, context.source, context.origin,
                                       hit.position, filter, bullet);
                if (_mm_cvttss_si32(_mm_set_ss(bullet.damage)) != 0) {
                    const auto info = context.ballistics.entity_info(target);
                    const auto trial = final_bullet_damage(
                        bullet.damage, bullet.armor_ratio,
                        std::bit_cast<std::int32_t>(bullet.reserved), bullet.weapon_id,
                        hit.hitgroup, info.armor, info.helmet);
                    if (trial != 0 && trial >= policy.minimum_damage &&
                        rage_detail::shots_to_kill(health, trial) <= shots) {
                        reference_damage = trial;
                        keep = true;
                    }
                }
            }
            if (keep)
                upper = midpoint;
            else
                lower = midpoint;
        }
        if (upper != 1.0f) {
            fraction = (upper + 1.0f) * 0.5f;
            interpolate = true;
        }
    }
    if (interpolate) {
        for (std::size_t axis = 0; axis != 3; ++axis)
            point.position[axis] = (original[axis] - reference[axis]) * fraction + reference[axis];
        point.descriptor_weight *= fraction;
        point.damage = _mm_cvttss_si32(_mm_set_ss(
            (float(damage) - float(reference_damage)) * fraction + float(reference_damage)));
    }
    const auto minimum =
        minimum_multiplier(policy, health, point.damage, point.hitgroup, context.weapon_id);
    const auto bullet = prepare_target_bullet(context.weapon, context.weapon_item_offset,
                                              context.allow_penetration);
    const auto delta = rage_point_detail::subtract(point.position, context.origin);
    const auto axis = rage_point_detail::normalize(
        delta, std::sqrt((delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2]));
    const auto basis = spread_basis(axis);
    const auto chance =
        (point.flags & 1U)
            ? evaluate_scan_safe_hitchance(
                  {context.origin, basis, bullet.range, point.hitgroup, minimum, geometry.views()},
                  context.samples, context.box_clipper)
            : normal_chance(context, player, geometry, basis, bullet.range, point.hitgroup,
                            minimum);
    point.chance = chance.fraction;
}
}
