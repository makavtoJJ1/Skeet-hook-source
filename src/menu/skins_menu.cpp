#include "skeet/menu/skins_menu.hpp"
#include "skeet/skins/widget_setters.hpp"
#include "skeet/skins/catalog_sort.hpp"
#include "config_binding.hpp"
#include <algorithm>
#include <stdexcept>

namespace skeet::ui {
using namespace menu_detail;
namespace {
constexpr Requirement required[] = {
    {0xDE88B6DC, ConfigType::boolean}, {0xF6D4D395, ConfigType::boolean},
    {0x588FC522, ConfigType::integer}, {0x82097AD3, ConfigType::integer},
    {0x3A8EF907, ConfigType::boolean}, {0xE5249CA0, ConfigType::boolean},
    {0x7439E8B5, ConfigType::integer}, {0x5101537E, ConfigType::integer},
    {0xE13F61E0, ConfigType::integer}, {0x7CFD0011, ConfigType::integer},
    {0x191A5A61, ConfigType::boolean}, {0x6CAF4832, ConfigType::boolean},
    {0xD347BC87, ConfigType::boolean}, {0xD4F2A320, ConfigType::boolean},
    {0xE0BDA969, ConfigType::integer}, {0xB3C4B6CA, ConfigType::integer},
    {0x3EB3F20B, ConfigType::integer}, {0x8191D9BC, ConfigType::integer},
    {0x9AB5333C, ConfigType::boolean}, {0xB6218FEC, ConfigType::integer_array},
};
}
SkinsMenu::SkinsMenu(Tab& tab, ConfigRegistries& settings, WidgetIdentities& identities,
                     skins::Catalog& catalog, skins::Refresh& refresh)
    : tab_(tab), settings_(settings), catalog_(catalog), refresh_(refresh),
      models_(&tab, u"Model options", {0, 12, 0, 24}, 0, false, identities) {
    require_settings(settings, required);
    if (descriptor(settings, 0xB6218FEC).serialized_size != 0x410)
        throw std::invalid_argument("SKINS requires the native 65-record configuration");
    auto boolean = [&](std::uint32_t h) { return value<BooleanConfig>(settings, h); };
    auto integer = [&](std::uint32_t h) { return value<IntegerConfig>(settings, h); };
    storage_.reserve(24);
    bindings_.reserve(24);
    tab.categories.push_back(&models_);
    team_ = &add<Slider>(models_, 0, u"Team", 2, 3, &selected_team_, true, 0, 1.0f, identities);
    team_->set_visible(false);
    team_->callbacks.push_back({this, team_changed}); // 0x433EB2C8
    knife_ = &add<Checkbox>(models_, 0xD347BC87, u"Knife changer", boolean(0xD347BC87), identities);
    knife_->callbacks.push_back({this, knife_enabled_changed});
    knife_model_ = &add<ComboBox>(models_, 0xE0BDA969, u"", integer(0xE0BDA969), false, identities);
    knife_model_->set_options({{u"Bayonet", 0},
                               {u"Flip", 1},
                               {u"Gut", 2},
                               {u"Karambit", 3},
                               {u"M9 Bayonet", 4},
                               {u"Tactical", 5},
                               {u"Butterfly", 6},
                               {u"Falchion", 7},
                               {u"Shadow dagger", 8},
                               {u"Survival bowie", 9},
                               {u"Ursus", 10},
                               {u"Navaja", 11},
                               {u"Stiletto", 12},
                               {u"Talon", 13},
                               {u"Classic knife", 14},
                               {u"Paracord", 15},
                               {u"Survival", 16},
                               {u"Nomad", 17},
                               {u"Skeleton", 18}});
    knife_model_->callbacks.push_back({this, knife_model_changed});
    knife_model_->set_visible(false);
    dependencies_[0] = {knife_model_, &knife_->tied};
    knife_->tied.next = &dependencies_[0];
    glove_ = &add<Checkbox>(models_, 0x3A8EF907, u"Glove changer", boolean(0x3A8EF907), identities);
    glove_->callbacks.push_back({this, glove_enabled_changed}); // 0x433D66B5
    glove_model_ = &add<ComboBox>(models_, 0x7439E8B5, u"", integer(0x7439E8B5), false, identities);
    glove_model_->callbacks.push_back({this, glove_model_changed}); // 0x4333CEA9
    glove_paint_ = &add<ComboBox>(models_, 0xE13F61E0, u"", integer(0xE13F61E0), false, identities);
    // 0x433CCE8E is an actual empty callback; no replacement work is attached.
    // The model and paint choices are supplied by the live item schema.
    glove_model_->set_visible(false);
    glove_paint_->set_visible(false);
    dependencies_[1] = {glove_paint_, &dependencies_[2]};
    dependencies_[2] = {glove_model_, &glove_->tied};
    glove_->tied.next = &dependencies_[1];
    mask_ = &add<Checkbox>(models_, 0xDE88B6DC, u"Mask changer", boolean(0xDE88B6DC), identities);
    mask_model_ = &add<ComboBox>(models_, 0x588FC522, u"", integer(0x588FC522), false, identities);
    mask_model_->set_options({{u"Hoxton", 0},     {u"Doll", 1},     {u"Samurai", 3},
                              {u"Clown", 4},      {u"Wolf", 5},     {u"Sheep", 6},
                              {u"Gold bunny", 7}, {u"Anaglyph", 8}, {u"Japanese", 9},
                              {u"Dallas", 10},    {u"Pumpkin", 11}, {u"Bloody sheep", 12},
                              {u"Devil", 13},     {u"Boar", 14},    {u"Chains", 15},
                              {u"Tiki", 16},      {u"Bunny", 17},   {u"Gold sheep", 18},
                              {u"Zombie", 19},    {u"Chicken", 20}, {u"Gold skull", 21}});
    mask_model_->set_visible(false);
    dependencies_[3] = {mask_model_, &mask_->tied};
    mask_->tied.next = &dependencies_[3];
    agent_ = &add<Checkbox>(models_, 0x191A5A61, u"Agent changer", boolean(0x191A5A61), identities);
    agent_->callbacks.push_back({this, agent_enabled_changed});
    agent_t_ = &add<ComboBox>(models_, 0x3EB3F20B, u"", integer(0x3EB3F20B), false, identities);
    agent_ct_ = &add<ComboBox>(models_, 0x8191D9BC, u"", integer(0x8191D9BC), false, identities);
    agent_t_->set_visible(false);
    agent_ct_->set_visible(false);
    weapon_ = std::make_unique<Category>(&tab, u"Weapon skin", GridPosition{12, 12, 0, 24}, 14,
                                         false, identities);
    tab.categories.push_back(weapon_.get());
    weapon_selector_ =
        &add<Slider>(*weapon_, 0, u"Weapon", 0, 1000, &selected_weapon_, true, 0, 1.0f, identities);
    weapon_selector_->set_visible(false);
    weapon_selector_->callbacks.push_back({this, weapon_changed});
    auto& record =
        static_cast<skins::WeaponSkinsConfig&>(descriptor(settings, 0xB6218FEC)).value[0];
    enabled_ = &add<Checkbox>(*weapon_, 0xB6218FEC, u"Enabled", &record.enabled, identities);
    enabled_->callbacks.push_back({this, enabled_changed});
    stattrak_ = &add<Checkbox>(*weapon_, 0xB6218FEC, u"StatTrak", &record.stattrak, identities);
    quality_ = &add<Slider>(*weapon_, 0xB6218FEC, u"Quality", 0, 100, &record.quality, true, 0x25,
                            1.0f, identities);
    seed_ = &add<Slider>(*weapon_, 0xB6218FEC, u"Seed", 0, 1000, &record.seed, true, 0, 1.0f,
                         identities);
    filter_ =
        &add<Checkbox>(*weapon_, 0x9AB5333C, u"Filter by weapon", boolean(0x9AB5333C), identities);
    filter_->callbacks.push_back({this, filter_changed});
    skin_ = &add<Listbox>(*weapon_, 0xB6218FEC, u"Skin", Point{158, 322}, &record.paintkit, true,
                          identities);
    skin_->callbacks.push_back({this, paint_changed});
}
SkinsMenu::~SkinsMenu() {
    for (auto* category : categories())
        std::erase(tab_.categories, category);
}
void SkinsMenu::append_team_config(std::vector<ConfigDescriptor*>& descriptors) const {
    for (const auto& setting : required)
        descriptors.push_back(&descriptor(settings_, setting.hash));
}
void SkinsMenu::refresh() {
    refresh_.invalidate_weapon_models();
    refresh_.schedule_full_update(refresh_.current_time() + 0.25f);
}
void SkinsMenu::select_weapon(std::int32_t item, std::int32_t team) {
    selected_weapon_ = item;
    const auto slot = skins::weapon_config_slot(item, team);
    if (slot < 0)
        return;
    auto& r = static_cast<skins::WeaponSkinsConfig&>(descriptor(settings_, 0xB6218FEC)).value[slot];
    enabled_->value = &r.enabled;
    enabled_->config_loaded();
    stattrak_->value = &r.stattrak;
    stattrak_->config_loaded();
    stattrak_->enabled = true;
    quality_->value = &r.quality;
    quality_->config_loaded();
    seed_->value = &r.seed;
    seed_->config_loaded();
    skin_->value = &r.paintkit;
    skin_->config_loaded();
    rebuild_skin_list();
}
void SkinsMenu::select_team(std::int32_t team) { // 0x4334A06E
    selected_team_ = team;
    auto previous_paint = UINT32_MAX;
    if (team == 2 || team == 3) {
        const bool ct = team == 3;
        previous_paint = *reinterpret_cast<std::uint32_t*>(
            value<IntegerConfig>(settings_, ct ? 0x7CFD0011 : 0xE13F61E0));
        mask_->value = value<BooleanConfig>(settings_, ct ? 0xF6D4D395 : 0xDE88B6DC);
        mask_->config_loaded();
        mask_model_->value = reinterpret_cast<std::uint32_t*>(
            value<IntegerConfig>(settings_, ct ? 0x82097AD3 : 0x588FC522));
        mask_model_->config_loaded();
        glove_->value = value<BooleanConfig>(settings_, ct ? 0xE5249CA0 : 0x3A8EF907);
        glove_->config_loaded();
        glove_paint_->value = reinterpret_cast<std::uint32_t*>(
            value<IntegerConfig>(settings_, ct ? 0x7CFD0011 : 0xE13F61E0));
        glove_paint_->config_loaded();
        glove_model_->value = reinterpret_cast<std::uint32_t*>(
            value<IntegerConfig>(settings_, ct ? 0x5101537E : 0x7439E8B5));
        glove_model_->config_loaded();
        knife_->value = value<BooleanConfig>(settings_, ct ? 0xD4F2A320 : 0xD347BC87);
        knife_->config_loaded();
        knife_model_->value = reinterpret_cast<std::uint32_t*>(
            value<IntegerConfig>(settings_, ct ? 0xB3C4B6CA : 0xE0BDA969));
        knife_model_->config_loaded();
        agent_->value = value<BooleanConfig>(settings_, ct ? 0x6CAF4832 : 0x191A5A61);
        agent_->config_loaded();
    }
    knife_state_ = knife_->value ? *knife_->value : 0;
    glove_state_ = glove_->value ? *glove_->value : 0;
    mask_state_ = mask_->value ? *mask_->value : 0;
    set_agent_team(team);
    const auto model = glove_model_->value ? *glove_model_->value : 0;
    rebuild_glove_paint_list(std::uint16_t(model), previous_paint);
}
void SkinsMenu::initialize_schema_options() {
    auto copy = [](std::span<const skins::CatalogChoice> source) {
        std::vector<ComboBoxOption> out;
        out.reserve(source.size());
        for (const auto& x : source)
            out.push_back({x.name, x.value});
        return out;
    };
    glove_model_->set_options(copy(catalog_.glove_models()));
    agent_t_->set_options(copy(catalog_.agent_models(2)));
    agent_ct_->set_options(copy(catalog_.agent_models(3)));
}
void SkinsMenu::rebuild_glove_paint_list() { // 0x43368201
    const auto model = glove_model_->value ? *glove_model_->value : 0;
    const auto previous = glove_paint_->value ? *glove_paint_->value : 0;
    rebuild_glove_paint_list(std::uint16_t(model), previous);
}
void SkinsMenu::rebuild_glove_paint_list(std::uint16_t model, std::uint32_t previous) {
    if (!glove_paint_)
        return;
    if (previous == UINT32_MAX)
        previous = glove_paint_->value ? *glove_paint_->value : 0;
    if (!catalog_.available())
        return;
    std::vector<ComboBoxOption> options;
    bool found = false;
    for (auto id : catalog_.paintkits(model)) {
        if (id == previous)
            found = true;
        if (auto name = catalog_.paintkit_name(id))
            options.push_back({std::move(*name), id});
    }
    skins::sort_catalog_choices(std::span<ComboBoxOption>(options));
    const auto selection = found ? previous : (options.empty() ? previous : options.front().value);
    glove_paint_->set_options(std::move(options));
    skins::set_combo_value_atomic(*glove_paint_, selection); // 43398ECF follows list replacement.
}
void SkinsMenu::activate() { // 0x4331FB95
    set_agent_team(refresh_.agent_team());
    if (skin_->options.empty() && glove_paint_->options.empty())
        initialize_default_paints();
}
void SkinsMenu::initialize_default_paints() { // 0x433D7108
    const auto previous = glove_paint_->value ? *glove_paint_->value : 0;
    rebuild_glove_paint_list(0x13A3, previous);
    rebuild_skin_list();
    if (!skin_->options.empty()) {
        const auto first = skin_->options.front().value;
        for (auto& record :
             static_cast<skins::WeaponSkinsConfig&>(descriptor(settings_, 0xB6218FEC)).value)
            record.paintkit = first;
    }
}
void SkinsMenu::set_agent_team(std::int32_t team) { // 0x433AB7C4
    const bool active = agent_->value && *agent_->value;
    if (team == 3) {
        agent_t_->set_visible(false);
        agent_ct_->set_visible(active);
    } else {
        agent_ct_->set_visible(false);
        agent_t_->set_visible(team == 2 && active);
    }
}
void SkinsMenu::rebuild_skin_list() { // 0x434084F0
    if (!skin_ || !catalog_.available())
        return;
    std::vector<ListboxOption> entries;
    auto add = [&](std::uint32_t id) {
        if (auto name = catalog_.paintkit_name(id))
            entries.push_back({std::int32_t(id), std::move(*name)});
    };
    if (filter_->value && *filter_->value) {
        for (auto id : catalog_.paintkits(std::uint16_t(selected_weapon_)))
            add(id);
    } else {
        const auto max = catalog_.maximum_paintkit();
        for (std::uint32_t id = 0; id <= max; ++id) {
            add(id);
            if (id == UINT32_MAX)
                break;
        }
    }
    skins::sort_catalog_choices(std::span<ListboxOption>(entries));
    skin_->set_options(&entries);
    skin_->config_loaded();
}
void SkinsMenu::team_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    s.select_team(s.selected_team_);
}
void SkinsMenu::weapon_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    s.select_weapon(s.selected_weapon_);
}
void SkinsMenu::filter_changed(void* p, Widget&) {
    static_cast<SkinsMenu*>(p)->rebuild_skin_list();
}
void SkinsMenu::enabled_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    auto& a = static_cast<skins::WeaponSkinsConfig&>(descriptor(s.settings_, 0xB6218FEC)).value;
    if (skins::synchronize_enabled(a, s.enabled_bits_))
        s.refresh();
}
void SkinsMenu::paint_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    if (s.enabled_->value && *s.enabled_->value)
        s.refresh();
}
void SkinsMenu::knife_enabled_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    const auto state = s.knife_->value ? *s.knife_->value : std::uint8_t(0);
    if (state != s.knife_state_) {
        s.knife_state_ = state;
        s.refresh();
    }
}
void SkinsMenu::knife_model_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    if (s.knife_->value && *s.knife_->value)
        s.refresh();
}
void SkinsMenu::agent_enabled_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    s.set_agent_team(s.refresh_.agent_team());
}
void SkinsMenu::glove_model_changed(void* p, Widget&) {
    static_cast<SkinsMenu*>(p)->rebuild_glove_paint_list();
}
void SkinsMenu::glove_enabled_changed(void* p, Widget&) {
    auto& s = *static_cast<SkinsMenu*>(p);
    s.glove_state_ = s.glove_->value ? *s.glove_->value : 0;
}
} // namespace skeet::ui
