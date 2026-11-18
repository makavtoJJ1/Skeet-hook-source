#include "skeet/menu/rage_menu.hpp"
#include "skeet/rage/settings.hpp"
#include "config_binding.hpp"
#include <algorithm>

namespace skeet::ui {
using namespace menu_detail;
namespace {
constexpr Requirement requirements[] = {
    {0x4346EDB5, ConfigType::integer}, {0xA4D31376, ConfigType::boolean},
    {0x59BD212A, ConfigType::boolean}, {0x64715C40, ConfigType::boolean},
    {0x86BFE317, ConfigType::boolean}, {0x63660060, ConfigType::boolean},
    {0xA09C0D52, ConfigType::boolean}, {0xD23ED2A7, ConfigType::hotkey},
    {0x4034958C, ConfigType::boolean}, {0x20E889DE, ConfigType::integer},
    {0xD6D22A5A, ConfigType::integer}, {0xE82BE8DD, ConfigType::boolean},
    {0x2B385DC7, ConfigType::integer}, {0x141C9037, ConfigType::boolean},
    {0xEC178029, ConfigType::hotkey},  {0x4F67FC33, ConfigType::integer},
    {0xC9FCBE4B, ConfigType::color},   {0x4CB8E4AA, ConfigType::integer},
    {rage::experimental_rage_hash, ConfigType::boolean},
    {rage::experimental_rage_option_hashes[std::size_t(rage::ExperimentalRageOption::animation)],
     ConfigType::boolean},
    {rage::experimental_rage_option_hashes[std::size_t(rage::ExperimentalRageOption::extrapolated_pose)],
     ConfigType::boolean},
};
}
RageOtherMenu::RageOtherMenu(Tab& tab, ConfigRegistries& settings, WidgetIdentities& identities)
    : tab_(tab), category_(&tab, u"Other", {12, 12, 0, 24}, 8, true, identities) {
    require_settings(settings, requirements);
    tab.categories.push_back(&category_); //43487B42, before the first bound control.
    category_.movable = category_.resizable = false;
    storage_.reserve(14 + experimental_links_.size());
    bindings_.reserve(19 + experimental_links_.size());
    auto checkbox = [&](std::uint32_t hash, std::u16string_view label) -> Checkbox& {
        return add<Checkbox>(category_, hash, label, value<BooleanConfig>(settings, hash),
                             identities);
    };
    auto& accuracy = add<ComboBox>(category_, 0x4346EDB5, u"Accuracy boost",
                                   value<IntegerConfig>(settings, 0x4346EDB5), false, identities);
    accuracy.set_options({{u"Low", 1}, {u"Medium", 3}, {u"High", 2}, {u"Maximum", 4}});
    checkbox(0xA4D31376, u"Anti-aim correction");
    checkbox(0x59BD212A, u"Automatic fire");
    checkbox(0x64715C40, u"Automatic penetration");
    checkbox(0x86BFE317, u"Silent aim");
    checkbox(0x63660060, u"Remove recoil");
    checkbox(0xA09C0D52, u"Delay shot");
    quick_peek_ = std::make_unique<QuickPeekMenu>(category_, settings, identities);
    const auto peek_bindings = quick_peek_->controls();
    bindings_.insert(bindings_.end(), peek_bindings.begin(), peek_bindings.end());
    add<Label>(category_, 0, u"Duck peek assist", identities);
    add<HotkeyWidget>(category_, 0xD23ED2A7, value<HotkeyConfig>(settings, 0xD23ED2A7), true,
                      identities);
    checkbox(0x4034958C, u"Reduce aim step");
    add<Slider>(category_, 0x20E889DE, u"Reduce aim step limit", 2, 29,
                value<IntegerConfig>(settings, 0x20E889DE), true, 0xB0, 1.0f, identities)
        .set_visible(false);
    add<Slider>(category_, 0xD6D22A5A, u"Maximum FOV", 1, 180,
                value<IntegerConfig>(settings, 0xD6D22A5A), true, 0xB0, 1.0f, identities);
    checkbox(0xE82BE8DD, u"Log misses due to spread");
    auto& mitigations =
        add<MultiSelect>(category_, 0x2B385DC7, u"Low FPS mitigations",
                         value<IntegerConfig>(settings, 0x2B385DC7), identities, false);
    mitigations.add_option("Force low accuracy boost", 1);
    mitigations.add_option("Disable multipoint: feet", 2);
    mitigations.add_option("Disable multipoint: arms", 4);
    mitigations.add_option("Disable multipoint: legs", 8);
    mitigations.add_option("Disable hitbox: feet", 0x10);
    mitigations.add_option("Lower hit chance precision", 0x40);
    mitigations.add_option("Limit targets per tick", 0x80);
    auto& experimental = checkbox(rage::experimental_rage_hash, u"Experimental RAGE");
    experimental.color = 0xBB82F0F0;
    using Option = rage::ExperimentalRageOption;
    constexpr auto& hashes = rage::experimental_rage_option_hashes;
    auto& animation = checkbox(hashes[std::size_t(Option::animation)], u"Animation rebuild");
    auto& extrapolation = checkbox(hashes[std::size_t(Option::extrapolated_pose)],
                                   u"Extrapolated pose rebuild");
    experimental_links_[0] = {&animation, &experimental_links_[1]};
    experimental_links_[1] = {&extrapolation, &experimental.tied};
    experimental.tied.next = &experimental_links_.front();
    experimental.config_loaded();
}
RageOtherMenu::~RageOtherMenu() {
    std::erase(tab_.categories, &category_);
}
} // namespace skeet::ui
