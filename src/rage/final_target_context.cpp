#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/acquisition.hpp"
#include "skeet/rage/preamble.hpp"
#include "skeet/rage/experimental_validation.hpp"
#include "skeet/rage/damage_model.hpp"
#include "skeet/lagcomp/history_selection.hpp"
#include "final_target_context_detail.hpp"
#include "scanning_detail.hpp"
#include "skeet/engine/interface_call.hpp"
#include <bit>
#include <cstring>
#include <memory>

namespace skeet::rage {
namespace {
template <class T> T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
void* pointer(BallisticEntity entity) noexcept {
    return reinterpret_cast<void*>(std::uintptr_t(entity));
}

// Source owners43366346/433304C1, with the original filter prefix. Methods1..4
// are the genuine43475B90 engine methods. Each sample owns its own prefix;
// engine source/last-hit setters must never mutate a shared worker filter.
struct FinalTraceFilter {
    void** vtable;
    std::array<std::uint32_t, 5> fields_04{};
    std::uint32_t mask, exit_mask;
    float minimum_damage = 1.0f;
    std::int32_t team = 0;
    std::uint8_t teammates_are_enemies = 0;
    std::array<std::byte, 3> padding_29{};
    const FinalTargetContextBindings* bindings;
    BulletFilterMethods methods;
    FinalTraceFilter(const FinalTargetContextBindings& b, bool teammate, BallisticEntity preferred,
                     std::int32_t own_team = 0, bool enemies = false)
        : vtable(nullptr), mask(0x4600400BU), exit_mask(0x4600400BU), team(own_team),
          teammates_are_enemies(enemies), bindings(&b),
          methods(b.scan.trace_filter_vtable, teammate ? reinterpret_cast<void*>(teammate_hit)
                                                       : reinterpret_cast<void*>(regular_hit)) {
        fields_04[4] = preferred;
        vtable = methods.data();
    }
    BallisticFilter binding() noexcept {
        return {this, fields_04[4], mask, exit_mask, minimum_damage};
    }
    bool delegate(BallisticEntity entity, std::uint32_t contents) {
        const auto method =
            reinterpret_cast<engine::MemberFunction<bool, BallisticEntity, std::uint32_t>>(
                bindings->scan.trace_filter_vtable[0]);
        return method(this, entity, contents);
    }
#if defined(_M_IX86) || defined(__i386__)
    static bool __thiscall regular_hit(void* receiver, BallisticEntity entity,
                                       std::uint32_t contents) {
#else
    static bool regular_hit(void* receiver, BallisticEntity entity, std::uint32_t contents) {
#endif
        auto& self = *static_cast<FinalTraceFilter*>(receiver);
        const auto& entities = self.bindings->scan.trace_entities;
        if (entity && entity != self.fields_04[4]) {
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
        return self.delegate(entity, contents);
    }
#if defined(_M_IX86) || defined(__i386__)
    static bool __thiscall teammate_hit(void* receiver, BallisticEntity entity,
                                        std::uint32_t contents){
#else
    static bool teammate_hit(void* receiver, BallisticEntity entity, std::uint32_t contents) {
#endif
        auto& self = *static_cast<FinalTraceFilter*>(receiver);
    const auto& scan = self.bindings->scan;
    const auto& entities = scan.trace_entities;
    if (entity) {
        auto current = entities.base_entity(entity);
        while (current) {
            if (entities.is_player(current)) {
                if (!self.fields_04[4])
                    return false;
                if (current != self.fields_04[4] &&
                    (self.teammates_are_enemies ||
                     read<std::int32_t>(pointer(current), scan.targeting.offsets.team) !=
                         self.team))
                    return false;
                break;
            }
            if (engine::invoke<bool>(pointer(current), 166) ||
                engine::invoke<bool>(pointer(current), 159) ||
                entities.collision_group(current) == 11)
                return false;
            const auto handle = entities.parent_handle(current);
            if (handle == -1)
                break;
            current = entities.by_handle(handle);
        }
    }
    return self.delegate(entity, contents);
}
};
#if defined(_M_IX86) || defined(__i386__)
static_assert(offsetof(FinalTraceFilter, mask) == 0x18);
static_assert(offsetof(FinalTraceFilter, team) == 0x24);
static_assert(offsetof(FinalTraceFilter, teammates_are_enemies) == 0x28);
#endif
}

namespace final_target_detail {
void bind_teammate_policy(FinalHitchanceContext& context, const Producer& producer) {
    context.check_teammates = final_teammate_precheck_required(
        context.context_flags, producer.bindings.scan.teammate_convars);
    // That source policy has already read the terminal teammates ConVar only
    // when the native damage policy reaches it. A true result means zero.
    context.teammates_are_enemies = false;
}
bool teammate_hit(const Producer& producer, BallisticEntity entity) {
    return engine::invoke<bool>(pointer(entity), 158) &&
           read<std::int32_t>(pointer(entity), producer.bindings.scan.targeting.offsets.team) ==
               std::bit_cast<std::int32_t>(producer.frame.field_1d0);
}
bool select_studio_hitbox(FinalHitchanceContext& context, Producer& producer,
                          const TargetCandidate& candidate, const lagcomp::LagRecord& record) {
    const auto& offsets = producer.bindings.scan.model_offsets;
    const auto wrapper = read<const void*>(context.target_player, offsets.studio_header);
    const void* set = nullptr;
    if (wrapper) {
        const auto header = read<const void*>(wrapper, 0);
        if (header) {
            const auto index = read<std::int32_t>(context.target_player, offsets.hitbox_set);
            set = static_cast<const std::byte*>(header) + read<std::int32_t>(header, 0xB0) +
                  std::ptrdiff_t(index) * 12;
            if (!read<std::uint32_t>(set, 4))
                set = nullptr;
        }
    }
    // Native4341BB8D computes this descriptor even on regular points. A valid
    // selected studio set is its contract; a missing set is not substituted.
    if (!set || candidate.primary.hitbox < 0 ||
        candidate.primary.hitbox >= read<std::int32_t>(set, 4))
        return false;
    const auto descriptor = static_cast<const std::byte*>(set) + read<std::int32_t>(set, 8) +
                            std::ptrdiff_t(candidate.primary.hitbox) * 0x44;
    producer.selected_descriptor = descriptor;
    return (candidate.primary.flags & 1U) && (record.header.flags & 0x80U) && descriptor &&
           read<float>(descriptor, 0x30) > 0.0f;
}
std::vector<RageScanHitbox> hitbox_geometry(const FinalHitchanceContext& context,
                                            const Producer& producer,
                                            const HitboxMatrices& matrices) {
    const auto model =
        scan_detail::model(context.target_player, producer.bindings.scan.model_offsets);
    return prepare_rage_scan_hitboxes(model.hitboxes, matrices, context.trigonometry);
}
void apply_record(const FinalHitchanceContext& context, const Producer& producer,
                  lagcomp::EntityRecordScope& scope, const lagcomp::LagRecord& record) {
    const auto& model = producer.bindings.scan.model_offsets;
    scope.apply_live_model(record, {model.studio_header, model.hitbox_set},
                           context.current_bone_counter);
}
void publish_failure(FinalHitchanceFailure& state, const Producer& producer,
                     std::uint32_t entity_index) {
    if (state.context_flags & 8U)
        return;
    if (requests_body_aim(state, &producer.frame)) {
        auto& native = producer.bindings.scan.players.get(entity_index)->native;
        const std::int32_t mode = 3;
        std::memcpy(reinterpret_cast<std::byte*>(&native) + 0xF8, &mode, sizeof mode);
    }
    state.context_flags |= 0x40U;
}
std::int32_t trace_direction(const FinalTargetContextBindings& bindings, BallisticEngine& engine,
                             const FinalHitchanceSample& input, const RageVector& direction,
                             BallisticQuery& query) {
    FinalTraceFilter filter(bindings, false, input.target);
    return trace_target_direction(engine, input.source, input.origin, direction, filter.binding(),
                                  query);
}
std::int32_t trace_projected(const FinalTargetContextBindings& bindings, BallisticEngine& engine,
                             const FinalHitchanceSample& input, const RageVector& point,
                             BallisticQuery& query) {
    anti_aim::TargetTraceFilter filter(bindings.scan.trace_filter_vtable,
                                       bindings.scan.trace_entities, 0x600400BU, 0x600400BU, 1.0f);
    return trace_ballistic_bullet(engine, input.source, input.origin, point, filter.binding(),
                                  query);
}
std::int32_t trace_teammate(const Producer& producer, FinalHitchanceContext& context,
                            const RageVector& direction, BallisticQuery& query) {
    FinalTraceFilter filter(producer.bindings, true, 0, context.team,
                            context.teammates_are_enemies);
    return trace_target_direction(context.ballistics, context.source, context.origin, direction,
                                  filter.binding(), query);
}
}

bool FinalTargetContextBindings::validates_records() const noexcept {
    return history && bool(history->extrapolation.validate_record);
}
bool FinalTargetContextBindings::precise_damage() const noexcept {
    return scan.damage_context && scan.damage_context->enabled;
}
bool FinalTargetContextBindings::full_seed_hitchance() const noexcept {
    return preamble && bool(preamble->settings.experimental(ExperimentalRageOption::hitchance));
}

bool evaluate_final_target(client::CommandFrame& frame, PreambleState& state,
                           const TargetCandidate& candidate,
                           const AcquisitionEvaluation& evaluation, std::uint32_t& context_flags,
                           const FinalTargetContextBindings& bindings, float& chance) {
    const bool precise_damage = bindings.precise_damage();
    const bool full_seed_hitchance = bindings.full_seed_hitchance();
    const bool precise_samples = precise_damage || full_seed_hitchance;
    const bool validates_records = bindings.validates_records();
    if (validates_records) {
        const auto& history = *bindings.history;
        const lagcomp::HistoryTickTiming timing{
            history.current_time, read<float>(&frame, 0x198), history.interpolation,
            bindings.scan.preparation.entity.interval, history.maximum_unlag_ms,
            read<std::int32_t>(&frame, 0xD4), std::bit_cast<std::int32_t>(frame.field_18c)};
        const auto* player = candidate.entity_index && candidate.entity_index <= 64
                                 ? bindings.scan.players.get(candidate.entity_index) : nullptr;
        if (!player || !experimental_target_valid(
                           candidate, evaluation.record, player->history, timing, frame.origin_dc,
                           frame.spread_d0, frame.inaccuracy_c4,
                           std::bit_cast<std::int32_t>(evaluation.settings.words[5]),
                           std::bit_cast<float>(evaluation.settings.words[6]))) {
            chance = 0;
            return false;
        }
    }
    const bool stock_revolver_bypass = (context_flags & 0x10U) && frame.weapon_id_d8 == 64 &&
                                       (frame.flags & 0x40U);
    if (!precise_samples && ((frame.flags & 0x4000U) ||
        (validates_records && (!(context_flags & 0x10U) || stock_revolver_bypass)))) {
        chance = 1.0f;
        return true;
    }
    auto& scan = bindings.scan;
    auto* target = engine::invoke<void*>(scan.targeting.entity_list, 3, candidate.entity_index);
    if (!target)
        return false;
    std::unique_ptr<CompleteSpread> complete;
    if (precise_samples) {
        if ((precise_damage && !scan.damage_context->valid) || !bindings.preamble) {
            chance = 0;
            return false;
        }
        const auto& preamble = *bindings.preamble;
        SpreadParameters parameters{
            std::uint32_t(frame.weapon_id_d8), std::int32_t(frame.field_b0), frame.spread_d0,
            context_flags & 8U ? std::bit_cast<float>(frame.field_160) - frame.spread_d0
                               : frame.inaccuracy_c4,
            std::bit_cast<float>(frame.field_100), frame.pellet_count_19c,
            preamble.shotgun_patterns, frame.weapon_kind_124};
        // weapon_accuracy_nospread removes dispersion, not the damage test.
        if (frame.flags & 0x4000U)
            parameters.spread = parameters.inaccuracy = 0;
        complete = std::make_unique<CompleteSpread>();
        if (!prepare_complete_spread(*complete, parameters, preamble.spread_tables,
                                      preamble.random, preamble.pattern, preamble.trigonometry)) {
            chance = 0;
            return false;
        }
    }
    final_target_detail::Producer producer{bindings, frame, state, {}, nullptr};
    FinalHitchanceContext context{
        .ballistics = scan.ballistics,
        .entity_runtime = scan.preparation.entity,
        .target_player = target,
        .source = scan.source,
        .target = BallisticEntity(reinterpret_cast<std::uintptr_t>(target)),
        .weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address)),
        .weapon_item_offset = scan.weapon_item_offset,
        .allow_penetration = scan.allow_penetration,
        .weapon_flags = frame.flags,
        .context_flags = context_flags,
        .previous_targets = {},
        .origin = frame.origin_dc,
        .velocity = {frame.velocity_150[0], frame.velocity_150[1]},
        .baseline = frame.inaccuracy_c4,
        .current = std::bit_cast<float>(frame.field_108),
        .spread = frame.spread_d0,
        .inaccuracy = frame.inaccuracy_c4,
        .check_teammates = false,
        .teammates_are_enemies = false,
        .team = std::bit_cast<std::int32_t>(frame.field_1d0),
        .target_aim_mode = nullptr,
        .local_hitboxes = {},
        .hitbox_bones = {},
        .current_bone_counter = bindings.current_bone_counter,
        .saved_bones = producer.saved_bones,
        .trigonometry = scan.trigonometry,
        .box_clipper = scan.box_clipper,
        .direction_filter = {},
        .projected_filter = {},
        .teammate_filter = {},
        .samples = state.spread.primary,
        .settings_menu = bindings.settings_menu,
        .jobs = bindings.jobs,
        .full_seed_hitchance = full_seed_hitchance,
        .damage_context = scan.damage_context,
        .complete_spread = complete.get(),
    };
    return evaluate_target_hitchance(context, candidate, evaluation.record,
                                     std::bit_cast<std::int32_t>(evaluation.settings.words[5]),
                                     std::bit_cast<float>(evaluation.settings.words[6]), chance,
                                     producer);
}
} // namespace skeet::rage
