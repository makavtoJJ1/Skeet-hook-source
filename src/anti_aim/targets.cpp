#include "skeet/anti_aim/targets.hpp"
#include "skeet/esp/player_draw.hpp"
#include "skeet/esp/entity_overlay.hpp"
#include "skeet/esp/player_engine.hpp"
#include "skeet/rage/point_penetration.hpp"
#include "skeet/rage_segments.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace skeet::anti_aim {
std::uint32_t TargetPlayerHistoryView::record_count() const noexcept {
    return source_ ? source_->history.count : decoded_->timeline.record_count;
}
const lagcomp::LagRecord& TargetPlayerHistoryView::at(std::uint32_t age) const noexcept {
    return source_ ? source_->history.at(age) : decoded_->timeline.at(age);
}
const esp::PlayerGeometry& TargetPlayerHistoryView::geometry() const noexcept {
    return source_ ? source_->geometry : decoded_->geometry;
}
float TargetPlayerHistoryView::prediction_field() const noexcept {
    return source_ ? source_->native.target_prediction_524 : decoded_->field_524;
}
void TargetPlayerHistoryView::last_damage_tick(std::int32_t value) const noexcept {
    if (source_)
        source_->native.last_target_damage_tick = value;
    else
        decoded_->last_damage_tick = value;
}
TargetPlayerHistoryView TargetPlayerHistories::operator[](std::uint32_t index) const {
    if (source_)
        return TargetPlayerHistoryView(*source_->get(index));
    return TargetPlayerHistoryView(decoded_[index & 63u]);
}
namespace {
template <class T> T field(const void* p, std::uint32_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(p) + offset, sizeof(value));
    return value;
}
std::int32_t add(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
std::int32_t sub(std::int32_t a, std::int32_t b) {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
esp::PlayerWeaponMetadata weapon_metadata(void* weapon, const TargetSelectorContext& context) {
    const auto* info = engine::invoke<const std::byte*>(weapon, 461);
    esp::PlayerWeaponMetadata result;
    result.definition =
        field<std::uint16_t>(weapon, std::uint32_t(context.targeting.offsets.weapon_identifier));
    result.penetration = field<float>(info, 0x100);
    result.range = field<float>(info, 0x10c);
    result.range_modifier = field<float>(info, 0x110);
    result.armor_ratio = engine::invoke<float>(weapon, 434);
    result.damage = float(engine::invoke<std::int32_t>(weapon, 431));
    result.kind = field<std::int32_t>(info, 0xfc);
    return result;
}
esp::PlayerWeaponMetadata player_metadata(void* player, const TargetSelectorContext& c) {
    const auto get_weapon = [&](std::int32_t handle) -> void* {
        return engine::invoke<void*>(c.targeting.entity_list, 4, handle);
    };
    const auto is_ammo = [&](void* weapon) {
        return esp::ammunition_weapon(
            field<std::uint16_t>(weapon, std::uint32_t(c.targeting.offsets.weapon_identifier)));
    };
    const auto active =
        field<std::int32_t>(player, std::uint32_t(c.targeting.offsets.active_weapon));
    if (active != -1)
        if (void* weapon = get_weapon(active))
            if (is_ammo(weapon))
                return weapon_metadata(weapon, c);
    if (field<std::uint8_t>(player, std::uint32_t(c.targeting.offsets.rejection_bytes[2])))
        for (unsigned i = 0; i < 2; ++i) {
            const auto handle = field<std::int32_t>(player, c.offsets.inventory + i * 4);
            if (handle == -1)
                continue;
            if (void* weapon = get_weapon(handle))
                if (engine::invoke<bool>(weapon, 166) && is_ammo(weapon))
                    return weapon_metadata(weapon, c);
        }
    return {};
}
BallisticQuery bullet_query(const esp::PlayerWeaponMetadata& m) {
    BallisticQuery query;
    query.allow_penetration = 1;
    query.weapon_id = m.definition;
    query.penetration = m.penetration;
    query.range = m.range;
    query.range_modifier = m.range_modifier;
    query.armor_ratio = m.armor_ratio;
    query.base_damage = m.damage;
    query.reserved = std::uint32_t(m.kind);
    return query;
}
std::int32_t target_prediction_ticks(TargetPlayerHistoryView history,
                                     const TargetSelectorContext& c) {
    if (!history.record_count())
        return 1;
    const auto& p = c.prediction_limit;
    const auto limit = lagcomp::prediction_tick_limit(
        p.local_available, p.field_bf567619, p.field_dda99ab5, p.field_b7ea8fda, p.configured);
    const auto received = engine::invoke<float>(c.engine_client, 14);
    const auto elapsed = sub(lagcomp::receive_time_to_tick(received, c.receive_interval),
                             history.at(0).header.receive_tick);
    if (limit < 0)
        return limit;
    return elapsed < 0 ? 0 : elapsed > limit ? limit : elapsed;
}
template <class Scored> float clock_time(const Scored& c) {
    return c.clock_owner && *c.clock_owner ? field<float>(*c.clock_owner, c.local_time_offset)
                                           : 0.0f;
}
std::int32_t integer_convar(const void* object) {
    for (;;) {
        const auto p = field<const void*>(object, 0x1c);
        if (p == object)
            break;
        object = p;
    }
    return std::bit_cast<std::int32_t>(field<std::uint32_t>(object, 0x30) ^
                                       std::uint32_t(reinterpret_cast<std::uintptr_t>(object)));
}
std::int32_t player_index(void*, const esp::RuntimePlayer& player) {
    return engine::invoke<std::int32_t>(static_cast<std::byte*>(player.engine_entity) + 8, 10);
}
esp::RuntimePlayer rule_player(void* entity, const TargetSelectorContext& c) {
    esp::RuntimePlayer result;
    result.engine_entity = entity;
    result.team = field<std::int32_t>(entity, std::uint32_t(c.targeting.offsets.team));
    result.squad = field<std::int32_t>(entity, std::uint32_t(c.targeting.offsets.survival_team));
    return result;
}
template <class Scored> bool special_crouch(void* entity, const Scored& c) {
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(entity) + 8, 10);
    const auto history = c.common.histories[std::uint32_t(index) & 63];
    if (history.record_count() > 1 &&
        sub(history.at(0).header.simulation_tick, history.at(1).header.simulation_tick) > 2) {
        const auto duck =
            field<float>(entity, std::uint32_t(c.common.targeting.offsets.duck_amount));
        if (duck > 0.0f && duck < 1.0f && field<float>(entity, c.offsets.field_fb7e3404) == 8.0f)
            return true;
    }
    return false;
}
std::int32_t selector_health(void* entity, const TargetScoredInputs& c) {
    if (!engine::invoke<bool>(static_cast<std::byte*>(entity) + 8, 9))
        return engine::invoke<std::int32_t>(entity, 122);
    if (!c.player_rules.present)
        return 0;
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(entity) + 8, 10);
    return c.player_rules.players[std::uint32_t(index)].connected
               ? c.dormant_health[std::uint32_t(index)]
               : 0;
}
std::int32_t selector_health(void* entity, const LiveTargetScoredContext& c) {
    if (!engine::invoke<bool>(static_cast<std::byte*>(entity) + 8, 9))
        return engine::invoke<std::int32_t>(entity, 122);
    auto* const resource = c.players.resource();
    if (!resource)
        return 0;
    const auto index = engine::invoke<std::int32_t>(static_cast<std::byte*>(entity) + 8, 10);
    return c.players.connected(resource, index) ? c.players.resource_health(resource, index) : 0;
}
bool dormant_target(void* local, void* player, const TargetScoredInputs& c) {
    const auto a = rule_player(local, c.common), b = rule_player(player, c.common);
    esp::PlayerRuntimeEngine engine{};
    engine.index = player_index;
    return esp::dormant_enemy(&a, &b, engine, c.player_rules) &&
           esp::dormant_alive(&b, engine, c.player_rules);
}
bool dormant_target(void* local, void* player, const LiveTargetScoredContext& c) {
    auto* const a = c.players.identity(local);
    auto* const b = c.players.identity(player);
    return esp::dormant_enemy(a, b, c.players) && esp::dormant_alive(b, c.players);
}
}
#if defined(_M_IX86) || defined(__i386__)
bool __thiscall TargetTraceFilter::should_hit(void* receiver, BallisticEntity entity,
                                              std::uint32_t mask) {
#else
bool TargetTraceFilter::should_hit(void* receiver, BallisticEntity entity, std::uint32_t mask) {
#endif
    auto& self = *static_cast<TargetTraceFilter*>(receiver);
    const auto& entities = *self.entities;
    if (entity) {
        auto current = entities.base_entity(entity);
        while (current) {
            if (entities.collision_group(current) == 11 || entities.is_player(current))
                return false;
            const auto handle = entities.parent_handle(current);
            if (handle == -1)
                break;
            current = entities.by_handle(handle);
        }
    }
    const auto delegate =
        reinterpret_cast<engine::MemberFunction<bool, BallisticEntity, std::uint32_t>>(
            self.engine_vtable[0]);
    return delegate(receiver, entity, mask);
}
TargetTraceFilter::TargetTraceFilter(void* const* base, const TargetTraceEntities& e,
                                     std::uint32_t entry_mask, std::uint32_t leave_mask,
                                     float minimum)
    : vtable(nullptr), mask(entry_mask), exit_mask(leave_mask), minimum_damage(minimum),
      engine_vtable(base), entities(&e), methods(base, reinterpret_cast<void*>(should_hit)) {
    vtable = methods.data();
}
BallisticFilter TargetTraceFilter::binding() noexcept {
    return {this, fields_04[4], mask, exit_mask, minimum_damage};
}
BallisticEntity RawTargetTraceEntities::base_entity(BallisticEntity entity) const {
    return std::uint32_t(reinterpret_cast<std::uintptr_t>(
        engine::invoke<void*>(reinterpret_cast<void*>(std::uintptr_t(entity)), 7)));
}
std::int32_t RawTargetTraceEntities::collision_group(BallisticEntity entity) const {
    return field<std::int32_t>(reinterpret_cast<void*>(std::uintptr_t(entity)),
                               offsets_.collision_group);
}
bool RawTargetTraceEntities::is_player(BallisticEntity entity) const {
    return engine::invoke<bool>(reinterpret_cast<void*>(std::uintptr_t(entity)), 158);
}
std::int32_t RawTargetTraceEntities::parent_handle(BallisticEntity entity) const {
    return field<std::int32_t>(reinterpret_cast<void*>(std::uintptr_t(entity)),
                               offsets_.move_parent_handle);
}
BallisticEntity RawTargetTraceEntities::by_handle(std::int32_t handle) const {
    return std::uint32_t(reinterpret_cast<std::uintptr_t>(engine::invoke<void*>(list_, 4, handle)));
}
void clear_target_selection(TargetState& state) noexcept {
    state.handle = -1;
    state.fields_18 = {0, 0, 0x7FFFFFFFu, 0};
}
void insert_target_candidate(std::span<TargetCandidateOrder> candidates, std::uint32_t candidate,
                             std::uint32_t& head) noexcept {
    auto* link = &head;
    for (auto current = head;
         current != UINT32_MAX && !(candidates[current].score > candidates[candidate].score);
         current = candidates[current].next)
        link = &candidates[current].next;
    candidates[candidate].next = *link;
    *link = candidate;
}
void select_penetrable_yaw_target(TargetState& state, const TargetSelectorContext& c) {
    void* local = c.targeting.local_player;
    const auto* local_origin = engine::invoke<const RageVector*>(local, 10);
    const auto view = field<RageVector>(local, c.offsets.view_offset);
    const float eye_x = (*local_origin)[0] + view[0], eye_y = (*local_origin)[1] + view[1];
    engine::invoke<const RageVector*>(local, 77);
    const auto latency = lagcomp::mean_history_timing_ticks(c.timing);
    const auto* minimum = engine::invoke<const RageVector*>(local, 76);
    const auto* maximum = engine::invoke<const RageVector*>(local, 77);
    const std::array<RageVector, 4> corners{{{(*minimum)[0], (*minimum)[1], (*minimum)[2]},
                                             {(*minimum)[0], (*maximum)[1], (*minimum)[2]},
                                             {(*maximum)[0], (*maximum)[1], (*maximum)[2]},
                                             {(*maximum)[0], (*minimum)[1], (*maximum)[2]}}};
    float nearest = std::numeric_limits<float>::max();
    void* selected = nullptr;
    RageVector selected_position{};
    const auto limit = *c.max_clients;
    for (std::int32_t index = 1; index <= limit; ++index) {
        void* player = engine::invoke<void*>(c.targeting.entity_list, 3, index);
        if (!rage::target_is_eligible(c.targeting, player))
            continue;
        auto position = field<RageVector>(player, c.offsets.network_origin);
        if (latency) {
            const auto ticks =
                add(target_prediction_ticks(c.histories[std::uint32_t(index) & 63], c), latency);
            const auto velocity = field<MovementVector>(player, c.offsets.velocity);
            // The native predictor does not query hull virtuals outside this speed gate.
            const auto speed = velocity.x * velocity.x + velocity.y * velocity.y;
            if (speed > 25.0f && speed < 115600.0f) {
                const auto minimum_hull = *engine::invoke<const MovementVector*>(player, 76);
                const auto maximum_hull = *engine::invoke<const MovementVector*>(player, 77);
                MovementVector predicted{position[0], position[1], position[2]};
                predict_movement_origin(predicted, velocity, minimum_hull, maximum_hull, ticks,
                                        c.interval, c.collision, c.trace_flag);
                position = {predicted.x, predicted.y, predicted.z};
            }
        }
        const RageVector start{position[0], position[1], position[2] + 64.0f};
        auto query = bullet_query(player_metadata(player, c));
        const auto x = eye_x - start[0], y = eye_y - start[1];
        const auto inverse =
            1.0f / (std::sqrt(y * y + x * x) + std::numeric_limits<float>::epsilon());
        const auto dx = x * inverse, dy = y * inverse;
        bool penetrable = false;
        for (const auto& corner : corners) {
            const RageVector point{(dx * corner[0] - dy * corner[1]) + (*local_origin)[0],
                                   (dy * corner[0] + dx * corner[1]) + (*local_origin)[1],
                                   (*local_origin)[2] + corner[2]};
            if (trace_point_penetration(c.ballistics, start, point, 1, query, c.trace_flag) > 0) {
                penetrable = true;
                break;
            }
        }
        if (!penetrable)
            continue;
        const auto distance_y = position[1] - (*local_origin)[1],
                   distance_x = position[0] - (*local_origin)[0],
                   distance_z = position[2] - (*local_origin)[2];
        const auto distance =
            (distance_y * distance_y + distance_x * distance_x) + distance_z * distance_z;
        if (distance < nearest) {
            nearest = distance;
            selected = player;
            selected_position = position;
        }
    }
    if (selected) {
        state.handle = *engine::invoke<const std::int32_t*>(selected, 2);
        state.position = selected_position;
    }
}
template <class Scored> static void select_scored_target(TargetState& state, const Scored& scored) {
    const auto& c = scored.common;
    void* local = c.targeting.local_player;
    bool close_weapon = false;
    const auto weapon_handle =
        field<std::int32_t>(local, std::uint32_t(c.targeting.offsets.active_weapon));
    if (weapon_handle != -1)
        if (void* weapon = engine::invoke<void*>(c.targeting.entity_list, 4, weapon_handle)) {
            const auto kind = engine::invoke<std::int32_t>(weapon, 455);
            if (kind == 0)
                close_weapon = true;
            else if (kind == 8)
                close_weapon = field<std::int32_t>(weapon, scored.offsets.weapon_ammunition) > 0;
        }
    const auto latency = lagcomp::mean_history_timing_ticks(c.timing);
    const auto cap = std::int32_t(0.2f / c.interval + 0.5f);
    const auto prediction = latency < cap ? latency : cap;
    RageVector angles;
    engine::invoke<void>(c.engine_client, 18, angles.data());
    const auto yaw = angles[1] * std::bit_cast<float>(0x3C8EFA35u);
    const auto sy = scored.trigonometry.sine(yaw), cy = scored.trigonometry.cosine(yaw);
    const auto pitch = angles[0] * std::bit_cast<float>(0x3C8EFA35u);
    const auto sp = scored.trigonometry.sine(pitch), cp = scored.trigonometry.cosine(pitch);
    const RageVector direction{
        cp * cy, cp * sy, std::bit_cast<float>(std::bit_cast<std::uint32_t>(sp) ^ 0x80000000u)};
    const auto* current = engine::invoke<const MovementVector*>(local, 10);
    const auto velocity = field<MovementVector>(local, c.offsets.velocity);
    const auto flags =
        field<std::uint32_t>(local, std::uint32_t(c.targeting.offsets.movement_flags));
    MovementVector position;
    if constexpr (std::is_same_v<Scored, TargetScoredContext> ||
                  std::is_same_v<Scored, LiveTargetScoredContext>)
        position = advance_live_movement_origin(local, *current, velocity, flags, scored.movement,
                                                scored.movement_environment, scored.command,
                                                scored.input_buttons, scored.yaw, c.collision);
    else
        position = advance_movement_origin(
            std::uint32_t(reinterpret_cast<std::uintptr_t>(local)), *current, velocity, flags,
            scored.movement_player, scored.movement_rules, scored.movement_environment,
            scored.command, scored.input_buttons, scored.yaw, c.collision);
    const RageVector origin{position.x, position.y, position.z};
    const RageVector eye{origin[0], origin[1], origin[2] + 64.0f};
    struct Candidate {
        std::int32_t index;
        bool dormant;
        RageVector origin;
        float distance;
    };
    std::array<Candidate, 64> candidates{};
    std::array<TargetCandidateOrder, 64> ordering{};
    std::uint32_t count = 0, head = UINT32_MAX;
    bool use_distance = false;
    const auto limit = *c.max_clients;
    for (std::int32_t index = 1; index <= limit; ++index) {
        void* player = engine::invoke<void*>(c.targeting.entity_list, 3, index);
        if (!player || !engine::invoke<bool>(player, 158) ||
            field<std::uint8_t>(player, std::uint32_t(c.targeting.offsets.rejection_bytes[1])) ||
            field<std::uint8_t>(player, std::uint32_t(c.targeting.offsets.rejection_bytes[2])) ||
            c.targeting.player_adjustments.records[std::uint32_t(index)].adjustments.whitelist)
            continue;
        // Engine slot 8 writes the full 0x158-byte player_info_t. A 0x148-byte
        // buffer overwrites the first candidate's index, dormant flag and XY.
        std::array<std::byte, 0x158> info{};
        if (!engine::invoke<bool>(c.engine_client, 8, index, info.data()) ||
            info[0x13D] != std::byte{})
            continue;
        const auto history = c.histories[std::uint32_t(index) & 63];
        const bool dormant = engine::invoke<bool>(static_cast<std::byte*>(player) + 8, 9);
        if (dormant) {
            if (history.geometry().alpha <= 0.0f)
                continue;
            if (!dormant_target(local, player, scored))
                continue;
        } else if (field<std::uint8_t>(player,
                                       std::uint32_t(c.targeting.offsets.rejection_bytes[0])) ||
                   !rage::target_is_enemy(c.targeting, player))
            continue;
        const RageVector point =
            dormant ? RageVector{history.geometry().origin.x, history.geometry().origin.y,
                                 history.geometry().origin.z}
                    : field<RageVector>(player, c.offsets.network_origin);
        const float x = origin[0] - point[0], y = origin[1] - point[1], z = origin[2] - point[2];
        const float distance = std::sqrt((x * x + y * y) + z * z);
        if (close_weapon && distance < 500.0f)
            use_distance = true;
        const float dz = (point[2] + 36.0f) - eye[2], dy = point[1] - eye[1],
                    dx = point[0] - eye[0];
        const float projection = (dy * direction[1] + dx * direction[0]) + dz * direction[2];
        float score = std::numeric_limits<float>::max();
        if (!(projection < 0.0f) && !(5000.0f < projection)) {
            const auto px = point[0] - (direction[0] * projection + eye[0]);
            const auto py = point[1] - (direction[1] * projection + eye[1]);
            const auto pz = (point[2] + 36.0f) - (direction[2] * projection + eye[2]);
            score = std::sqrt((px * px + py * py) + pz * pz);
        }
        if (score == std::numeric_limits<float>::max()) {
            const float x2 = point[0] - eye[0], y2 = point[1] - eye[1], z2 = point[2] - eye[2];
            const auto length = std::sqrt((x2 * x2 + y2 * y2) + z2 * z2);
            score =
                2002.0f - ((direction[1] * y2 + direction[0] * x2) + direction[2] * z2) / length;
        } else {
            const RageSegment segment{point, {point[0], point[1], point[2] + 72.0f}};
            const auto nearest = closest_rage_ray_segment({eye, direction}, segment);
            const auto fraction = nearest.second_fraction, complement = 1.0f - fraction;
            const auto x2 = nearest.first_point[0] -
                            (segment.first[0] * complement + segment.second[0] * fraction);
            const auto y2 = nearest.first_point[1] -
                            (segment.first[1] * complement + segment.second[1] * fraction);
            const auto z2 = nearest.first_point[2] -
                            (segment.second[2] * fraction + segment.first[2] * complement);
            score = std::sqrt((x2 * x2 + y2 * y2) + z2 * z2);
        }
        candidates[count] = {index, dormant, point, distance};
        ordering[count].score = score;
        insert_target_candidate(ordering, count, head);
        ++count;
    }
    RageVector delta{origin[0] - scored.local_pose_origin[0],
                     origin[1] - scored.local_pose_origin[1],
                     origin[2] - scored.local_pose_origin[2]};
    if ((delta[1] * delta[1] + delta[0] * delta[0]) + delta[2] * delta[2] > 122500.0f)
        delta = {};
    TargetState best;
    best.handle = -1;
    float best_distance = std::numeric_limits<float>::max();
    bool dormant_processed = false;
    for (auto cursor = head; cursor != UINT32_MAX; cursor = ordering[cursor].next) {
        const auto& candidate = candidates[cursor];
        const auto history = c.histories[std::uint32_t(candidate.index) & 63];
        void* player = engine::invoke<void*>(c.targeting.entity_list, 3, candidate.index);
        if (!use_distance && ordering[cursor].score > 2000.0f && best.handle != -1)
            break;
        if (candidate.dormant) {
            if (dormant_processed)
                continue;
            dormant_processed = true;
        }
        auto point = candidate.origin;
        if (prediction && !candidate.dormant && history.prediction_field() != 0.0f &&
            history.record_count() >= 2) {
            const auto& previous = history.at(0).header.velocity;
            if (previous[0] * previous[0] + previous[1] * previous[1] > 6400.0f) {
                const auto movement = field<MovementVector>(player, c.offsets.velocity);
                const auto squared = movement.x * movement.x + movement.y * movement.y;
                if (squared > 25.0f && squared < 115600.0f) {
                    const auto minimum = *engine::invoke<const MovementVector*>(player, 76),
                               maximum = *engine::invoke<const MovementVector*>(player, 77);
                    MovementVector predicted{point[0], point[1], point[2]};
                    predict_movement_origin(predicted, movement, minimum, maximum, prediction,
                                            c.interval, c.collision, c.trace_flag);
                    point = {predicted.x, predicted.y, predicted.z};
                }
            }
        }
        const bool dormant = engine::invoke<bool>(static_cast<std::byte*>(player) + 8, 9);
        const bool standing =
            dormant || special_crouch(player, scored) ||
            !(field<float>(player, std::uint32_t(c.targeting.offsets.duck_amount)) >= 1.0f);
        const RageVector shooter{point[0], point[1], point[2] + (standing ? 64.0f : 46.0f)};
        const auto metadata = player_metadata(player, c);
        auto query = bullet_query(metadata);
        // Prediction changes the firing eye only. Native publication copies
        // the candidate-list origin saved before43359415, not its result.
        TargetState selected{*scored.sequence,
                             clock_time(scored),
                             *engine::invoke<const std::int32_t*>(player, 2),
                             candidate.origin,
                             {0, 0, 0x7FFFFFFFu, metadata.definition}};
        auto points =
            target_points_from_entity(local, scored.geometry_offsets, scored.local_pose_origin,
                                      shooter, scored.local_pose, scored.trigonometry);
        TargetTraceFilter engine_filter(scored.trace_filter_vtable, scored.trace_entities);
        const auto filter = engine_filter.binding();
        std::int32_t maximum_damage = 0;
        for (unsigned i = 0; i < 6; ++i) {
            for (unsigned j = 0; j < 3; ++j)
                points.points[i][j] += delta[j];
            trace_ballistic_bullet(c.ballistics,
                                   std::uint32_t(reinterpret_cast<std::uintptr_t>(player)), shooter,
                                   points.points[i], filter, query);
            const auto damage = final_bullet_damage(
                query.damage, query.armor_ratio, std::int32_t(query.reserved), query.weapon_id,
                points.hitgroups[i], field<std::int32_t>(local, scored.offsets.local_armor),
                field<std::uint8_t>(local, scored.offsets.local_helmet));
            if (damage > 0) {
                const auto time = engine::invoke<float>(c.engine_client, 14);
                selected.fields_18[0] = 4;
                if (damage > maximum_damage)
                    maximum_damage = damage;
                selected.fields_18[1] = std::uint32_t(maximum_damage);
                history.last_damage_tick(lagcomp::receive_time_to_tick(time, c.receive_interval));
            }
        }
        if (use_distance) {
            if (best_distance > candidate.distance) {
                best_distance = candidate.distance;
                best = selected;
            }
        } else {
            std::int32_t shots = INT32_MAX;
            if (maximum_damage > 0)
                shots =
                    add(sub(selector_health(player, scored), 1), maximum_damage) / maximum_damage;
            selected.fields_18[2] = std::uint32_t(shots);
            if (shots < std::bit_cast<std::int32_t>(best.fields_18[2]))
                best = selected;
        }
        if (best.handle == -1)
            best = selected;
    }
    if (best.handle != -1)
        state = best;
    else
        clear_target_selection(state);
}
void select_scored_yaw_target(TargetState& state, const TargetScoredContext& scored) {
    select_scored_target(state, scored);
}
void select_decoded_scored_yaw_target(TargetState& state,
                                      const DecodedTargetScoredContext& scored) {
    select_scored_target(state, scored);
}
template <class Scored>
static void refresh_target(TargetState& state, const Scored& scored, const void* selector_convar) {
    if (*scored.sequence == state.sequence)
        return;
    if (integer_convar(selector_convar) == 0)
        select_scored_target(state, scored);
    else
        select_penetrable_yaw_target(state, scored.common);
    state.sequence = *scored.sequence;
    if (state.handle == -1)
        clear_target_selection(state);
    else
        state.timestamp = clock_time(scored);
}
void refresh_yaw_target(TargetState& state, const TargetRefreshContext& c) {
    refresh_target(state, c.scored, c.selector_convar);
}
void refresh_live_yaw_target(TargetState& state, const LiveTargetScoredContext& c,
                             const void* selector_convar) {
    refresh_target(state, c, selector_convar);
}
void refresh_decoded_yaw_target(TargetState& state, const DecodedTargetScoredContext& scored,
                                const void* selector_convar) {
    refresh_target(state, scored, selector_convar);
}
} // namespace skeet::anti_aim
