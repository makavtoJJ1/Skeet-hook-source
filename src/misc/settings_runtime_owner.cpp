#include "skeet/misc/settings_runtime_owner.hpp"
#include "skeet/misc/unload.hpp"
#include "skeet/misc/config.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/config_storage/owner.hpp"
#include "skeet/menu/startup_window.hpp"
#include "skeet/menu/startup_data.hpp"
#include "skeet/menu/root_menu.hpp"
#include <array>
#include <stdexcept>

namespace skeet::misc {
namespace {
client::ClientRuntimeStorage& require_owners(client::ClientRuntimeStorage& storage,
                                             client::ConfigurationRuntimeOwner& settings,
                                             ui::MainMenuWindowOwner& window,
                                             ConfigStorageOwner& configuration,
                                             UnloadRuntime& unload) {
    auto& data = storage.data();
    auto& menu = window.window();
    if (&settings.storage() != &storage || &window.menu_data().data() != &data ||
        &configuration.native_storage() != &storage.config_storage() ||
        &configuration.configuration() != &storage.configuration() ||
        &storage.menu_startup_data().dpi != &window.menu_data().dpi || &unload.main_menu != &menu ||
        unload.input != &storage.input_runtime() || storage.input_runtime().main_menu != &menu ||
        unload.config_registry != &storage.configuration().descriptors || !unload.hooks ||
        &unload.hooks->state.owners != &storage.owner_registries().at(0x43475B78u) ||
        &unload.hooks->state.engine != &data.native_cell<void*>(0x4346A514u))
        throw std::invalid_argument(
            "Settings callbacks require the same source window, CONFIG, input and unload owners");
    if (!menu.event_state || !data.native_cell<void*>(0x4346A514u) ||
        !data.native_cell<void*>(0x43468364u) ||
        &settings.settings_menu().dpi_scale != &window.menu_data().dpi.selection ||
        &settings.settings_menu().lock_layout != &menu.event_state->window_size_locked ||
        &settings.custom_events().enabled.value != &data.native_cell<std::uint8_t>(0x434691B8u) ||
        data.native_cell<std::array<std::int32_t, 21>>(0x43468CB0u)[0] != 9)
        throw std::invalid_argument(
            "Settings callbacks require initialized canonical engine, setting and default-layout cells");
    return storage;
}
}
SettingsRuntimeOwner::SettingsRuntimeOwner(client::ClientRuntimeStorage& storage,
                                           client::ConfigurationRuntimeOwner& settings,
                                           ui::MainMenuWindowOwner& window,
                                           ConfigStorageOwner& configuration, UnloadRuntime& unload)
    : storage_(require_owners(storage, settings, window, configuration, unload)), window_(window),
      configuration_(configuration),
      runtime_(storage.data().native_cell<std::int32_t>(0x4347CA04u),
               storage.data().native_cell<std::uint8_t>(0x4346D550u),
               storage.data().native_cell<std::uint8_t>(0x4347CA38u),
               storage.data().native_cell<void*>(0x4346A514u),
               storage.data().native_cell<void*>(0x43468364u),
               storage.data().native_cell<std::array<std::int32_t, 21>>(0x43468CB0u),
               storage.data().native_cell<std::array<std::uint32_t, 2>>(0x43476B34u)) {
    runtime_.gui_dpi = &window.menu_data().dpi;
    runtime_.main_menu = &window.window();
    runtime_.persistence = &configuration.persistence();
    runtime_.unload = &unload;
    runtime_.dpi_scale = &settings.settings_menu().dpi_scale;
    runtime_.lock_layout = &settings.settings_menu().lock_layout;
    runtime_.allow_custom_events = &settings.custom_events().enabled.value;
    runtime_.hidden_cvars.cvar = storage.data().native_cell<void*>(0x4346A87Cu);
}
void SettingsRuntimeOwner::capture_default_layout(ui::RootMenu& menu) {
    if (layout_captured_)
        throw std::logic_error("The original menu default layout has already been captured");
    if (&menu.window() != &window_.window() || runtime_.main_menu != &menu.window() ||
        !runtime_.fakelag_limit)
        throw std::invalid_argument(
            "Default layout capture requires the actual constructed RootMenu");
    const auto targets = menu.layout_targets();
    constexpr std::array<std::size_t, 15> indices{2,  3,  4,  5,  6,  7,  8, 9,
                                                  10, 11, 12, 13, 14, 15, 16};
    for (std::size_t index = 0; index < indices.size(); ++index)
        if (!runtime_.reset_targets.categories[index] ||
            runtime_.reset_targets.categories[index] != targets.categories[indices[index]])
            throw std::invalid_argument(
                "Default layout capture requires RootMenu's own category bindings");
    for (std::size_t index = 0; index < targets.players.size(); ++index)
        if (!runtime_.reset_targets.additional_categories[index] ||
            runtime_.reset_targets.additional_categories[index] != targets.players[index])
            throw std::invalid_argument(
                "Default layout capture requires RootMenu's own player categories");
    menu.capture_layout(runtime_.default_layout);
    layout_captured_ = true;
}
} // namespace skeet::misc
