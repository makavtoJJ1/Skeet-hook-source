#include "skeet/menu/rage_weapon_groups.hpp"
#include "skeet/menu/category_config.hpp"

namespace skeet::ui {
void switch_rage_weapon_group(RageWeaponGroupState& state, const std::uint32_t* selected_group) {
    const auto previous = state.last_group;
    const auto selected = selected_group ? *selected_group : 0u;
    if (selected >= 12 || selected == previous)
        return;

    if (previous < 12) {
        ConfigBytes changes;
        save_category_config(state.window, state.registry, "rage", "aimbot", changes,
                             state.baseline_a.bytes(), state.baseline_b.bytes());
        // The original deliberately retains the stored group when the current
        // values serialize to the header alone; it does not clear that group.
        if (changes.size() > 12)
            state.groups[previous]->value = changes;
    }
    load_category_config(state.window, state.registry, "rage", "aimbot",
                         state.groups[selected]->value, state.baseline_a.bytes(),
                         state.baseline_b.bytes());
    state.last_group = selected;
}
void reload_rage_weapon_groups_from_preset(RageWeaponGroupState& state,
                                           const std::uint32_t& selected, bool initialize_global) {
    state.last_group = UINT32_MAX;
    if (selected >= state.groups.size())
        return;
    if (initialize_global && state.groups[0]->value.empty()) {
        ConfigBytes changes;
        save_category_config(state.window, state.registry, "rage", "aimbot", changes,
                             state.baseline_a.bytes(), state.baseline_b.bytes());
        if (changes.size() > 12)
            state.groups[0]->value = std::move(changes);
    }
    switch_rage_weapon_group(state, &selected);
}
}
