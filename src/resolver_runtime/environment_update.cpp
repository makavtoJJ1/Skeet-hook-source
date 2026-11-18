#include "skeet/resolver_runtime/environment_update.hpp"
#include "skeet/animation/bone_runtime.hpp"
#include "skeet/client/player_state.hpp"
#include <bit>
#include <cmath>
#include <cstring>

namespace skeet::resolver_runtime {
namespace {
template <class T>
[[gnu::always_inline]] inline T read(const void* object, std::ptrdiff_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
struct RestoreSuppression {
    std::uint8_t* flag;
    std::uint8_t saved = 0;
    explicit RestoreSuppression(std::uint8_t* pointer) : flag(pointer) {
        if (flag != nullptr) {
            saved = *flag;
            *flag = 0;
        }
    }
    ~RestoreSuppression() {
        if (flag != nullptr)
            *flag = saved;
    }
};
float squared_distance(const RageVector& a, const RageVector& b) {
    const float y = a[1] - b[1], x = a[0] - b[0], z = a[2] - b[2];
    return (y * y + x * x) + z * z;
}
struct OwnerView {
    std::array<std::uint8_t*, 4> attempts;
    std::uint8_t& status;
    float& probe_time;
    float& probe_stamp;
    std::int32_t& probe_count;
    float& reference_time;
    RageVector& reference_origin;
    std::int32_t& last_update_tick;
    float& reference_yaw;
};
OwnerView view(EnvironmentOwner& owner) noexcept {
    auto& attempts = owner.selection.attempt_by_mode;
    return {{&attempts[0], &attempts[1], &attempts[2], &attempts[3]},
            owner.selection.status,
            owner.probe_time,
            owner.probe_stamp,
            owner.probe_count,
            owner.reference_time,
            owner.reference_origin,
            owner.last_update_tick,
            owner.reference_yaw};
}
OwnerView view(client::PlayerState& owner) noexcept {
    auto& state = owner.native;
    return {{&state.resolver_slots[0].attempt, &state.resolver_slots[1].attempt,
             &state.resolver_slots[2].attempt, &state.resolver_slots[3].attempt},
            state.resolver_status,
            state.probe_time,
            state.probe_stamp,
            state.probe_count,
            state.reference_time,
            state.reference_origin,
            state.last_update_tick,
            state.reference_yaw};
}
struct CapturedRequest {
    const EnvironmentUpdateInput& input;
    const LagRecordHistory& records;
    bool strict() const noexcept { return input.strict; }
    LagRecordHistory history() const noexcept {
        return records;
    }
    [[gnu::always_inline]] inline float local_time() const noexcept {
        return input.local_time;
    }
    [[gnu::always_inline]] inline float current_time() const noexcept {
        return input.current_time;
    }
    std::int32_t tick() const noexcept {
        return lagcomp::corrected_command_tick(input.command_tick, input.previous_command,
                                               input.current_command);
    }
    RageVector local_origin() const noexcept {
        return input.local_origin;
    }
    RageVector target_origin() const noexcept {
        return input.target_origin;
    }
    const std::array<EnvironmentCapsule, 2>* hitboxes() const noexcept {
        return input.hitboxes;
    }
};
struct LiveRequest {
    client::PlayerState& owner;
    const EnvironmentLiveUpdateInput& input;
    std::array<EnvironmentCapsule, 2> capsules;
    bool strict() const noexcept { return input.strict; }
    LagRecordHistory history() const noexcept {
        return {owner.history.records, owner.history.first, owner.history.count};
    }
    [[gnu::always_inline]] inline float local_time() const noexcept {
        const auto* chain = input.local_time_entity;
        return chain && *chain ? read<float>(*chain, input.local_time_offset) : 0.0f;
    }
    [[gnu::always_inline]] inline float current_time() const noexcept {
        return input.current_time;
    }
    std::int32_t tick() const noexcept {
        return lagcomp::corrected_command_tick(input.command_tick, input.previous_command,
                                               input.current_command);
    }
    RageVector local_origin() const noexcept {
        return input.local_origin;
    }
    RageVector target_origin() const noexcept {
        return read<RageVector>(input.target, input.target_origin_offset);
    }
    const std::array<EnvironmentCapsule, 2>* hitboxes() noexcept {
        //43349119..4334928B:433FD30D returns the current wrapper/header, then
        // the current selected set supplies descriptors0 and3. This lookup is
        // deliberately after every world trace and penetration query.
        const auto& fields = input.bones.fields;
        const auto* wrapper = read<const void*>(input.target, fields.studio_wrapper_32e4daf1);
        if (!wrapper)
            return nullptr;
        const auto* header = read<const std::byte*>(wrapper, 0);
        if (!header)
            return nullptr;
        const auto relative =
            std::uint32_t(read<std::int32_t>(input.target, fields.hitbox_set_c2b6fb10)) * 12u +
            std::uint32_t(read<std::int32_t>(header, 0xB0));
        const auto* set = header + std::bit_cast<std::int32_t>(relative);
        if (!set)
            return nullptr;
        const auto count = read<std::int32_t>(set, 4);
        if (count == 0 || (input.strict && count < 4))
            return nullptr;
        for (std::size_t index = 0; index != 2; ++index) {
            const auto ordinal = index == 0 ? 0u : 3u;
            const auto offset = std::uint32_t(read<std::int32_t>(set, 8)) + ordinal * 0x44u;
            const auto* descriptor = set + std::bit_cast<std::int32_t>(offset);
            if (!descriptor)
                return nullptr;
            const auto radius = read<float>(descriptor, 0x30);
            if (!(radius > 0.0f))
                return nullptr;
            capsules[index] = {ordinal, read<RageVector>(descriptor, 8),
                               read<RageVector>(descriptor, 0x14), radius};
        }
        return &capsules;
    }
};
template <class Request>
bool ready(OwnerView owner, std::uint32_t count, std::uint8_t resolver_index,
           const Request& input) noexcept {
    if (count == 0 || (resolver_index != 1 && resolver_index != 2) || owner.status == 3)
        return false;
    const auto local_time = input.local_time();
    if (local_time == owner.reference_time && owner.status == 2 &&
        squared_distance(owner.reference_origin, input.target_origin()) < 10000.0f)
        return false;
    if (local_time == owner.probe_time &&
        !(std::fabs(input.current_time() - owner.probe_stamp) > 5.0f))
        return false;
    return true;
}
void publish(OwnerView owner, LagRecordHistory& history, lagcomp::LagRecord& source,
             std::uint8_t selected_mode, std::uint8_t previous_attempt,
             std::uint8_t attempt) noexcept {
    // The first destination slot was selected before the probe. The later
    // movement-mode synchronization reads the source mode again at49D92.
    *owner.attempts[selected_mode] = attempt;
    owner.status = 1;
    const auto mode = source.header.resolver_index;
    if (mode == 1)
        *owner.attempts[2] = attempt;
    else if (mode == 2)
        *owner.attempts[1] = attempt;
    apply_feedback(history, source, previous_attempt, attempt);
}
template <class Request>
bool update(OwnerView owner, Request& input, const EnvironmentEngine& engine) {
    RestoreSuppression suppression(engine.suppression_flag);
    const auto tick = input.tick();
    if (tick <= owner.last_update_tick)
        return false;
    owner.last_update_tick = tick;
    const auto history = input.history();
    for (std::uint32_t index = 0; index != history.count; ++index) {
        const auto flags = history.records[(history.first + index) & 31U]->header.flags;
        if ((flags & 0x200U) != 0)
            break;
        if ((flags & 0x20U) != 0)
            return false;
    }
    auto& source = *history.records[history.first & 31U];
    const auto& header = source.header;
    const auto mode = header.resolver_index;
    if (tick <= header.command_tick || (header.flags & 0x22U) != 0 || (mode != 1 && mode != 2))
        return false;
    const auto previous_attempt = input.strict() ? header.resolver_mode : *owner.attempts[mode];
    const auto local_time = input.local_time();
    if (local_time == owner.probe_time &&
        (owner.probe_count >= 2 || std::fabs(input.current_time() - owner.probe_stamp) < 5.0f))
        return false;
    if (local_time == owner.reference_time && owner.status == 2 &&
        squared_distance(owner.reference_origin, input.target_origin()) < 10000.0f)
        return false;
    if ((header.flags & 0x80U) == 0)
        return false;
    const auto local_origin = input.local_origin();
    const auto probe = probe_environment(local_origin, input.target_origin(), engine);
    if (!probe.selected)
        return false;

    const float yaw_delta = owner.reference_yaw - std::bit_cast<float>(header.player_history_1ac);
    const auto target_origin = input.target_origin();
    const bool rebase = yaw_delta != 0.0f || header.origin[0] != target_origin[0] ||
                        header.origin[1] != target_origin[1] ||
                        header.origin[2] != target_origin[2];
    const auto* hitboxes = input.hitboxes();
    if (hitboxes == nullptr)
        return false;
    for (const auto& hitbox : *hitboxes)
        if (!(hitbox.radius > 0.0f))
            return false;
    const RageVector direction{-probe.direction[0], -probe.direction[1], -probe.direction[2]};
    const RageVector origin{local_origin[0], local_origin[1], local_origin[2] + 64.0f};
    EnvironmentSelection selection;
    if (rebase) {
        const EnvironmentRebase correction{yaw_delta, header.origin, target_origin};
        selection = score_environment_hypotheses(source, *hitboxes, direction, origin, correction,
                                                 engine.trigonometry, input.strict());
    } else
        selection = score_environment_hypotheses(source, *hitboxes, direction, origin, input.strict());
    if (input.strict() && !selection.valid())
        return false;
    owner.probe_time = local_time;
    owner.last_update_tick = input.tick();
    auto publication_history = input.history();
    publish(owner, publication_history, source, mode, previous_attempt,
            std::uint8_t(selection.attempt));
    return true;
}
}
bool environment_update_ready(EnvironmentOwner& owner, const LagRecordHistory& history,
                              std::uint8_t resolver_index,
                              const EnvironmentUpdateInput& input) noexcept {
    const CapturedRequest request{input, history};
    return ready(view(owner), history.count, resolver_index, request);
}
bool environment_update_ready(client::PlayerState& owner,
                              const EnvironmentLiveUpdateInput& input) noexcept {
    const LiveRequest request{owner, input, {}};
    return ready(view(owner), owner.history.count, owner.native.resolver_index, request);
}
bool update_environment(EnvironmentOwner& owner, LagRecordHistory& history,
                        const EnvironmentUpdateInput& input, const EnvironmentEngine& engine) {
    CapturedRequest request{input, history};
    return update(view(owner), request, engine);
}
bool update_environment(client::PlayerState& owner, const EnvironmentLiveUpdateInput& input,
                        const EnvironmentEngine& engine) {
    LiveRequest request{owner, input, {}};
    return update(view(owner), request, engine);
}
} // namespace skeet::resolver_runtime
