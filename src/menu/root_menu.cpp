#include "skeet/menu/root_menu.hpp"
#include "skeet/config_storage/load_state.hpp"
#include "skeet/menu/category_config.hpp"
#include "skeet/menu/anti_aim_menu.hpp"
#include "skeet/menu/anti_aim_extra_menu.hpp"
#include "skeet/menu/legit_menu.hpp"
#include "skeet/menu/rage_menu.hpp"
#include "skeet/rage/group_storage.hpp"
#include "skeet/menu/skins_menu.hpp"
#include "skeet/menu/players_menu.hpp"
#include "skeet/menu/config_menu.hpp"
#include "skeet/misc/settings_runtime.hpp"
#include "skeet/skins/native_catalog.hpp"
#include "skeet/script/owner.hpp"
#include "skeet/render/icon_cache.hpp"
#include "skeet/render/backend_container.hpp"
#include "skeet/render/menu_window_assets.hpp"
#include "skeet/render/dpi_bridge.hpp"
#include "skeet/input/runtime.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace skeet::ui {

namespace {
struct ScopedTabConfigLoad {
    const bool previous = std::exchange(config_loading_flag(), true);
    ~ScopedTabConfigLoad() { config_loading_flag() = previous; }
};
void aa_notification(MenuWindow&, std::uint32_t);
struct AANotification {
    MenuWindow& window;
    WindowNotification node{0x16, aa_notification, nullptr};
    explicit AANotification(MenuWindow& owner) : window(owner) {
        if (window.notifications.size() < 2)
            throw std::logic_error("Main menu requires initialized window notifications");
        const auto bucket = node.key % (window.notifications.size() - 1);
        node.next = window.notifications[bucket];
        window.notifications[bucket] = &node;
    }
    ~AANotification() {
        if (window.notifications.size() < 2)
            return;
        auto** link = &window.notifications[node.key % (window.notifications.size() - 1)];
        while (*link && *link != window.notifications.back() && *link != &node)
            link = &(*link)->next;
        if (*link == &node)
            *link = node.next;
    }
};
struct RootRageTab final : Tab {
    RootMenuBindings& bindings;
    std::uint32_t selected = 0;
    RageWeaponGroupState group_state;
    RageWeaponClipboardContext clipboard;
    RageWeaponMenu weapon;
    RageAimbotMenu aimbot;
    RageOtherMenu other;
    RootRageTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"RAGE", next, b.identities),
          bindings(b), group_state{window,
                                   b.configuration.descriptors,
                                   b.rage_groups.blob_descriptors(),
                                   b.baseline_a,
                                   b.baseline_b,
                                   0},
          clipboard{group_state, &selected, &b.clipboard},
          weapon(*this, selected, b.rage_groups.enabled_descriptors(),
                 rage_weapon_clipboard_actions(clipboard), b.identities),
          aimbot(*this, b.configuration, b.identities),
          other(*this, b.configuration, b.identities) {
        weapon.selector().icons = b.icons.selector_provider();
    }
    void activate() override {
        update_rage_hit_chance_visibility(*aimbot.find(0x3f5e5553), bindings.engine,
                                          bindings.accuracy_variable);
    }
};
struct RootAntiAimTab final : Tab {
    AANotification notification;
    AntiAimMenu angles;
    FakeLagMenu fake_lag;
    AntiAimOtherMenu other;
    RootAntiAimTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"AA", next, b.identities),
          notification(window), angles(*this, b.configuration, b.identities),
          fake_lag(*this, b.configuration, b.identities),
          other(*this, b.configuration, b.identities) {}
};
void aa_notification(MenuWindow& window, std::uint32_t) { // 4334191F
    if (window.tabs.size() > static_cast<std::size_t>(RootTab::anti_aim))
        static_cast<RootAntiAimTab*>(window.tabs[static_cast<std::size_t>(RootTab::anti_aim)])
            ->fake_lag.limit_slider()
            .update_value_text();
}
struct RootLegitTab final : Tab {
    LegitMenu menu;
    RootLegitTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"LEGIT", next, b.identities),
          menu(*this, b.configuration, b.identities) {
        menu.weapon_selector().icons = b.icons.weapon_provider();
    }
};
struct RootVisualsTab final : Tab {
    PlayerEspMenu player;
    ColoredModelsMenu models;
    OtherEspMenu other;
    EffectsMenu effects;
    RootVisualsTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"VISUALS", next, b.identities),
          player(*this, b.configuration, b.identities),
          models(*this, b.configuration, b.identities), other(*this, b.configuration, b.identities),
          effects(*this, b.configuration, b.identities, b.camera) {}
};
struct RootMiscTab final : Tab {
    RootMenuBindings& bindings;
    MiscellaneousMenu miscellaneous;
    MovementMenu movement;
    SettingsMenu settings;
    RootMiscTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"MISC", next, b.identities),
          bindings(b), miscellaneous(*this, b.configuration, b.identities, b.miscellaneous,
                                     b.upgrade_tablet, b.expose_tablet),
          movement(*this, b.configuration, b.identities),
          settings(*this, b.configuration, b.identities, b.settings_values, b.settings_runtime) {}
    void activate() override { // 433F4A39
        auto* slider = settings.max_usercmd_ticks_slider();
        if (!slider || !slider->visible ||
            bindings.settings_values.maximum_usercmd_ticks ==
                bindings.globals.previous_max_usercmd_ticks)
            return;
        bindings.globals.previous_max_usercmd_ticks =
            bindings.settings_values.maximum_usercmd_ticks;
        slider->value_changed();
    }
};
struct RootSkinsTab final : Tab {
    SkinsMenu menu;
    RootSkinsTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"SKINS", next, b.identities),
          menu(*this, b.configuration, b.identities, b.skins_catalog, b.skins_refresh) {}
    void activate() override {
        menu.activate();
    }
};
struct RootPlayersTab final : Tab {
    PlayersMenu menu;
    RootPlayersTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"PLAYERS", next, b.identities),
          menu(*this, b.identities, b.player_state, b.player_bindings) {}
    void activate() override {
        menu.activate();
    }
};
struct RootConfigTab final : Tab {
    ConfigPresetsMenu presets;
    ScriptsMenu scripts;
    RootConfigTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"CONFIG", next, b.identities),
          presets(*this, b.identities, b.presets),
          scripts(*this, b.identities, b.script_values, b.scripts.modules().manager()) {
        scripts.bind_presets(presets);
        presets.refresh();
    }
    void activate() override {
        scripts.activate();
    }
};
struct RootLuaTab final : Tab {
    std::unique_ptr<Category> first, second;
    RootLuaTab(MenuWindow& window, RootMenuBindings& b, Point& next)
        : Tab(window.navigation, window.input, window.geometry, u"Lua", next, b.identities) {}
    void construct_categories(WidgetIdentities& identities) {
        first =
            std::make_unique<Category>(this, u"A", GridPosition{0, 12, 0, 24}, 0, true, identities);
        categories.push_back(first.get());
        second = std::make_unique<Category>(this, u"B", GridPosition{12, 12, 0, 24}, 0, true,
                                            identities);
        categories.push_back(second.get());
    }
};
void reset_tab(void* raw) {
    static_cast<Tab*>(raw)->reset_input();
}
void activate_tab(void* raw) {
    static_cast<Tab*>(raw)->activate();
}
}

struct RootMenu::Data {
    struct TabConfigPopup : PopupMenu {
        Data& owner;
        TabConfigPopup(Data& data, WidgetIdentities& identities)
            : PopupMenu(nullptr, {u"Copy", u"Paste"}, identities), owner(data) {
            selected = [](Widget& tab, PopupMenu& popup, std::uint32_t action) {
                static_cast<TabConfigPopup&>(popup).owner.clipboard_action(static_cast<Tab&>(tab),
                                                                          action);
            };
        }
    };
    MenuWindow& window;
    RootMenuBindings bindings;
    script::UiBindings& script_ui;
    script::ClientBindings& script_client;
    std::optional<TabConfigPopup> tab_config_popup;
    MenuWindow* window_address;
    PlayersMenu* player_menu_address = nullptr;
    MainMenuAssets assets;
    std::optional<RendererDpiBridge> renderer_dpi;
    std::array<MenuTab, 9> navigation{};
    RootRageTab* rage = nullptr;
    RootAntiAimTab* anti_aim = nullptr;
    RootLegitTab* legit = nullptr;
    RootVisualsTab* visuals = nullptr;
    RootMiscTab* misc = nullptr;
    RootSkinsTab* skins = nullptr;
    RootPlayersTab* players = nullptr;
    RootConfigTab* config = nullptr;
    RootLuaTab* lua = nullptr;
    Data(MenuWindow& w, RootMenuBindings b)
        : window(w), bindings(b), script_ui(b.scripts.modules().ui_bindings()),
          script_client(b.scripts.modules().client().bindings), window_address(&w) {}
    template <class Type> Type* add(Point& next) {
        auto tab = std::make_unique<Type>(window, bindings, next);
        auto* result = tab.get();
        append_menu_tab(window, std::move(tab));
        return result;
    }
    std::vector<ConfigDescriptor*> tab_descriptors(Tab& tab) {
        auto descriptors = collect_tab_config(tab, bindings.configuration.descriptors);
        if (&tab == rage) {
            const auto enabled = bindings.rage_groups.enabled_descriptors();
            descriptors.insert(descriptors.end(), enabled.begin(), enabled.end());
            const auto groups = bindings.rage_groups.blob_descriptors();
            descriptors.insert(descriptors.end(), groups.begin(), groups.end());
        } else if (&tab == legit) {
            legit->menu.append_weapon_keys(descriptors);
        } else if (&tab == skins) {
            skins->menu.append_team_config(descriptors);
        }
        for (const auto& record : script_ui.records)
            if (record.widget && record.configuration && owning_tab(record.widget) == &tab)
                descriptors.push_back(record.configuration);
        std::vector<ConfigDescriptor*> scope;
        scope.reserve(descriptors.size());
        for (auto* descriptor : descriptors) {
            auto& header = config_native_header(*descriptor);
            if (config_value_supported(header) && std::ranges::find(scope, &header) == scope.end())
                scope.push_back(&header);
        }
        return scope;
    }
    void copy_tab_config(Tab& tab, std::span<ConfigDescriptor*> descriptors) {
        ConfigBytes group;
        const ConfigDescriptor* active_group = nullptr;
        if (&tab == rage && rage->selected < rage->group_state.groups.size()) {
            auto& state = rage->group_state;
            if (save_category_config(window, state.registry, "rage", "aimbot", group,
                                     state.baseline_a.bytes(), state.baseline_b.bytes()) &&
                group.size() < UINT16_MAX)
                active_group = &config_native_header(*state.groups[rage->selected]);
        }
        ConfigBytes stream;
        initialize_config_stream(stream);
        for (const auto* descriptor : descriptors) {
            if (descriptor == active_group) {
                auto key = config_record_key(*descriptor);
                key.size = static_cast<std::uint16_t>(group.size());
                append_config_record(stream, key, group);
            } else {
                serialize_config_value(*descriptor, stream);
            }
        }
        write_config_clipboard(stream, bindings.clipboard, window.native_window);
    }
    void paste_tab_config(Tab& tab, std::span<ConfigDescriptor*> descriptors) {
        ConfigBytes stream;
        read_config_clipboard(stream, bindings.clipboard);
        if (!valid_config_stream(stream))
            return;
        // A header-only preset represents defaults. Other streams must
        // contain at least one setting belonging to the clicked tab.
        const auto contains_setting = [&](const ConfigDescriptor* descriptor) {
            return find_config_record(stream, config_record_key(*descriptor)).has_value();
        };
        if (stream.size() > 12 && !std::ranges::any_of(descriptors, contains_setting))
            return;
        if (&tab == rage) {
            for (const auto* group : bindings.rage_groups.blob_descriptors()) {
                const auto record = find_config_record(stream, config_record_key(*group));
                if (record && !record->value.empty() && !valid_config_stream(record->value))
                    return;
            }
        }
        ScopedTabConfigLoad loading;
        load_category_config_values(descriptors, stream, bindings.baseline_a.bytes(),
                                    bindings.baseline_b.bytes());
        if (&tab == rage) {
            const auto* global = rage->group_state.groups[0];
            reload_rage_weapon_groups_from_preset(rage->group_state, rage->selected,
                                                  !contains_setting(global));
        }
        tab.config_loaded();
    }
    void clipboard_action(Tab& tab, std::uint32_t action) {
        auto descriptors = tab_descriptors(tab);
        if (descriptors.empty())
            return;
        if (action == 0)
            copy_tab_config(tab, descriptors);
        else if (action == 1)
            paste_tab_config(tab, descriptors);
    }
    RootMenuLayoutTargets targets() const {
        const auto legit_categories = legit->menu.categories();
        return {{{&rage->aimbot.category(), &rage->other.category(), &anti_aim->angles.category(),
                  &anti_aim->other.category(), &anti_aim->fake_lag.category(), legit_categories[1],
                  legit_categories[2], legit_categories[3], &visuals->player.category(),
                  &visuals->models.category(), &visuals->other.category(),
                  &visuals->effects.category(), &misc->miscellaneous.category(),
                  &misc->movement.category(), &misc->settings.category(),
                  &config->presets.category(), &config->scripts.category()}},
                {{&players->menu.players_category(), &players->menu.adjustments_category()}}};
    }
};
RootMenu::RootMenu(MenuWindow& window, RootMenuBindings bindings)
    : data_(std::make_unique<Data>(window, bindings)) {
    auto& modules = bindings.scripts.modules();
    if (!bindings.baseline_a.same_source(bindings.presets.baseline_view()) ||
        !bindings.baseline_b.is_script_state(modules.shutdown_state()))
        throw std::invalid_argument(
            "Main menu requires the preset baseline and its script owner's live configuration");
    modules.validate_menu(window, bindings.configuration, bindings.identities,
                          bindings.player_state, bindings.input);
    modules.validate_renderer_binding(bindings.renderer, bindings.dpi);
    modules.bind_script_settings(bindings.script_values);
    if (!window.tabs.empty())
        throw std::invalid_argument("Main menu composition requires its initialized empty window");
    if (bindings.input.main_menu != &window)
        throw std::invalid_argument("Main menu composition requires the canonical input window");
    if ((bindings.input.hotkey_registry &&
         bindings.input.hotkey_registry != &bindings.configuration.hotkeys) ||
        (bindings.input.menu_hotkey &&
         bindings.input.menu_hotkey != &bindings.settings_values.menu_key))
        throw std::invalid_argument("Main menu and input must use the same registered menu hotkey");
    if (static_cast<const void*>(&bindings.settings_values.lock_layout) !=
        static_cast<const void*>(&window.event_state->window_size_locked))
        throw std::invalid_argument("Main menu layout lock must use the native window-state byte");
    if (&bindings.settings_values.dpi_scale != &bindings.dpi.selection)
        throw std::invalid_argument("Main menu DPI must use the shared settings value");
    bindings.input.bind_configuration(bindings.configuration, bindings.settings_values.menu_key);
    auto& d = *data_;
    d.renderer_dpi.emplace(bindings.dpi, bindings.renderer);
    bindings.settings_runtime.gui_dpi = &bindings.dpi;
    bindings.settings_runtime.main_menu = &window;
    bindings.settings_runtime.engine = bindings.engine;
    window.tabs.reserve(9);
    Point next{6, 20};
    d.rage = d.add<RootRageTab>(next);
    d.anti_aim = d.add<RootAntiAimTab>(next);
    bindings.settings_runtime.fakelag_limit = &d.anti_aim->fake_lag.limit_slider();
    d.legit = d.add<RootLegitTab>(next);
    d.visuals = d.add<RootVisualsTab>(next);
    d.misc = d.add<RootMiscTab>(next);
    d.skins = d.add<RootSkinsTab>(next);
    d.players = d.add<RootPlayersTab>(next);
    d.player_menu_address = &d.players->menu;
    d.config = d.add<RootConfigTab>(next);
    d.lua = d.add<RootLuaTab>(next);
    d.lua->construct_categories(bindings.identities);
    d.lua->set_visible(false);
    bindings.skins_catalog.initialize(d.skins->menu);
    d.lua->resize(window.input.slider_metrics);
    const auto targets = d.targets();
    constexpr std::array<std::size_t, 15> reset_indices{2,  3,  4,  5,  6,  7,  8, 9,
                                                        10, 11, 12, 13, 14, 15, 16};
    for (std::size_t i = 0; i < reset_indices.size(); ++i)
        bindings.settings_runtime.reset_targets.categories[i] =
            targets.categories[reset_indices[i]];
    bindings.settings_runtime.reset_targets.additional_categories = targets.players;
    for (std::size_t i = 0; i < d.navigation.size(); ++i)
        d.navigation[i] = {window.tabs[i], reset_tab, activate_tab};
    window.navigation.tabs = d.navigation;
    modules.bind_menu(window, bindings.configuration, bindings.identities, bindings.player_state,
                      bindings.input, d.players->menu);
    auto& popup = d.tab_config_popup.emplace(d, bindings.identities);
    popup.dpi_changed(window.input.scale, window.input.slider_metrics);
    window.tab_context_menu = &popup;
}
RootMenu::~RootMenu() {
    auto& d = *data_;
    if (d.tab_config_popup && d.window.tab_context_menu == &*d.tab_config_popup)
        d.window.tab_context_menu = nullptr;
    if (d.bindings.input.main_menu == d.window_address &&
        d.window.navigation.tabs.data() == d.navigation.data())
        d.window.navigation.tabs = {};
    if (d.script_ui.window == d.window_address)
        d.script_ui.window = nullptr;
    if (d.script_client.players_menu == d.player_menu_address)
        d.script_client.players_menu = nullptr;
}
Tab& RootMenu::tab(RootTab index) const {
    return *data_->window.tabs.at(static_cast<std::size_t>(index));
}
const std::uintptr_t& RootMenu::native_window() const noexcept {
    return data_->window.native_window;
}
MenuWindow& RootMenu::window() const noexcept {
    return data_->window;
}
RageWeaponMenu& RootMenu::rage_weapon_menu() const {
    return data_->rage->weapon;
}
RageWeaponGroupState& RootMenu::rage_group_state() const {
    return data_->rage->group_state;
}
std::uint32_t& RootMenu::selected_rage_group() const {
    return data_->rage->selected;
}
SkinsMenu& RootMenu::skins_menu() const {
    return data_->skins->menu;
}
LegitMenu& RootMenu::legit_menu() const {
    return data_->legit->menu;
}
PlayersMenu& RootMenu::players_menu() const {
    return data_->players->menu;
}
ConfigPresetsMenu& RootMenu::presets_menu() const {
    return data_->config->presets;
}
ConfigPresetsManager& RootMenu::preset_manager() const noexcept {
    return data_->bindings.presets;
}
ScriptsMenu& RootMenu::scripts_menu() const {
    return data_->config->scripts;
}
script::ScriptModulesOwner& RootMenu::script_modules() const noexcept {
    return data_->bindings.scripts.modules();
}
script::ScriptOwner& RootMenu::script_owner() const noexcept {
    return data_->bindings.scripts;
}
void RootMenu::initialize_renderer_resources(RendererContainer& renderer) {
    initialize_main_menu_assets(data_->window, renderer.backend(), data_->assets);
}
std::uint32_t RootMenu::tab_atlas_texture() const noexcept {
    return data_->assets.tab_atlas;
}
RootMenuLayoutTargets RootMenu::layout_targets() const {
    return data_->targets();
}
void RootMenu::capture_layout(std::array<std::int32_t, 21>& destination) {
    auto& runtime = data_->bindings.settings_runtime;
    capture_menu_layout(data_->targets(), destination, &destination == &runtime.default_layout,
                        runtime.reset_targets.additional_defaults);
}
}
