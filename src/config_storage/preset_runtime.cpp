#include "skeet/config_storage/preset_runtime.hpp"
#include "skeet/config_storage/settings_persistence.hpp"
#include "skeet/menu/category_config.hpp"
#include "skeet/menu/root_menu.hpp"
#include "skeet/script/manager.hpp"
#include "skeet/script/modules.hpp"
#include "skeet/script/ui.hpp"
#include <cstring>
#include <stdexcept>

namespace skeet {
namespace {
int no_arguments(script::Bridge&, void*) {
    return 0;
} // 43336942
}
PresetRuntime::PresetRuntime(ui::MenuWindow& window, script::Bridge& bridge,
                             script::UiBindings& script_ui, SettingsPersistence& persistence,
                             ui::RageClipboardApi& clipboard)
    : window_(window), bridge_(bridge), script_ui_(script_ui), persistence_(persistence),
      clipboard_(clipboard) {}

void PresetRuntime::bind_scripts(ConfigPresetsManager& presets, script::ScriptManager& scripts) {
    auto& state = scripts.shutdown_state();
    if (&state.bridge != &bridge_ || state.ui != &script_ui_ || &presets.services_ != this)
        throw std::invalid_argument(
            "Preset defaults require their actual script manager, bridge and UI owner");
    if ((presets_ && presets_ != &presets) ||
        (!script_defaults_.is_unbound() && !script_defaults_.is_script_state(state)))
        throw std::invalid_argument("Preset defaults are already bound to another owner");
    script_defaults_ = ConfigStreamView::script_state(state);
    presets_ = &presets;
    scripts.preserve_hotkey_toggle = &presets.preserve_hotkey_toggle;
}

void PresetRuntime::bind_menu(ui::RootMenu& menu, ConfigPresetsManager& presets,
                              script::ScriptManager& scripts) {
    auto& group = menu.rage_group_state();
    if (&group.window != &window_)
        throw std::invalid_argument("Preset runtime and RAGE must share their menu window");
    auto& modules = menu.script_modules();
    if (&modules.bridge() != &bridge_ || &modules.ui_bindings() != &script_ui_ ||
        &modules.manager() != &scripts || &menu.preset_manager() != &presets)
        throw std::invalid_argument(
            "Preset runtime and menu require the same Lua and configuration owners");
    modules.validate_config_binding(presets, *this);
    if (!group.baseline_a.same_source(presets.baseline_view()) ||
        !group.baseline_b.is_script_state(modules.shutdown_state()))
        throw std::invalid_argument(
            "Preset runtime and RAGE require the same live configuration streams");
    bind_scripts(presets, scripts);
    rage_ = &group;
    selected_group_ = &menu.selected_rage_group();
}
ui::RageWeaponGroupState& PresetRuntime::rage() const {
    if (!rage_ || !selected_group_)
        throw std::logic_error("Preset runtime requires its constructed RAGE menu");
    return *rage_;
}
const bool& PresetRuntime::preserve_hotkey_toggle() const {
    if (!presets_)
        throw std::logic_error("Preset runtime requires its preset manager");
    return presets_->preserve_hotkey_toggle;
}
void PresetRuntime::event(events::EventName name) {
    if (bridge_.state)
        script::dispatch_callbacks(bridge_, bridge_.name(name).get(), no_arguments, nullptr);
}
void PresetRuntime::before_baseline_load() { // 4340B293; name initialized at 43480995
    event(events::EventName::pre_config_load);
}
void PresetRuntime::loaded_event() { // 43349E9A; name initialized at 43480A0B
    event(events::EventName::post_config_load);
}
void PresetRuntime::saving_event(bool before) { // 433B5972 / 433B5A2B
    event(before ? events::EventName::pre_config_save : events::EventName::post_config_save);
}
void PresetRuntime::flush_active_weapon_group() { // 433EC9A4
    auto& state = rage();
    const auto selected = *selected_group_;
    if (selected >= state.groups.size())
        return;
    ConfigBytes changes;
    ui::save_category_config(window_, state.registry, "rage", "aimbot", changes,
                             state.baseline_a.bytes(), state.baseline_b.bytes());
    if (changes.size() > 12)
        state.groups[selected]->value = std::move(changes);
}
void PresetRuntime::after_baseline_load() { // 433AB1C2 -> 4337EABC
    const auto defaults = script_defaults_.bytes(); // capture this invocation's native range
    script::restore_script_ui_values(script_ui_, defaults, &preserve_hotkey_toggle());
}
void PresetRuntime::restore_script_defaults() { // 4337ED22..4337ED71
    if (!bridge_.state)
        return;
    const auto defaults = script_defaults_.bytes(); // range stays fixed during this traversal
    if (defaults.empty())
        return;
    const auto count = script_ui_.records.size();
    for (std::size_t index = 0; index < count; ++index) {
        auto& binding = script_ui_.records[index];
        if (!binding.borrowed && binding.configuration)
            deserialize_config_value(*binding.configuration, defaults, preserve_hotkey_toggle());
    }
}
void PresetRuntime::after_values_load() { // 4338A0E9
    for (auto* tab : std::span(window_.tabs))
        tab->config_loaded();
}
void PresetRuntime::after_named_load() { // 4332F0AC
    auto& state = rage();
    ui::reload_rage_weapon_groups_from_preset(state, *selected_group_);
}
void PresetRuntime::persist_settings() {
    (void)persistence_.persist(0, 0);
}

std::string PresetRuntime::read_clipboard_export() {
    auto& api = clipboard_;
    std::string result;
    if (!api.available(api.context, 1) || !api.open(api.context, 0))
        return result;
    if (const auto memory = api.get_data(api.context, 1)) {
        if (const auto text = api.wire(api.context, memory)) {
            const auto pointer = reinterpret_cast<std::uintptr_t>(text);
            const auto capacity = api.size(api.context, pointer);
            std::size_t length = 0;
            while (length < capacity && text[length])
                ++length;
            result.assign(text, length);
            api.unlock(api.context, pointer);
        }
    }
    api.close(api.context);
    return result;
}
void PresetRuntime::write_clipboard_export(std::string_view text) {
    auto& api = clipboard_;
    if (text.empty() || !api.open(api.context, 0))
        return;
    if (api.empty(api.context)) {
        if (const auto memory = api.allocate(api.context, 0, text.size() + 1)) {
            auto* target = reinterpret_cast<char*>(memory);
            std::memcpy(target, text.data(), text.size());
            target[text.size()] = '\0';
            if (!api.set_data(api.context, 1, memory))
                api.free(api.context, memory);
        }
    }
    api.close(api.context);
}
}
