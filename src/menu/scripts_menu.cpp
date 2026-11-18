#include "skeet/menu/scripts_menu.hpp"
#include "skeet/menu/config_menu.hpp"
#include "skeet/config_storage/presets.hpp"
#include "skeet/script/manager.hpp"
#include <algorithm>
#include <bit>
#include <utility>

namespace skeet::ui {

ScriptsMenu::ScriptsMenu(Tab& tab, WidgetIdentities& identities, ScriptsMenuValues values,
                         script::ScriptManager& manager)
    : tab_(tab), category_(tab, identities), values_(values), manager_(manager) {
    storage_.reserve(8);
    bindings_.reserve(8);
    auto& enabled = add<Checkbox>(category_, 0, u"Enabled", &values_.enabled, identities);
    enabled.callbacks.push_back({this, enabled_changed});
    auto& unsafe =
        add<Checkbox>(category_, 0, u"Allow unsafe scripts", &values_.allow_unsafe, identities);
    unsafe.callbacks.push_back({this, unsafe_changed});
    add<Button>(category_, 0, u"Reload active scripts", Point{158, 28},
                WidgetCallback{this, reload_active}, identities);
    list_ = &add<Listbox>(category_, 0, u"Scripts", Point{158, 126}, nullptr, true, identities);
    list_->callbacks.push_back({this, selection_changed});
    list_->selection_callbacks.push_back({this, script_selected});
    list_->enabled = true;
    category_.scripts = list_;
    updated_ = &add<Label>(category_, 0, u" ", identities);
    startup_ = &add<Checkbox>(category_, 0, u"Load on startup", nullptr, identities);
    startup_->callbacks.push_back({this, startup_changed});
    load_ = &add<Button>(category_, 0, u"Load script", Point{158, 28}, WidgetCallback{this, load},
                         identities);
    load_->set_visible(false);
    unload_ = &add<Button>(category_, 0, u"Unload script", Point{158, 28},
                           WidgetCallback{this, unload}, identities);
    unload_->set_visible(false);
    tab_.categories.push_back(&category_);
}

ScriptsMenu::~ScriptsMenu() {
    if (presets_) {
        std::erase_if(presets_->list().selection_callbacks, [this](const auto& callback) {
            return callback.context == this;
        });
        presets_->selection_refreshed = {};
    }
    std::erase(tab_.categories, &category_);
}

void ScriptsMenu::bind_presets(ConfigPresetsMenu& presets) {
    presets_ = &presets;
    presets.list().selection_callbacks.push_back({this, preset_selected});
    presets.selection_refreshed = {this, preset_refreshed};
    update_startup_selection();
}

void ScriptsMenu::refresh() { // 0x4336D863
    auto discovered =
        script::discover_scripts(manager_.builtins_, manager_.files_, manager_.settings_.get());
    startup_->value = nullptr;
    startup_->config_loaded();
    std::vector<ListboxOption> options;
    // The native owner vector is replaced only for nonempty discovery. Its
    // visible list is cleared even when the previous owner records survive.
    if (!discovered.empty()) {
        manager_.records_ = std::move(discovered);
        const auto records = manager_.records();
        options.reserve(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            auto marker = static_cast<std::uint32_t>(i);
            if (manager_.ready() && manager_.loaded(records[i].name))
                marker |= 0x80000000u;
            options.push_back({std::bit_cast<std::int32_t>(marker), records[i].name});
        }
    }
    list_->set_options(&options);
    update_startup_selection();
}

void ScriptsMenu::activate() {
    if (!values_.enabled) {
        update_startup_selection();
        return;
    }
    refresh();
    update_selection();
}

void ScriptsMenu::update_selection() { // 0x433F1A21
    if (!values_.enabled)
        return;
    bool can_load = false, can_unload = false;
    const auto records = manager_.records();
    const auto index = std::bit_cast<std::uint32_t>(list_->selected_item);
    if (!list_->filtered_indices.empty() && index < records.size()) {
        auto& record = records[index];
        can_unload = manager_.ready() && manager_.loaded(record.name);
        can_load = !can_unload;
        updated_->name = script_update_age(record.modified_time, read_time(clock_context));
    }
    load_->set_visible(can_load);
    unload_->set_visible(can_unload);
    update_startup_selection();
}

void ScriptsMenu::update_startup_selection() {
    startup_value_ = 0;
    bool available = false;
    if (preset_target_ && presets_) {
        auto& presets = presets_->manager();
        available = !presets_->list().filtered_indices.empty() &&
                    presets.selected_index < presets.entries().size();
        startup_value_ = available && presets.selected_for_startup();
        startup_->name = u"Load config on startup";
    } else {
        const auto records = manager_.records();
        const auto index = std::bit_cast<std::uint32_t>(list_->selected_item);
        available = values_.enabled && !list_->filtered_indices.empty() && index < records.size();
        if (available)
            startup_value_ = records[index].startup;
        startup_->name = u"Load on startup";
    }
    // The checkbox owns its displayed byte; discovery may replace script records.
    startup_->value = available ? &startup_value_ : nullptr;
    startup_->enabled = available;
    startup_->config_loaded();
}

void ScriptsMenu::preset_selected(void* context, Widget&) {
    auto& self = *static_cast<ScriptsMenu*>(context);
    self.preset_target_ = true;
    self.update_startup_selection();
}

void ScriptsMenu::preset_refreshed(void* context, Widget&) {
    static_cast<ScriptsMenu*>(context)->update_startup_selection();
}

void ScriptsMenu::enabled_changed(void* context, Widget& source) { // 0x43382337
    auto& self = *static_cast<ScriptsMenu*>(context);
    if (self.values_.enabled == self.values_.effective_enabled)
        return;
    self.values_.effective_enabled = self.values_.enabled;
    for (auto* control : self.category_.elements)
        if (control != &source)
            control->enabled = self.values_.enabled != 0;
    if (self.values_.enabled) {
        self.manager_.initialize();
        self.refresh();
        self.update_selection();
    } else if (self.manager_.ready()) {
        self.manager_.shutdown();
    }
    self.update_startup_selection();
}

void ScriptsMenu::unsafe_changed(void* context, Widget&) { // 0x43386459
    auto& self = *static_cast<ScriptsMenu*>(context);
    if (self.values_.allow_unsafe != self.values_.effective_unsafe) {
        self.values_.effective_unsafe = self.values_.allow_unsafe;
        self.manager_.persist_settings();
    }
}

void ScriptsMenu::reload_active(void* context, Widget&) { // 0x43359BC3
    static_cast<ScriptsMenu*>(context)->values_.reload_requested = 1;
}

void ScriptsMenu::selection_changed(void* context, Widget&) { // 0x433E1D52
    static_cast<ScriptsMenu*>(context)->update_selection();
}
void ScriptsMenu::script_selected(void* context, Widget&) {
    auto& self = *static_cast<ScriptsMenu*>(context);
    self.preset_target_ = false;
    self.update_selection();
}

void ScriptsMenu::startup_changed(void* context, Widget& source) { // 0x433A125F
    auto& self = *static_cast<ScriptsMenu*>(context);
    if (!static_cast<Checkbox&>(source).value)
        return;
    if (self.preset_target_ && self.presets_) {
        self.presets_->manager().set_selected_startup(self.startup_value_ != 0);
    } else {
        const auto records = self.manager_.records();
        const auto index = std::bit_cast<std::uint32_t>(self.list_->selected_item);
        if (self.values_.enabled && !self.list_->filtered_indices.empty() && index < records.size()) {
            records[index].startup = self.startup_value_;
            self.manager_.startup_changed();
        }
    }
    self.update_startup_selection();
}

void ScriptsMenu::load(void* context, Widget&) { // 0x43349FD6
    auto& self = *static_cast<ScriptsMenu*>(context);
    if (self.list_->filtered_indices.empty())
        return;
    auto& option = self.list_->options[std::uint32_t(self.list_->selected_item)];
    const auto index = std::bit_cast<std::uint32_t>(option.value) & 0x7FFFFFFFu;
    const auto records = self.manager_.records();
    if (index >= records.size())
        return;
    if (self.manager_.load(index) && self.manager_.ready() &&
        self.manager_.loaded(records[index].name))
        option.value =
            std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(option.value) | 0x80000000u);
    self.update_selection();
}

void ScriptsMenu::unload(void* context, Widget&) { // 0x433D5B5F
    auto& self = *static_cast<ScriptsMenu*>(context);
    if (self.list_->filtered_indices.empty())
        return;
    auto& option = self.list_->options[std::uint32_t(self.list_->selected_item)];
    const auto index = std::bit_cast<std::uint32_t>(option.value) & 0x7FFFFFFFu;
    if (index >= self.manager_.records().size())
        return;
    self.manager_.unload(index);
    option.value = std::int32_t(std::bit_cast<std::uint32_t>(option.value) & 0x7FFFFFFFu);
    self.update_selection();
}

} // namespace skeet::ui
