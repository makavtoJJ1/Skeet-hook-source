#include "skeet/menu/config_menu.hpp"
#include "skeet/config_storage/presets.hpp"
#include <algorithm>
#include <bit>
#include <immintrin.h>
#include <stdexcept>

namespace skeet::ui {

ConfigPresetsMenu::ConfigPresetsMenu(Tab& tab, WidgetIdentities& identities,
                                     ConfigPresetsManager& manager)
    : tab_(tab), category_(&tab, u"Presets", {0, 12, 0, 24}, 8, true, identities),
      manager_(manager) {
    if (manager_.menu_)
        throw std::logic_error("Preset manager already has a native configuration menu");
    storage_.reserve(9);
    bindings_.reserve(9);
    list_ = &add<Listbox>(category_, 0, u"Presets", Point{158, 126}, nullptr, false, identities);
    list_->callbacks.push_back({this, selection_changed});
    list_->enabled = true;
    hotkey_ = &add<HotkeyWidget>(category_, 0, nullptr, false, identities);
    hotkey_->set_visible(false);
    name_ = &add<Textbox>(category_, 0, identities);
    add<Button>(category_, 0, u"Load", Point{158, 28}, WidgetCallback{this, load}, identities);
    add<Button>(category_, 0, u"Save", Point{158, 28}, WidgetCallback{this, save}, identities);
    add<Button>(category_, 0, u"Delete", Point{158, 28}, WidgetCallback{this, erase}, identities);
    add<Button>(category_, 0, u"Reset", Point{158, 28}, WidgetCallback{this, reset}, identities);
    add<Button>(category_, 0, u"Import from clipboard", Point{158, 28},
                WidgetCallback{this, import_from_clipboard}, identities);
    add<Button>(category_, 0, u"Export to clipboard", Point{158, 28},
                WidgetCallback{this, export_to_clipboard}, identities);
    tab.categories.push_back(&category_);
    manager_.menu_ = this;
}

ConfigPresetsMenu::~ConfigPresetsMenu() {
    if (manager_.menu_ == this)
        manager_.menu_ = nullptr;
    std::erase(tab_.categories, &category_);
}

void ConfigPresetsMenu::read_name() {
    std::array<char16_t, 65> text{};
    const auto length = name_->copy_text(text);
    manager_.name.assign(text.data(), length);
}

void ConfigPresetsMenu::synchronize_from_manager() {
    const auto& entries = manager_.entries();
    std::vector<ListboxOption> options;
    options.reserve(entries.size());
    for (const auto& entry : entries)
        options.push_back({std::bit_cast<std::int32_t>(entry.marker), entry.name});
    list_->set_options(&options);
    synchronize_selection();
}

void ConfigPresetsMenu::update_markers() noexcept {
    for (std::size_t i = 0; i < list_->options.size(); ++i) {
        auto& option = list_->options[i];
        const auto marker =
            static_cast<std::uint32_t>(i) |
            (config_preset_hash(option.name) == manager_.active_hash ? 0x80000000u : 0u);
        option.value = std::bit_cast<std::int32_t>(marker);
    }
}

void ConfigPresetsMenu::synchronize_selection() {
    RegisteredHotkey* selected_key = nullptr;
    manager_.selected_index = std::bit_cast<std::uint32_t>(list_->selected_item);
    if (!list_->filtered_indices.empty()) {
        const auto& selected_name = list_->options[manager_.selected_index].name;
        // 0x4333C5F3 leaves the textbox untouched above 63 UTF-16 units.
        if (selected_name.size() <= 63) {
            while (name_->text_lock.exchange(1))
                _mm_pause();
            std::copy(selected_name.begin(), selected_name.end(), name_->text.begin());
            name_->text[selected_name.size()] = 0;
            name_->text_length.exchange(static_cast<std::uint32_t>(selected_name.size()));
            name_->text_lock.exchange(0);
        }
        const auto entries = manager_.entries();
        if (manager_.selected_index < entries.size())
            selected_key = entries[manager_.selected_index].hotkey;
    }
    read_name();
    hotkey_->value = selected_key;
    hotkey_->config_loaded();
    hotkey_->set_visible(hotkey_->value != nullptr);
    hotkey_->enabled = hotkey_->value != nullptr;
    if (selection_refreshed.function)
        selection_refreshed.function(selection_refreshed.context, *list_);
}

void ConfigPresetsMenu::refresh() {
    read_name();
    manager_.refresh();
}

void ConfigPresetsMenu::load_hotkey(RegisteredHotkey& hotkey) {
    const auto entries = manager_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].hotkey != &hotkey)
            continue;
        list_->set_selection(static_cast<std::uint32_t>(i));
        selection_changed(this, *list_);
        load(this, *list_);
        return;
    }
}

void ConfigPresetsMenu::selection_changed(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.synchronize_selection();
}

void ConfigPresetsMenu::load(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.read_name();
    self.manager_.load();
}
void ConfigPresetsMenu::save(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.read_name();
    self.manager_.save();
}
void ConfigPresetsMenu::erase(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.read_name();
    self.manager_.erase();
}
void ConfigPresetsMenu::reset(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.read_name();
    self.manager_.reset();
}
void ConfigPresetsMenu::import_from_clipboard(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.read_name();
    self.manager_.import_from_clipboard();
}
void ConfigPresetsMenu::export_to_clipboard(void* context, Widget&) {
    auto& self = *static_cast<ConfigPresetsMenu*>(context);
    self.read_name();
    self.manager_.export_to_clipboard();
}

} // namespace skeet::ui
