#include "skeet/config_storage/settings_capture.hpp"
#include "skeet/menu/root_menu.hpp"
#include "skeet/menu/misc_menu.hpp"
#include "skeet/menu/scripts_menu.hpp"
#include "skeet/script/manager.hpp"
#include "skeet/script/modules.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace skeet {
MenuSettingsCapture::MenuSettingsCapture(const ui::MenuWindow& window,
                                         ui::SettingsMenuValues settings,
                                         ui::ScriptsMenuValues scripts) noexcept
    : window_(window), menu_color_(settings.menu_color), menu_key_(settings.menu_key),
      lock_layout_(settings.lock_layout), dpi_scale_(settings.dpi_scale),
      allow_unsafe_(scripts.allow_unsafe) {}

void MenuSettingsCapture::bind_menu(const ui::RootMenu& menu,
                                    const script::ScriptManager& scripts) {
    if (&menu.rage_group_state().window != &window_)
        throw std::invalid_argument("Settings capture and menu must share their window");
    if (&menu.script_modules().manager() != &scripts)
        throw std::invalid_argument("Settings capture requires the menu's Lua manager");
    menu_ = &menu;
    scripts_ = &scripts;
}

SettingsPersistenceInputs MenuSettingsCapture::operator()() const { //433EEC8B
    if (!menu_ || !scripts_)
        throw std::logic_error("Settings capture requires its constructed menu and script manager");
    SettingsPersistenceInputs result;
    result.global_value = std::bit_cast<std::int32_t>(menu_color_);
    result.menu_key = menu_key_.binding.packed;
    result.input_mode = lock_layout_;
    result.dpi_scale = dpi_scale_;
    const auto size = window_.size_for_persistence();
    result.menu_width = size.x;
    result.menu_height = size.y;

    std::array<std::int32_t, 21> layout{};
    std::array<std::uint32_t, 2> player_defaults{};
    ui::capture_menu_layout(menu_->layout_targets(), layout, false, player_defaults);
    std::copy_n(layout.begin() + 1, 15, result.menu_control_values.begin());
    result.menu_control_values[15] = layout[19];
    result.menu_control_values[16] = layout[20];

    const auto records = scripts_->records();
    // Lua may be disabled before its directory is discovered. A config/menu
    // preference must not erase the saved script startup list in that case.
    result.capture_scripts = !records.empty();
    result.scripts.reserve(records.size());
    for (const auto& record : records)
        result.scripts.push_back({record.name, record.startup != 0});
    result.flag = allow_unsafe_;
    return result;
}
}
