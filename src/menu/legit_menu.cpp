#include "skeet/menu/legit_menu.hpp"
#include "config_binding.hpp"
#include <algorithm>
#include <stdexcept>

namespace skeet::ui {
using namespace menu_detail;
namespace {
struct ArrayRequirement {
    std::uint32_t hash;
    std::uint16_t size;
};
constexpr ArrayRequirement arrays[] = {
    {0x0BB660A3, 9},  {0x1435B790, 36}, {0x9D9E24B1, 36}, {0xC68AC43E, 36}, {0xF7648818, 36},
    {0x836DBF4B, 36}, {0xE7B78410, 9},  {0xAFD7D6D3, 9},  {0xE9197E9C, 9},  {0xA2FBFAC6, 36},
    {0x1716EAD7, 36}, {0x8946733C, 36}, {0xB9327935, 9},  {0x6ECF9C72, 9},  {0x771C5252, 9},
    {0xA09C5083, 36}, {0xEE182BDA, 9},  {0x5BDB158D, 36}, {0x47A24717, 36}, {0xD1DC4D24, 9},
    {0x5BC951E6, 36}, {0x8A823C30, 9},  {0x90B2820A, 36}, {0x6D349307, 9},  {0xB870359F, 9},
    {0x6FDB5A10, 9},  {0xBE6224C0, 9},  {0xC8E339F1, 9},  {0xCFCEE7E5, 9},  {0x6CEA053A, 36},
    {0xFDE7159F, 36}, {0x2D41A289, 36}, {0x2BE03F3A, 9},
};
}

LegitMenu::LegitMenu(Tab& tab, ConfigRegistries& settings, WidgetIdentities& identities)
    : tab_(tab), weapon_(&tab, u"Weapon type", {0, 24, 0, -1}, 0, true, identities) {
    tab.categories.push_back(&weapon_);
    for (const auto& required : arrays) {
        auto& d = descriptor(settings, required.hash);
        if (d.type != ConfigType::integer_array || d.serialized_size != required.size)
            throw std::invalid_argument("LEGIT requires the native array descriptor size and type");
    }
    for (std::uint32_t i = 0; i != 9; ++i) {
        auto& aim = descriptor(settings, legit::hotkey_hash(0xA31AE814, i));
        auto& trigger = descriptor(settings, legit::hotkey_hash(0xA53DC8B7, i));
        if (aim.type != ConfigType::hotkey || trigger.type != ConfigType::hotkey ||
            aim.serialized_size != 8 || trigger.serialized_size != 8)
            throw std::invalid_argument("LEGIT requires its eighteen native hotkey descriptors");
        aim_keys_[i] = &static_cast<HotkeyConfig&>(aim);
        trigger_keys_[i] = &static_cast<HotkeyConfig&>(trigger);
    }
    storage_.reserve(35);
    bindings_.reserve(35);
    arrays_.reserve(31);
    weapon_.default_fixed_height = weapon_.fixed_height = 60;
    weapon_.resizable = weapon_.movable = false;
    selector_ = &add<LegitWeaponSelector>(weapon_, 0, &selected_weapon_, identities);
    selector_->callbacks.push_back({this, weapon_changed}); // 0x433E0A18
    auto checkbox = [&](Category& owner, std::uint32_t hash,
                        std::u16string_view label) -> Checkbox& {
        auto& d = descriptor(settings, hash);
        auto& w = add<Checkbox>(owner, hash, label,
                                &static_cast<legit::BooleanArrayConfig&>(d).value[1], identities);
        arrays_.push_back({&w, &d, true});
        return w;
    };
    auto slider = [&](Category& owner, std::uint32_t hash, std::u16string_view label,
                      std::int32_t minimum, std::int32_t maximum, std::uint32_t suffix = 0,
                      float scale = 1.0f) -> Slider& {
        auto& d = descriptor(settings, hash);
        auto& w = add<Slider>(owner, hash, label, minimum, maximum,
                              &static_cast<IntegerArrayConfig&>(d).value[1], true, suffix, scale,
                              identities);
        arrays_.push_back({&w, &d, false});
        return w;
    };
    aimbot_ = std::make_unique<Category>(&tab, u"Aimbot", GridPosition{0, 12, 0, 24}, 14, true,
                                         identities);
    tab.categories.push_back(aimbot_.get());
    checkbox(*aimbot_, 0x0BB660A3, u"Enabled");
    aim_key_ = &add<HotkeyWidget>(*aimbot_, 0xA31AE814, &aim_keys_[1]->value, true, identities);
    aim_key_->callbacks.push_back({this, aim_key_changed}); // 0x4336ED87
    slider(*aimbot_, 0x9D9E24B1, u"Speed", 0, 700, 0, 0.01f);
    slider(*aimbot_, 0xC68AC43E, u"Speed (in attack)", 0, 700, 0, 0.01f);
    slider(*aimbot_, 0xF7648818, u"Speed scale - FOV", 1, 100, 0x25);
    auto& lock = slider(*aimbot_, 0x8946733C, u"Maximum lock-on time", 1, 1001, 0x73006D);
    lock.value_labels.emplace(1001, L"\u221E");
    lock.update_value_text();
    slider(*aimbot_, 0x836DBF4B, u"Reaction time", 0, 200, 0x73006D);
    slider(*aimbot_, 0x1435B790, u"Maximum FOV", 1, 900, 0xB0, 0.1f);
    add<Label>(*aimbot_, 0, u"Recoil compensation (P/Y)", identities);
    slider(*aimbot_, 0xA2FBFAC6, u"", 0, 125, 0x25);
    slider(*aimbot_, 0x1716EAD7, u"", 0, 100, 0x25);
    checkbox(*aimbot_, 0xE9197E9C, u"Quick stop");
    checkbox(*aimbot_, 0xE7B78410, u"Aim through smoke");
    checkbox(*aimbot_, 0xAFD7D6D3, u"Aim while blind");
    checkbox(*aimbot_, 0xB9327935, u"Head");
    checkbox(*aimbot_, 0x6ECF9C72, u"Chest");
    checkbox(*aimbot_, 0x771C5252, u"Stomach");
    first_delay_ = &slider(*aimbot_, 0xA09C5083, u"First shot delay range", 0, 64, 0x6E0069);
    first_delay_->set_visible(false);
    first_delay_->value_labels.emplace(0, L"Off");
    first_delay_->update_value_text();

    trigger_ = std::make_unique<Category>(&tab, u"Triggerbot", GridPosition{12, 12, 0, 16}, 12,
                                          true, identities);
    tab.categories.push_back(trigger_.get());
    checkbox(*trigger_, 0xEE182BDA, u"Enabled");
    trigger_key_ =
        &add<HotkeyWidget>(*trigger_, 0xA53DC8B7, &trigger_keys_[1]->value, true, identities);
    slider(*trigger_, 0x5BDB158D, u"Minimum hit chance", 0, 100, 0x25);
    trigger_reaction_ = &slider(*trigger_, 0x6CEA053A, u"Reaction time", 0, 200, 0x73006D);
    trigger_reaction_->callbacks.push_back({this, reaction_changed}); // 0x4336B6EB
    burst_ = &checkbox(*trigger_, 0xD1DC4D24, u"Burst fire");
    burst_time_ = &slider(*trigger_, 0x5BC951E6, u"", 1, 500, 0x73006D);
    burst_time_->set_visible(false);
    burst_link_ = {burst_time_, &burst_->tied};
    burst_->tied.next = &burst_link_;
    slider(*trigger_, 0x47A24717, u"Minimum damage", 1, 100);
    penetration_ = &checkbox(*trigger_, 0x6D349307, u"Automatic penetration");
    checkbox(*trigger_, 0xB870359F, u"Shoot through smoke");
    checkbox(*trigger_, 0x6FDB5A10, u"Shoot while blind");
    checkbox(*trigger_, 0xBE6224C0, u"Head");
    checkbox(*trigger_, 0xC8E339F1, u"Chest");
    checkbox(*trigger_, 0xCFCEE7E5, u"Stomach");

    other_ = std::make_unique<Category>(&tab, u"Other", GridPosition{12, 12, 16, 8}, 0, true,
                                        identities);
    tab.categories.push_back(other_.get());
    auto& accuracy_d = descriptor(settings, 0xFDE7159F);
    accuracy_ =
        &add<ComboBox>(*other_, 0xFDE7159F, u"Accuracy boost",
                       &static_cast<IntegerArrayConfig&>(accuracy_d).value[1], false, identities);
    accuracy_->set_options({{u"Off", 0}, {u"Low", 1}, {u"High", 2}});
    arrays_.push_back({accuracy_, &accuracy_d, false});
    accuracy_->callbacks.push_back({this, accuracy_changed}); // 0x433B7738
    accuracy_range_ = &slider(*other_, 0x2D41A289, u"Accuracy boost range", 1, 64, 0x6E0069);
    accuracy_range_->set_visible(false);
    accuracy_range_->value_labels.emplace(64, L"\u221E");
    accuracy_range_->update_value_text();
    checkbox(*other_, 0x2BE03F3A, u"Standalone recoil compensation");
    for (auto* category : categories())
        category->reflow(); // 0x433CF58B
}
LegitMenu::~LegitMenu() {
    for (auto* category : categories())
        std::erase(tab_.categories, category);
}
void LegitMenu::append_weapon_keys(std::vector<ConfigDescriptor*>& descriptors) const {
    descriptors.insert(descriptors.end(), aim_keys_.begin(), aim_keys_.end());
    descriptors.insert(descriptors.end(), trigger_keys_.begin(), trigger_keys_.end());
}
void LegitMenu::weapon_changed(void* context, Widget&) {
    static_cast<LegitMenu*>(context)->bind_weapon(); // 0x4334B817
}
void LegitMenu::aim_key_changed(void* context, Widget&) {
    static_cast<LegitMenu*>(context)->first_delay_->set_visible(false); // 0x4336ED87
}
void LegitMenu::reaction_changed(void* context, Widget&) { // 0x4341F19E
    auto& self = *static_cast<LegitMenu*>(context);
    const auto delay = self.trigger_reaction_->value ? *self.trigger_reaction_->value : 0;
    if (delay != 0)
        self.penetration_->set_value(false);
    self.penetration_->enabled = delay == 0;
}
void LegitMenu::accuracy_changed(void* context, Widget&) { // 0x4332927A
    auto& self = *static_cast<LegitMenu*>(context);
    self.accuracy_range_->set_visible(self.accuracy_->value && *self.accuracy_->value != 0);
}
void LegitMenu::bind_weapon() { // 0x4334B817..0x4334BB75
    if (selected_weapon_ < 0 || selected_weapon_ >= 9)
        return;
    const auto index = std::size_t(selected_weapon_);
    aim_key_->value = &aim_keys_[index]->value;
    aim_key_->config_loaded();
    for (std::size_t i = 0; i != arrays_.size(); ++i) {
        if (i == 16) {
            trigger_key_->value = &trigger_keys_[index]->value;
            trigger_key_->config_loaded();
        }
        auto& binding = arrays_[i];
        if (binding.bytes)
            static_cast<Checkbox*>(binding.widget)->value =
                &static_cast<legit::BooleanArrayConfig&>(*binding.descriptor).value[index];
        else {
            auto* value = &static_cast<IntegerArrayConfig&>(*binding.descriptor).value[index];
            if (auto* slider = dynamic_cast<Slider*>(binding.widget))
                slider->value = value;
            else
                static_cast<ComboBox*>(binding.widget)->value =
                    reinterpret_cast<std::uint32_t*>(value);
        }
        binding.widget->config_loaded();
    }
    reaction_changed(this, *trigger_reaction_);
    first_delay_->set_visible(false);
    accuracy_changed(this, *accuracy_);
    burst_time_->set_visible(burst_->value && *burst_->value != 0);
}

} // namespace skeet::ui
