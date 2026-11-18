#include "skeet/rage/damage_model.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace skeet::rage {
namespace {
template<class T> T read(const void* object, std::ptrdiff_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
float convar_float(const void* object) {
    for (unsigned depth = 0; object && depth != 16; ++depth) {
        const auto* parent = read<const void*>(object, 0x1C);
        if (parent == object)
            return std::bit_cast<float>(read<std::uint32_t>(object, 0x2C) ^
                                        std::uint32_t(reinterpret_cast<std::uintptr_t>(object)));
        object = parent;
    }
    return std::numeric_limits<float>::quiet_NaN();
}
}
float bullet_health_damage(float raw, std::int32_t group, const BulletDamageModel& model) noexcept {
    if (!model.valid || group < 0 || group > 8 || !std::isfinite(raw) || raw < 0 ||
        !std::isfinite(model.armor_ratio) || model.armor_ratio < 0 || model.armor_ratio > 2 ||
        !std::isfinite(model.head_multiplier) || model.head_multiplier < 0 ||
        !std::isfinite(model.head_scale) || model.head_scale < 0 ||
        !std::isfinite(model.body_scale) || model.body_scale < 0)
        return 0;
    if (model.weapon_id == 31)
        return raw;
    // Installed server.dll: TraceAttack at RVA 46F673 uses weapon data +F4,
    // team scales and the heavy-armor head factor before OnTakeDamage's armor.
    float damage = raw;
    if (group == 1)
        damage = (raw * model.head_multiplier) *
                 (model.head_scale * (model.heavy_armor ? 0.5f : 1.0f));
    else if (group >= 2 && group <= 8)
        damage = (raw * (group == 3 ? 1.25f : group == 6 || group == 7 ? 0.75f : 1.0f)) *
                 model.body_scale;
    const bool armored = model.armor > 0 &&
        (model.heavy_armor || group == 0 || (group >= 2 && group <= 5) ||
         ((group == 1 || group == 8) && model.helmet));
    if (armored) {
        // Server RVA 46D52E: armor absorption is capped by remaining armor.
        // Heavy armor's cooperative bonus is conditional (game_type == 4).
        const float ratio = (model.armor_ratio * 0.5f) * (model.heavy_armor ? 0.2f : 1.0f);
        const bool cooperative_armor = model.heavy_armor && model.cooperative;
        const float bonus = cooperative_armor ? 0.33f : 0.5f;
        const float scale = cooperative_armor ? 0.25f : 1.0f;
        const float health = damage * ratio;
        const float absorbed = (damage - health) * (scale * bonus);
        damage = absorbed > float(model.armor) ? damage - float(model.armor) / bonus : health;
    }
    return std::isfinite(damage) && damage >= 0 ? damage : 0;
}
std::int32_t damage_lower_bound(float damage) noexcept {
    // Never round a value just below the requested damage upward.
    if (!std::isfinite(damage) || damage <= 0 || double(damage) >= INT32_MAX)
        return 0;
    return std::int32_t(damage);
}
ExperimentalDamageContext DamageRulesCache::capture(bool enabled, void* cvar,
                                                      const void* weapon_data,
                                                      std::ptrdiff_t heavy_offset, int game_type) {
    ExperimentalDamageContext result;
    result.enabled = enabled;
    if (!enabled)
        return result;
    constexpr const char* names[]{"mp_damage_scale_ct_head", "mp_damage_scale_ct_body",
                                  "mp_damage_scale_t_head", "mp_damage_scale_t_body"};
    if (owner_ != cvar) {
        owner_ = cvar;
        variables_ = {};
        warned_ = false;
    }
    result.valid = cvar && weapon_data && heavy_offset > 0;
    for (unsigned i = 0; i < variables_.size(); ++i) {
        if (cvar && !variables_[i])
            variables_[i] = engine::invoke<void*>(cvar, 15, names[i]);
        result.scales[i] = convar_float(variables_[i]);
        result.valid &= std::isfinite(result.scales[i]) && result.scales[i] >= 0;
    }
    result.heavy_armor_offset = heavy_offset;
    result.head_multiplier = weapon_data ? read<float>(weapon_data, 0xF4) : NAN;
    result.cooperative = game_type == 4;
    result.valid &= std::isfinite(result.head_multiplier) && result.head_multiplier >= 0;
    if (!result.valid && cvar && !warned_) {
        // Variadic ICvar methods use cdecl, including the explicit receiver.
        using Print = void(__cdecl*)(void*, const char*, ...);
        const auto table = *static_cast<void***>(cvar);
        reinterpret_cast<Print>(table[26])(
            cvar, "%s", "[gamesense] Experimental RAGE: damage inputs unavailable; shot rejected.\n");
        warned_ = true;
    }
    return result;
}
BulletDamageModel capture_damage_model(BallisticEngine& engine, BallisticEntity target,
                                        const ExperimentalDamageContext& context,
                                        const BallisticQuery& bullet) {
    const auto info = engine.entity_info(target);
    const auto index = info.team == 3 ? 0u : 2u;
    const bool valid = context.enabled && context.valid && target &&
                       (info.team == 2 || info.team == 3) &&
                       std::isfinite(bullet.base_damage) && bullet.base_damage > 0 &&
                       std::isfinite(bullet.range) && bullet.range > 0 &&
                       std::isfinite(bullet.range_modifier) && bullet.range_modifier > 0 &&
                       std::isfinite(bullet.penetration) && bullet.penetration >= 0;
    const bool heavy = valid && read<std::uint8_t>(reinterpret_cast<const void*>(
        std::uintptr_t(target)), context.heavy_armor_offset) != 0;
    return {bullet.armor_ratio, context.head_multiplier, context.scales[index],
            context.scales[index + 1], info.armor, bullet.weapon_id, info.helmet != 0,
            heavy, context.cooperative, valid};
}
DamageIntersection intersect_damage_ray(const RageVector& origin, const RageVector& direction,
                                         float range, std::span<const RageScanHitbox> hitboxes,
                                         const RageBoxClipper& clipper) {
    DamageIntersection result;
    if (!std::isfinite(range) || range <= 0)
        return result;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(origin[i]) || !std::isfinite(direction[i]))
            return result;
    RageRay ray{origin, {direction[0] * range, direction[1] * range, direction[2] * range}};
    float nearest = 1;
    for (std::size_t i = 0; i < hitboxes.size(); ++i) {
        const auto& box = hitboxes[i];
        RageTrace trace; trace.fraction = 1;
        const int hit = box.capsule.radius > 0
                            ? clip_rage_ray_to_capsule(ray, box.capsule, trace)
                            : clipper.clip ? clipper.clip(clipper.context, ray, box, trace) : -1;
        if (hit < 0 || !std::isfinite(trace.fraction) || trace.fraction < 0 ||
            trace.fraction >= nearest)
            continue;
        nearest = trace.fraction;
        result.hitbox = int(i); result.hitgroup = box.hitgroup; result.found = true;
        for (unsigned axis = 0; axis < 3; ++axis)
            result.position[axis] = origin[axis] + ray.delta[axis] * nearest;
    }
    return result;
}
DamageRayResult trace_damage_ray(BallisticEngine& engine, BallisticEntity source,
                                  const RageVector& origin, const RageVector& direction,
                                  std::span<const RageScanHitbox> geometry,
                                  const RageBoxClipper& clipper, const BallisticFilter& filter,
                                  BallisticQuery bullet, const BulletDamageModel& model) {
    DamageRayResult result;
    if (!model.valid)
        return result;
    result.hit = intersect_damage_ray(origin, direction, bullet.range, geometry, clipper);
    if (!result.hit.found)
        return result;
    trace_ballistic_bullet(engine, source, origin, result.hit.position, filter, bullet);
    result.health_damage = bullet_health_damage(bullet.damage, result.hit.hitgroup, model);
    result.flags = bullet.flags;
    return result;
}
}
