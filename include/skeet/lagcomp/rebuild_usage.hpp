#pragma once

namespace skeet::lagcomp {
struct AnimationRebuildUsage {
    bool matrices = false;
    bool replay = false;
};
struct RebuildUsage {
    AnimationRebuildUsage animation{};
    bool extrapolated_pose = false;
};
}
