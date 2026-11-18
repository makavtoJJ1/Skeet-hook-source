#include "skeet/esp/grenade_prediction.hpp"
#include <cmath>

namespace skeet::esp {
bool begin_grenade_trajectory(GrenadeTrajectoryOwner& owner, const GrenadeEspConfig& config,
                              const GrenadeTrajectoryFrame& frame) noexcept {
    if (frame.throw_input.compensate_movement)
        owner.smoothed_velocity = frame.motion_target;
    else {
        owner.smoothed_velocity.x += (frame.motion_target.x - owner.smoothed_velocity.x) * 0.2f;
        owner.smoothed_velocity.y += (frame.motion_target.y - owner.smoothed_velocity.y) * 0.2f;
        owner.smoothed_velocity.z += (frame.motion_target.z - owner.smoothed_velocity.z) * 0.2f;
    }
    if (frame.engine_reject_148 || frame.engine_reject_174 || frame.game_player_override)
        return false;
    if (!config.trajectory.value || !frame.player_entity || frame.movement_type == 10 ||
        frame.life_state != 0)
        return false;
    if (!frame.selected_is_local && std::abs(frame.field_e23a9be1) > 50.0f)
        return false;
    if (!frame.weapon_present)
        return false;
    switch (frame.kind) {
    case GrenadeKind::flash:
    case GrenadeKind::explosive:
    case GrenadeKind::smoke:
    case GrenadeKind::molotov:
    case GrenadeKind::decoy:
    case GrenadeKind::incendiary:
    case GrenadeKind::tactical:
    case GrenadeKind::diversion:
        break;
    default:
        return false;
    }
    if (frame.selected_is_local) {
        if (frame.field_9e2078a1 == 190)
            return false;
    } else if (frame.throw_time > 0.0f)
        return false;
    return true;
}
} // namespace skeet::esp
