#include "skeet/rage/targeting_commit.hpp"
#include "skeet/menu/misc_menu.hpp"
#include "skeet/engine/interface_call.hpp"
#include <cmath>
#include <cstring>
#include <xmmintrin.h>
#include <atomic>
#include <bit>
#include "skeet/rage_segments.hpp"
#include "../rage_math.hpp"
#include "skeet/engine/jobs.hpp"
#include <new>
#include "../rage_point_math.hpp"
#include "final_target_context_detail.hpp"
#include "skeet/rage/preamble.hpp"
#include "skeet/rage/damage_model.hpp"

namespace skeet::rage {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) {
    T result;
    std::memcpy(&result, static_cast<const std::byte*>(object) + offset, sizeof result);
    return result;
}
RageVector subtract(const RageVector& a, const RageVector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
RageVector advance(const RageVector& a, const RageVector& d, float scale) {
    return {d[0] * scale + a[0], d[1] * scale + a[1], d[2] * scale + a[2]};
}
float length(const RageVector& v) {
    return std::sqrt((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
}
BallisticRay ray(const RageVector& a, const RageVector& b) {
    const auto delta = subtract(b, a);
    return {a, delta, (delta[1] * delta[1] + delta[0] * delta[0]) + delta[2] * delta[2] != 0.0f};
}
std::uint32_t convar(const void* node, std::ptrdiff_t offset) noexcept {
    for (;;) {
        const auto parent = read<const void*>(node, 0x1c);
        if (parent == node)
            break;
        node = parent;
    }
    return read<std::uint32_t>(node, offset) ^
           std::uint32_t(reinterpret_cast<std::uintptr_t>(node));
}
}

bool final_teammate_precheck_required(std::uint32_t flags,
                                      const FinalTeammateConvars& variables) noexcept {
    if (flags & 8U)
        return false;
    const bool forced = convar(variables.mode_43467c3c, 0x30) != 0 &&
                        std::bit_cast<float>(convar(variables.coefficient_43467084, 0x2c)) > 0.0f;
    if (!forced && std::bit_cast<float>(convar(variables.coefficient_43468c90, 0x2c)) > 0.0f)
        return false;
    return convar(variables.teammates_are_enemies, 0x30) == 0;
}

BallisticQuery prepare_target_bullet(void* weapon, std::ptrdiff_t item_offset,
                                     std::uint8_t allow_penetration) {
    BallisticQuery query;
    query.allow_penetration = allow_penetration;
    reset_ballistic_query(query);
    const auto data = engine::invoke<const void*>(weapon, 461);
    query.weapon_id = read<std::uint16_t>(weapon, item_offset);
    query.penetration = read<float>(data, 0x100);
    query.range = read<float>(data, 0x10c);
    query.range_modifier = read<float>(data, 0x110);
    query.armor_ratio = engine::invoke<float>(weapon, 434);
    query.base_damage = float(engine::invoke<std::int32_t>(weapon, 431));
    query.reserved = read<std::uint32_t>(data, 0xfc);
    return query;
}

std::int32_t trace_target_direction(BallisticEngine& engine, BallisticEntity source,
                                    const RageVector& origin, const RageVector& direction,
                                    const BallisticFilter& filter, BallisticQuery& query) {
    reset_ballistic_query(query);
    engine.bind_source(filter, source);
    engine.bind_last_hit(filter, 0);
    float damage = query.base_damage;
    const auto minimum = maximum_number(1.0f, filter.minimum_damage - 5.0f);
    if (!(damage > minimum))
        return 0;
    auto position = origin;
    float travelled = 0.0f;
    for (;;) {
        const auto remaining = query.range - travelled;
        const auto end = advance(position, direction, remaining);
        engine.trace_ray(
            {ray(position, end), filter.mask, BallisticFilterKind::supplied, &filter, source, 0},
            query.trace);
        if (filter.mask & 0x40000000U) {
            const auto extended = advance(end, direction, 40.0f);
            const auto fraction =
                length(subtract(query.trace.end, position)) / length(subtract(position, extended));
            clip_ballistic_entities(engine, position, extended, 0.0f, fraction, filter,
                                    query.trace);
        }
        if (query.trace.fraction == 1.0f) {
            query.damage = 0;
            return 0;
        }
        query.points[query.point_count++] = query.trace.end;
        const auto surface = engine.surface(query.trace.surface_property);
        BallisticEntity hit = 0;
        if (query.trace.entity && engine.entity_info(query.trace.entity).virtual_278)
            hit = query.trace.entity;
        engine.bind_last_hit(filter, hit);
        travelled = remaining * query.trace.fraction + travelled;
        damage = float(std::pow(double(query.range_modifier), double(travelled * 0.002f)) *
                       double(damage));
        if (minimum > damage) {
            query.damage = 0;
            return 0;
        }
        if ((travelled > 3000.0f && query.penetration > 0.0f) ||
            double(surface.penetration_modifier) < 0.1)
            query.remaining_penetrations = 0;
        if (!(filter.mask & 0x40000000U))
            query.trace.hitgroup = 1;
        if (query.trace.entity && engine.entity_info(query.trace.entity).virtual_280) {
            if (!hit) {
                query.damage = 0;
                return 0;
            }
            const auto info = engine.entity_info(hit);
            if (std::uint32_t(query.trace.hitgroup) - 1U < 8U && !info.target_state) {
                if (info.target_index_blocked ||
                    (source && !ballistic_enemy(engine, source, hit))) {
                    query.damage = 0;
                    return 0;
                }
                if (filter.mask & 0x40000000U)
                    damage = hitgroup_damage_scale(info.armor, info.helmet, query.trace.hitgroup,
                                                   query.weapon_id, query.armor_ratio) *
                             damage;
                query.damage = damage;
                return _mm_cvttss_si32(_mm_set_ss(damage));
            }
        }
        if (query.remaining_penetrations < 1) {
            query.damage = 0;
            return 0;
        }
        if (handle_ballistic_penetration(engine, direction, position, source, filter, query,
                                         surface, damage)) {
            query.flags |= 4U;
            query.damage = 0;
            return 0;
        }
        if (!(damage > minimum)) {
            query.damage = 0;
            return 0;
        }
    }
}

FinalHitchancePlan
plan_final_hitchance(bool safe, float minimum_chance, std::uint32_t context_flags,
                     std::uint32_t entity_index,
                     const std::array<std::uint32_t, 2>& previous_targets) noexcept {
    const auto index = entity_index & 63U;
    const auto samples =
        (context_flags & 8U) || (previous_targets[index >> 5] & (1U << (index & 31U))) ? 32U : 64U;
    const auto total = std::int32_t(samples * (safe ? 3U : 1U));
    const auto required = _mm_cvtss_si32(_mm_set_ss(float(total) * minimum_chance));
    return {samples, 128U / samples,
            rage_detail::signed_word(std::uint32_t(total) - std::uint32_t(required) + 2U)};
}

RageSpreadBasis final_target_spread_basis(const RageVector& point,
                                          const RageVector& origin) noexcept {
    const auto delta = subtract(point, origin);
    const auto center = rage_point_detail::normalize(
        delta, std::sqrt((delta[2] * delta[2] + delta[0] * delta[0]) + delta[1] * delta[1]));
    const auto transverse = rage_point_detail::basis_from_axis(center);
    return {center, transverse.side, transverse.upper};
}

FinalHitchancePlan plan_experimental_hitchance(float minimum_chance) noexcept {
    if (!std::isfinite(minimum_chance) || minimum_chance < 0 || minimum_chance > 1)
        return {0, 1, 0};
    // Count complete seed outcomes, not averaged pose votes. The required count
    // must round upward; early rejection is valid only when it is unreachable.
    constexpr unsigned samples = CompleteSpread::seed_count;
    const auto required = std::max(1, std::int32_t(std::ceil(double(minimum_chance) * samples)));
    return {samples, 1, std::int32_t(samples) - required + 1};
}

namespace {
bool identical_seed_rays(const CompleteSpread* spread) {
    if (!spread || !spread->valid || !spread->pellets ||
        spread->pellets > CompleteSpread::maximum_pellets)
        return false;
    for (unsigned seed = 1; seed < CompleteSpread::seed_count; ++seed)
        for (unsigned pellet = 0; pellet < spread->pellets; ++pellet)
            if (spread->rays[seed][pellet].first != spread->rays[0][pellet].first ||
                spread->rays[seed][pellet].second != spread->rays[0][pellet].second)
                return false;
    return true;
}
bool complete_seed_hit(BallisticEngine& engine, const FinalHitchanceSample& input,
                       const BallisticQuery& base, unsigned seed) {
    if (!input.complete_spread || !input.complete_spread->valid ||
        seed >= CompleteSpread::seed_count || (input.damage_model && !input.damage_model->valid))
        return false;
    auto damage_for_pose = [&](std::span<const RageScanHitbox> geometry) {
        float total = 0;
        for (unsigned pellet = 0; pellet < input.complete_spread->pellets; ++pellet) {
            const auto& spread = input.complete_spread->rays[seed][pellet];
            RageVector direction;
            for (unsigned axis = 0; axis < 3; ++axis)
                direction[axis] = (spread.first * input.basis.first[axis] + input.basis.center[axis]) +
                                  spread.second * input.basis.second[axis];
            const auto magnitude = length(direction);
            if (!std::isfinite(magnitude) || magnitude <= 0)
                return 0.0f;
            for (auto& value : direction)
                value /= magnitude;
            const auto hit = intersect_damage_ray(input.origin, direction, base.range,
                                                   geometry, input.box_clipper);
            if (!hit.found)
                continue;
            auto query = base;
            if (input.live_bindings) {
                final_target_detail::trace_projected(*input.live_bindings, engine, input,
                                                      hit.position, query);
            } else {
                auto filter = input.projected_filter;
                filter.mask = filter.exit_mask = 0x600400bU;
                filter.minimum_damage = 1;
                trace_ballistic_bullet(engine, input.source, input.origin, hit.position, filter, query);
            }
            // Retaining the starting armor for each pellet is a lower bound if
            // earlier pellets exhaust it. Never invent damage from an averaged ray.
            if (input.damage_model) {
                total += bullet_health_damage(query.damage, hit.hitgroup, *input.damage_model);
            } else {
                const auto& target = input.legacy_target;
                const auto scale = hitgroup_damage_scale(target.armor, target.helmet, hit.hitgroup,
                                                         base.weapon_id, base.armor_ratio);
                total += float(damage_lower_bound(query.damage * scale));
            }
        }
        return total;
    };
    const auto qualifies = [&](std::span<const RageScanHitbox> geometry) {
        const auto damage = damage_lower_bound(damage_for_pose(geometry));
        return damage > 0 && damage >= input.minimum_damage;
    };
    if (!qualifies(input.primary))
        return false;
    if (input.safe)
        for (const auto& hypothesis : input.hypotheses)
            if (!qualifies(hypothesis))
                return false;
    return true;
}
unsigned seed_for_sample(const FinalHitchancePlan& plan, unsigned ordinal) {
    return plan.samples == CompleteSpread::seed_count
               ? ordinal : cached_spread_seed(ordinal * plan.sample_stride);
}
}
void evaluate_final_hitchance_sample(BallisticEngine& engine, const FinalHitchanceSample& input,
                                     RageSpreadSample sample, std::int32_t failure_budget,
                                     FinalHitchanceVotes& votes, unsigned seed_ordinal) {
    if (votes.stopped.load(std::memory_order_relaxed))
        return;
    auto query = input.captured_bullet ? *input.captured_bullet
                                      : prepare_target_bullet(input.weapon, input.weapon_item_offset,
                                                              input.allow_penetration);
    if (input.complete_spread) {
        const auto hit = complete_seed_hit(engine, input, query, seed_ordinal);
        const auto packed = votes.packed.fetch_add((1u << 16) | unsigned(hit),
                                                   std::memory_order_relaxed) + (1u << 16) + unsigned(hit);
        const auto failures = (packed >> 16) - (packed & 0xffffU);
        if (failures >= unsigned(failure_budget))
            votes.stopped.store(1, std::memory_order_relaxed);
        return;
    }
    RageVector direction;
    for (std::size_t i = 0; i != 3; ++i)
        direction[i] = (sample.first * input.basis.first[i] + input.basis.center[i]) +
                       sample.second * input.basis.second[i];
    const auto inverse = 1.0f / (length(direction) + 0x1p-23f);
    for (auto& value : direction)
        value *= inverse;
    const auto delta = subtract(input.point, input.origin);
    const auto projection =
        (delta[1] * direction[1] + delta[0] * direction[0]) + delta[2] * direction[2];
    float distance;
    if (projection < 0.0f || projection > query.range + 40.0f)
        distance = 3.4028234663852886e38f;
    else
        distance = length(subtract(input.point, advance(input.origin, direction, projection)));
    std::uint32_t successes = 0;
    if (60.0f > distance) {
        if (!input.safe) {
            auto filter = input.direction_filter;
            filter.preferred_entity = input.target;
            filter.mask = filter.exit_mask = 0x4600400bU;
            filter.minimum_damage = 1.0f;
            const auto damage = input.live_bindings
                                    ? final_target_detail::trace_direction(
                                          *input.live_bindings, engine, input, direction, query)
                                    : trace_target_direction(engine, input.source, input.origin,
                                                             direction, filter, query);
            const auto total = std::uint32_t(damage) * query.reserved;
            if (damage > 0 && std::bit_cast<std::int32_t>(total) >= input.minimum_damage)
                successes = 1;
        } else {
            const RageRay ray{input.origin,
                              {direction[0] * query.range, direction[1] * query.range,
                               direction[2] * query.range}};
            for (const auto& hitboxes : input.hypotheses) {
                bool hit = false;
                if (input.hitgroup == 1) {
                    RageTrace trace;
                    if (input.box_clipper.clip(input.box_clipper.context, ray, hitboxes.front(),
                                               trace) >= 0) {
                        hit = true;
                        const auto head_fraction = trace.fraction;
                        for (const auto& hitbox : hitboxes) {
                            if (!(hitbox.capsule.radius > 0.0f) ||
                                (hitbox.hitgroup != 2 && hitbox.hitgroup != 3))
                                continue;
                            trace.fraction = 1.0f;
                            if (input.box_clipper.clip(input.box_clipper.context, ray, hitbox,
                                                       trace) >= 0 &&
                                trace.fraction < head_fraction) {
                                hit = false;
                                break;
                            }
                        }
                    }
                } else {
                    const RageSegment shot{input.origin,
                                           advance(input.origin, direction, query.range)};
                    for (const auto& hitbox : hitboxes) {
                        if (!(hitbox.capsule.radius > 0.0f) ||
                            !(input.minimum_hitgroup_multiplier <=
                              rage_detail::hitgroup_multipliers[std::uint32_t(hitbox.hitgroup) &
                                                                15U]))
                            continue;
                        if (hitbox.capsule.radius >=
                            rage_segment_distance({hitbox.capsule.first, hitbox.capsule.second},
                                                  shot)) {
                            hit = true;
                            break;
                        }
                    }
                }
                if (hit)
                    ++successes;
                else if (input.robust_safe) {
                    successes = 0;
                    break;
                }
            }
            if (input.robust_safe)
                successes = successes == 3 ? 1u : 0u;
            if (successes) {
                auto filter = input.projected_filter;
                filter.mask = filter.exit_mask = 0x600400bU;
                filter.minimum_damage = 1.0f;
                const auto projected =
                    (input.point_delta[1] * direction[1] + input.point_delta[0] * direction[0]) +
                    input.point_delta[2] * direction[2];
                const auto point = advance(input.origin, direction, projected);
                const auto damage = input.live_bindings
                                        ? final_target_detail::trace_projected(
                                              *input.live_bindings, engine, input, point, query)
                                        : trace_ballistic_bullet(engine, input.source, input.origin,
                                                                 point, filter, query);
                const auto scaled = _mm_cvttss_si32(_mm_set_ss(input.base_damage * query.damage));
                if (damage <= 0 || scaled < input.minimum_damage)
                    successes = 0;
            }
        }
    }
    const auto increment = successes | ((input.safe && !input.robust_safe ? 3U : 1U) << 16);
    const auto packed = votes.packed.fetch_add(increment, std::memory_order_relaxed) + increment;
    const auto failures = std::bit_cast<std::int32_t>(packed) >> 16;
    if (failures - std::int32_t(packed & 0xffffU) > failure_budget)
        votes.stopped.exchange(1, std::memory_order_relaxed);
}

RageHitchance final_hitchance_fraction(const FinalHitchanceVotes& votes,
                                       std::int32_t failure_budget) noexcept {
    const auto packed = votes.packed.load(std::memory_order_relaxed);
    auto hits = packed & 0xffffU;
    auto count = std::bit_cast<std::int32_t>(packed) >> 16;
    if (count - std::int32_t(hits) >= failure_budget ||
        votes.stopped.load(std::memory_order_relaxed)) {
        hits = 0;
        count = 0;
    }
    return {hits, count ? float(hits) / float(count) : 0.0f};
}

RageHitchance evaluate_final_hitchance_inline(BallisticEngine& engine,
                                              const FinalHitchanceSample& input,
                                              std::span<const RageSpreadSample, 128> samples,
                                              const FinalHitchancePlan& plan) {
    FinalHitchanceVotes votes;
    if (plan.samples != CompleteSpread::seed_count && plan.samples * plan.sample_stride > samples.size())
        return {};
    if (!input.complete_spread && plan.samples == CompleteSpread::seed_count)
        return {};
    if (plan.samples == CompleteSpread::seed_count && identical_seed_rays(input.complete_spread)) {
        // Exact reduction for identical outcomes (including no-spread servers).
        // It preserves the full probability without repeating identical traces.
        evaluate_final_hitchance_sample(engine, input, {}, plan.failure_budget, votes, 0);
        const bool hit = (votes.packed.load(std::memory_order_relaxed) & 0xffffU) != 0;
        return {hit ? CompleteSpread::seed_count : 0u, hit ? 1.0f : 0.0f};
    }
    for (std::size_t i = 0; i != plan.samples; ++i)
        evaluate_final_hitchance_sample(engine, input,
                                        input.complete_spread ? RageSpreadSample{} : samples[i * plan.sample_stride],
                                        plan.failure_budget, votes,
                                        input.complete_spread ? seed_for_sample(plan, unsigned(i)) : 0);
    return final_hitchance_fraction(votes, plan.failure_budget);
}

namespace {
struct FinalWork {
    BallisticEngine* engine;
    const FinalHitchanceSample* input;
    RageSpreadSample sample;
    std::int32_t budget;
    FinalHitchanceVotes* votes;
    unsigned ordinal;
};
struct FinalJobItem {
    FinalWork* work;
#if UINTPTR_MAX == UINT32_MAX
    std::uint32_t padding;
#endif
};
static_assert(sizeof(FinalJobItem) == 8);
#if defined(_M_IX86) || defined(__i386__)
void __fastcall final_sample_job(void* raw) {
#else
void final_sample_job(void* raw) {
#endif
    const auto& work = *static_cast<FinalJobItem*>(raw)->work;
    evaluate_final_hitchance_sample(*work.engine, *work.input, work.sample, work.budget,
                                    *work.votes, work.ordinal);
}
}

RageHitchance evaluate_final_hitchance_parallel(BallisticEngine& engine,
                                                const FinalHitchanceSample& input,
                                                std::span<const RageSpreadSample, 128> samples,
                                                const FinalHitchancePlan& plan,
                                                engine::JobDispatcher& jobs) {
    if (identical_seed_rays(input.complete_spread))
        return evaluate_final_hitchance_inline(engine, input, samples, plan);
    FinalHitchanceVotes votes;
    if ((plan.samples != CompleteSpread::seed_count && plan.samples * plan.sample_stride > samples.size()) ||
        (plan.samples == CompleteSpread::seed_count && !input.complete_spread))
        return {};
    std::array<FinalWork, CompleteSpread::seed_count> work;
    std::array<FinalJobItem, CompleteSpread::seed_count> items;
    for (std::size_t i = 0; i != plan.samples; ++i) {
        work[i] = {&engine, &input,
                   input.complete_spread ? RageSpreadSample{} : samples[i * plan.sample_stride],
                   plan.failure_budget, &votes,
                   input.complete_spread ? seed_for_sample(plan, unsigned(i)) : 0};
        items[i].work = &work[i];
    }
    auto task = jobs.create_eight(final_sample_job);
    if (!task)
        throw std::bad_alloc();
    jobs.dispatch_eight(*task, items.data(), plan.samples);
    engine::release_job(*task);
    return final_hitchance_fraction(votes, plan.failure_budget);
}

bool final_target_detail::requests_body_aim(const FinalHitchanceFailure& state) {
    return requests_body_aim(state, nullptr);
}
bool final_target_detail::requests_body_aim(const FinalHitchanceFailure& state,
                                            const client::CommandFrame* frame) {
    const auto flags = frame ? frame->flags : state.weapon_flags;
    bool request_body = (flags & 0x40000U) && state.hitgroup == 1 && (flags & 0x400U);
    const auto weapon =
        frame ? reinterpret_cast<void*>(std::uintptr_t(frame->weapon_address)) : state.weapon;
    if (request_body && weapon) {
        const auto maximum = engine::invoke<float>(weapon, 442);
        if (maximum != 0.0f) {
            const auto low = maximum * 0.34f, high = maximum * 0.95f;
            const auto x = frame ? frame->velocity_150[0] : state.velocity[0];
            const auto y = frame ? frame->velocity_150[1] : state.velocity[1];
            const auto speed = std::sqrt(x * x + y * y);
            if (low == high)
                request_body = speed < high;
            else {
                auto fraction = (speed - low) / (high - low);
                fraction = minimum_number(maximum_number(fraction, 0.0f), 1.0f) + 0.0f;
                request_body = fraction == 0.0f;
            }
        }
    }
    if (request_body) {
        const auto delta = frame ? std::bit_cast<float>(frame->field_108) - frame->inaccuracy_c4
                                 : state.current - state.baseline;
        const auto damage = _mm_cvttss_si32(_mm_set_ss(float(state.point_damage) * 0.3125f + 0.5f));
        return delta >= 0.0f && 0.0001f > delta && damage >= state.minimum_damage;
    }
    return false;
}
void apply_final_hitchance_failure(FinalHitchanceFailure& state) {
    if (state.context_flags & 8U)
        return;
    if (final_target_detail::requests_body_aim(state))
        *state.target_aim_mode = 3;
    state.context_flags |= 0x40U;
}

namespace {
bool evaluate_target_hitchance_impl(FinalHitchanceContext& context,
                                    const TargetCandidate& candidate,
                                    const lagcomp::LagRecord& record, std::int32_t minimum_damage,
                                    float minimum_chance, float& chance,
                                    final_target_detail::Producer* producer) {
    const bool precise_damage = context.damage_context && context.damage_context->enabled;
    const bool precise_samples = precise_damage || context.full_seed_hitchance;
    if (precise_samples &&
        (!std::isfinite(minimum_chance) || minimum_chance < 0 || minimum_chance > 1)) {
        chance = 0;
        return false;
    }
    if ((context.weapon_flags & 0x4000U) && !precise_samples) {
        chance = 1.0f;
        return true;
    }
    if (!context.target_player)
        return false;
    auto bullet = prepare_target_bullet(context.weapon, context.weapon_item_offset,
                                        context.allow_penetration);
    BulletDamageModel model;
    BallisticEntityInfo legacy_target{};
    if (precise_samples && (!context.complete_spread || !context.complete_spread->valid)) {
        chance = 0;
        return false;
    }
    if (precise_damage) {
        model = capture_damage_model(context.ballistics, context.target, *context.damage_context, bullet);
        if (!model.valid) {
            chance = 0;
            return false;
        }
    } else if (precise_samples)
        legacy_target = context.ballistics.entity_info(context.target);
    const auto delta = subtract(candidate.primary.position, context.origin);
    const auto basis = final_target_spread_basis(candidate.primary.position, context.origin);
    if (producer)
        final_target_detail::bind_teammate_policy(context, *producer);
    if (!(context.context_flags & 8U) && context.check_teammates &&
        !context.teammates_are_enemies) {
        const auto scale = context.spread + minimum_number(1.0f, context.inaccuracy);
        constexpr std::array<RageSpreadSample, 4> offsets{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
        auto filter = context.teammate_filter;
        filter.mask = filter.exit_mask = 0x4600400bU;
        filter.minimum_damage = 1.0f;
        for (const auto& offset : offsets) {
            RageVector direction;
            for (std::size_t i = 0; i != 3; ++i)
                direction[i] = (basis.first[i] * (offset.first * scale) + basis.center[i]) +
                               (offset.second * scale) * basis.second[i];
            direction = rage_point_detail::normalize(direction, length(direction));
            if (producer)
                final_target_detail::trace_teammate(*producer, context, direction, bullet);
            else
                trace_target_direction(context.ballistics, context.source, context.origin,
                                       direction, filter, bullet);
            const auto& trace = bullet.trace;
            if ((trace.fraction < 1.0f || trace.start_solid || trace.all_solid) && trace.entity) {
                bool teammate;
                if (producer)
                    teammate = final_target_detail::teammate_hit(*producer, trace.entity);
                else {
                    const auto info = context.ballistics.entity_info(trace.entity);
                    teammate = info.virtual_278 && info.team == context.team;
                }
                if (teammate) {
                    chance = 0;
                    return false;
                }
            }
        }
    }
    const bool safe =
        producer ? final_target_detail::select_studio_hitbox(context, *producer, candidate, record)
                 : (candidate.primary.flags & 1U) && (record.header.flags & 0x80U) &&
                       context.local_hitboxes[std::size_t(candidate.primary.hitbox)].radius > 0.0f;
    if (producer && !producer->selected_descriptor) {
        chance = 0;
        return false;
    }
    const auto plan = context.full_seed_hitchance
                          ? plan_experimental_hitchance(minimum_chance)
                          : plan_final_hitchance(
                                safe && !precise_samples, minimum_chance, context.context_flags, candidate.entity_index,
                                producer ? producer->state.previous_targets : context.previous_targets);
    std::array<std::vector<RageScanHitbox>, 3> geometry;
    std::vector<RageScanHitbox> primary_geometry;
    RageResolverHitboxes hypotheses;
    float minimum_multiplier = 0, base_damage = 0;
    lagcomp::EntityRecordScope scope(context.entity_runtime, context.target_player,
                                     context.saved_bones);
    if (precise_samples)
        primary_geometry = producer ? final_target_detail::hitbox_geometry(context, *producer, record.matrices)
                                    : prepare_rage_scan_hitboxes(context.local_hitboxes, record.matrices,
                                                                 context.trigonometry);
    if (safe) {
        if ((producer ? producer->frame.weapon_id_d8 : std::int32_t(bullet.weapon_id)) == 31)
            base_damage = 1;
        else {
            const auto info = context.ballistics.entity_info(context.target);
            base_damage = hitgroup_damage_scale(info.armor, info.helmet, candidate.primary.hitgroup,
                                                bullet.weapon_id, bullet.armor_ratio);
            minimum_multiplier =
                rage_detail::hitgroup_multipliers[std::uint32_t(candidate.primary.hitgroup) & 15U];
            const auto shots =
                rage_detail::shots_to_kill(candidate.health, candidate.primary.damage);
            const auto raw = float(candidate.primary.damage) / minimum_multiplier;
            constexpr float multipliers[]{.75f, 1, 1.25f, 4};
            for (const auto multiplier : multipliers) {
                const auto damage = _mm_cvttss_si32(_mm_set_ss(multiplier * raw));
                if (rage_detail::shots_to_kill(candidate.health, damage) <= shots) {
                    minimum_multiplier = multiplier;
                    break;
                }
            }
        }
        // The native consumer prepares the primary set before the three
        // resolver sets, retaining its engine descriptor and matrix ordering.
        if (!precise_samples)
            primary_geometry = producer ? final_target_detail::hitbox_geometry(context, *producer, record.matrices)
                                        : prepare_rage_scan_hitboxes(context.local_hitboxes, record.matrices,
                                                                     context.trigonometry);
        for (std::size_t i = 0; i != 3; ++i) {
            geometry[i] = producer ? final_target_detail::hitbox_geometry(
                                         context, *producer, record.alternatives[i].matrices)
                                   : prepare_rage_scan_hitboxes(context.local_hitboxes,
                                                                record.alternatives[i].matrices,
                                                                context.trigonometry);
            hypotheses[i] = geometry[i];
        }
    } else {
        if (producer)
            final_target_detail::apply_record(context, *producer, scope, record);
        else
            scope.apply(record, context.hitbox_bones, context.current_bone_counter);
    }
    const auto hitgroup = producer ? read<std::int32_t>(producer->selected_descriptor, 4)
                                   : candidate.primary.hitgroup;
    if (producer)
        context.samples = (context.context_flags & 8U) ? producer->state.spread.compensated
                                                       : producer->state.spread.primary;
    FinalHitchanceSample input{context.weapon,
                               context.weapon_item_offset,
                               context.allow_penetration,
                               context.source,
                               context.target,
                               context.origin,
                               candidate.primary.position,
                               delta,
                               basis,
                               minimum_damage,
                               hitgroup,
                               minimum_multiplier,
                               base_damage,
                               safe,
                               hypotheses,
                               context.box_clipper,
                               context.direction_filter,
                               context.projected_filter,
                               producer ? &producer->bindings : nullptr,
                               precise_samples && safe,
                               precise_damage ? &model : nullptr, primary_geometry,
                               precise_samples ? context.complete_spread : nullptr,
                               precise_samples ? &bullet : nullptr, legacy_target};
    const auto result =
        context.settings_menu.multithreading
            ? evaluate_final_hitchance_parallel(context.ballistics, input, context.samples, plan,
                                                *context.jobs)
            : evaluate_final_hitchance_inline(context.ballistics, input, context.samples, plan);
    chance = result.fraction;
    const bool accepted = !(minimum_chance > chance) && (!precise_samples || result.hits > 0);
    if (!accepted) {
        FinalHitchanceFailure failure{context.context_flags, context.weapon_flags,
                                      context.weapon,        candidate.primary.hitgroup,
                                      context.velocity,      context.baseline,
                                      context.current,       candidate.primary.damage,
                                      minimum_damage,        context.target_aim_mode};
        if (producer)
            final_target_detail::publish_failure(failure, *producer, candidate.entity_index);
        else
            apply_final_hitchance_failure(failure);
        context.context_flags = failure.context_flags;
    }
    scope.restore();
    return accepted;
}
}
bool evaluate_target_hitchance(FinalHitchanceContext& context, const TargetCandidate& candidate,
                               const lagcomp::LagRecord& record, std::int32_t minimum_damage,
                               float minimum_chance, float& chance) {
    return evaluate_target_hitchance_impl(context, candidate, record, minimum_damage,
                                          minimum_chance, chance, nullptr);
}
bool evaluate_target_hitchance(FinalHitchanceContext& context, const TargetCandidate& candidate,
                               const lagcomp::LagRecord& record, std::int32_t minimum_damage,
                               float minimum_chance, float& chance,
                               final_target_detail::Producer& producer) {
    return evaluate_target_hitchance_impl(context, candidate, record, minimum_damage,
                                          minimum_chance, chance, &producer);
}

TargetShotChoice select_target_for_shot(TargetBatch& batch,
                                        std::span<const FinalTargetEvaluation> evaluations,
                                        std::uint32_t context_flags, std::uint32_t weapon_flags,
                                        std::int32_t weapon_id, float chance_floor,
                                        float point_penalty) {
    if (!batch.count)
        return {};
    const auto* context = evaluations.empty() ? nullptr : evaluations.front().context;
    const bool experimental = context && (context->full_seed_hitchance ||
        (context->damage_context && context->damage_context->enabled));
    if (!experimental && (context_flags & 0x10U) && weapon_id == 64 && (weapon_flags & 0x40U)) {
        batch.candidates[0]->primary.chance = 1.0f;
        return {batch.candidates[0], 0};
    }
    for (std::size_t i = 0; i != batch.count; ++i) {
        auto& candidate = *batch.candidates[i];
        float chance = 1.0f;
        if (experimental || (context_flags & 0x10U)) {
            const auto& evaluation = evaluations[i];
            const auto evaluate = [&] {
                return evaluate_target_hitchance(*evaluation.context, candidate, *evaluation.record,
                                                 evaluation.minimum_damage,
                                                 evaluation.minimum_chance, chance);
            };
            if (!evaluate()) {
                if (!(weapon_flags & 0x400U) || !(candidate.flags & 8U))
                    continue;
                candidate.flags &= ~8U;
                candidate.primary = candidate.alternate_first;
                if (!evaluate()) {
                    if (!(candidate.flags & 0x10U))
                        continue;
                    candidate.flags &= ~0x10U;
                    candidate.primary = candidate.alternate_second;
                    if (!evaluate())
                        continue;
                }
            }
        }
        const auto effective = (candidate.primary.flags & 0x20U) ? chance * point_penalty : chance;
        if (!(chance_floor >= effective)) {
            candidate.primary.chance = chance;
            return {&candidate, i};
        }
    }
    return {};
}

MovementAngles target_aim_angles(const RageVector& point, const RageVector& origin,
                                 const RageVector& recoil, bool compensate_recoil,
                                 std::uint32_t& weapon_flags, const TargetAngleMath& math) {
    auto direction = subtract(point, origin);
    const auto inverse = 1.0f / (length(direction) + 0x1p-23f);
    for (auto& value : direction)
        value *= inverse;
    MovementAngles angles;
    if (direction[0] == 0.0f && direction[1] == 0.0f)
        angles = {direction[2] > 0.0f ? 270.0f : 90.0f, 0, 0};
    else {
        constexpr float degrees = 57.2957763671875f;
        const auto yaw = float(math.atan2(double(direction[1]), double(direction[0]))) * degrees;
        const auto horizontal =
            std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]);
        const auto pitch = float(math.atan2(double(-direction[2]), double(horizontal))) * degrees;
        angles = {pitch, yaw, 0};
    }
    const auto squared = (recoil[0] * recoil[0] + recoil[1] * recoil[1]) + recoil[2] * recoil[2];
    if (compensate_recoil && squared != 0.0f) {
        angles.pitch -= recoil[0];
        angles.yaw -= recoil[1];
        angles.roll -= recoil[2];
        weapon_flags |= 0x10U;
    }
    normalize_movement_angles(angles);
    return angles;
}

RageShotCommit commit_target_choice(const TargetShotChoice& choice, RageShotState& state,
                                    RageShotCommitState& shared, UserCommand& command,
                                    float special_gate_value, bool alternate_path,
                                    bool send_mode) noexcept {
    const RageShotCandidate candidate{choice.candidate->flags, choice.candidate->primary.position};
    return commit_accepted_rage_shot(state, shared, command, candidate, special_gate_value,
                                     alternate_path, send_mode);
}
}
