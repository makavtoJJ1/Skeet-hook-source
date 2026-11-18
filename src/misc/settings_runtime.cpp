#include "skeet/misc/settings_runtime.hpp"
#include "skeet/config_storage/settings_persistence.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/engine/native_data_image.hpp"
#include <algorithm>
#include <bit>

namespace skeet::misc {
namespace {
void restore_grid(ui::Category& category, std::uint32_t packed) {
    category.column = std::bit_cast<std::int8_t>(std::uint8_t(packed));
    category.column_span = std::bit_cast<std::int8_t>(std::uint8_t(packed >> 8));
    category.row = std::bit_cast<std::int8_t>(std::uint8_t(packed >> 16));
    category.row_span = std::bit_cast<std::int8_t>(std::uint8_t(packed >> 24));
}
}

SettingsRuntime::SettingsRuntime() noexcept
    : previous_dpi_scale(standalone_previous_dpi_), previous_lock_layout(standalone_previous_lock_),
      previous_custom_events(standalone_previous_custom_events_), engine(standalone_engine_),
      custom_event_state(standalone_event_manager_), default_layout(standalone_layout_) {}
SettingsRuntime::SettingsRuntime(std::int32_t& previous_dpi, std::uint8_t& previous_lock,
                                 std::uint8_t& previous_custom, void*& engine_client,
                                 void*& event_manager, std::array<std::int32_t, 21>& layout,
                                 std::array<std::uint32_t, 2>& additional) noexcept
    : reset_targets(additional), previous_dpi_scale(previous_dpi),
      previous_lock_layout(previous_lock), previous_custom_events(previous_custom),
      engine(engine_client), custom_event_state(event_manager), default_layout(layout) {}

void initialize_source_settings_runtime_data(engine::NativeDataImage& data) {
    data.native_cell<std::int32_t>(0x4347CA04u) = 0;
    data.native_cell<std::uint8_t>(0x4346D550u) = 1;
    data.native_cell<std::uint8_t>(0x4347CA38u) = 0;
    data.native_cell<std::array<std::int32_t, 21>>(0x43468CB0u) = {9};
    data.native_cell<std::array<std::uint32_t, 2>>(0x43476B34u) = {};
}

void SettingsRuntime::dpi_scale_changed() { // 0x433379D4
    gui_dpi->refresh(false);
    lock_layout_changed();
}

void SettingsRuntime::lock_layout_changed() { // 0x4332A6C5
    const auto current_dpi = *dpi_scale;
    const auto current_lock = *lock_layout;
    if (previous_dpi_scale == current_dpi && previous_lock_layout == current_lock)
        return;
    previous_dpi_scale = current_dpi;
    previous_lock_layout = current_lock;
    persistence->persist(0, 0); // 0x4340B9C9, module gate and exact registry snapshot
}

void SettingsRuntime::allow_custom_events_changed() { // 0x4333D25B
    // Native4333D25B treats an absent widget value pointer as zero.
    const auto value = allow_custom_events ? *allow_custom_events : std::uint8_t{};
    if (value != previous_custom_events && engine::invoke<bool>(engine, 26))
        *(static_cast<std::uint8_t*>(custom_event_state) + 0xA0) = 1;
    previous_custom_events = value;
}
void SettingsRuntime::unlock_hidden_cvars_changed() {
    hidden_cvars.set_enabled(unlock_hidden_cvars && *unlock_hidden_cvars);
}

void SettingsRuntime::reset_menu_layout() { // 0x43342FF1 with native built-in block
    if (default_layout[0] == 9) {
        // The original intentionally ignores record slots 1, 2 and 16..18.
        constexpr std::size_t indices[] = {3, 5, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 19, 20};
        for (std::size_t i = 0; i < std::size(indices); ++i)
            restore_grid(*reset_targets.categories[i], std::uint32_t(default_layout[indices[i]]));
        for (std::size_t i = 0; i < reset_targets.additional_categories.size(); ++i)
            restore_grid(*reset_targets.additional_categories[i],
                         reset_targets.additional_defaults[i]);
    }
    for (auto* tab : main_menu->tabs) {
        tab->layout_categories();
        tab->reflow_categories();
        tab->resize(main_menu->input.slider_metrics);
    }
}

void SettingsRuntime::max_usercmd_ticks_changed(std::int32_t value) { // 0x433EE188
    const auto decremented = std::bit_cast<std::int32_t>(std::uint32_t(value) - 1u);
    const auto maximum = std::clamp(decremented, 2, 60);
    fakelag_limit->maximum = maximum;
    auto* setting = fakelag_limit->value;
    if (setting && maximum < *setting)
        __atomic_exchange_n(setting, maximum, __ATOMIC_SEQ_CST);
    fakelag_limit->value_changed();
}
} // namespace skeet::misc
