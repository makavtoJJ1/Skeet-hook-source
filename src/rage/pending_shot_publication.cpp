#include "skeet/rage/pending_shot_publication.hpp"
#include "skeet/rage/feedback_owner.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/misc/logs.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <emmintrin.h>

namespace skeet::rage {
namespace {
template <class T> T field(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template <class T> void field(void* object, std::ptrdiff_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
std::uint32_t bits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}
float real(std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}
std::int32_t integer(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}
bool weapon_prefix(const char* name) noexcept {
    constexpr char prefix[] = "weapon_";
    // Native774FB8A0 is msvcrt!_strnicmp, not the case-sensitive strncmp.
    for (std::size_t index = 0; index < 7; ++index) {
        auto value = static_cast<unsigned char>(name[index]);
        if (value >= 'A' && value <= 'Z')
            value = static_cast<unsigned char>(value + ('a' - 'A'));
        if (value != static_cast<unsigned char>(prefix[index]))
            return false;
    }
    return true;
}
bool convar_nonzero(const void* object) noexcept {
    for (;;) {
        const auto parent = field<const void*>(object, 0x1c);
        if (parent == object)
            break;
        object = parent;
    }
    return field<std::uint32_t>(object, 0x30) !=
           std::uint32_t(reinterpret_cast<std::uintptr_t>(object));
}
ShotFeedbackRecord& append_pending_shot(ShotFeedbackState& state) {
    const auto count = state.pending.size();
    if (count == state.pending.capacity()) {
        //43406E18 grows by1 for an empty queue and2*size otherwise. Its copy
        // constructor433D68DA copies34 prefix words plus the complete1190
        // state, leaving the allocation's zeroed eight-byte prefix hole.
        std::vector<ShotFeedbackRecord> grown;
        grown.reserve(count ? count * 2 : 1);
        grown.resize(count + 1);
        for (std::size_t index = 0; index < count; ++index)
            lagcomp::copy_construct_evaluation(grown[index], state.pending[index]);
        state.pending.swap(grown);
    } else
        state.pending.emplace_back();
    auto& result = state.pending.back();
    std::memset(&result, 0, sizeof result);
    state.events.has_pending_shots = true;
    return result;
}
ShotFeedbackRecord* append_current_pending_shot(const PendingShotPublicationBindings& bindings) {
    //4341D459 reads the full owner once, after the caller's aim_fire dispatch.
    // Its EDI record is retained after append; no state/owner view is carried
    // into subsequent engine callbacks or used for the later recording read.
    auto* owner = bindings.active;
    if (!owner || !bindings.recording_enabled)
        return nullptr;
    return &append_pending_shot(*feedback_owner_state(owner));
}
void capture_weapon_bullet(void* system, std::uint16_t identifier, void* destination) {
    if (!system) {
        // Exact immutable4346F1B8 table, used by the original null-system path.
        constexpr std::array<std::uint32_t, 7> native_default{
            0x26U, 0x40200000U, 0x46000000U, 0x3f7ae148U, 0x3fd33333U, 0x42a00000U, 1U};
        std::memcpy(destination, native_default.data(), sizeof native_default);
        return;
    }
    const auto* data = engine::invoke<const void*>(system, 2, std::uint32_t(identifier));
    field(destination, 0, identifier);
    field(destination, 4, field<std::uint32_t>(data, 0x100));
    field(destination, 8, field<std::uint32_t>(data, 0x10c));
    field(destination, 12, field<std::uint32_t>(data, 0x110));
    field(destination, 16, field<std::uint32_t>(data, 0xf8));
    const auto damage = field<std::uint32_t>(data, 0xf0);
    auto converted = _mm_cvtsi32_sd(_mm_setzero_pd(), integer(damage));
    converted = _mm_add_sd(converted, _mm_set_sd((damage >> 31) ? 4294967296.0 : 0.0));
    field(destination, 20, bits(_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), converted))));
    field(destination, 24, field<std::uint32_t>(data, 0xfc));
}
void write_mode_yaw(client::NativePlayerStateLayout& owner,
                    const lagcomp::EvaluationState& source) noexcept {
    const auto mode = source.record.header.resolver_index;
    const auto yaw = source.record.header.player_history_1ac;
    if (mode == 1) {
        field(&owner, 0x3c, yaw);
        field(&owner, 0x60, real(yaw) + 180.0f);
    } else if (mode == 2) {
        field(&owner, 0x60, yaw);
        field(&owner, 0x3c, real(yaw) + 180.0f);
    }
}
}

void capture_shot_weapon_bullet(void* system, std::uint16_t identifier,
                                client::CommandBullet& destination) {
    capture_weapon_bullet(system, identifier, &destination);
}

void shot_capture_target(ShotFeedbackRecord& destination, const lagcomp::EvaluationState& source,
                         const RageVector& origin, std::uint8_t low_flags,
                         const PendingShotPublicationBindings& bindings) {
    const auto index = source.words[1];
    bindings.players.get(index);
    const auto* target = engine::invoke<void*>(bindings.entity_list, 3, index);
    if (!target)
        return;
    destination.words[0] = (destination.words[0] & ~1U) | low_flags;
    destination.words[6] =
        std::uint32_t(lagcomp::time_to_tick(*bindings.prediction_time, *bindings.interval));
    for (std::size_t axis = 0; axis < 3; ++axis)
        destination.words[9 + axis] = bits(origin[axis]);
    destination.words[12] =
        std::uint32_t(std::int32_t(field<std::int16_t>(target, bindings.offsets.model_index)));
    destination.words[13] = field<std::uint32_t>(target, bindings.offsets.hitbox_set);
    std::memcpy(&destination.state, &source, sizeof source);
}

bool publish_pending_shot(const client::CommandFrame& frame, const lagcomp::EvaluationState& source,
                          std::int32_t shot_index, const PendingShotPublicationBindings& bindings) {
    auto* appended = append_current_pending_shot(bindings);
    if (!appended)
        return false;
    auto& shot = *appended;
    shot.words[1] = std::uint32_t(frame.command_number_ac);
    shot.words[3] = std::uint32_t(frame.command_number_ac);
    shot.words[4] = bits(*bindings.current_time);
    shot.words[2] = std::uint32_t(engine::invoke<std::int32_t>(bindings.engine_client, 205));
    shot.words[5] = std::uint32_t(shot_index);
    shot.words[14] = 0x4dc70629U;
    shot.words[15] = 0x09bfc248U;
    if (frame.weapon_address) {
        auto* weapon = reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address));
        if (const auto* name = engine::invoke<const char*>(weapon, 459))
            shot.words[14] = events::event_hash(name);
        if (const auto* name = engine::invoke<const char*>(weapon, 143))
            shot.words[15] = events::event_hash(weapon_prefix(name) ? name + 7 : name);
        shot.words[7] = *engine::invoke<const std::uint32_t*>(weapon, 2);
        shot.words[8] = convar_nonzero(bindings.life_count_convar)
                            ? UINT32_MAX
                            : field<std::uint32_t>(weapon, bindings.offsets.life_count) - 1U;
        capture_weapon_bullet(bindings.weapon_system, std::uint16_t(frame.weapon_id_d8),
                              shot.words.data() + 27);
        shot.words[21] = frame.field_100;
        shot.words[22] = bits(minimum_number(1.0f, frame.inaccuracy_c4));
        shot.words[23] = bits(frame.spread_d0);
        for (std::size_t axis = 0; axis < 3; ++axis)
            shot.words[24 + axis] = bits(frame.recoil_angles_9c[axis]);
    } else {
        shot.words[7] = UINT32_MAX;
        shot.words[8] = UINT32_MAX;
    }
    shot_capture_target(shot, source, frame.origin_dc, 0, bindings);
    auto& player = *bindings.players.get(source.words[1]);
    const auto mode = source.record.header.resolver_index;
    field(&player.native, 0x14 + std::uint32_t(mode) * 0x24, bits(*bindings.current_time));
    write_mode_yaw(player.native, source);
    // Publication is not feedback. Wait for the actual shot ray before
    // rejecting a pose; a pending high-chance shot has not missed yet.
    if (bindings.resolver_rework)
        return true;
    if (bindings.jitter_fix) {
        const auto* evidence = player.jitter.find(source.record.header);
        if (evidence && evidence->applied_to(source.record.header))
            return true;
    }
    if (!(shot.state.words[13] & 4U) || !shot.state.record.header.resolver_index ||
        !(real(shot.state.words[14]) > 0.9f) ||
        integer(shot.state.words[10]) < integer(shot.state.words[2]))
        return true;
    shot.words[0] |= 0x80U;
    if (*bindings.scheduler_flags & 0x40U)
        return true;
    RageVector direction;
    const RageVector delta{real(shot.state.words[3]) - real(shot.words[9]),
                           real(shot.state.words[4]) - real(shot.words[10]),
                           real(shot.state.words[5]) - real(shot.words[11])};
    const auto length =
        std::sqrt((delta[0] * delta[0] + delta[1] * delta[1]) + delta[2] * delta[2]);
    const auto inverse = 1.0f / (length + std::numeric_limits<float>::epsilon());
    for (std::size_t axis = 0; axis < 3; ++axis)
        direction[axis] = inverse * delta[axis];
    const auto previous = shot.state.record.header.resolver_mode;
    const resolver_runtime::AttemptShot attempt_shot{shot.state.record, shot.words[0],
                                                     shot.state.words[13], shot.state.words[1],
                                                     real(shot.words[29])};
    const RageVector origin{real(shot.words[9]), real(shot.words[10]), real(shot.words[11])};
    const auto next = resolver_runtime::choose_next_attempt(
        {field<float>(&player.native, 0x88), field<float>(&player.native, 0x1a0)}, attempt_shot,
        *bindings.current_time, bindings.recording_enabled != 0, &origin, &direction,
        bindings.attempts);
    player.native.resolver_slots[mode].attempt = next;
    if (previous == next)
        return true;
    player.native.resolver_status = 0;
    shot.words[0] |= 0x40U;
    for (std::uint32_t age = 0; age < player.history.count; ++age) {
        auto& record = *player.history.records[(player.history.first + age) & 31U];
        if (record.header.resolver_index == shot.state.record.header.resolver_index &&
            record.header.resolver_mode == previous)
            record.header.flags |= 0x40U;
        if (record.header.simulation_tick < integer(shot.state.words[45]))
            break;
    }
    return true;
}
}
