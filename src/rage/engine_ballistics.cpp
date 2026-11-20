#include "skeet/rage/engine_ballistics.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/engine/trace.hpp"
#include "skeet/rage/bullet_filter_methods.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>

namespace skeet {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
void* pointer(BallisticEntity value) noexcept {
    return reinterpret_cast<void*>(std::uintptr_t(value));
}
BallisticEntity address(void* value) noexcept {
    return static_cast<BallisticEntity>(reinterpret_cast<std::uintptr_t>(value));
}
std::uint32_t offset(const EngineBallisticBindings& b, std::uint32_t slot, std::uint32_t key,
                     std::uint32_t salt) {
    return *b.offsets.pointers.at(slot) ^ b.offsets.keys.at(key) ^ salt;
}
std::int32_t convar_integer(const void* object) noexcept {
    for (;;) {
        const auto parent = field<const void*>(object, 0x1c);
        if (parent == object)
            break;
        object = parent;
    }
    return std::bit_cast<std::int32_t>(field<std::uint32_t>(object, 0x30) ^
                                       std::uint32_t(reinterpret_cast<std::uintptr_t>(object)));
}
engine::TraceRay native_ray(const BallisticRay& source) noexcept {
    engine::TraceRay ray;
    ray.start = {source.start[0], source.start[1], source.start[2], 0.0f};
    ray.delta = {source.delta[0], source.delta[1], source.delta[2], 0.0f};
    ray.is_ray = 1;
    ray.swept = source.swept;
    return ray;
}

#if defined(_M_IX86) || defined(__i386__)
#define BALLISTIC_MEMBER __thiscall
#else
#define BALLISTIC_MEMBER
#endif
//4346EDC0 forwards only the trace-facing methods needed here into the live
//434758B4 table. The receiver remains the original four-word native prefix.
struct SkipFilter {
    engine::TraceSkipFilterPrefix prefix;
    const EngineBallisticBindings* bindings;
    static bool BALLISTIC_MEMBER should_hit(void* receiver, void* entity, std::uint32_t mask) {
        const auto& self = *static_cast<SkipFilter*>(receiver);
        const auto table = self.bindings->skip_filter_vtable;
        if (!table)
            throw std::logic_error("Required ballistic skip filter table is not bound");
        return reinterpret_cast<engine::MemberFunction<bool, void*, std::uint32_t>>(table[0])(
            receiver, entity, mask);
    }
    static std::int32_t BALLISTIC_MEMBER trace_type(void* receiver) {
        const auto& self = *static_cast<SkipFilter*>(receiver);
        const auto table = self.bindings->skip_filter_vtable;
        if (!table)
            throw std::logic_error("Required ballistic skip filter table is not bound");
        return reinterpret_cast<engine::MemberFunction<std::int32_t>>(table[1])(receiver);
    }
    SkipFilter(BallisticEntity source, const EngineBallisticBindings& b)
        : prefix{table(), pointer(source), 0, 0}, bindings(&b) {}
    static void* const* table() noexcept {
        static void* const methods[]{reinterpret_cast<void*>(should_hit),
                                     reinterpret_cast<void*>(trace_type)};
        return methods;
    }
};

//4346DCEC forwards all of its used methods to43475B90.4346EEBC replaces
//ShouldHitEntity/GetTraceType with false/0 and retains the same real setters.
struct ExtendedFilter {
    void* const* vtable;
    std::array<std::uint32_t, 5> fields_04{};
    std::uint32_t mask = 0, exit_mask = 0;
    float minimum_damage = 0;
    const EngineBallisticBindings* bindings;
    BallisticFilterKind kind;
    BulletFilterMethods methods;
    static ExtendedFilter& self(void* receiver) noexcept {
        return *static_cast<ExtendedFilter*>(receiver);
    }
    static void* method(void* receiver, std::size_t slot) {
        const auto table = self(receiver).bindings->extended_filter_vtable;
        if (!table)
            throw std::logic_error("Required ballistic extended filter table is not bound");
        return table[slot];
    }
    static bool BALLISTIC_MEMBER should_hit(void* receiver, void* entity, std::uint32_t mask) {
        const auto& filter = self(receiver);
        if (filter.kind == BallisticFilterKind::reject_entities ||
            filter.kind == BallisticFilterKind::world_only)
            return false;
        if (filter.kind == BallisticFilterKind::reject_players && entity) {
            auto current = engine::invoke<void*>(entity, 7);
            const auto& b = *filter.bindings;
            while (current) {
                if (field<std::int32_t>(current, offset(b, 0x434683c8, 0x4346758c, 0xe4e2d1a7)) ==
                        11 ||
                    engine::invoke<bool>(current, 158))
                    return false;
                const auto handle = field<std::int32_t>(
                    current, offset(b, 0x43469344, 0x43467018, 0x49d93e62) - 0x18);
                if (handle == -1)
                    break;
                current = engine::invoke<void*>(b.entity_list, 4, handle);
            }
        }
        return reinterpret_cast<engine::MemberFunction<bool, void*, std::uint32_t>>(
            method(receiver, 0))(receiver, entity, mask);
    }
    static std::int32_t BALLISTIC_MEMBER trace_type(void* receiver) {
        if (self(receiver).kind == BallisticFilterKind::reject_entities)
            return 0;
        if (self(receiver).kind == BallisticFilterKind::world_only)
            return 1;
        return reinterpret_cast<engine::MemberFunction<std::int32_t>>(method(receiver, 1))(
            receiver);
    }
    ExtendedFilter(const EngineBallisticBindings& b) : ExtendedFilter(b, BallisticFilter{}) {}
    ExtendedFilter(const EngineBallisticBindings& b, const BallisticFilter& f)
        : vtable(nullptr), fields_04(f.native_fields_04), mask(f.mask), exit_mask(f.exit_mask),
          minimum_damage(f.minimum_damage), bindings(&b), kind(f.kind),
          methods(b.extended_filter_vtable, reinterpret_cast<void*>(should_hit),
                  reinterpret_cast<void*>(trace_type)) {
        fields_04[4] = f.preferred_entity;
        vtable = methods.data();
    }
    ExtendedFilter(const ExtendedFilter&) = delete;
    ExtendedFilter& operator=(const ExtendedFilter&) = delete;
    void save(const BallisticFilter& f) const noexcept {
        f.native_fields_04 = fields_04;
    }

};
#undef BALLISTIC_MEMBER
#if UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(ExtendedFilter, fields_04) == 4);
static_assert(offsetof(ExtendedFilter, mask) == 0x18);
static_assert(offsetof(ExtendedFilter, minimum_damage) == 0x20);
#endif
}

std::int32_t EngineBallistics::trace_bullet(BallisticEntity source, const BallisticVector& start,
                                           const BallisticVector& target, BallisticQuery& query,
                                           bool skip_players) {
    const auto mask = skip_players ? 0x0600400Bu : 0x4600400Bu;
    BallisticFilter filter{nullptr, 0, mask, mask, 1.0f};
    ExtendedFilter native(bindings_, filter);
    if (skip_players)
        native.kind = BallisticFilterKind::reject_players;
    filter.object = &native;
    return trace_ballistic_bullet(*this, source, start, target, filter, query);
}

std::int32_t EngineBallistics::filter_type(const BallisticFilter& filter) {
    if (filter.kind == BallisticFilterKind::world_only)
        return 1;
    if (filter.kind == BallisticFilterKind::reject_entities)
        return 0;
    if (filter.kind == BallisticFilterKind::reject_players) {
        ExtendedFilter native(bindings_, filter);
        return engine::invoke<std::int32_t>(&native, 1);
    }
    return engine::invoke<std::int32_t>(filter.object, 1);
}
void EngineBallistics::bind_source(const BallisticFilter& filter, BallisticEntity source) {
    if (filter.kind == BallisticFilterKind::reject_entities ||
        filter.kind == BallisticFilterKind::reject_players ||
        filter.kind == BallisticFilterKind::world_only) {
        ExtendedFilter native(bindings_, filter);
        engine::invoke<void>(&native, BulletFilterMethods::source_slot, pointer(source));
        native.save(filter);
        return;
    }
    engine::invoke<void>(filter.object, BulletFilterMethods::source_slot, pointer(source));
}
void EngineBallistics::bind_last_hit(const BallisticFilter& filter, BallisticEntity hit) {
    if (filter.kind == BallisticFilterKind::reject_entities ||
        filter.kind == BallisticFilterKind::reject_players ||
        filter.kind == BallisticFilterKind::world_only) {
        ExtendedFilter native(bindings_, filter);
        engine::invoke<void>(&native, BulletFilterMethods::last_hit_slot, pointer(hit));
        native.save(filter);
        return;
    }
    engine::invoke<void>(filter.object, BulletFilterMethods::last_hit_slot, pointer(hit));
}
bool EngineBallistics::accepts(const BallisticFilter& filter, BallisticEntity entity,
                               std::uint32_t mask) {
    if (filter.kind == BallisticFilterKind::world_only ||
        filter.kind == BallisticFilterKind::reject_entities)
        return false;
    if (filter.kind == BallisticFilterKind::reject_players) {
        ExtendedFilter native(bindings_, filter);
        return engine::invoke<bool>(&native, 0, pointer(entity), mask);
    }
    return engine::invoke<bool>(filter.object, 0, pointer(entity), mask);
}
std::uint32_t EngineBallistics::point_contents(const BallisticVector& point, std::uint32_t mask,
                                               bool world_only) {
    //433C098C uses slot1 with two arguments;433E111C uses slot0 and a null
    //out-entity argument. These are different genuine engine entrypoints.
    if (world_only)
        return engine::invoke<std::uint32_t>(bindings_.engine_trace, 1, &point, mask);
    return engine::invoke<std::uint32_t>(bindings_.engine_trace, 0, &point, mask,
                                         static_cast<void**>(nullptr));
}
void EngineBallistics::trace_ray(const BallisticTraceRequest& request, BallisticTrace& result) {
    auto ray = native_ray(request.ray);
    switch (request.filter_kind) {
    case BallisticFilterKind::supplied:
        if (!request.filter || !request.filter->object)
            throw std::logic_error("Required supplied ballistic trace filter is not bound");
        engine::invoke<void>(bindings_.engine_trace, 5, &ray, request.mask, request.filter->object,
                             &result);
        return;
    case BallisticFilterKind::skip_source: {
        SkipFilter filter(request.source, bindings_);
        engine::invoke<void>(bindings_.engine_trace, 5, &ray, request.mask, &filter, &result);
        return;
    }
    case BallisticFilterKind::skip_source_and_hit: {
        ExtendedFilter filter(bindings_);
        filter.fields_04[0] = request.source;
        filter.fields_04[3] = request.skip; //native exit local+10
        engine::invoke<void>(bindings_.engine_trace, 5, &ray, request.mask, &filter, &result);
        return;
    }
    case BallisticFilterKind::world_only: {
        if (!request.filter)
            throw std::logic_error("Required ballistic world filter prefix is not bound");
        ExtendedFilter filter(bindings_, *request.filter);
        engine::invoke<void>(bindings_.engine_trace, 5, &ray, request.mask, &filter, &result);
        filter.save(*request.filter);
        return;
    }
    case BallisticFilterKind::reject_entities:
    case BallisticFilterKind::reject_players: {
        if (!request.filter)
            throw std::logic_error("Required ballistic filter prefix is not bound");
        ExtendedFilter filter(bindings_, *request.filter);
        engine::invoke<void>(bindings_.engine_trace, 5, &ray, request.mask, &filter, &result);
        filter.save(*request.filter);
        return;
    }
    }
    throw std::logic_error("Invalid ballistic filter kind");
}
void EngineBallistics::clip_ray_to_entity(const BallisticRay& source, std::uint32_t mask,
                                          BallisticEntity entity, BallisticTrace& result) {
    auto ray = native_ray(source);
    engine::invoke<void>(bindings_.engine_trace, 3, &ray, mask, pointer(entity), &result);
}
PenetrationSurface EngineBallistics::surface(std::int16_t property) {
    const auto data =
        engine::invoke<const void*>(bindings_.surface_properties, 5, std::int32_t(property));
    return {field<std::uint16_t>(data, 0x60), field<float>(data, 0x58)};
}
BallisticEntity EngineBallistics::entity(std::int32_t index) {
    return address(engine::invoke<void*>(bindings_.entity_list, 3, index));
}
std::int32_t EngineBallistics::maximum_clients() {
    return *bindings_.maximum_clients;
}

BallisticEntityInfo EngineBallistics::read_entity(BallisticEntity value, bool clipping) {
    auto* entity = pointer(value);
    BallisticEntityInfo result;
    result.virtual_278 = engine::invoke<bool>(entity, 158);
    result.virtual_280 = engine::invoke<bool>(entity, 160);
    if (!result.virtual_278)
        return result;
    const auto networkable = static_cast<std::byte*>(entity) + 8;
    result.dormant = engine::invoke<bool>(networkable, 9);
    const auto index = engine::invoke<std::int32_t>(networkable, 10);
    result.target_index_blocked =
        bindings_.player_adjustments.records[std::uint32_t(index)].adjustments.whitelist != 0;
    result.target_state =
        clipping
            ? field<std::uint8_t>(entity, offset(bindings_, 0x43468e78, 0x434684e4, 0x9d920e35))
            : field<std::uint8_t>(entity, offset(bindings_, 0x4346a5dc, 0x43468430, 0xcde290d2));
    result.helmet =
        field<std::uint8_t>(entity, offset(bindings_, 0x43468c04, 0x43468fe8, 0x7d6280c7));
    result.armor =
        field<std::int32_t>(entity, offset(bindings_, 0x43467908, 0x434682e4, 0x47a6f50c));
    result.team =
        field<std::int32_t>(entity, offset(bindings_, 0x434688b0, 0x4346a518, 0x8b5586f6));
    result.survival_team =
        field<std::int32_t>(entity, offset(bindings_, 0x434678ac, 0x43467b7c, 0x430c8993));
    result.origin = *engine::invoke<const BallisticVector*>(entity, 10);
    result.minimum_z = (*engine::invoke<const BallisticVector*>(entity, 76))[2];
    result.maximum_z = (*engine::invoke<const BallisticVector*>(entity, 77))[2];
    return result;
}
BallisticEntityInfo EngineBallistics::entity_info(BallisticEntity entity) {
    return read_entity(entity, false);
}
BallisticEntityInfo EngineBallistics::clip_entity_info(BallisticEntity entity) {
    return read_entity(entity, true);
}

BallisticBreakableInfo EngineBallistics::breakable_info(BallisticEntity value) {
    void* entity = pointer(value);
    BallisticBreakableInfo result;
    if (bindings_.primary_breakable_class) {
        const auto unknown = engine::invoke<void*>(entity, 4);
        if (unknown) {
            const auto type = engine::invoke<void*>(unknown, 2);
            result.primary_class_matches = type && type == bindings_.primary_breakable_class;
            if (result.primary_class_matches) {
                result.primary_class_state = field<std::uint8_t>(
                    entity, offset(bindings_, 0x4346a360, 0x43468314, 0xf0374295));
                return result;
            }
        }
    }
    if (!bindings_.alternate_breakable_class)
        return result;
    const auto unknown = engine::invoke<void*>(entity, 4);
    if (!unknown)
        return result;
    const auto type = engine::invoke<void*>(unknown, 2);
    result.alternate_class_matches = type && type == bindings_.alternate_breakable_class;
    if (!result.alternate_class_matches)
        return result;
    auto collideable = engine::invoke<void*>(entity, 3);
    result.solid_type = engine::invoke<std::int32_t>(collideable, 11);
    if (result.solid_type != 1)
        return result;
    result.collision_group =
        field<std::uint8_t>(entity, offset(bindings_, 0x43468bd4, 0x4346a678, 0x7501cb91));
    if (result.collision_group != 7)
        return result;
    collideable = engine::invoke<void*>(entity, 3);
    result.solid_flags = engine::invoke<std::uint32_t>(collideable, 12);
    if (!(result.solid_flags & 0x800))
        return result;
    result.health =
        field<std::int32_t>(entity, offset(bindings_, 0x43467fcc, 0x43467938, 0x8d1bd8d0));
    if (result.health != -1)
        return result;
    result.effects =
        field<std::uint32_t>(entity, offset(bindings_, 0x434688d0, 0x43468c24, 0xe2fbe695));
    if (result.effects & 0x2000000)
        return result;
    result.first_scale =
        field<float>(entity, offset(bindings_, 0x43468bd0, 0x43469278, 0x8dbc9874));
    if (result.first_scale == 0.0f)
        result.second_scale =
            field<float>(entity, offset(bindings_, 0x43467948, 0x4346919c, 0x68140cd8));
    return result;
}
BallisticModelInfo EngineBallistics::model_info(std::int32_t index) {
    BallisticModelInfo result;
    const auto object = engine::invoke<void*>(bindings_.model_collision, 4, index);
    if (!object)
        return result;
    result.entity_type = engine::invoke<std::int32_t>(object, 11);
    if (result.entity_type != 6)
        return result;
    const auto model = engine::invoke<const void*>(object, 7);
    result.model_present = model != nullptr;
    if (!model)
        return result;
    result.model_type = field<std::int32_t>(model, 0x110);
    if (result.model_type != 3)
        return result;
    result.studio_model_present = engine::invoke<void*>(bindings_.model_info, 32, model) != nullptr;
    if (!result.studio_model_present)
        return result;
    const auto cycle = engine::invoke<float>(object, 19);
    const auto flags = engine::invoke<const std::uint16_t*>(
        bindings_.model_cache, 16, std::int32_t(field<std::uint16_t>(model, 0x138)), cycle);
    if (flags)
        result.activity_flags = *flags;
    return result;
}
std::uint8_t EngineBallistics::damage_mode(BallisticEntity entity) {
    return field<std::uint8_t>(pointer(entity),
                               offset(bindings_, 0x43469198, 0x4346a538, 0xd5fd2e2c) + 0x18);
}
void EngineBallistics::set_damage_mode(BallisticEntity entity, std::uint8_t value) {
    auto* destination = static_cast<std::byte*>(pointer(entity)) +
                        offset(bindings_, 0x43469198, 0x4346a538, 0xd5fd2e2c) + 0x18;
    std::memcpy(destination, &value, 1);
}
bool EngineBallistics::has_engine_breakable_callback() {
    return bindings_.breakable != nullptr;
}
bool EngineBallistics::engine_breakable(BallisticEntity entity) {
    return bindings_.breakable(pointer(entity));
}
BallisticRules EngineBallistics::rules() {
    const auto game_type = convar_integer(bindings_.game_type.get());
    if (game_type == 6)
        return {game_type, false};
    return {game_type, convar_integer(bindings_.teammates_are_enemies.get()) != 0};
}
} //namespace skeet
