#include "skeet/menu/misc_menu.hpp"
#include "skeet/misc/settings_runtime.hpp"
#include "config_binding.hpp"
#include <algorithm>

namespace skeet::ui {
using namespace menu_detail;
namespace {
constexpr Requirement required[] = {
    {0x851E6ABE, ConfigType::boolean},
    {0x526A2BF4, ConfigType::boolean},
    {0x643364E6, ConfigType::boolean},
    {0x740C32AE, ConfigType::boolean},
};
}
SettingsMenu::SettingsMenu(Tab& tab, ConfigRegistries& settings, WidgetIdentities& identities,
                           SettingsMenuValues values, misc::SettingsRuntime& runtime)
    : tab_(tab), category_(&tab, u"Settings", {12, 12, 12, 12}, 8, true, identities),
      runtime_(runtime) {
    require_settings(settings, required);
    runtime_.dpi_scale = &values.dpi_scale;
    runtime_.lock_layout = &values.lock_layout;
    runtime_.allow_custom_events = value<BooleanConfig>(settings, 0x526A2BF4);
    initialize_boolean_config(settings.descriptors, unlock_hidden_cvars_, 0x331C4230u);
    runtime_.unlock_hidden_cvars = &unlock_hidden_cvars_.value;
    storage_.reserve(24);
    bindings_.reserve(24);
    tab.categories.push_back(&category_);
    auto checkbox = [&](std::uint32_t hash, std::u16string_view name) -> Checkbox& {
        return add<Checkbox>(category_, hash, name, value<BooleanConfig>(settings, hash),
                             identities);
    };
    auto direct_checkbox = [&](std::u16string_view name, std::uint8_t& setting) -> Checkbox& {
        return add<Checkbox>(category_, 0, name, &setting, identities);
    };
    auto slider = [&](std::u16string_view name, int low, int high, std::int32_t& setting,
                      std::uint32_t suffix) -> Slider& {
        return add<Slider>(category_, 0, name, low, high, &setting, true, suffix, 1.0f, identities);
    };
    add<Label>(category_, 0, u"Menu key", identities);
    add<HotkeyWidget>(category_, 0, &values.menu_key, false, identities);
    add<Label>(category_, 0, u"Menu color", identities);
    add<ColorPicker>(category_, 0, &values.menu_color, identities);
    auto& dpi = add<ComboBox>(category_, 0, u"DPI scale", &values.dpi_scale, false, identities);
    if (runtime_.gui_dpi)
        runtime_.gui_dpi->setting_widget = &dpi;
    dpi.set_options({{u"100%", 0}, {u"125%", 1}, {u"150%", 2}, {u"175%", 3}, {u"200%", 4}});
    dpi.callbacks.push_back({this, dpi_scale_changed});
    checkbox(0x851E6ABE, u"Anti-untrusted");
    add<Checkbox>(category_, 0x331C4230u, u"Unlock hidden cvars", &unlock_hidden_cvars_.value,
                  identities).callbacks.push_back({this, unlock_hidden_cvars_changed});
    auto& custom = checkbox(0x526A2BF4, u"Allow custom game events");
    custom.callbacks.push_back({this, allow_custom_events_changed});
    custom.color = 0xBB82F0F0;
    checkbox(0x643364E6, u"Hide from OBS");
    checkbox(0x740C32AE, u"Low FPS warning");
    direct_checkbox(u"Lock menu layout", values.lock_layout)
        .callbacks.push_back({this, lock_layout_changed});
    direct_checkbox(u"Multithreading", values.multithreading);
    add<Button>(category_, 0, u"Reset menu layout", Point{158, 28},
                WidgetCallback{this, reset_layout}, identities);
    add<Button>(category_, 0, u"Unload", Point{158, 28}, WidgetCallback{this, unload}, identities);
    auto& unlag = slider(u"sv_maxunlag2", 200, 1000, values.maximum_unlag, 0x0073006D);
    unlag.callbacks.push_back({this, maximum_unlag_changed});
    auto& ticks = slider(u"sv_maxusrcmdprocessticks2", 4, 62, values.maximum_usercmd_ticks, 0);
    maximum_usercmd_ticks_ = &ticks;
    ticks.callbacks.push_back({this, maximum_usercmd_ticks_changed});
    slider(u"sv_clockcorrection_msecs2", 0, 1000, values.clock_correction, 0x0073006D);
    direct_checkbox(u"sv_maxusrcmdprocessticks_holdaim", values.hold_aim);
    direct_checkbox(u"Fix viewmodel lag", values.fix_viewmodel_lag);
    direct_checkbox(u"Fix networking", values.fix_networking);
    direct_checkbox(u"Fix animations", values.fix_animations);
    direct_checkbox(u"Faster grenade toss", values.faster_grenade_toss);
}
SettingsMenu::~SettingsMenu() {
    runtime_.hidden_cvars.set_enabled(false);
    runtime_.unlock_hidden_cvars = nullptr;
    unlock_hidden_cvars_.previous->next = unlock_hidden_cvars_.next;
    unlock_hidden_cvars_.next->previous = unlock_hidden_cvars_.previous;
    std::erase(tab_.categories, &category_);
}
void SettingsMenu::dpi_scale_changed(void* context, Widget& widget) {
    widget.reset_input(); // source vfunc +0x18 in 0x433379D4
    auto& runtime = static_cast<SettingsMenu*>(context)->runtime_;
    runtime.dpi_scale_changed();
}
void SettingsMenu::allow_custom_events_changed(void* context, Widget&) {
    static_cast<SettingsMenu*>(context)->runtime_.allow_custom_events_changed();
}
void SettingsMenu::unlock_hidden_cvars_changed(void* context, Widget&) {
    static_cast<SettingsMenu*>(context)->runtime_.unlock_hidden_cvars_changed();
}
void SettingsMenu::lock_layout_changed(void* context, Widget&) {
    static_cast<SettingsMenu*>(context)->runtime_.lock_layout_changed();
}
void SettingsMenu::reset_layout(void* context, Widget&) {
    static_cast<SettingsMenu*>(context)->runtime_.reset_menu_layout();
}
void SettingsMenu::unload(void* context, Widget&) {
    static_cast<SettingsMenu*>(context)->runtime_.request_unload();
}
void SettingsMenu::maximum_unlag_changed(void*, Widget&) {
    // 0x433CCE8E is a native return instruction.
}
void SettingsMenu::maximum_usercmd_ticks_changed(void* context, Widget& widget) {
    auto* setting = static_cast<Slider&>(widget).value;
    static_cast<SettingsMenu*>(context)->runtime_.max_usercmd_ticks_changed(*setting);
}
} // namespace skeet::ui
